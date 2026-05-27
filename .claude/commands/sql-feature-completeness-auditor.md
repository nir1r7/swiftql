---
description: Audit whether a SQL feature is fully implemented end-to-end across parser, AST, planner, executor, schema handling, and tests in SwiftQL.
---

# SQL Feature Completeness Auditor

You are auditing whether a named SQL feature is fully implemented end-to-end in SwiftQL. "Fully implemented" means the feature works correctly through every layer of the pipeline and has test coverage.

## Pipeline Layers to Check

For the feature under audit, verify each layer in order:

### 1. Lexer (`src/parser/`)
- [ ] All required tokens exist in `TokenType` enum
- [ ] Lexer correctly tokenizes the feature's keywords
- [ ] No token conflicts with existing keywords

### 2. Parser (`src/parser/parser.cc`)
- [ ] Grammar rule exists and is connected to the right parse method
- [ ] Parser produces the correct AST node type(s) on a valid input
- [ ] Parser raises `ParseError` with a useful message on invalid inputs
- [ ] Operator precedence is correct if the feature involves expressions
- [ ] Edge cases handled: empty lists, trailing commas, missing clauses

### 3. AST (`src/parser/ast.h`)
- [ ] AST node(s) exist with the correct fields
- [ ] Fields are the right types (not stringly-typed where structured types exist)
- [ ] Node correctly distinguishes all variants (e.g. `COUNT(*)` vs `COUNT(col)`)

### 4. Validator (`src/planner/validator.cc`)
- [ ] Feature's semantic constraints are enforced (column existence, type compatibility, clause ordering)
- [ ] Invalid uses produce clean error messages, not crashes
- [ ] Cross-clause constraints checked (e.g. non-aggregate SELECT columns must appear in GROUP BY)

### 5. Planner (`src/planner/planner.cc`)
- [ ] Planner produces the correct `PlanNode` type(s) for this feature
- [ ] Plan node tree structure is correct (right parent/child relationships)
- [ ] Output schema of the plan node is correct
- [ ] Feature interacts correctly with other plan nodes (e.g. filter pushed below aggregate)

### 6. Plan Node (`src/planner/plan_nodes.h` / `.cc`)
- [ ] Plan node struct/class exists with the right fields
- [ ] `explain()` / `toString()` output is correct
- [ ] For `EXPLAIN ANALYZE`: node tracks `rows_in`, `rows_out`, `time_ms`

### 7. Execution — Volcano (`src/execution/` or embedded in plan nodes)
- [ ] Operator `open()` initializes state correctly
- [ ] Operator `next()` produces correct rows
- [ ] Operator `close()` releases resources and calls child `close()`
- [ ] NULL handling correct
- [ ] Edge case: empty input produces correct output (empty or a single group for aggregates)

### 8. Execution — Vectorized (Phase 3+)
- [ ] `VecXxxNode` exists and is wired into the `--execution vectorized` path
- [ ] Processes `DataChunk` / `SelectionVector` correctly
- [ ] Late materialization respected where applicable

### 9. Optimizer (Phase 4+)
- [ ] Optimizer pass handles this feature's plan nodes correctly
- [ ] Optimizer does not incorrectly reorder or remove plan nodes related to this feature
- [ ] Statistics used correctly if feature interacts with predicate pushdown or join ordering

### 10. Tests
- [ ] Unit test for the plan node or operator in isolation
- [ ] Integration test in `tests/test_execution.cc` with at least one query using this feature
- [ ] Edge cases tested: empty result, NULL input, boundary values
- [ ] Correctness verified against SQLite via `compare_against_sqlite.py`

## Output Format

```
Feature: GROUP BY / HashAggregateNode

Layer                Status   Notes
─────────────────────────────────────────────────────
Lexer                ✅        GROUP, BY tokens present
Parser               ✅        parseGroupBy() wired correctly
AST                  ✅        SelectStatement.group_by: vector<string>
Validator            ⚠️        Missing check: aggregate in GROUP BY list
Planner              ✅        HashAggregateNode produced correctly
Plan Node            ✅        explain() output correct
Volcano Execution    ❌        COUNT(*) counts NULLs — see plan_nodes.cc:142
Vectorized Execution ✅        VecHashAggregateNode implemented
Optimizer            ✅        Not affected by predicate pushdown
Tests                ⚠️        No test for GROUP BY on empty table

Verdict: INCOMPLETE — 1 bug, 2 gaps
```

Always end with a prioritized fix list: bugs first (wrong answers), then missing coverage, then hardening.
