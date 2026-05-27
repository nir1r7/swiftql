---
description: Audit the open/close symmetry, cursor advancement, exhaustion behavior, state reset, child propagation, and resource cleanup of SwiftQL's Volcano-model iterators.
---

# Volcano Iterator Lifecycle Auditing

You are auditing the iterator lifecycle of SwiftQL's Volcano-model operators. Each operator implements:

```cpp
void open();    // initialize state
Row* next();    // return next row, or nullptr when exhausted
void close();   // release resources
```

Iterator lifecycle bugs are subtle and repetitive — they often pass unit tests but fail on integration with other operators, early termination, or re-execution.

## Checklist

For each operator under audit, work through every check below. Mark ✅ / ❌ / ⚠️ and cite file:line for any issue.

---

### 1. open() — Initialization Correctness

- [ ] All state variables reset in `open()` — no stale state from a previous execution
- [ ] Child's `open()` called in parent's `open()` (or deferred correctly, but consistently)
- [ ] Memory allocated in `open()` is freed in `close()` — no leak if `open()` is called without a matching `close()`
- [ ] `open()` is idempotent if called twice: second call resets correctly, no double-initialization
- [ ] For pipeline breakers (`HashAggregate`, `Sort`, `Distinct`): internal hash map / sort buffer initialized to empty in `open()`

---

### 2. next() — Cursor Advancement

- [ ] Cursor advances on every successful call — no row returned twice
- [ ] For `SeqScanNode`: row index increments by exactly 1 per `next()` call
- [ ] For `FilterNode`: inner loop re-calls child's `next()` until a passing row is found or nullptr — verify the inner loop terminates
- [ ] For `ProjectNode`: calls child exactly once per `next()` call
- [ ] For `HashAggregateNode`: build phase called exactly once (on first `next()` call), not once per `next()`
- [ ] For `LimitNode`: counter increments by 1 per row emitted; stops pulling child after exactly N rows
- [ ] For `HashJoinNode` probe phase: after emitting all matching build rows for one probe row, advances to the next probe row — not stuck on the same probe row

---

### 3. Exhaustion Behavior

- [ ] `next()` returns `nullptr` when the input is exhausted — never returns a stale row
- [ ] `next()` returns `nullptr` consistently after the first `nullptr` — not once then a row, then nullptr again
- [ ] For `FilterNode`: if all remaining rows fail the predicate, returns `nullptr` (does not loop infinitely after child returns `nullptr`)
- [ ] For `LimitNode`: returns `nullptr` after N rows even if child has more rows
- [ ] For pipeline breakers: emits all accumulated rows before returning `nullptr`; does not skip the last group / row

---

### 4. close() — Resource Cleanup and Symmetry

- [ ] Child's `close()` called in parent's `close()`
- [ ] All heap-allocated memory freed in `close()`
- [ ] `close()` is safe to call even if the operator was not fully exhausted (early termination via `LimitNode`)
- [ ] `close()` is safe to call if `open()` was never called — no crash
- [ ] `close()` is idempotent: calling it twice does not double-free or crash
- [ ] For `HashAggregateNode` and `SortNode`: internal buffers cleared in `close()`, not left populated

---

### 5. State Reset for Re-execution

If an operator can be re-opened (e.g., the build side of a `HashJoinNode` is scanned twice in a nested-loop fallback, or tests re-run the same operator), verify:

- [ ] `close()` followed by `open()` produces the same results as the first execution
- [ ] No state leaks between executions: counters at 0, hash maps empty, sort buffers empty
- [ ] For `HashJoinNode` build side: hash map rebuilt on re-open, not reused from previous open

---

### 6. Child Iterator Propagation

- [ ] Every operator calls `child->open()` before calling `child->next()`
- [ ] Every operator calls `child->close()` in its own `close()`
- [ ] `HashJoinNode`: both `left_child->close()` and `right_child->close()` called
- [ ] No operator calls `child->next()` after child returned `nullptr` (undefined behavior risk)
- [ ] No operator calls `child->close()` before it has returned `nullptr` from `next()`, unless this is an intentional early termination

---

### 7. LimitNode Early Termination

`LimitNode` stops pulling its child after N rows. This means the child is `close()`d while it may still have rows. Verify:

- [ ] `LimitNode::close()` calls `child->close()` even if child is not exhausted
- [ ] Child operators handle `close()` called before exhaustion without crash or resource leak
- [ ] Especially `SeqScanNode`: `close()` while cursor is mid-table — no issue expected; verify
- [ ] Especially `HashJoinNode` probe phase: `close()` while probe is mid-table — hash map must be freed

---

### 8. Empty Input

- [ ] Every operator returns `nullptr` immediately from the first `next()` call when its child has zero rows
- [ ] `HashAggregateNode`: no output rows for empty input (not a phantom empty-group row) — except `COUNT(*)` on empty table which should return one row with value 0 (verify SwiftQL's behavior matches its documented semantics)
- [ ] `SortNode` on empty input: returns `nullptr` immediately
- [ ] `HavingNode` on empty input: returns `nullptr` immediately

---

## Output Format

For each operator:
```
Operator: FilterNode  (src/planner/plan_nodes.cc:FilterNode)

open()           ✅  cursor reset, child->open() called
next()           ⚠️  inner loop calls child->next() after child returned nullptr — line 88
exhaustion       ✅  returns nullptr consistently
close()          ✅  child->close() called, no heap allocations
state reset      ✅  no stale state
child propagation ✅
LimitNode compat ✅  close() safe mid-stream
empty input      ✅  returns nullptr immediately

Issues:
  ⚠️  line 88: `while (row = child->next())` — if child returned nullptr on previous call,
      this calls child->next() again. Undefined if child does not guarantee idempotent nullptr.
      Fix: add early-exit check before re-entering loop.
```

Final verdict per operator: **LIFECYCLE CORRECT** / **MINOR ISSUE** / **BUG**.
