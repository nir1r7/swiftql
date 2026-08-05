---
name: phase5-week
description: Run one full Phase 5 week of SwiftQL development as an orchestrated teach → implement → audit → fix cycle, with orchestrator-run verification gates between every stage, then split the week into conventional commits and push to main. Use when advancing the README's Phase 5 week plan (weeks 26–36), invoked as `/phase5-week <week>`.
---

# Phase 5 Week Cycle

You are the **orchestrator**. You do not write engine code. You launch subagents, run the
verification gates yourself, keep durable state, and own the commit split.

The single rule that makes this work: **a subagent's claim that tests pass is not evidence.**
You run the gates. If gate output is not in your context, the week is not verified.

Invoked as `/phase5-week <week>` — e.g. `/phase5-week 26`. Phase is always 5.

---

## Before starting

1. Read `README.md` → the section for this week, and the `37-Week Plan` row. The week's
   **checkpoint line in the README is the bar**, not TPC-H 22/22 (weeks 26–34 have no TPC-H
   data — the harness lands in Week 35, so 22/22 is unmeasurable before Week 36).
2. Read any "Starting notes" / "Carried into Week N" blocks addressed to this week. Weeks 24
   and 25 left real hazards for later weeks; they are the highest-value input the agent gets.
3. Confirm the tree is clean (`git status`) and you are on `main`.
4. Open or create `scratchpad/phase5-state.md` (see **State** below). Your own context will be
   compacted across a long week — the file is what survives.

---

## State

Keep `scratchpad/phase5-state.md` current. Update it after every gate run and every round.

```markdown
# Phase 5 orchestrator state
Current: week <y>, round <n>
Gates: build ✅ | unit ✅ (524) | sqlite ✅ | regression ✅   (last run: after round 2 fixes)
Snapshots: scratchpad/week-<y>-round-1.patch, ...
Deferred to later weeks: <item> → Week <n> (noted in README)
Open concerns: <anything unresolved>
```

---

## The cycle

### Step 1 — Agent A teaches

Launch Agent A. Substitute `<y>`; phase is 5.

> Before responding, read the entire readme.md thoroughly. The goal of this reading is not
> just context — it is correctness. Every explanation, code snippet, and implementation step
> you provide must be consistent with the existing architecture, conventions, and constraints
> of the project. Guidance that contradicts or ignores the established codebase will introduce
> bugs and confusion. Once you have a complete picture of the project, locate Phase 5 week
> `<y>`, and internally validate an implementation plan before teaching anything. Ask yourself:
> Does this approach fit the existing data flow? Does it conflict with anything already built?
> Are there edge cases introduced by prior design decisions? Only once you are confident the
> plan is sound should you proceed to teaching. You are acting as a teacher/mentor — not an
> implementer. Do NOT write code directly into the codebase. For each task in Week `<y>`,
> provide the following in order: **Why it matters** — Explain the role this component plays in
> the overall project architecture. Be explicit about dependencies and downstream impact.
> **Conceptual explanation** — Cover the theory and logic behind the task before any code.
> **Code snippets** — Provide illustrative snippets that demonstrate the pattern, with inline
> comments explaining each part. Snippets must reflect the actual patterns, naming conventions,
> and libraries used in this project — not generic examples. **Implementation guidance** —
> Step-by-step instructions a developer can follow independently, anticipating common mistakes
> or gotchas specific to this codebase. **Verification** — How the developer should test or
> confirm their implementation is correct. Keep explanations precise. Avoid vague
> encouragement. If a task has prerequisite knowledge, flag it.
>
> The bar for this week is the README's own checkpoint for Week `<y>`, plus any "Starting
> notes" or "Carried into Week `<y>`" blocks addressed to it — read those and address them
> explicitly. The phase goal is running TPC-H at 22/22; treat it as context for design
> decisions, not as this week's deliverable. Do not build anything this week's checkpoint does
> not require.
>
> Write your full teaching output to `docs/week-<y>-plan.md` as well as returning it.

**Persist the plan.** It is the most expensive artifact in the cycle. If A degrades later, a
fresh agent can resume from the file instead of re-reading the README and re-deriving the plan.

### Step 2 — Agent A implements

Same agent, follow-up message:

> Implement the plan that you described above. Implement all changes on the main branch, do not
> commit anything and do not create any new branches. Provide a brief high level summary of the
> changes made. Make sure to update the regression tests and regression queries where
> applicable and run them all as verification.
>
> Any new query you add must go into `python_tools/compare_against_sqlite.py` (the external
> oracle), not only `python_tools/test_new_queries.py`. Never weaken either harness to make
> something pass.

The second paragraph is not optional. `test_new_queries.py` is authored by the same agent whose
work it validates; `compare_against_sqlite.py` diffs against real SQLite. Only the second is an
independent oracle, and a wrong expectation baked in this week becomes the baseline every later
week trusts.

### Step 3 — GATE + snapshot

**You run this, not an agent.**

1. Invoke the `verify` skill. Record the pass/fail table in state.
2. `git diff > scratchpad/week-<y>-round-1.patch` (also `git status` for new files —
   use `git add -N .` first so untracked files appear in the diff).

If a gate fails, hand the raw failure back to Agent A and re-run the gate. Do not proceed to
audit on a red tree — auditors waste their pass on symptoms of a known break.

### Step 4 — Agent B audits (full)

Fresh agent, no shared context with A. Single prompt:

> - Audit the uncommitted work that the previous agent has made
> - Use the readme as high level guidance
> - Manually trace through the code to look for issues
> - Report all issues with severity markings

### Step 5 — Agent A fixes

Paste Agent B's output **verbatim** — do not summarize an audit. In the same prompt:

> - Read the audit and all the issues
> - Verify all the issues and identify those that require action
> - Take the steps and actions necessary to implement the changes on main (do not commit)
> - Provide a summary of the changes made
> - Make any small notes in the readme plan for future weeks (ONLY IF REQUIRED, the preference
>   is to resolve as much as possible in the current session). A handoff note is a real
>   deliverable, not a concession — the Week 26 "Starting notes" and the "Carried into Week 36"
>   block are why later weeks start with hazards enumerated instead of discovered. Write one
>   when a decision genuinely belongs to a later week; do not write one to avoid work.

### Step 6 — GATE + snapshot

Run `verify` again. Snapshot to `scratchpad/week-<y>-round-2.patch`. Update state.

### Step 7 — Agent B audits (blockers only)

Fresh agent. Same prompt as Step 4, plus:

> Focus only on blockers and real logic issues — correctness, invariant violations, crashes,
> wrong results, silent dispatch-site omissions. Do not report style, naming, or minor
> inconveniences.

Then repeat Step 5 (fix) and Step 6 (gate + snapshot as round 3).

### Step 8 — Third audit, your judgment

Read Agent A's most recent response. Run one more audit+fix round if any of these hold:

- A's fix round changed planner, execution, or storage code (not just tests/docs)
- A deferred anything to a later week
- The audit surfaced a disagreement A resolved by reasoning rather than by running something
- Gates passed only after more than one fix attempt

Otherwise proceed. Snapshot after any extra round.

### Step 9 — Commit split + push

Final `verify` run must be green before anything is committed.

Split the week's diff into conventional commits, matching the existing history (`git log`
shows the pattern from Week 25). Order by pipeline layer so each commit is coherent:

```
feat(common):    shared helpers / value / date utils
feat(parser):    lexer, AST, grammar
feat(planner):   binder, validator, logical plan, optimizer passes, physical planning
feat(execution): volcano + vectorized nodes, evaluators
feat(storage):   columnar layout, encodings, pruning
test:            unit tests
test(harness):   python_tools regression + SQLite queries
docs:            README, development.md, docs/*
```

Use `fix(...)` for corrections to existing behaviour, `perf(...)` for measured speedups.
Skip layers the week did not touch. Then:

```bash
git push -u origin main
```

Retry network failures up to 4 times with exponential backoff (2s, 4s, 8s, 16s).

Delete that week's snapshot patches once pushed. Clear the round state, set `Current` to the
next week, and carry forward any deferred items.

---

## Advancing weeks

Re-read the README to pick the next week rather than assuming `+1` — the plan has inserted
fractional weeks before (Week 23.5). Weeks 26–36 are currently all integers.

Stop after **Week 36**. Week 37 is the user's.

**Week 36 is not one iteration.** It is where every deferred gap lands at once — the
`extract(year from d)` STRING-vs-integer decision, the masked-evaluation `CASE` kernel, the
`SUM`-in-`double` precision declaration, and whatever weeks 26–35 handed forward. It is also
the first week TPC-H 22/22 is measurable at all. Budget several cycles, one coherent group of
queries at a time, and treat the deferral list in state as its work queue.

---

## Final phase audit (after Week 36)

Do **not** run one auditor per week. Each week already had 2–3 audits; a week-shaped auditor
re-treads covered ground. What has never been audited is the **seams between weeks**. Launch
these in parallel, each with the matching project skill:

| Auditor | Scope | Skill |
|---|---|---|
| Join chain | Weeks 26→27→28→29 — does DP enumeration still hold once outer joins exist? Build/probe swap under N relations, fixed `[FROM]++[JOIN]` schema order at depth | `invariants` |
| Subquery chain | Weeks 30→31→32→33→34 — semi/anti lowering vs decorrelation vs derived tables; scalar cardinality checks | `operator-correctness` |
| Engine divergence | Every operator added in Phase 5 on both paths — Volcano is the baseline, vectorized must agree | `vectorized-audit` |
| Optimizer preservation | optimized ≡ `--no-optimize` ≡ SQLite across the whole Phase 5 query surface; estimate quality on multi-join plans | `optimizer-diff` |
| Storage | Any Phase 5 change reaching chunks, pruning, or reconstruction | `storage-verify` |

Fix findings using the Step 5 prompt shape. Re-run the full `verify` gate after each fix round.
Repeat the seam pass up to 5 times, stopping as soon as a pass returns no blockers.

---

## Done criteria for a week

- `verify` green on the final tree — build, 524+ unit tests, SQLite oracle, regression harness
  in all modes including the optimizer invariant
- The README checkpoint for that week is actually met, not asserted
- New queries present in `compare_against_sqlite.py`, neither harness weakened
- Commits split by layer, pushed to `main`
- State file updated; snapshots deleted; deferred items recorded in both state and README
