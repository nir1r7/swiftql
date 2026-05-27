---
description: Validate that the SwiftQL planner's assumptions, the executor's expectations, and the schema contracts all align at the planner/executor boundary — the most common source of boundary bugs in database systems.
---

# Planner/Executor Boundary Validation

You are validating the contract between SwiftQL's planner and its executor. The planner builds a plan tree; the executor walks that tree. Bugs at this boundary are common because the planner and executor are written independently and can drift apart silently.

## The Contract

The planner/executor boundary has three components:

1. **Plan node fields** — what the planner stores in a plan node struct
2. **Executor reads** — what the executor reads from that struct at runtime
3. **Schema contract** — what schema the executor claims to produce vs. what it actually produces

All three must agree. If the planner stores a column name and the executor reads a column index, or the planner stores the schema and the executor ignores it, bugs follow.

---

## Check 1 — Plan Node Field Completeness

For each plan node type in `src/planner/plan_nodes.h`, verify:

- Every field the executor reads at runtime is populated by the planner
- No field is left at its default value when the executor depends on it being set
- Optional fields: executor handles both present and absent cases

Format:
```
HashAggregateNode fields:
  group_by_columns: vector<string>   planner sets: ✅   executor reads: ✅
  aggregates: vector<AggExpr>        planner sets: ✅   executor reads: ✅
  output_schema: Schema              planner sets: ✅   executor reads: ⚠️  (executor recomputes it — drift risk)
  child: PlanNode*                   planner sets: ✅   executor reads: ✅
```

---

## Check 2 — Schema Contract

For each plan node, validate that the output schema the planner declares matches the schema the executor actually produces.

**Protocol:**
1. Find where the planner sets `output_schema` for the node (or where it is computed)
2. Find where the executor produces rows and check the column order, names, and types
3. Verify they match

**High-risk nodes:**
- `ProjectNode`: planner derives schema from SELECT list; executor evaluates expressions — verify column order matches
- `HashAggregateNode`: schema is `[group_by_columns..., agg_results...]` — verify agg result column names match what downstream `HavingNode` or `ProjectNode` expects
- `HashJoinNode`: schema is `[left_columns..., right_columns...]` — verify left schema and right schema are concatenated in the correct order, with no name collision handling (or verify collisions are handled)
- `SortNode`: schema unchanged — verify executor does not accidentally drop or reorder columns

---

## Check 3 — Column Resolution at Runtime

The executor resolves column references by name (via schema lookup). Verify:

- Column names stored in plan node expressions match column names in the schema at that plan node's level
- Especially in joins: after `HashJoinNode`, upstream operators reference columns by their qualified name (`laps.team`) or unqualified name (`team`) — verify the schema uses the right naming convention
- After `ProjectNode`, upstream operators can only reference the projected columns — verify no upstream operator references a column that was projected away
- In `HavingNode`: the predicate references aggregate result column names (e.g. `AVG(speed)`) — verify the `HashAggregateNode` output schema uses exactly those names

---

## Check 4 — Child Pointer Wiring

Each plan node holds a pointer to its child (or children for join). Verify:

- `child` pointer is set by the planner, not null
- For `HashJoinNode`: `left_child` and `right_child` are set; the build side choice is reflected in which child is labeled "build" vs "probe" — and the executor respects this
- The plan node tree is a valid tree (no cycles, no shared subtrees — each node has exactly one parent)
- Plan nodes are heap-allocated and owned by the planner — no dangling pointer risk

---

## Check 5 — Optimizer/Executor Agreement

After the optimizer rewrites the plan tree (Phase 4+), verify the executor still sees a valid tree:

- Predicate pushdown: `FilterNode` moved below `HashAggregateNode` — the filter predicate now references columns in the scan schema, not the aggregate schema. Verify column names resolve correctly at the new position.
- Join side swap: if the optimizer swaps `left_child` and `right_child` of `HashJoinNode`, verify the executor reads the correct child as the build side.
- Predicate reordering: multiple predicates in a `FilterNode` reordered — verify expression evaluation order is not coupled to any external state.

---

## Check 6 — EXPLAIN vs Execution Agreement

`--explain` and `--explain-analyze` print the plan tree as the planner produces it. Verify:

- The `explain()` output for each plan node accurately describes what the executor will do
- `rows_in` / `rows_out` in `EXPLAIN ANALYZE` are updated by the executor, not the planner — verify they are wired to the correct measurement points
- A discrepancy between `--explain` output and actual execution behavior indicates a planner/executor contract violation

---

## Output Format

```
Plan Node: HashJoinNode
─────────────────────────────────────────────────────────────────
Field completeness:
  left_child          ✅ set by planner, read by executor
  right_child         ✅ set by planner, read by executor
  join_column_left    ✅ set by planner, read by executor
  join_column_right   ✅ set by planner, read by executor
  output_schema       ❌ planner sets [laps.*, drivers.*] but executor produces [laps.team, drivers.name] (wrong width)

Schema contract:
  Planner declares:   [lap_id, driver_id, team, speed, season, driver_id, name, nationality, age]  (9 cols, duplicate driver_id)
  Executor produces:  [lap_id, driver_id, team, speed, season, name, nationality, age]              (8 cols, driver_id deduplicated)
  Verdict: ❌ MISMATCH — upstream ProjectNode resolves driver_id against wrong schema

Column resolution:
  ⚠️ After join, "driver_id" is ambiguous — both tables have this column. Verify resolution policy.

Verdict: CONTRACT VIOLATION at HashJoinNode output schema. Fix: decide on duplicate column handling policy and enforce it consistently in both planner and executor.
```
