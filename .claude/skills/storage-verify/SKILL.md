---
name: storage-verify
description: Verify SwiftQL's columnar storage layer — chunk structure, RLE and dictionary encoding invariants, zone-map min/max metadata, chunk-pruning safety, and columnar-to-row reconstruction. Use when modifying src/storage/, adding encodings, changing the loader, or when columnar-mode results diverge from row-mode.
---

# Columnar Storage Verification

Storage bugs are silent: encoding bugs corrupt values, zone-map errors skip live rows, transposition pollutes columns. The row-based path is the unaffected baseline — any columnar-vs-row divergence is a storage bug until proven otherwise.

## 1. Chunk structure (`ColumnChunk`, `CHUNK_SIZE = 8192`)

- All chunks except the last hold exactly `CHUNK_SIZE` rows; totals across chunks equal `row_count` for EVERY column
- Chunk boundaries identical across columns: row i lives in chunk `i / CHUNK_SIZE`, offset `i % CHUNK_SIZE`, in every column simultaneously

## 2. Encodings (current implementations)

### RLE (`src/storage/rle_column.h`)
`(value, run_length)` pairs + parallel `run_starts` prefix-sum; `get(row_idx)` binary-searches `run_starts` — O(log n_runs). Applied only when `n_runs < n_rows / 4`; otherwise raw `vector<int64_t>`.

- Sum of run lengths == row count; `run_starts[i]` == sum of lengths before run i (strictly increasing, starts at 0)
- No two consecutive runs share a value (must be merged)
- Round-trip: decode → naive re-encode → identical
- Threshold respected: columns at/above `n_rows/4` runs stay raw
- Classic bug: off-by-one at a run boundary — `get(run_starts[i])` and `get(run_starts[i]-1)` must return different runs' values

### Dictionary (`src/storage/dictionary_encoder.h`)
Unique strings → dense `int32_t` IDs; column stored as `vector<int32_t>`.

- Every ID in the column exists in the dictionary; dictionary injective (no duplicate strings); IDs dense from 0
- Round-trip: `dict[code[i]]` reproduces the original column exactly
- Predicate fast path: literal translated to ID once; literal absent from dictionary → empty match, not a crash

## 3. Zone maps

Per chunk per column `(min, max)`, computed at load, never mutated.

- `min <= every value <= max` within the chunk (typed comparison — INT compared numerically, never lexicographically)
- NULL handling: min/max over non-NULL values only; all-NULL chunk → undefined markers, not 0/""

## 4. Chunk-pruning safety (`chunk_pruner.h`)

Skip a chunk iff NO row can satisfy the predicate:

| Predicate | Safe skip condition |
|---|---|
| `col = C` | `C < min` or `C > max` |
| `col > C` | `max <= C` |
| `col >= C` | `max < C` |
| `col < C` | `min >= C` |
| `col <= C` | `min > C` |
| `col != C` | **never skippable** via zone map |

- **False negatives (skipping a chunk with matches) are bugs; false positives are only inefficiency**
- Boundary bug class: `<` vs `<=` confusion when the constant equals min/max — test predicates exactly at chunk min/max
- Pruner only prunes FROM-side predicates (`relation_slot < 1`) — never JOIN-side
- Verify engagement: `--explain-analyze` scan `rows_in` < table rows when a prunable predicate is present; `== total` means pruning silently disengaged

## 5. Columnar-to-row reconstruction

- Row i, column j = decoded `column[j]` at position i; column order matches Schema exactly
- Differential check: load same CSV via both paths, sort both outputs by all columns, compare byte-identical (pruning disabled)
- Classic bug: column index off-by-one during assembly — column j gets column j+1's values

## 6. Loader constraints

- CSV cannot express NULLs (empty numeric field throws in `csv_loader.cc parseField`) — null paths need in-memory unit tests
- Commas inside string values unsupported
- Phase 5's pipe-delimited TPC-H loader must re-verify every invariant above at scale-factor sizes

## Output format

Per component: **PASS** (with evidence) / **FAIL** (chunk index, column, expected vs actual, producing code path) / **SKIP** (not applicable). Summary table with risk-if-violated. Any FAIL in encoding or pruning blocks all other work — it corrupts every query above it.
