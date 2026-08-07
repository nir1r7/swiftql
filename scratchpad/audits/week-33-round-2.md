# Week 33 — Round 2 Audit (the fix round as new code)

Range: `ecee221..11f2cee`, excluding `chore:`-only scratchpad commits.
Round 1 findings (`scratchpad/audits/week-33-round-1.md`) are not re-audited.

Status: IN PROGRESS (append-as-confirmed; container may reclaim scratchpad).

## Findings

### R2-C1 (CRITICAL) — the SAME repeat-defect the fix round repaired, unrepaired at its second site: a correlated `IN` subquery is lowered to a semi-join with the outer ref resolved by bare name

**Site of the stale precondition:** `src/planner/subquery_lowering.h:25-31`

```
// PRECONDITIONS, all established by Validator::validate, which both planner
// entry points run FIRST — the same trust-don't-recheck stance
// materializeSubqueries takes ...
//   - no correlated subquery anywhere (refused in Week 33's name), so every
//     ColumnRef in a body is query_level 0 against the BODY's range table;
```

This is the *identical* sentence, citing the *identical* deleted refusal, that
`7c46bdf` removed from `subquery_materialization.cc`. Week 33 deleted the
Validator refusal; `subquery_lowering.{h,cc}` still trusts it. The fix round
repaired one of the two files that named it.

**Why nothing else catches it.** `src/planner/logical_plan.cc:814` runs
`lowerInSubqueries` on the WHERE conjuncts, and `refuseUnloweredCorrelated`
only runs at `src/planner/logical_plan.cc:827` — *after* lowering has already
consumed the conjunct. So the tripwire added this round never sees a correlated
`IN`. `lowerInSubqueries` (`src/planner/subquery_lowering.cc:36-53`) branches
only on `sq->kind != Kind::IN`; it never reads `sq->correlated`, and
`planBody` (`subquery_lowering.cc:19-26`) hands the correlated body straight to
`LogicalPlanBuilder::build`, where the outer ref falls back to bare-name
resolution against the BODY's own schema — `l.team = d.team` becomes
`laps.team = laps.team`, a tautology, exactly the mechanism `7c46bdf`'s message
describes.

**Grounded, reproduced on HEAD** (`--storage columnar --execution vectorized
--no-cache`, project `catalog.json`; SQLite reference computed from the same
two CSVs):

| query | SQLite | SwiftQL |
|---|---|---|
| `SELECT COUNT(*) FROM drivers d WHERE d.driver_id IN (SELECT l.lap_id FROM laps l WHERE l.team = d.team)` | **6** | **20** |
| `SELECT COUNT(*) FROM drivers d WHERE d.driver_id NOT IN (SELECT l.lap_id FROM laps l WHERE l.team = d.team)` | **14** | **0** |
| `SELECT COUNT(*) FROM drivers d WHERE d.age IN (SELECT l.season FROM laps l WHERE l.driver_id = d.driver_id)` | 0 | 0 (agrees by luck — the tautological body still misses) |

No error, no warning: the correlated predicate is silently discarded and the
semi-join degenerates to "does the body have any row at all". This is a silent
wrong answer for every correlated `IN`/`NOT IN`, which is the third time this
project has shipped this defect shape and the second time it has produced a
wrong answer.

**Why the rejection suite does not catch it.**
`WEEK33_CORRELATED_BINDS` (`python_tools/compare_against_sqlite.py:815-866`)
contains no *directly* correlated `IN`. The one nearby entry,
`WEEK33_CORRELATED_NESTED_IN_BODY` (`compare_against_sqlite.py:807-810`), is an
**uncorrelated** `IN` whose *body* holds a correlated scalar — a different
shape, refused for a different reason (the nested scalar), which is why it
passes while this one silently answers.

**Minimal fix:** in `lowerInSubqueries` (`src/planner/subquery_lowering.cc:38`),
skip lowering when `sq->correlated` (push it to `kept`) so it reaches
`refuseUnloweredCorrelated` at `logical_plan.cc:827`; or refuse by name at that
point. Then delete the stale precondition bullet at `subquery_lowering.h:29`
and add both queries above to `WEEK33_CORRELATED_BINDS`.

