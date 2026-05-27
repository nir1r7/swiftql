---
description: Interpret SwiftQL's EXPLAIN and EXPLAIN ANALYZE output to diagnose correctness bugs and performance bottlenecks — reading the node tree, detecting row count anomalies, localizing bugs to specific operators, and correlating timing with plan structure across all phases.
---

# EXPLAIN ANALYZE Interpretation

You are reading SwiftQL's EXPLAIN or EXPLAIN ANALYZE output to diagnose a problem. The output is the primary diagnostic tool for this engine — it shows the full operator tree, row counts, timing, and execution fractions. Learn to read it before touching code.

## Output Format Reference

```
[ProjectNode]        rows_in=500  rows_out=3   time=0.12ms  (2.1%)
  [HashAggregateNode] rows_in=500  rows_out=3   time=4.81ms  (84.3%)
    [FilterNode]       rows_in=1000 rows_out=500 time=0.61ms  (10.7%)
      [SeqScanNode]    rows_in=1000 rows_out=1000 time=0.17ms (3.0%)

Parse: 0.03ms  Plan: 0.02ms  Execution: 5.71ms  Total: 5.76ms
```

- **rows_in**: rows pulled from children (input to this operator)
- **rows_out**: rows emitted to parent (output of this operator)
- **time**: exclusive self-time (does NOT include child execution time)
- **(%)**: this node's exclusive time as a percentage of total execution time
- Footer: Parse / Plan / Execution broken out; Execution = sum of all node self-times

## Step 1: Read the Tree Top-Down

Root is at top. Children are indented beneath their parent. Data flows **up** (children push to parents via pull model — parent calls child's `next()`).

Identify:
1. What is the root node? (Usually `ProjectNode` or `LimitNode`)
2. How many operators are in the tree?
3. Which operators are pipeline breakers? (`HashAggregateNode`, `SortNode`, `DistinctNode`, `HashJoinNode` build phase) — these accumulate all input before emitting first output

## Step 2: Row Count Anomaly Detection

Check every operator's `rows_in` and `rows_out` against its expected behavior:

| Operator | Expected relationship | Anomaly |
|---|---|---|
| `SeqScanNode` | `rows_out == rows_in == table_rows` (or < if columnar pruning) | `rows_out > rows_in` = impossible |
| `FilterNode` | `rows_out <= rows_in` | `rows_out > rows_in` = **bug** |
| `ProjectNode` | `rows_out == rows_in` | Any mismatch = **bug** |
| `HashAggregateNode` | `rows_out <= rows_in`; `rows_out` = number of groups | `rows_out > rows_in` = **bug** |
| `HavingNode` | `rows_out <= rows_in` | `rows_out > rows_in` = **bug** |
| `DistinctNode` | `rows_out <= rows_in` | `rows_out > rows_in` = **bug** |
| `SortNode` | `rows_out == rows_in` | Any mismatch = **bug** |
| `LimitNode` | `rows_out <= min(rows_in, N)` | `rows_out > N` = **bug** |
| `HashJoinNode` | `rows_out` can exceed `rows_in` (cross-product) | Depends on data |

**If rows_out == 0 but the query should return results:**
- Schema mismatch causing all rows to fail a predicate — check column name resolution
- Early termination bug — operator returning nullptr after first call
- Empty child output propagating up

**If rows_out == rows_in on a selective FilterNode:**
- Predicate not evaluating (always returns true or null)
- Wrong schema reference — predicate references a column that doesn't exist and silently evaluates to null
- NULL semantics: if most rows have NULL in the predicate column, all evaluate to NULL (falsy), resulting in rows_out ≈ 0, not rows_out == rows_in

## Step 3: Localize the Bug

Work from the bottom up. The first operator where the row count is wrong is the likely source.

**Protocol:**
1. Start at `SeqScanNode` — is `rows_out` equal to the expected table row count?
2. Move up one level — does the next operator's `rows_in` equal the previous `rows_out`? (They must match — parent's rows_in == child's rows_out)
3. At the first operator where `rows_out` is wrong relative to expectation, that operator contains the bug
4. Check its schema, predicate expressions, and NULL handling

## Step 4: Timing Interpretation

**Self-time (ms):** Time this operator spent computing, not including children. If an operator's self-time is unexpectedly high:
- `FilterNode` high self-time: evaluating a complex predicate per row — check for unnecessary allocations in expression evaluation
- `HashAggregateNode` high self-time: hash map operations — expected for large GROUP BY, concerning for small ones
- `ProjectNode` high self-time: expression evaluation or string allocation per row
- `SeqScanNode` high self-time in columnar mode: zone-map not pruning expected chunks — check predicate eligibility

**% of total:** Use this to identify the optimization target. The operator consuming the largest % is the bottleneck.

## Step 5: Using `--explain` (No Execution)

`--explain` shows the plan tree without running the query. Use it to:
- Verify predicate appears at the right node (WHERE predicate should be at `FilterNode` below `SeqScanNode`, not above `HashAggregateNode`)
- Verify GROUP BY columns appear in `HashAggregateNode` output schema
- Verify join sides: which table is build, which is probe
- Confirm column names in the plan match what the query references

If `--explain` shows a wrong plan structure, the bug is in the **planner** (not the executor). Fix planner code first; don't trace into execution.

## Phase-Specific Patterns

### Phase 2: Columnar Storage

```
[SeqScanNode (columnar)]  rows_in=12000  rows_out=12000  ...
```
If a WHERE predicate is selective and the column has good zone-map coverage, `rows_in` on `SeqScanNode` should be **less than** total table rows (pruned chunks are never scanned).

- `rows_in == total_table_rows` with a selective predicate → zone-map pruning not firing
  - Check: is the predicate column indexed in the zone-map?
  - Check: is the predicate operator supported (`=`, `<`, `>`, `<=`, `>=`)? (`!=` cannot prune)
  - Check: is the constant within the column's actual min/max range?

### Phase 3: Vectorized Execution

```
[VecProjectNode]   rows_in=1000  rows_out=1000  ...
  [VecFilterNode]  rows_in=1000  rows_out=342   ...
    [VecSeqScanNode] rows_in=1000 rows_out=1000 ...
```
`rows_in` and `rows_out` are **row counts**, not chunk counts. A chunk of 1024 with 342 surviving rows reports `rows_out=342`.

- `VecFilterNode rows_out > rows_in`: SelectionVector length exceeded chunk size — **invariant violation**
- `VecProjectNode rows_out != VecFilterNode rows_out`: materialization lost or duplicated rows — **bug**

### Phase 4: Optimizer

Compare `--explain` pre- and post-optimizer:

```diff
- [HashJoinNode] build=laps probe=drivers
+ [HashJoinNode] build=drivers probe=laps    ← optimizer swapped sides (drivers is smaller)
```

After optimizer, run `--explain-analyze` and check:
- Estimated rows (from optimizer's cost model) vs. actual `rows_out`
- If actual >> estimated: filter is less selective than the optimizer predicted → stats are stale or formula is wrong
- If the optimizer-chosen plan is **slower** than the baseline: the cost model preferred a worse physical plan → examine which node's cost estimate was wrong

## Quick Reference: Bug Localization by Symptom

| Symptom | Likely cause | Where to look |
|---|---|---|
| Empty result, query should return rows | Schema mismatch or predicate on wrong column | Check `rows_out` at each node; first zero is the culprit |
| Correct row count, wrong values | Expression evaluation bug | `ProjectNode` or `FilterNode` expression evaluator |
| Too many rows (duplicates) | Missing `DistinctNode`, or `HashJoinNode` cross-product | Check plan structure with `--explain` |
| Too few rows | Over-selective filter, or early termination | `FilterNode rows_out` vs expected; check NULL semantics |
| Slow `SeqScanNode` | Zone-map not pruning | Check predicate eligibility for pruning |
| Slow `HashAggregateNode` | Too many groups, or hash collision | Expected for high-cardinality GROUP BY |
| Self-time >> child time | Per-row allocation in operator | Check for `new`/`malloc` in hot path |
