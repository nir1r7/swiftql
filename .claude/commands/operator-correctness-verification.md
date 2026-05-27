---
description: Verify whether a SwiftQL relational operator implementation matches correct relational algebra semantics across projection, join, aggregation, NULL handling, deduplication, iterator lifecycle, schema propagation, and ordering.
---

# Operator Correctness Verification

You are verifying the correctness of a SwiftQL operator implementation against expected relational semantics. Database bugs are often semantic, not syntactic — the code compiles and runs but produces wrong answers.

## Checklist

For each operator under review, work through every applicable check below. Mark each ✅ correct / ❌ wrong / ⚠️ uncertain, and explain your reasoning.

### Projection (`ProjectNode` / `VecProjectNode`)
- [ ] Output schema matches exactly the SELECT list — no extra columns, no missing columns
- [ ] Column references resolve against the correct input schema (not the table schema)
- [ ] Aliased expressions produce the aliased name in output schema
- [ ] In vectorized mode: columns are only materialized for rows passing the `SelectionVector`
- [ ] No side effects on the input row (row is read-only)

### Filter (`FilterNode` / `VecFilterNode`)
- [ ] Predicate evaluated against the correct schema
- [ ] NULL semantics: `NULL = x` evaluates to NULL (falsy), not true or false
- [ ] `IS NULL` / `IS NOT NULL` correctly identifies null `Value` objects
- [ ] Rows failing the predicate are never emitted — they are not passed through as NULL rows
- [ ] In vectorized mode: `SelectionVector` contains only indices of passing rows; no data copied

### Join (`HashJoinNode` / `VecHashJoinNode`)
- [ ] Build phase uses the smaller table (by row count from catalog stats)
- [ ] Hash key includes all join columns, not just the first
- [ ] Probe emits one output row per (probe_row, matching_build_row) pair — correct cardinality
- [ ] Inner join: rows with no match on either side are dropped (not emitted as NULLs)
- [ ] Output schema is left_columns + right_columns, in that order
- [ ] Duplicate keys in build side: all matching build rows emitted for each probe row
- [ ] Build hash map cleared after query (no cross-query state leak)

### Aggregation (`HashAggregateNode` / `VecHashAggregateNode`)
- [ ] `COUNT(*)` counts all rows including NULLs
- [ ] `COUNT(col)` counts only non-NULL values of `col`
- [ ] `SUM`, `AVG`, `MIN`, `MAX` skip NULL values (do not treat NULL as zero)
- [ ] `AVG` computed as running sum / count, not sum of (1/n) (floating point safety)
- [ ] Groups with no matching rows are not emitted (no phantom groups)
- [ ] Group key comparison uses value equality, not pointer equality
- [ ] All input rows consumed before any output row emitted (pipeline breaker semantics)

### Having (`HavingNode`)
- [ ] Filter applied after aggregation, against the aggregated schema
- [ ] Same NULL semantics as `FilterNode`
- [ ] `HAVING` without `GROUP BY` is rejected at planning time (validator check)

### Distinct (`DistinctNode`)
- [ ] Two rows are duplicates iff all column values are equal
- [ ] NULL == NULL for deduplication purposes (two NULL values in the same column are duplicates)
- [ ] All input consumed before first output (pipeline breaker semantics)

### Sort (`SortNode`)
- [ ] Default sort order: ascending
- [ ] NULLs: standard SQL orders NULLs last in ascending order — verify SwiftQL's behavior matches its documented semantics
- [ ] Stable sort (equal keys preserve input order) — verify if required by tests
- [ ] All input consumed before first output (pipeline breaker semantics)

### Limit (`LimitNode`)
- [ ] Exactly N rows emitted, not N+1 or N-1
- [ ] Stops pulling from child after N rows — does not exhaust child
- [ ] N=0 emits zero rows

### Schema Propagation
- [ ] Each operator's `output_schema()` matches what `next()` / `nextChunk()` actually produces
- [ ] Schema width (column count) is constant across all rows emitted by an operator
- [ ] Schema passed to child matches what child expects

### Iterator Lifecycle (Volcano model)
- [ ] `open()` called before first `next()`
- [ ] `close()` called after last `next()` (including early termination via `LimitNode`)
- [ ] `next()` returns `nullptr` on exhaustion, never crashes
- [ ] Second call after `nullptr` returns `nullptr` again (idempotent exhaustion)
- [ ] `close()` called on children recursively

## Output Format

For each operator, produce:
1. A completed checklist with ✅/❌/⚠️ per item
2. For each ❌ or ⚠️: the file path and line number of the suspect code, a one-sentence description of the bug, and the minimal fix
3. A final verdict: **CORRECT** / **INCORRECT** / **NEEDS INVESTIGATION**
