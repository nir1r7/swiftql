---
name: bisect-stage
description: Localize a SwiftQL bug to its causal pipeline stage by elimination — mode-matrix bisection (row/columnar × volcano/vectorized × optimize/no-optimize) followed by plan inspection — then apply the smallest fix at that stage. Use whenever a query returns wrong results, crashes, or errors unexpectedly.
---

# Bisect Stage

Database engines are fragile under over-refactoring. A fix that touches the planner when the bug is in an executor introduces risk without solving the problem. Localize first; fix minimally.

## Step 1 — Reproduce

Before touching code, establish:
1. The exact failing query.
2. Expected output — from SQLite (`sqlite3` over `data/*.csv`, or the harness) — SQLite is the oracle.
3. Actual output (wrong rows / crash / error).
4. Always pass `--no-cache` while reproducing — rule out the result cache immediately.

```bash
./build/swiftql --catalog catalog.json --no-cache --query "<sql>"
```

If not reproducible, stop and say so.

## Step 2 — Mode-matrix bisection

Run the query in each mode and record pass/fail:

```bash
BASE="./build/swiftql --catalog catalog.json --no-cache --query"
$BASE "<sql>"                                                        # row + volcano
$BASE "<sql>" --storage columnar --execution volcano                 # columnar + volcano
$BASE "<sql>" --storage columnar --execution vectorized              # columnar + vectorized (optimized)
$BASE "<sql>" --storage columnar --execution vectorized --no-optimize
```

Inference table:

| Failure pattern | Causal layer |
|---|---|
| Fails in ALL modes | Lexer / Parser / Binder / Validator / common Value+Schema (`src/parser/`, `src/planner/binder.cc`, `src/common/`) |
| Fails only with `--storage columnar` (both executions) | Storage layer: `csv_to_columnar`, `dictionary_encoder`, `rle_column`, zone-map chunk pruning (`src/storage/`) |
| Fails only with `--execution vectorized` (opt AND no-opt) | Vectorized planning/execution: `vectorized_plan_builder.cc`, `vec_*_node.cc`, `columnar_eval.cc` |
| Fails only optimized (passes `--no-optimize`) | Optimizer pass: `predicate_pushdown.cc`, `cost_model.cc`, cardinality-driven decisions |
| Fails only on volcano | Rare — `plan_nodes.cc` / `evaluator.cc`; check before trusting the vectorized result, volcano is the baseline |

## Step 3 — Stage inspection within the layer

- `--explain` — is the plan SHAPE wrong (missing/extra/misplaced node, wrong join build side)? → planner/optimizer bug.
- `--explain-analyze` — walk nodes bottom-up; find the FIRST node whose `rows_out` diverges from expectation. The bug is in that node or its input contract. Structural anomaly check per operator:

| Operator | Must hold | Violation = |
|---|---|---|
| Scan | `rows_out == table rows` (or fewer under pruning) | pruning/loader bug |
| Filter / Having / Distinct | `rows_out <= rows_in` | bug, always |
| Project / Sort | `rows_out == rows_in` | bug, always |
| Aggregate | `rows_out == group count`; scalar agg over empty input → exactly 1 row | grouping/empty-input bug |
| Limit N | `rows_out <= N`; upstream `rows_in` shrinks (early termination) | limit/termination bug |
| Join | `rows_out` may exceed inputs (dup keys); `== 0` with known matches = key resolution bug | probe/key bug |

  `rows_out == 0` at a node that should produce rows → column resolution (relation_slot), over-pruning, or premature exhaustion.
- For binder-suspect bugs: does the query involve aliases, self-joins, or qualified columns? Check `relation_slot` handling (see `invariants` skill).

## Step 4 — Minimal fix

1. Write a failing regression test at the causal stage's test file FIRST; watch it fail.
2. Fix at the causal stage only. Do not refactor neighbors, do not "harden" other stages speculatively.
3. Add the query to `python_tools/test_new_queries.py` if it's a new query shape.
4. Run the `verify` skill — all gates, all modes.
5. State clearly which stage was at fault and why the fix is the smallest causal change.
