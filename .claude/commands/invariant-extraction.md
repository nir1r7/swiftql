---
description: Identify and document the invariants that must hold before modifying SwiftQL code — schema invariants, row count monotonicity, iterator ownership, tuple immutability, and expression purity.
---

# Invariant Extraction

Before modifying any SwiftQL code, identify and document the invariants that govern the code you are about to touch. Violating a database engine invariant often produces silent wrong answers, not crashes.

## Protocol

Given a file or function you are about to modify, extract invariants in each category below. For each invariant, state:
- The invariant itself (one sentence)
- Where it is enforced (file:line or "by convention")
- What breaks if it is violated

---

## Category 1 — Schema Invariants

Schema invariants govern the column structure that flows between operators.

**Invariants to check:**
- Schema width (column count) is constant across all rows emitted by one operator
- Schema produced by `output_schema()` matches the schema of rows actually returned by `next()`
- Column names in the schema are unique within a single operator's output
- After `ProjectNode`, the schema contains exactly the projected columns — no extras
- After `HashAggregateNode`, the schema is `[group_by_columns..., aggregate_result_columns...]`
- After `HashJoinNode`, the schema is `[left_table_columns..., right_table_columns...]` — no column loss or duplication

**For the code you're modifying:** list each schema-affecting operation and verify it preserves the invariant.

---

## Category 2 — Row Count Monotonicity

Row counts can only decrease or stay equal as rows flow upward through the plan tree. No operator can produce more rows than its child, except `HashJoinNode` (fan-out) and `HashAggregateNode` (fan-in).

**Invariants to check:**
- `FilterNode`: `rows_out ≤ rows_in`
- `ProjectNode`: `rows_out == rows_in`
- `HashAggregateNode`: `rows_out == distinct_group_count ≤ rows_in`
- `HavingNode`: `rows_out ≤ rows_in`
- `DistinctNode`: `rows_out ≤ rows_in`
- `SortNode`: `rows_out == rows_in`
- `LimitNode`: `rows_out == min(N, rows_in)`
- `HashJoinNode`: `rows_out == sum over probe rows of (matching build rows count)`

**For the code you're modifying:** identify whether the change can affect row counts and verify the invariant holds.

---

## Category 3 — Iterator Ownership and Lifecycle

In the Volcano model, each operator owns its child iterator. The lifecycle is: `open()` → `next()*` → `close()`. These calls must be symmetric.

**Invariants to check:**
- `open()` is called exactly once before the first `next()`
- `close()` is called exactly once, after the last `next()` (including early termination)
- A child's `close()` is called by its parent's `close()` — not by the child itself
- `next()` returning `nullptr` is idempotent — subsequent calls also return `nullptr`
- `open()` resets all state — a re-opened operator behaves as if freshly constructed
- No operator holds a reference to a row returned by a previous `next()` call after calling `next()` again (row ownership transfers upward)

**For the code you're modifying:** trace the `open`/`next`/`close` call sequence and verify symmetry.

---

## Category 4 — Tuple Immutability

Rows flowing through the pipeline are read-only. An operator must not modify a row it received from its child.

**Invariants to check:**
- `FilterNode` passes through the child row unchanged — it does not copy or modify it
- `ProjectNode` constructs a new row from the child row's values — it does not mutate the child row
- Expression evaluation (`evaluate(expr, row, schema)`) is read-only on the row
- Sorting compares rows without modifying them

**For the code you're modifying:** if you are changing how rows are passed between operators, verify this invariant.

---

## Category 5 — Expression Evaluation Purity

`evaluate(expr, row, schema)` must be a pure function: same inputs always produce the same output, no side effects.

**Invariants to check:**
- No global state read or written during expression evaluation
- Aggregate functions (`COUNT`, `SUM`, etc.) are not called from `evaluate()` directly — they are handled by `HashAggregateNode`
- `IS NULL` / `IS NOT NULL` expressions never throw — null `Value` objects are valid inputs
- Division by zero in expressions: document whether it throws or returns NULL

**For the code you're modifying:** if you are changing the evaluator, verify these hold.

---

## Category 6 — NULL Propagation

NULL has specific semantics throughout SwiftQL (scoped to `IS NULL` / `IS NOT NULL` per the project scope).

**Invariants to check:**
- A null `Value` passed to a comparison expression (`=`, `<`, `>`, etc.) evaluates to NULL (not true or false)
- `IS NULL` correctly identifies null `Value` objects
- Aggregates: `SUM`, `AVG`, `MIN`, `MAX` skip NULLs; `COUNT(col)` skips NULLs; `COUNT(*)` counts all rows
- NULL values in GROUP BY keys: NULLs form their own group (NULL == NULL for grouping)
- NULL display in output: rendered as `NULL` string

---

## Output Format

Before writing any code, produce a table:

```
Invariant                          Holds Before?   Holds After?   Risk if Violated
─────────────────────────────────────────────────────────────────────────────────
Schema width constant at Filter    ✅               ✅              Silent wrong schema downstream
Row count monotone at Filter       ✅               ✅              Overcounting
open/close symmetry in Sort        ✅               ✅              Resource leak
Tuple immutability in Project      ✅               ✅              Corrupted upstream rows
Expression purity in evaluate()    ✅               ✅              Non-deterministic results
NULL propagation in BinaryExpr     ✅               ⚠️  (check)     Wrong filter results on NULLs
```

If any invariant is ⚠️ or ❌ after your change, either redesign the change or explicitly document the intentional deviation and why it is safe.
