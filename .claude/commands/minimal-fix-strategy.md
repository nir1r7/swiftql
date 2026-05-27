---
description: Identify the smallest causal change that fixes a SwiftQL bug, without touching the planner, parser, or surrounding code unnecessarily. Preserves invariants and avoids engine rewrites.
---

# Minimal Fix Strategy

You are finding the smallest possible fix for a SwiftQL bug. Database engines are fragile under over-refactoring — a fix that touches the planner when the bug is in the executor, or rewrites an operator when one condition is wrong, introduces new risk without solving the original problem.

## Protocol

### Step 1 — Reproduce the Bug

Before touching any code:
1. State the exact query that triggers the bug
2. State the expected output (from SQL semantics or SQLite reference)
3. State the actual output (wrong rows, crash, error message)
4. Confirm the bug is reproducible in the current build

If you cannot reproduce it, stop here and flag the uncertainty.

### Step 2 — Locate the Causal Stage

Narrow the bug to the smallest pipeline stage by elimination. The SwiftQL pipeline is:

```
Lexer → Parser → Validator → Planner → Optimizer → Execution
```

Ask: *At which stage does the data first become wrong?*

Strategies:
- Add `--explain` and check the plan tree — if the plan is wrong, the bug is in Planner or Optimizer
- If the plan is correct but output is wrong, the bug is in Execution
- If the plan is correct and a simpler version of the query works, the bug may be in operator interaction
- If the query is rejected incorrectly, the bug is in Validator

Do not proceed to a fix until you have identified the exact stage.

### Step 3 — Identify the Minimal Causal Change

Within the stage you identified, find the single smallest change that fixes the bug. Apply these filters:

**Do not touch:**
- Any file not in the causal stage
- Any method not on the call path for the failing query
- Any logic not related to the failing case (e't fix style issues, rename variables, or refactor unrelated code)

**Prefer:**
- A one-line condition fix over a method rewrite
- A missing null check over a new abstraction
- A corrected schema reference over a restructured plan node
- A predicate fix over a new operator

### Step 4 — State Invariants You Must Preserve

Before writing the fix, list the invariants that must hold after the change:

Examples:
- Schema width (column count) is constant across all rows emitted by an operator
- `next()` returning `nullptr` means the operator is exhausted — subsequent calls must also return `nullptr`
- `open()` is always called before `next()`, `close()` is always called after
- Row count from `SeqScan` equals row count in the CSV file
- Aggregation result count equals the number of distinct group keys
- `HashJoinNode` output schema = left_schema + right_schema (no column duplication or loss)
- `FilterNode` never modifies the row it passes through — only drops rows

State which invariants your fix might affect, and confirm they are preserved.

### Step 5 — Write the Fix

Produce only the changed lines with file path and line numbers. No surrounding refactor. No cleanup of unrelated code.

Format:
```
File: src/execution/plan_nodes.cc  Line: 142
Before:
  count++;  // counts all rows

After:
  if (!val.is_null()) count++;  // COUNT(col) skips NULLs
```

### Step 6 — Verify

State exactly how to verify the fix:
1. The query that was broken now produces the correct output
2. No existing test in `tests/test_execution.cc` regresses
3. The fix passes `compare_against_sqlite.py` for any affected query patterns

If you cannot verify programmatically, write a minimal test case.

## Anti-Patterns to Avoid

- Do not refactor the operator to "make it cleaner" while fixing the bug — submit two separate changes
- Do not add fallback behavior that masks the bug (e.g. returning empty rows instead of crashing) — fix the root cause
- Do not add unnecessary null checks throughout the file — add only the check that fixes this specific case
- Do not change the planner to work around an executor bug — fix the executor
- Do not change the schema representation to work around a schema mismatch — fix the mismatch at its source
