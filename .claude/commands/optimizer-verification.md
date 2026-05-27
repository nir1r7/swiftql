---
description: Verify SwiftQL's cost-based optimizer — statistics schema correctness, cardinality estimation formulas, plan transformation safety (predicate pushdown, join side selection, join ordering), and regression detection using EXPLAIN ANALYZE actuals vs. estimates.
---

# Optimizer Verification

You are verifying SwiftQL's cost-based optimizer (Phase 4). The optimizer introduces a new class of bugs: plans that are semantically correct but produce wrong performance predictions, or (more dangerously) plan transformations that accidentally change query semantics. The two failure modes are distinct and require different verification approaches.

## Failure Mode A: Wrong Performance (bad cost model)
The plan is semantically correct but the optimizer chose a worse plan than rule-based would have. Fix: improve statistics or cost formulas.

## Failure Mode B: Wrong Semantics (bad transformation)
The optimizer reordered or rewrote the plan in a way that changes which rows are produced. Fix: fix the transformation rule.

**Always check Failure Mode B first.** A query that runs fast but returns wrong results is strictly worse than one that runs slow but returns correct results.

---

## 1. Statistics Schema

What to collect per table, per column:
- `row_count` — total rows in the table
- `ndv[col]` — number of distinct values (not counting NULL)
- `min[col]`, `max[col]` — minimum and maximum non-NULL values
- `null_fraction[col]` — fraction of rows where the column is NULL (0.0 to 1.0)

**Invariants:**
- `ndv[col] <= row_count` always
- `ndv[col] >= 1` if any non-NULL values exist
- `min[col] <= max[col]` (same type)
- `null_fraction[col] + non_null_fraction == 1.0` (no rounding error gaps)
- Stats are computed over the same data the engine will query — not stale from an older load

**Verification:** After loading a table, compute stats independently (scan CSV) and compare to the engine's reported stats.

---

## 2. Cardinality Estimation Formulas

### Selectivity for Filter predicates

| Predicate form | Selectivity formula |
|---|---|
| `col = C` | `1 / ndv[col]` |
| `col != C` | `1 - 1/ndv[col]` |
| `col > C` | `(max - C) / (max - min)` (clamped to [0,1]) |
| `col >= C` | `(max - C + 1) / (max - min + 1)` |
| `col < C` | `(C - min) / (max - min)` (clamped to [0,1]) |
| `col IS NULL` | `null_fraction[col]` |
| `col IS NOT NULL` | `1 - null_fraction[col]` |
| `A AND B` | `selectivity(A) * selectivity(B)` (assumes independence) |
| `A OR B` | `1 - (1 - sel(A)) * (1 - sel(B))` |

Estimated output rows of a FilterNode = `input_rows * selectivity`.

### Join cardinality

For `R JOIN S ON R.a = S.b`:
- Estimated output rows = `row_count(R) * row_count(S) / max(ndv(R.a), ndv(S.b))`
- Rationale: each R row matches `row_count(S) / ndv(S.b)` S rows on average

**Verification protocol:**
1. Run queries with known data (small dataset where you can count manually)
2. Compare `estimated_rows` from optimizer against `actual rows_out` from EXPLAIN ANALYZE
3. Acceptable error: within 2× for most queries; >10× divergence signals a formula bug or stale stats

**Common bugs:**
- Using `min(ndv)` instead of `max(ndv)` in join formula — overestimates join output
- Not clamping range selectivity to [0, 1] — negative selectivity on out-of-range constants
- Multiplying selectivities when predicates are correlated (e.g., `city = 'LA' AND state = 'CA'`) — overestimates selectivity

---

## 3. Plan Transformation Safety

For each optimizer rule, verify two properties: **(a) semantic equivalence** and **(b) schema preservation**.

### Predicate Pushdown

**Rule:** Move a FilterNode below another node (e.g., push a WHERE predicate below a Join).

**Semantic equivalence check:**
- The predicate must reference only columns available **at the new position** in the tree
- A predicate referencing an aggregate result (e.g., `SUM(speed) > 100`) cannot be pushed below `HashAggregateNode` — it must stay as `HavingNode`
- A predicate on a join column can be pushed to the build/probe side only if it references that side's columns exclusively

**Schema preservation check:**
- After pushdown, the parent node still receives the same schema it expected
- The pushed FilterNode's output schema is identical to the original FilterNode's output schema (FilterNode does not change schema)

**Verification:** Run `--explain` before and after optimizer. Compare the root node's output schema — must be identical. Run the query and compare results to SQLite.

### Join Side Selection

**Rule:** Swap build/probe sides so the smaller table (by estimated row count) is the build side.

**Semantic equivalence check:**
- Inner join is commutative: `R JOIN S ON R.a = S.b` == `S JOIN R ON S.b = R.a`
- Output schema order changes: original is `[R columns, S columns]`; after swap it's `[S columns, R columns]`
- **Critical:** If the parent node references columns by position (not name), a schema order change breaks it. Verify parent references columns by name.

**Verification:** Run `--explain` and confirm build side is the table with fewer estimated rows. Run the query and compare to SQLite.

### Join Ordering (multi-join)

**Rule:** Reorder a sequence of joins to minimize intermediate result sizes.

**Verification:**
1. Run all join orderings on small data and confirm identical results
2. Confirm the chosen ordering has the lowest estimated intermediate row count
3. After reordering, confirm every predicate still references columns that exist at its new position in the tree

---

## 4. Optimizer/Executor Agreement

After the optimizer runs, the plan tree is handed to the executor. Verify:

- Every field the executor reads (join key column names, aggregate expressions, predicate expressions) is still valid after optimizer rewrites
- Column names referenced in expressions still resolve correctly against the output schema at each node
- Use `--explain` to print the post-optimizer plan and manually verify column name resolution for each node

---

## 5. Regression Detection

After the optimizer is enabled, run the full query suite with `--explain-analyze` and compare:

| Query | Pre-optimizer rows_out | Post-optimizer rows_out | Match? |
|---|---|---|---|
| Q1 | N | M | ✅/❌ |

If `rows_out` differs: the optimizer changed query semantics — this is a **correctness bug**.

Also compare wall-clock time:

| Query | Baseline time | Optimizer time | Improvement |
|---|---|---|---|
| Q1 | T₀ | T₁ | (T₀-T₁)/T₀ |

If optimizer time > baseline time: the cost model chose a worse plan — this is a **performance bug**. Identify which transformation caused the regression by running with individual rules disabled.

**SQLite comparison:** After each optimizer-enabled query, compare results to SQLite reference. Any difference in rows (regardless of order) is a correctness bug.

---

## Output Format

Report in two sections:

### Section 1: Transformation Safety
For each transformation rule applied to each query:
- **SAFE** — semantic equivalence verified, results match SQLite
- **UNSAFE** — results differ from SQLite; describe which transformation caused the divergence and at which node

### Section 2: Cost Model Accuracy
For each query:
- Estimated rows at each node vs. actual rows_out from EXPLAIN ANALYZE
- Error ratio (actual / estimated)
- Whether the optimizer chose the plan with lowest actual cost (in retrospect)

Flag any estimate with error ratio > 10× as a priority fix.
