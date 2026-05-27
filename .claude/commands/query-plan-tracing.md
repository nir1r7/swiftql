---
description: Trace execution flow through a SwiftQL query plan tree, following operators recursively and explaining tuple/data flow at each stage.
---

# Query Plan Tracing

You are tracing execution flow through a SwiftQL query plan. SwiftQL uses a volcano (iterator) model in Phase 1/2 and a vectorized chunk model in Phase 3. Both are tree-shaped: each operator has one or two children it pulls rows (or chunks) from.

## Protocol

1. **Identify the root node** from the plan tree (e.g. from `--explain` output or from `src/planner/plan_nodes.h`). The root is always the topmost operator — usually `ProjectNode` or `LimitNode`.

2. **Walk the tree top-down**, naming each operator and its role:
   - `SeqScanNode` / `VecScanNode` — leaf; reads raw rows or chunks from storage
   - `FilterNode` / `VecFilterNode` — applies predicate; rows that fail are dropped here, never reach parent
   - `ProjectNode` / `VecProjectNode` — narrows schema to output columns; materialization boundary in vectorized mode
   - `HashAggregateNode` / `VecHashAggregateNode` — pipeline breaker; consumes all input before emitting any output
   - `HavingNode` — post-aggregation filter; schema width unchanged
   - `DistinctNode` — pipeline breaker; hashes all input, deduplicates
   - `SortNode` — pipeline breaker; consumes all input, sorts in memory
   - `LimitNode` — pass-through until N rows emitted, then stops pulling
   - `HashJoinNode` / `VecHashJoinNode` — build phase (smaller child, full pipeline break) then probe phase

3. **Mark pipeline breakers** explicitly. A pipeline breaker means all upstream rows are materialized into memory before any downstream row is produced. In SwiftQL these are: `HashAggregateNode`, `DistinctNode`, `SortNode`, and the build phase of `HashJoinNode`.

4. **Track schema evolution** at each node:
   - `SeqScan`: emits full table schema (or pruned columns under columnar mode)
   - `Filter`: schema unchanged, row count reduced
   - `Project`: schema narrows to output columns
   - `HashAggregate`: schema becomes `[group_by_columns..., agg_result_columns...]`
   - `Having`: schema unchanged
   - `HashJoin`: schema is left table columns + right table columns concatenated

5. **Note materialization boundaries** in vectorized mode: `VecFilterNode` produces a `SelectionVector` (indices only, no data copy). Data is only materialized when `VecProjectNode` reads the selection vector and extracts columns. State this explicitly.

6. **Estimate row counts** at each operator boundary if statistics or test data sizes are available.

## Output Format

```
[root] ProjectNode
  schema_out: [team, AVG(speed)]
  ↑ pulls from:

  [1] HavingNode
    predicate: AVG(speed) > 300
    schema: unchanged from child
    ↑ pulls from:

    [2] HashAggregateNode  ← PIPELINE BREAKER
      group_by: team
      agg: AVG(speed)
      schema_out: [team, AVG(speed)]
      ↑ consumes all rows from:

      [3] FilterNode
        predicate: season = 2025
        rows_in: ~1,000,000  rows_out: ~48,000
        schema: unchanged
        ↑ pulls from:

        [4] SeqScanNode
          table: laps
          columns_read: [team, speed, season]
          rows_out: ~1,000,000
```

Always conclude with a plain-English summary of the data flow in one paragraph.
