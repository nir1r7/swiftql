---
name: vectorized-audit
description: Audit SwiftQL's vectorized execution path — SelectionVector invariants, DataChunk lifecycle, late-materialization correctness at VecProjectNode, VecFilterNode completeness/exclusivity, blocking-operator behavior, and selection-vector cascading. Use when implementing or debugging any vec_*_node, or when vectorized results diverge from Volcano.
---

# Vectorized Execution Audit

Vectorized operators fail differently from Volcano: a wrong SelectionVector index silently gives row i the values of row j. These bugs pass compilation and simple tests, appearing only on specific data patterns or operator compositions.

## Core model (current, `src/execution/vec_types.h`)

- `DataChunk` — up to `BATCH_SIZE = 1024` rows in columnar layout (`vector<ColumnVector>`). Distinct from storage zone-map `CHUNK_SIZE = 8192`.
- `SelectionVector` — `{vector<int> indices; int size;}` of valid row indices into the chunk. Data never moves through filters; only indices change.
- Late materialization: data stays columnar through filters; only `VecProjectNode` materializes, only required columns, only for selected rows.
- Week 21+: multi-conjunct scan-local predicates CASCADE selection vectors (each predicate filters the previous survivor set) rather than set-intersecting.

## 1. SelectionVector invariants

For every SelectionVector produced anywhere:
- **Range:** every `indices[i]` ∈ `[0, chunk.num_rows)`
- **Strictly increasing** (no duplicates in the base pipeline)
- **`size == indices.size()`** and `size <= chunk.num_rows`
- **Cascade:** a downstream predicate's output must be a subset of its input selection — never resurrect a filtered-out index

## 2. VecFilterNode: completeness + exclusivity

For predicate P over input selection of length N:
- **Completeness:** every input index whose row satisfies P appears in output — missing one is silent data loss
- **Exclusivity:** every output index satisfies P — including a failing row is silent corruption
- **NULL:** predicate evaluating to NULL → index excluded (falsy)
- Dictionary-encoded string columns: predicate compares int32 IDs in the hot loop — verify the ID translation of the literal happens once, outside the loop, and handles the literal-not-in-dictionary case (empty result, not a crash)

Protocol: hand-compute expected indices for a concrete chunk, diff against actual output.

## 3. Late materialization at VecProjectNode

- Output row i gathers from chunk position `sel.indices[i]` — NOT position `i` (the classic bug)
- Output has exactly `sel.size` rows; columns in SELECT-list order; resolution via `relation_slot`
- Test with a NON-CONTIGUOUS selection (e.g. `[0, 2, 5]`) — contiguous selections mask gather bugs

## 4. Blocking operators (`VecHashAggregateNode`, `VecSortNode`, `VecDistinctNode`)

- First `nextChunk()`: consume ALL input chunks, compute, then start emitting
- Subsequent calls: emit next output chunk — never re-read input
- Verify aggregate covers all chunks, not just the first (multi-chunk input test, >1024 rows)
- `VecSortNode`/`VecDistinctNode` collect only SURVIVING rows (respect incoming selection) before sorting/deduplicating

## 5. VecHashJoinNode

- Probe operates per-chunk; batch lookup into the build-side map
- Output schema fixed `[FROM] ++ [JOIN]` via `swapped_` — never follows build side
- A probe row with k build matches emits k rows; verify chunk-boundary handling when matches overflow the output chunk

## 6. VecLimitNode early termination

- Tracks rows across chunks; truncates the final chunk
- Must stop pulling from its child after the limit — verify the scan actually stops (check `rows_in` via `--explain-analyze`)

## Output format

Per component: **CORRECT** (with evidence), **BUG** (operator, `file:line`, expected vs produced, minimal reproducing chunk), or **UNTESTED**. Priority: corruption bugs → missing coverage → hardening. Cross-check every finding against Volcano output and SQLite (`bisect-stage` skill).
