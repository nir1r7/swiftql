---
name: operator-correctness
description: Verify a SwiftQL operator implementation against correct relational-algebra and SQL semantics — per-operator checklists for projection, filter, join, aggregation, distinct, sort, limit; NULL edge-case tables; Volcano iterator lifecycle; and hand-simulation on concrete rows. Use when implementing or reviewing any operator (Volcano or vectorized), or when a result is semantically suspicious.
---

# Operator Correctness Verification

Database bugs are usually semantic, not syntactic — the code compiles and returns the right *number* of columns but wrong answers on specific data patterns. Work the applicable checklists, mark each item ✅/❌/⚠️ with reasoning, and cite `file:line` for every ❌/⚠️.

## Per-operator semantic checklists

### Projection (`ProjectNode` / `VecProjectNode`)
- Output schema exactly matches the SELECT list — order, no extras, no omissions
- Columns resolve against the correct INPUT schema via `relation_slot` (see `invariants` skill), not the table schema
- Vectorized: values gathered at `sel[i]`, never at `i` — the classic silent-corruption bug
- Input rows are read-only

### Filter (`FilterNode` / `VecFilterNode`)
- NULL semantics: `NULL = x`, `NULL != x`, `NULL > x` all evaluate to NULL → row dropped (falsy)
- `col IS NULL OR col > 5` with NULL col → row KEPT (OR short-circuit on the TRUE branch)
- Failing rows are never emitted (not passed through as NULL rows)

### Join (`HashJoinNode` / `VecHashJoinNode`)
- Cardinality: one output row per (probe_row, matching_build_row) pair — 3 dup keys × 2 dup keys = 6 rows; verify ALL build matches emitted, not just the first
- NULL join keys never match (`NULL != NULL` in join comparison)
- Inner join: unmatched rows on either side dropped, never NULL-padded
- **Output schema is ALWAYS `[FROM cols] ++ [JOIN cols]` regardless of build/probe swap** — internal `swapped_` flag reorders values; column order must never follow the physical build side
- Hash map cleared between queries; no cross-query state leak

### Aggregation (`HashAggregateNode` / `VecHashAggregateNode`)

| Case | Correct | Drift risk |
|---|---|---|
| `COUNT(*)`, rows with NULLs | counts all rows | returns fewer |
| `COUNT(col)`, all NULL | 0 | returns row count |
| `SUM/AVG/MIN/MAX(col)`, all NULL | NULL | 0, NaN, or crash |
| Scalar aggregate over empty input | one row (`COUNT`=0, others NULL) | zero rows |
| `GROUP BY` over empty input | zero rows | phantom empty group |
| NULL in GROUP BY key | NULLs form one group (NULL==NULL for grouping only) | dropped or merged wrong |
| `AVG` | running sum / count | accumulating error |

- Group key equality by value, not pointer; composite keys hash ALL key columns
- Aggregate argument resolves by `relation_slot` (e.g. `AVG(l2.speed)` in self-join)
- Pipeline breaker: all input consumed before first emit

### Having / Distinct / Sort / Limit
- `HAVING`: applied against the AGGREGATED schema; requires GROUP BY (validator)
- `DISTINCT`: duplicates iff ALL columns equal; NULL == NULL for dedup; pipeline breaker
- `SORT`: ascending default; document NULL position; ORDER BY column accessible even if not in SELECT
- `LIMIT`: exactly N (N=0 → zero rows); stops pulling child — early termination must propagate to the scan (vectorized: `VecLimitNode` truncates the final chunk)

## Volcano lifecycle (condensed)

- `open()` resets ALL state (idempotent on re-open); calls `child->open()`
- `next()` never returns a row twice; returns `nullptr` on exhaustion, and again on every later call
- `FilterNode` inner loop terminates when child returns `nullptr` — never calls `child->next()` after that
- Pipeline breakers build exactly once (first `next()`), emit accumulated rows fully before `nullptr`
- `close()` propagates to all children, safe mid-stream (Limit early termination), idempotent
- `close()`+`open()` re-execution equals first execution (hash maps rebuilt, counters zeroed)

## Verification technique: hand simulation

For a suspicious operator, simulate on 5–15 concrete rows. Volcano — trace each `next()`:

```
--- Call 2: Project.next() ---
  Filter.next() → SeqScan.next() → Row{team=McLaren, speed=295}, cursor→2
  Filter: 295 > 300 → FALSE → drops, re-calls SeqScan.next()
  → Row{team=Mercedes, speed=310} → TRUE → emits
  Project emits Row{team=Mercedes}
```

Vectorized — trace at chunk granularity: scan emits `DataChunk` (≤1024 rows), filter emits `SelectionVector{indices=[0,2], size=2}` with NO data copied, project gathers at `sel[i]`. For pipeline breakers, simulate BUILD phase (all input) and EMIT phase separately. For NULLs, write the three-valued result explicitly (`NULL > 300 → NULL → falsy → drop`).

## Output format

Per operator: completed checklist, each ❌/⚠️ with `file:line` + one-sentence bug + minimal fix, then verdict **CORRECT / INCORRECT / NEEDS INVESTIGATION**. Severity order: silent data loss > wrong values > wrong row count > wrong ordering. Confirm findings against SQLite before fixing (`bisect-stage` skill).
