---
description: Verify correctness of SwiftQL's columnar storage layer — column chunk structure, RLE and dictionary encoding invariants, zone-map min/max metadata, chunk pruning predicate evaluation, and the columnar-to-row reconstruction boundary.
---

# Columnar Storage Verification

You are verifying SwiftQL's columnar storage layer. Columnar storage introduces failure modes that don't exist in the row-based path: encoding bugs cause silent data corruption, zone-map errors cause incorrect row skipping or missed pruning, and column transposition causes cross-column value pollution. Phase 1's row-based path is unaffected — this skill applies to Phase 2+ columnar code.

## 1. Column Chunk Structure Invariants

For each column in a columnar table:
- All chunks except the last must contain exactly `chunk_size` rows (typically 1024)
- The last chunk contains `total_rows % chunk_size` rows (may be 0 if total_rows is a multiple)
- Total values across all chunks = total_rows for every column (no column has more or fewer values)
- Chunk boundaries are identical across all columns for the same table — row i is always in chunk `i / chunk_size` at offset `i % chunk_size`, for every column simultaneously

**How to verify:** Sum `chunk.size()` across all chunks for each column. All columns must produce the same total.

## 2. Encoding Correctness

### RLE (Run-Length Encoding)

RLE stores `(value, run_length)` pairs.

Invariants:
- Sum of all `run_length` values equals the chunk's total row count
- No two consecutive runs have the same value (adjacent equal values must be merged into one run)
- Decoding: for each `(v, n)` pair, emit `v` exactly `n` times in order

**Verification protocol:**
1. Decode the RLE chunk to a flat value array
2. Re-encode the flat array naively
3. Compare decoded arrays — must be byte-identical
4. Check that consecutive run values are distinct

**Common bug:** Run boundary off-by-one — a value appears at the wrong position after a long run.

### Dictionary Encoding

Dictionary encoding stores a `{code → value}` map and a `code[]` array per chunk.

Invariants:
- Every code in the `code[]` array exists as a key in the dictionary
- No two dictionary entries have the same value (dictionary is injective)
- Code values are dense starting from 0 (no gaps), or at minimum form a valid finite domain
- Decoding: `code[i]` → `dictionary[code[i]]` for each position

**Verification protocol:**
1. Decode the dictionary chunk to a flat value array
2. Verify every emitted value matches the dictionary entry for its code
3. Verify dictionary has no duplicate values

**Common bug:** Dictionary built per-chunk but code namespace not reset between chunks — codes from chunk N collide with codes from chunk N+1.

## 3. Zone-Map Metadata Correctness

Zone-maps store `(min, max)` per chunk per column for predicate-based chunk skipping.

Invariants:
- `zone_map[col][chunk].min` ≤ every value in `column[col].chunks[chunk]`
- `zone_map[col][chunk].max` ≥ every value in `column[col].chunks[chunk]`
- If a chunk contains NULL values, min/max are computed over non-NULL values only; if all values are NULL, min/max are marked as "undefined" (not zero or empty string)
- Zone-maps are computed at load time and never mutated after

**Verification protocol:**
1. For each chunk, scan all values
2. Confirm `actual_min == zone_map.min` and `actual_max == zone_map.max`
3. For NULL-heavy chunks, confirm NULL values are excluded from min/max

**Common bug:** Zone-map computed before type coercion — comparing string "10" < "9" lexicographically when the column is actually INT.

## 4. Chunk Pruning Predicate Evaluation

A chunk can be safely skipped if and only if **no row in the chunk can satisfy the predicate**.

For a predicate `col OP constant`:
- `col = C`: skip if `C < zone_min` or `C > zone_max`
- `col > C`: skip if `zone_max <= C`
- `col >= C`: skip if `zone_max < C`
- `col < C`: skip if `zone_min >= C`
- `col <= C`: skip if `zone_min > C`
- `col != C`: **never safe to skip** based on zone-map alone

**Verification protocol — for each predicate that triggers pruning:**
1. Identify which chunks were skipped
2. Scan skipped chunks manually and confirm zero rows would have satisfied the predicate
3. Identify which chunks were not skipped
4. Confirm at least one row in each non-skipped chunk satisfies the predicate (otherwise pruning was too conservative but not wrong)

**Correctness rule:** False negatives (pruning a chunk that has matches) are bugs. False positives (keeping a chunk with no matches) are merely inefficient, not incorrect.

**Common bug:** Predicate uses `<=` zone-map logic but predicate is `<` — keeps chunks where only the max value equals the constant (which should be excluded).

## 5. Columnar-to-Row Reconstruction

When the executor calls `SeqScanNode::next()` on columnar storage, it receives a `Row` assembled from per-column chunks.

Invariants:
- Row i's value for column j = `column[j].chunks[i / chunk_size][i % chunk_size]` (after decoding)
- Column order in the reconstructed `Row` matches the table's Schema column order exactly
- No column is omitted; no column appears twice
- If a chunk was skipped (zone-map pruning), its rows are never reconstructed (they are simply not emitted)

**Verification protocol:**
1. Load the same CSV via both row-based and columnar paths
2. Collect all rows from each path (disable all pruning)
3. Sort both by every column
4. Compare — rows must be byte-identical

**Common bug:** Column index off-by-one when assembling the Row — column j gets the value from column j+1.

## 6. Storage/Execution Boundary

Columnar storage still feeds the same `SeqScanNode` iterator interface. All Volcano lifecycle invariants still apply:
- `open()` initializes chunk cursor to chunk 0, row offset 0
- `next()` advances cursor; on chunk boundary, advances to next non-pruned chunk
- `close()` releases chunk references
- After pruning, rows_in reported by EXPLAIN ANALYZE reflects only non-pruned rows — not total table rows

**Check:** Run `--explain-analyze` on a query with a prunable predicate. `SeqScanNode rows_in` should be less than total table row count when pruning fires. If `rows_in == total_rows`, zone-map pruning is not engaging.

## Output Format

For each verification step, report:
- **PASS** — invariant holds, with evidence
- **FAIL** — invariant violated, with: chunk index, column name, expected value, actual value, and which code path produced it
- **SKIP** — not applicable (e.g., no RLE encoding in use)

End with a summary table:

| Component | Status | Risk if violated |
|---|---|---|
| Chunk structure | | Data loss or extra rows |
| RLE encoding | | Silent value corruption |
| Dictionary encoding | | Cross-chunk code collision |
| Zone-map metadata | | Incorrect pruning |
| Chunk pruning logic | | False negative = missing rows |
| Row reconstruction | | Column transposition |
| Storage/execution boundary | | Lifecycle violation |
