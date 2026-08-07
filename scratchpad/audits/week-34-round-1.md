# Week 34 Round 1 Audit — IN PROGRESS

Range: fe17f40..HEAD, branch claude/phase5-week26-qomtkb. Started ~14:41 UTC.
Targets: Q17 decorrelation, countRelations fix, slot-0 stamping, COUNT(DISTINCT),
citation sweep, derived-table alias-list oracle blind spot.

## Findings

(appending as confirmed)

---

## F1 — BLOCKER — Q17's LEFT join returns NULL where SQL requires 0 for a COUNT body

`src/planner/subquery_decorrelation.cc:270-284` (the `join->join_type = JoinType::LEFT;`
block) and its rationale at `src/planner/subquery_decorrelation.h:87-92`.

The comment states the rule as "SQL says a scalar subquery over zero rows is NULL".
That is true for AVG/SUM/MIN/MAX and **false for COUNT**: an aggregate query with no
GROUP BY over an empty input returns exactly one row with `COUNT = 0`. The correlated
scalar subquery therefore evaluates to `0`, not NULL, for an outer row with no matching
body rows.

`requireDecorrelatableScalarBody` (`src/planner/subquery_decorrelation.cc:170-188`)
accepts *any* single aggregate, COUNT included. The rewrite groups the body by the
correlation key, so a key with zero rows produces **no group row at all** — the LEFT
join then NULL-extends it. NULL, not 0.

Concrete input (data/ shipped in-repo, every body row filtered out by `speed > 999`):

```
SELECT d.name FROM drivers d
WHERE d.age > (SELECT COUNT(*) FROM laps l
               WHERE l.driver_id = d.driver_id AND l.speed > 999)
```

- SQLite (python3 sqlite3, equivalent 2-row fixture and the shipped shape): every outer
  row qualifies, because the subquery is `0` and `age > 0`.
- SwiftQL `--storage columnar --execution vectorized --no-cache`: **0 rows**.
  Verified against `build/swiftql` (binary is current: built 14:38:25, last src commit 14:38:43).
- The AVG control of the same shape correctly returns 0 rows in both.

Silent wrong answer, in the exact deliverable Week 33 missed. It is invisible to the
oracle harness only because no Week 34 harness query uses a COUNT body in a correlated
scalar position.

Minimal fix, either:
 (a) refuse a COUNT body in `requireDecorrelatableScalarBody` by name (consistent with
     the week's "refuse rather than mis-rewrite" discipline), or
 (b) wrap the replacement `ColumnRef` in `COALESCE(x, 0)` for COUNT bodies only —
     which needs a COALESCE the dialect may not have, so (a) is the minimum.

Note the same class extends to any body expression that is non-NULL over an empty input;
the guard only admits a single bare aggregate, so COUNT is the whole reachable set today.
