---
name: add-sql-feature
description: End-to-end checklist for extending SwiftQL's SQL surface (new keyword, expression, predicate, clause, or operator). Walks the feature through lexer → parser → binder → validator → logical plan → optimizer → physical plan → both executors → tests → SQLite harness. Use whenever adding or extending SQL syntax or semantics (e.g. BETWEEN, LIKE, IN, CASE, arithmetic, multi-way joins, subqueries).
---

# Add SQL Feature

You are adding a SQL feature to SwiftQL. Features flow through a fixed pipeline; skipping a stage produces bind/plan/execution mismatches that surface as confusing downstream bugs. Work the stages in order, test-first at each stage.

## Before starting

1. State the target syntax and its SQL semantics (cite SQLite behaviour — it is the correctness oracle).
2. Check the README grammar section and `development.md` — does this feature interact with existing precedence (OR → AND → compare → primary)?
3. Read the relevant invariants first (see the `invariants` skill): relation_slot resolution, fixed FROM++JOIN schema order, Volcano-as-baseline.
4. Decide engine coverage: features must work on **both** Volcano and vectorized paths unless explicitly scoped otherwise. Volcano is the correctness baseline.

## Stage checklist

Work top to bottom. At each stage: write the failing test FIRST, see it fail, then implement.

| # | Stage | Files | Test file |
|---|---|---|---|
| 1 | Token | `src/parser/token.h`, `src/parser/lexer.{h,cc}` | `tests/test_parser.cc` |
| 2 | AST node | `src/parser/ast.h` (+ `expr_utils.h` if expression) | — |
| 3 | Parser rule | `src/parser/parser.cc` — one method per grammar rule; respect precedence chain | `tests/test_parser.cc` |
| 4 | Binder | `src/planner/binder.cc` — resolve columns to `relation_slot` (0=FROM, 1=JOIN) | `tests/test_binder.cc` |
| 5 | Validator | `src/planner/validator.cc` — semantic checks, clean error messages | `tests/test_planner.cc` |
| 6 | Logical plan | `src/planner/logical_plan.{h,cc}` | `tests/test_logical_plan.cc` |
| 7 | Cardinality | `src/planner/cardinality_estimator.cc` — add estimate or documented fallback selectivity | `tests/test_cardinality.cc` |
| 8 | Optimizer passes | `src/planner/predicate_pushdown.cc` (does the new predicate split/push correctly?), `src/planner/cost_model.cc` | `tests/test_predicate_pushdown.cc`, `tests/test_cost_model.cc` |
| 9 | Vectorized physical plan | `src/planner/vectorized_plan_builder.cc` → `vec_plan_node.h` | `tests/test_vec_plan_builder.cc` |
| 10 | Volcano execution | `src/execution/evaluator.cc`, `src/planner/plan_nodes.cc`, `src/planner/planner.cc` | `tests/test_execution.cc` |
| 11 | Vectorized execution | `src/execution/columnar_eval.cc` (fast path) + relevant `vec_*_node.cc` | `tests/test_vectorized.cc` |
| 12 | New test files | register in `tests/CMakeLists.txt` if adding a file | — |

Not every feature touches every stage — but explicitly state which stages are skipped and why (e.g. "pure parser sugar, no new plan node").

## Harness integration (mandatory)

- Add representative queries to `python_tools/test_new_queries.py` (regression surface) and, if it's a benchmark-class shape, `python_tools/compare_against_sqlite.py`.
- Queries must pass in all three modes: default (row+volcano), `--storage columnar --execution volcano`, `--storage columnar --execution vectorized`, and the optimizer invariant (optimized == `--no-optimize`).
- If the feature's semantics involve NULLs: CSV cannot express NULLs (empty numeric fields throw in the loader) — cover null behaviour with in-memory operator-level unit tests instead.

## Done criteria

Run the `verify` skill. Feature is done when: all unit tests pass, both harnesses pass in all modes, error messages for invalid usage are clean (no crashes), and README Feature Scope / grammar section is updated.
