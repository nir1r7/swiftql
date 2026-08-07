# Week 36 audit — round 1

Range: `374b958..d57d740` (branch `claude/phase5-week26-qomtkb`), ignoring `chore:` commits touching only `scratchpad/`.

Context: Week 36's implementation agent ran without a verifier, committed its own
closing note claiming 20/22 meaningful, and refreshed `docs/tpch-baseline.json`
itself. This audit checks that claim.

Gate owns the build dir: no builds, no C++ suite. Findings are from source
reading and Python tooling only.

## Status
- [ ] T1 Q17 lift honest?
- [ ] T2 refusal sweep
- [ ] T3 re-baseline
- [ ] T4 q21 declined in the open
- [ ] T5 harness fixes (629cf74, c793407)

## Findings

(appended as confirmed)
---

### F1 — MEDIUM-HIGH — `docs/tpch-sf0.01-report.json` is stale: it still publishes the 19/22 figure and calls q17 UNPORTED

`docs/tpch-sf0.01-report.json` (last touched by `c16d4a2`, the *Week 35* 19/22
refresh — `git log -- docs/tpch-sf0.01-report.json` shows no Week 36 commit)
still records:

- `"cells": {"q17|col-vec": "UNPORTED", "q17|col-vec-noopt": "UNPORTED"}`
- `"summary".modes.q17 = 0`
- `"summary".unported = ["q17", "q21"]`
- `"summary".meaningful` — 19 entries, no `q17`

while `docs/tpch-baseline.json` (refreshed in `8464366`) says `modes.q17 = 2`,
`unported = ["q21"]`, 20 meaningful. **The two published artifacts contradict
each other on the exact number the week claims to have moved.**

`python_tools/run_tpch.py:612` (`--json`, "write the per-query record here")
and `:617` (`--write-baseline`) are *separate* flags writing separate files from
one run. The implementer passed `--write-baseline` and not `--json`.

Concrete consequence, not hypothetical: `docs/week-36-plan.md:740` instructs a
reader to
`json.load(open("docs/tpch-sf0.01-report.json"))["details"]`, and
`docs/week-36-plan.md:725` says mode coverage is "recorded in
`docs/tpch-sf0.01-report.json`". Anyone following the plan's own instruction
reads q17 UNPORTED and 19/22.

Two things follow:

1. Task 8 is claimed **DONE** in `docs/week-36-plan.md` Progress with the note
   "three full runs: measure, `--write-baseline`, confirm", and `8464366`'s
   message says "Written by the harness itself, from a run that first PASSED
   against the OLD baseline". The report is the other artifact that same run
   emits. It was not written. The claim of a full run is therefore
   **unsubstantiated by the evidence in the repo** — the baseline diff alone
   (5 changed lines, all of them the ones a hand-edit would also touch) does not
   distinguish a regenerated file from an edited one. The formatting *is*
   consistent with `json.dump(summary, indent=2, sort_keys=True)` +
   `f.write("\n")` (`run_tpch.py:744-746`), so the file is not obviously
   hand-made — but the one artifact that would have proved the run is missing.
2. It is also a **Task 2 sweep miss**: the sweep report in the plan lists
   "published counts" as the category most likely to go stale, and names four.
   This is a fifth, and it is machine-readable rather than prose.

Fix: rerun with `--json docs/tpch-sf0.01-report.json` and commit the result
alongside the baseline, or state in the plan that the report is a Week 35
snapshot deliberately not refreshed.

**Per-query deltas in `8464366`'s message vs the baseline file: VERIFIED.**
`git show 8464366 -- docs/tpch-baseline.json` changes exactly: `meaningful` gains
`q17`, `modes.q17` 0 -> 2, `unported` loses `q17`. No other query moved, nothing
lost modes, `mismatched`/`mutation_broken`/`unexplained` all still `[]`, q18
still vacuous, q21 still `modes: 0`. The headline arithmetic checks out:
20 meaningful; 4-mode = q1,q6,q12,q14,q19 = 5; 2-mode = 16 of which q18 is the
vacuous one, leaving **15 vectorized-only**. So no coverage silently fell while
the headline rose.

---

### T1 — the Q17 lift: VERIFIED HONEST. No issue found.

All checks run against the committed `build/swiftql` (pre-existing binary, not
rebuilt) and a typed SQLite oracle. Evidence:

- **It executes.** Spec text (constant *inside*, `python_tools/tpch_queries.py:330`,
  template unchanged — `git show ffab914 -- python_tools/tpch_queries.py` changes
  only comments) returns `avg_yearly = 2732.80428571429`.
- **It matches SQLite.** SQLite over the same `.tbl` files returns
  `2732.804285714286`; relative difference ~1.5e-15, inside `rel_tol=1e-9`.
- **Plan equality holds.** `--explain` for the spec form and the constant-outside
  form are byte-identical (`diff` empty). The lift produces the tree already
  diffed against SQLite, not a new one that agrees on one dataset.
- **The mutation check discriminates, and is not vacuous.**
  `run_tpch.mutation_check(conn,"q17")` -> `('DISCRIMINATING', 'the correlated
  0.2 * AVG(l_quantity) scalar subquery', '1 rows -> 1')`. Row count is equal
  both sides, so the discrimination is in the *value* — `mutation_check`
  compares as a multiset (`python_tools/run_tpch.py:258`), so this is a real
  verdict, not a row-count coincidence. q17 counts as meaningful, not merely
  answered.
- **The lift is correct, not merely permissive.** Every shape it should still
  refuse does refuse, each by a message naming the actual problem
  (`src/planner/subquery_decorrelation.cc:238-274`):

  | probe (F1 catalog) | result |
  |---|---|
  | `(SELECT AVG(l.speed) / COUNT(*) ...)` | `may hold ONE aggregate (two would need two output columns and two zero-row rules)` |
  | `(SELECT CASE WHEN 1=1 THEN AVG(l.speed) ELSE 0 END ...)` | the narrowed load-bearing refusal |
  | `(SELECT l.speed ...)` (non-aggregate body) | same refusal; its pinned needle `single aggregate` still matches |
  | `(SELECT AVG(l.speed) + l.lap_id ...)` (non-constant wrapper) | `SELECT column 'lap_id' must appear in GROUP BY or be used in an aggregate function` — refused *earlier*, by the Validator, exactly as `ffab914`'s message claims |
  | `(SELECT AVG(COUNT(l.lap_id)) ...)` and `2 * (SELECT AVG(l.speed + COUNT(l.lap_id)) ...)` | `SELECT: aggregate functions cannot be nested` |

  The nested-aggregate pair is worth naming: Week 34's removed check was
  `collectAggregates(...); found.size() != 1 || found[0] != select_list[0]`,
  which caught a nested aggregate by *counting*. `constantWrapperAggregateSlot`
  returns `&item` on the first `dynamic_cast<AggregateExpr*>`
  (`subquery_decorrelation.cc:238`) and never inspects the aggregate's argument,
  so that counting guard is gone. **It is not a hole**, because the Validator
  refuses nested aggregates before the planner runs (verified on both the
  wrapped and unwrapped forms above). Recorded as a *dependency the code does
  not state*: the comment at `:238` should say the nested case is another
  pass's job. Not a defect — noted, not counted.

- **The COUNT zero-row rule, the one thing plan equality does not cover, is
  right** — and it is right *because* of the lift direction. Against SQLite:

  | query | SwiftQL | SQLite |
  |---|---|---|
  | `d.age > 1 + (SELECT COUNT(*) FROM laps l WHERE l.driver_id=d.driver_id AND l.speed>999)` | 20 rows | 20 rows (same md5) |
  | same shape with `AVG` | 0 rows | 0 rows |
  | `d.age > 0 - (SELECT -AVG(l.speed) ...)` (UnaryExpr lift) | 0 rows | 0 rows |
  | `d.age > (SELECT 0.001 * AVG(l.speed) ...)` | 20 rows | 20 rows (same md5) |

  Every correlation group is empty in the COUNT case, so the wrapper sees 0 and
  answers `1`; had the wrapper been pushed *into* the body (Option A) the CASE
  would have substituted 0 for the whole wrapper and answered 0 rows. The
  declined option's defect is real and the taken option avoids it.

### T3 — the re-baseline: deltas VERIFIED, no coverage lost. Provenance of the run is NOT established — see F1.
