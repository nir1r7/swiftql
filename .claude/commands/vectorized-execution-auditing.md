---
description: Audit SwiftQL's vectorized execution layer — SelectionVector invariants, DataChunk lifecycle, late materialization correctness at VecProjectNode, VecFilterNode output completeness, pipeline breaker behavior, and the vectorized/Volcano boundary.
---

# Vectorized Execution Auditing

You are auditing SwiftQL's vectorized execution path (Phase 3). Vectorized operators fail differently than Volcano — a wrong SelectionVector index causes silent data corruption (row i gets row j's values), and late materialization bugs only manifest when projection and filtering are composed. These bugs pass compilation and often pass basic tests; they appear only on specific data patterns or compositions.

## Core Concepts (establish these before auditing)

**DataChunk:** A batch of up to 1024 rows stored in columnar layout — one `vector<Value>` per column. Columns are not copied as data flows through filter nodes; only indices change.

**SelectionVector:** An array of indices (`uint16_t sel[]`) into the current DataChunk. `sel[i]` is the index of the i-th surviving row. Length ≤ chunk_size. Data in the DataChunk is not moved; the SelectionVector says which rows are "active."

**Late materialization:** Data stays in DataChunk columnar layout through all filter nodes. Only `VecProjectNode` gathers selected columns from the DataChunk into output rows. No data copy happens until materialization.

## 1. SelectionVector Invariants

For every SelectionVector produced or consumed by any operator:

- **Range:** Every `sel[i]` must be in `[0, chunk_size)` — never negative, never ≥ chunk_size
- **Non-decreasing:** `sel[i] <= sel[i+1]` for all i (monotonically non-decreasing, duplicates allowed only if the same row genuinely appears twice, which is rare)
- **No spurious duplicates:** Unless the operator explicitly produces duplicates (none in the base pipeline), each index appears at most once
- **Length:** `sel.length <= chunk_size`; `sel.length` equals the number of surviving rows after this operator

**Verification:** After any operator produces a SelectionVector, scan `sel[]` and assert all three properties. A single out-of-range index causes undefined memory access on the next DataChunk dereference.

## 2. DataChunk Lifecycle

- **Allocation:** DataChunk is allocated once per pipeline execution; columns are populated by `VecSeqScanNode`
- **Through filter path:** DataChunk is passed by reference (or pointer) to filter nodes; they read column values at `sel[i]` indices but do not modify the DataChunk
- **Invariant:** No node except `VecSeqScanNode` (and `VecProjectNode` for output) writes to DataChunk column data
- **Ownership:** DataChunk is owned by the scan node; filter nodes borrow it; project node reads it

**How to verify:** Add an assertion that DataChunk column data is identical before and after any `VecFilterNode::next()` call. Any modification is a bug.

## 3. VecFilterNode Correctness

`VecFilterNode` takes an input SelectionVector (or full chunk) and produces a subset SelectionVector.

For a chunk with `input_sel` of length N and predicate P:

**Completeness:** Every index `input_sel[i]` where `P(row[input_sel[i]])` is true must appear in `output_sel`. Missing a passing row is a silent data loss bug.

**Exclusivity:** Every index in `output_sel` must satisfy the predicate. Including a failing row is a silent data corruption bug.

**NULL semantics:** An index where the predicate evaluates to NULL must NOT appear in `output_sel` (NULL predicate result = falsy for filtering).

**Verification protocol:**
1. For a concrete DataChunk and predicate, compute the expected output indices by hand
2. Run `VecFilterNode::next()` and compare `output_sel` to expected
3. Confirm length matches
4. For each index in expected but not in output: **missing row bug**
5. For each index in output but not in expected: **spurious row bug**

## 4. Late Materialization Correctness at VecProjectNode

`VecProjectNode` is the only node that reads DataChunk column data and produces output `Row` objects.

For each surviving index `sel[i]`:
- **Row correspondence:** Output row `i` must contain values from DataChunk row `sel[i]`, not DataChunk row `i`
- **Column subset:** Output contains only projected columns (SELECT list), in SELECT list order, not table schema order
- **Expression evaluation:** Computed expressions (e.g., `speed * 1.1`) are evaluated on DataChunk values at `sel[i]`
- **No off-by-one:** Output has exactly `sel.length` rows — no extra, no missing

**Common bug:** `VecProjectNode` iterates `for i in 0..sel.length` but indexes DataChunk at `i` instead of `sel[i]` — produces wrong rows silently.

**Verification protocol:**
1. Set up a DataChunk with known values in each column
2. Set up a SelectionVector selecting non-contiguous rows (e.g., sel = [0, 2, 5])
3. Run `VecProjectNode` and inspect each output row
4. Confirm output row 0 = DataChunk row 0, output row 1 = DataChunk row 2, output row 2 = DataChunk row 5

## 5. Pipeline Breaker Behavior in Vectorized Mode

`VecHashAggregateNode` and `VecSortNode` are blocking — they must consume all input chunks before emitting the first output chunk.

Invariants:
- On first `next()` call: pull input chunks in a loop until input is exhausted, then compute result, then start emitting output chunks
- On subsequent `next()` calls: emit the next output chunk (do not re-read input)
- After input is exhausted (input returns empty chunk): never call input's `next()` again

**Common bug:** Pipeline breaker calls input's `next()` once per output chunk — re-reads input on every call, producing wrong results or infinite loops.

**Verification:** Run an aggregate query with multiple input chunks. Confirm the aggregate result is over all input rows, not just the first chunk.

## 6. Vectorized/Volcano Boundary

If Volcano operators (e.g., `LimitNode`, `HavingNode`) wrap vectorized operators in the plan tree, the boundary must be clean.

**Invariant:** When a Volcano `next()` call triggers vectorized execution, the vectorized path runs to completion for that chunk, producing a set of materialized rows, then returns them one at a time via successive `next()` calls.

**Schema invariant:** The `output_schema()` of a vectorized node as seen by its Volcano parent must match the schema of the materialized rows it produces.

**Verification:** Run a query where `LimitNode` (Volcano) wraps a `VecFilterNode` (vectorized). Confirm:
1. Correct rows are returned (no schema mismatch)
2. `LimitNode` stops pulling after N rows (early termination works across the boundary)
3. `close()` on the Volcano parent correctly propagates close to the vectorized child

## Output Format

For each audited component, report:

**CORRECT** — invariant holds, with evidence (data + expected + actual)
**BUG** — invariant violated, with: operator name, file:line, what was expected, what was produced, and a minimal reproducing DataChunk
**UNTESTED** — component exists but no verification was possible

End with a priority list:
1. Bugs (data corruption — fix before any other work)
2. Missing coverage (untested components)
3. Hardening opportunities (invariants that hold now but are fragile)
