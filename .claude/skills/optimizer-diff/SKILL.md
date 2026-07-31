---
name: optimizer-diff
description: Compare a SwiftQL query optimized vs --no-optimize — result diff (correctness), plan-shape diff (--explain), and estimated-vs-actual row analysis (--explain-analyze). Use when developing or debugging optimizer passes (pushdown, cost model, join selection, cardinality estimation) or evaluating whether the optimizer helped.
---

# Optimizer Diff

The optimizer runs ONLY on the columnar+vectorized path and must be **result-preserving**: optimized output ≡ `--no-optimize` output ≡ SQLite. Any divergence is an optimizer bug by definition.

## Protocol (for a given query)

All commands from repo root, always with `--no-cache`:

```bash
VEC="./build/swiftql --catalog catalog.json --no-cache --storage columnar --execution vectorized"

# 1. Result diff — must be identical
$VEC --query "<sql>"
$VEC --query "<sql>" --no-optimize

# 2. Plan shape diff
$VEC --query "<sql>" --explain
$VEC --query "<sql>" --explain --no-optimize

# 3. Timing + estimate accuracy
$VEC --query "<sql>" --explain-analyze
$VEC --query "<sql>" --explain-analyze --no-optimize
```

## What to check

1. **Correctness** — normalized results identical. If not: bug in the pass that changed the plan; bisect by disabling decisions mentally against the plan diff.
2. **Plan shape** — for each difference, name the responsible pass:
   - Filters moved below join → `predicate_pushdown.cc` (conjunction split, per-relation classification, cross-relation residuals stay above)
   - Build/probe side flipped → `cost_model.cc` physical join selection (filtered cardinality, not raw row count)
   - Scan-local predicate ordering → predicate work-ordering + selection-vector cascade
   - Regardless of build/probe swap, output schema must stay fixed FROM++JOIN order (`swapped_` flag) — verify column order didn't drift.
3. **Estimate accuracy** — per node, compare estimated rows (logical plan annotation) vs actual `rows_in`/`rows_out`. Flag misestimates >10× and trace them to `cardinality_estimator.cc` (which selectivity rule or fallback fired?). Known gap: group-count NDV on self-joins uses bare-name lookup (estimate-only, not a correctness bug).
4. **Did it help?** — compare execution times (average the query over ~5 runs; ignore parse/plan noise). An optimization that preserves results but slows the query is still a finding — report it.

## Reference: expected estimation formulas

When tracing a misestimate to `cardinality_estimator.cc`, check the fired rule against these:

| Predicate | Selectivity |
|---|---|
| `col = C` | `1 / ndv[col]` |
| `col != C` | `1 - 1/ndv[col]` |
| `col > C` / `col < C` | `(max−C)/(max−min)` / `(C−min)/(max−min)`, clamped to [0,1] |
| `col IS NULL` / `IS NOT NULL` | `null_fraction` / `1 − null_fraction` |
| `A AND B` | `sel(A) × sel(B)` (independence assumption) |
| `A OR B` | `1 − (1−sel(A))(1−sel(B))` |
| Join `R.a = S.b` | `|R| × |S| / max(ndv(R.a), ndv(S.b))` |

Statistics invariants: `ndv <= row_count`; `min <= max` (typed comparison); stats computed over the exact data loaded, never stale. Common formula bugs: `min(ndv)` instead of `max(ndv)` in the join rule (overestimates), unclamped range selectivity (negative on out-of-range constants), multiplying correlated conjunct selectivities (systematic underestimate — expected with this model; document, don't "fix" per-query).

Known gap: group-count NDV on self-joins uses bare-name lookup (estimate-only, not correctness).

## Report format

| Aspect | Optimized | No-optimize | Verdict |
|---|---|---|---|
| Results | — | — | identical / DIVERGED |
| Plan shape | (tree) | (tree) | which passes fired |
| Worst estimate error | node, est vs actual | — | cause |
| Execution time | Xms | Yms | speedup/regression |

Divergent results are always a bug — never rationalize them. Add the query to the `WEEK*_QUERIES` blocks in `python_tools/test_new_queries.py` when it exposes a new shape.
