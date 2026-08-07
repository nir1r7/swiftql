---
name: phase5-week
description: Run SwiftQL's Phase 5 week plan as an auto-advancing orchestrated cycle — teach, implement, audit, fix, commit, next week — with every bulk-text step delegated to a subagent that writes to a file and returns a short summary, so one orchestrator can carry weeks 26 through 36 without exhausting context. Use when advancing the README's Phase 5 plan, invoked as `/phase5-week <start-week>`.
---

# Phase 5 Week Cycle

You are the **orchestrator**. You do not write engine code, do not run builds, do not read
diffs, and do not read audits. You launch subagents, keep durable state, and decide what
happens next.

Invoked as `/phase5-week <start-week>` — e.g. `/phase5-week 26`. Phase is always 5. The cycle
**auto-advances** through Week 36 and then runs the seam audit. Week 37 belongs to the user.

## The two rules that make this work

1. **Nothing verifies its own work.** The agent that writes code never reports whether it
   passes. A separate verifier with no stake runs the gates and returns exact counts. This is
   the same reason your auditor is a different agent than your implementer.
2. **Bulk text goes to a file, never to the orchestrator.** Teaching docs, audits, gate logs,
   and diffs are written to disk by the subagent that produces them, and the next subagent is
   pointed at the path. You receive a summary of **≤10 lines** from every subagent. Anything
   longer means the prompt asked for the wrong thing — tighten it, don't absorb it.

Eleven weeks fit in one orchestrator only if rule 2 holds every single time. Your per-week
context budget is roughly 80 lines. Guard it.

---

## Layout

```
docs/week-<y>-plan.md                    teaching output (a project artifact, committed)
scratchpad/phase5-state.md               orchestrator state — authoritative, survives compaction
scratchpad/gates/week-<y>-round-<n>.log  raw gate output
scratchpad/audits/week-<y>-round-<n>.md  raw audit
scratchpad/week-<y>-round-<n>.patch      round snapshot, deleted after the week is pushed
```

## State

`scratchpad/phase5-state.md` is authoritative. Rewrite it after every step. If your context is
compacted mid-run, re-read it and resume from `Current` — it is the reason a fresh session can
pick this up.

```markdown
# Phase 5 orchestrator state
Current: week <y>, round <n>, step <step name>
Weeks done: 26 ✅ 27 ✅ ...
Last gate: GREEN (week 27 round 3) | RED — <one line>
Deferred to later weeks: <item> → Week <n> (noted in README)
Open concerns: <anything unresolved>
```

Keep it under 25 lines. It is a resume point, not a log.

---

## Subagent check-ins

A subagent can burn an hour circling a triviality and you would never know — its only report
comes at the end, and by then the time is spent. Worse, an agent can die without ever
notifying you, and "still running" is indistinguishable from "dead" unless you check.

**You have no internal clock. Stamp every launch, or this whole section is unenforceable.**
Run `date` when you launch an agent, record that time in state next to the agent, and run
`date` again at each check-in. Without that, "has it been 10 minutes?" is unanswerable and you
will sit on a dead agent indefinitely — this happened once for 15 hours, and every status
report given in the meantime was wrong.

Check in on any agent running **longer than 10 minutes**, and every ~10 minutes after that.

The cheapest liveness probe is the artifact the agent was told to write:

```bash
date                                          # now
ls -la --time-style=+%H:%M scratchpad/audits/ scratchpad/gates/   # did its file land?
```

An agent past its expected duration with no artifact and no recent file writes is dead or
hung, not thinking. Relaunch it — do not keep waiting. Give the replacement an explicit time
budget and tell it to write its findings file even if it has to stop early: a partial audit
that exists beats a complete one that never lands.

**Never read the agent's transcript file to do this.** It is full JSONL and reading it
overflows exactly the context this skill exists to protect. The check is cheap probes only:

```bash
git status --short | wc -l                                    # is the diff growing?
find src tests python_tools docs -type f -newermt '-5 minutes' | wc -l   # still writing?
git diff --stat | tail -1                                     # net churn
```

Read those three numbers against how long the agent has been out:

| Signal | Reading | Action |
|---|---|---|
| Files still being written, diff growing | Working normally | Leave it alone |
| No writes in 5+ min, agent still live | Stuck, or thinking hard | Wait one more cycle, then ask |
| Diff churning but not growing (same files rewritten) | Circling on a decision | `SendMessage`: ask what it is deciding |
| Diff far wider than the week's scope | Scope creep | `SendMessage`: name the checkpoint, tell it to stop |
| Verifier out >12 min | A gate is ~5 min; something hung | Ask; `TaskStop` and relaunch if no answer |
| Auditor out >25 min | Over-reporting minors | `SendMessage`: blockers and real logic issues only |

When you do interrupt, `SendMessage` the agent — do not kill it. It keeps its context, and a
one-line redirect ("you are 40 minutes in on a checkpoint that needs three-table joins to
execute; what is unresolved?") recovers a circling agent far more cheaply than restarting.
Reserve `TaskStop` for an agent that has stopped responding or is provably off-task.

Log nothing about check-ins in state unless one changed what an agent was doing. A quiet
check-in is not an event.

---

## Pacing

Steps are serial by dependency, so the week's wall-clock is the sum of its steps. Two things
keep that honest:

- **Run the gate and the audit concurrently** once the previous gate was GREEN. Neither
  modifies source — the gate writes `build/` and `scratchpad/gates/`, the auditor writes
  `scratchpad/audits/` — so they observe the same tree. Reword the audit prompt: it can no
  longer claim the tree already passed, but it must still be told not to run the test suite as
  a substitute for reading. **After a RED gate, go back to serial** until a gate is green
  again; a concurrent audit of a broken tree spends its pass on symptoms, and its findings
  describe pre-fix code.
- **Do not buy a gate cycle for a lone minor.** If the only findings left are minors, carry
  them into the next week's "Starting notes" rather than running another fix+gate round. A
  blocker or major always earns its round.

---

## Per-week cycle

### Before the week

Read only the README section for this week and its `37-Week Plan` row — not the whole README.
Confirm `git status` is clean and you are on `main`. Note any "Starting notes" or "Carried into
Week `<y>`" block so you can point Agent A at it by name.

The week's bar is the **README's own checkpoint**. TPC-H 22/22 is unmeasurable before Week 36
(the data harness lands in Week 35), so it is design context, not this week's deliverable.

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
> Write your full teaching output to `docs/week-<y>-plan.md`. Do NOT repeat it in your reply.
> Reply with only: the file path, and a numbered list of the task headings you covered. Ten
> lines maximum.

The plan lives on disk. You never read it — Agent A reads its own file in the next step, and
the human reads it whenever they like.

### Step 2 — Agent A implements

Same agent, follow-up:

> Implement the plan you wrote to `docs/week-<y>-plan.md`. Implement all changes on the main
> branch, do not commit anything and do not create any new branches. Make sure to update the
> regression tests and regression queries where applicable.
>
> Any new query you add must go into `python_tools/compare_against_sqlite.py` (the external
> oracle), not only `python_tools/test_new_queries.py`. Never weaken either harness to make
> something pass.
>
> Do not report whether tests pass — a separate verifier owns that. Reply with a high level
> summary of the changes, ten lines maximum, listing the files touched by layer.

The SQLite-oracle paragraph is load-bearing. `test_new_queries.py` is authored by the same
agent whose work it validates; only `compare_against_sqlite.py` diffs against real SQLite. A
wrong expectation baked in this week becomes the baseline every later week trusts.

The last paragraph is also load-bearing — an implementer that self-reports green is the exact
failure mode this design exists to prevent.

### Step 3 — Verifier subagent (GATE)

Fresh agent, no shared context with A. It has one job and no authority to fix anything.

> Run the `verify` skill on the current working tree. Do not fix, modify, or comment on any
> code — you are a measurement instrument, not a reviewer.
>
> Write the complete raw output of every gate to `scratchpad/gates/week-<y>-round-<n>.log`.
>
> Reply with exactly this block and nothing else. Report real numbers taken from the output —
> never "all tests pass", never a count you did not read:
>
> ```
> GATE week <y> round <n>
> build:      PASS | FAIL — <first error line>
> unit:       PASS (<passed>/<total>) | FAIL (<n> failed: <test names>)
> sqlite:     PASS (<n> queries) | FAIL (<query>, mode <mode>)
> regression: PASS (<n> queries, all modes) | FAIL (<query>, mode <mode>)
> tpch:       <the harness's final `GATE tpch:` line, copied verbatim>
> VERDICT:    GREEN | RED
> log:        scratchpad/gates/week-<y>-round-<n>.log
> ```
>
> The `tpch:` line is copied, never composed: `run_tpch.py` prints it as its last line
> and it already carries the honest shape — meaningful out of 22, the mode split, and
> vacuous and unported counted separately. `FAIL` or `NO-BASELINE` there is RED like any
> other gate. Never quote the count as "correct" or "TPC-H compliant"; the oracle is
> SQLite over synthetic data, so the only available claim is "matches SQLite".

The `tpch:` line is why the block is five gates and not four. Phase 5's deliverable IS the
TPC-H figure, and until Week 35 no gate looked at it — a verifier could report GREEN over a
broken or regressed TPC-H run in perfect good faith. Baseline at the end of Week 35: **19/22
meaningful (5 in all four modes, 14 vectorized-only), 1 vacuous, 2 unported**, recorded in
`docs/tpch-baseline.json`. It reads 19 rather than the 17 first published because the round-2
audit showed q2's INERT and q19's ALL_NULL were artifacts of the SPEC's validation parameters
against this generator's distributions, not properties of the queries; both were re-chosen from
within the spec's own value domains and both now discriminate. **Week 36 raised it to 20/22 meaningful (5 in all four modes, 15 vectorized-only), 1 vacuous, 1 unported** — q17, TPC-H's own Q17 text, by lifting the constant-wrapper restriction rather than by touching the template. q21 stays unported with its requirement recorded. An improvement passes the gate
and says so, and the improving commit must carry the refreshed baseline with it.

Then snapshot, yourself — it is two cheap commands:

```bash
git add -N . && git diff > scratchpad/week-<y>-round-<n>.patch
```

`git add -N` first, or new files are silently absent from the patch and the snapshot is
worthless exactly when you need it.

**On RED:** send Agent A the verdict block plus the log path — not the log contents — and tell
it to fix and stop short of re-running gates. Then re-run the verifier at the same round
number. Never proceed to audit on a red tree; auditors waste their pass on symptoms of a known
break. If three consecutive gate runs are RED, stop the whole cycle and surface it to the user
— something is wrong that another fix round will not solve.

### Step 4 — Agent B audits

Fresh agent, no shared context with A.

> - Audit the uncommitted work that the previous agent has made
> - Use the readme as high level guidance
> - Manually trace through the code to look for issues
> - Report all issues with severity markings
>
> This tree has already passed the full gate — build, unit tests, the SQLite oracle, the
> regression harness in every mode, and the TPC-H harness against its recorded baseline.
> Running the tests tells you nothing you do not already
> know, and a green suite is not evidence of correctness. Your job is to find what the tests
> do not catch: read the code and trace it by hand. Do not run the test suite as a substitute
> for reading.
>
> Trace the actual execution paths the change introduces. Follow a concrete row or chunk
> through each new or modified operator. Check both engines — Volcano is the correctness
> baseline and vectorized must agree with it. Check the dispatch checklist in
> `development.md` for sites the change should have touched and did not; silent omissions
> there are the recurring failure mode in this codebase, and they never fail a test.
>
> Every issue must cite `file:line` and state the concrete input or state that produces the
> wrong behaviour. An issue you cannot ground that way is a hunch — mark it as such or leave
> it out.
>
> Write the full audit to `scratchpad/audits/week-<y>-round-<n>.md`. Do NOT repeat it in your
> reply. Reply with only the file path and a severity tally, e.g.
> `2 blocker, 3 major, 5 minor — scratchpad/audits/week-<y>-round-1.md`. Two lines maximum.

On the **second** audit (round 2) and any later one, append:

> Focus only on blockers and real logic issues — correctness, invariant violations, crashes,
> wrong results, silent dispatch-site omissions. Do not report style, naming, or minor
> inconveniences.

### Step 5 — Agent A fixes

The audit reaches A through the file, not through you. This preserves exactly what
copy-pasting was for — A sees the audit verbatim, unsummarized — while costing you two lines
instead of two full audits.

> Read `scratchpad/audits/week-<y>-round-<n>.md` in full.
>
> - Verify all the issues and identify those that require action
> - Take the steps and actions necessary to implement the changes on main (do not commit)
> - Make any small notes in the readme plan for future weeks (ONLY IF REQUIRED, the preference
>   is to resolve as much as possible in the current session). A handoff note is a real
>   deliverable, not a concession — the Week 26 "Starting notes" and the "Carried into Week 36"
>   block are why later weeks start with hazards enumerated instead of discovered. Write one
>   when a decision genuinely belongs to a later week; do not write one to avoid work.
>
> Do not report whether tests pass. Reply with ten lines maximum: which issues you actioned,
> which you rejected and why, and anything you deferred to a later week.

### Step 6 — Repeat

Gate (Step 3, round 2) → audit (Step 4, blockers-only) → fix (Step 5) → gate (round 3).

### Step 7 — Third round, by checklist

Run one more audit+fix+gate round if **any** of these hold. Use the checklist, not your
judgment — judgment degrades over a long session in a way a checklist does not:

- The last fix round changed planner, execution, or storage code (not just tests/docs)
- Agent A deferred anything to a later week
- Agent A rejected an audit finding by reasoning rather than by running something
- A gate went RED more than once this week

### Step 8 — Commit split subagent

Only on a GREEN gate. Delegate — the week's diff is far too large for you to hold.

> All Phase 5 Week `<y>` work is uncommitted on `main` and has passed verification. Split it
> into conventional commits matching the existing history (see `git log` for the Week 25
> pattern). Order by pipeline layer so each commit is coherent and independently sensible:
>
> ```
> feat(common):    shared helpers / value / date utils
> feat(parser):    lexer, AST, grammar
> feat(planner):   binder, validator, logical plan, optimizer passes, physical planning
> feat(execution): volcano + vectorized nodes, evaluators
> feat(storage):   columnar layout, encodings, pruning
> test:            unit tests
> test(harness):   python_tools regression + SQLite queries
> docs:            README, development.md, docs/*
> ```
>
> Use `fix(...)` for corrections to existing behaviour, `perf(...)` for measured speedups. Skip
> layers this week did not touch. Include `docs/week-<y>-plan.md` in the `docs:` commit. Do not
> commit anything under `scratchpad/`. Every change in the working tree must land in exactly
> one commit — leave nothing uncommitted.
>
> Then push: `git push -u origin main`. On network failure retry up to 4 times with exponential
> backoff (2s, 4s, 8s, 16s).
>
> Reply with the output of `git log --oneline` for your commits and `git status --short`.
> Nothing else.

Confirm yourself with one cheap call — `git status --short` must be empty:

```bash
git status --short && git log --oneline -1
```

If the tree is not clean, the split dropped something. Send it back to the same agent.

### Step 9 — Advance

Delete this week's snapshots (`rm -f scratchpad/week-<y>-round-*.patch`). Gate logs and audits
are small — keep them for the run.

Update state: mark the week done, carry forward deferred items, set `Current` to the next week.

Then **re-read the README** to pick the next week rather than assuming `+1` — the plan has
inserted fractional weeks before (Week 23.5). Weeks 26–36 are currently all integers.

Loop to Step 1. After Week 36, go to the seam audit.

---

## Week 36 is not one iteration

It is where every deferred gap lands at once — the `extract(year from d)` STRING-vs-integer
decision, the masked-evaluation `CASE` kernel, the `SUM`-in-`double` precision declaration, and
whatever weeks 26–35 hand forward. It is also the first week TPC-H 22/22 is measurable at all.

Run the cycle repeatedly on Week 36, one coherent group of queries per iteration, using the
deferred-items list in state as the work queue. Commit and push each iteration. Move on when
the gate is GREEN and the deferred list is empty.

---

## Final seam audit (after Week 36)

Do **not** run one auditor per week. Each week already had 2–3 audits; a week-shaped auditor
re-treads covered ground. What has never been audited is the **seams between weeks**. Launch
these five in parallel, each with the matching project skill, each writing to
`scratchpad/audits/seam-<name>-pass-<n>.md` and replying with a path plus severity tally:

| Auditor | Scope | Skill |
|---|---|---|
| Join chain | Weeks 26→27→28→29 — does DP enumeration still hold once outer joins exist? Build/probe swap under N relations, fixed `[FROM]++[JOIN]` schema order at depth | `invariants` |
| Subquery chain | Weeks 30→31→32→33→34 — semi/anti lowering vs decorrelation vs derived tables; scalar cardinality checks | `operator-correctness` |
| Engine divergence | Every operator added in Phase 5 on both paths — Volcano is the baseline, vectorized must agree | `vectorized-audit` |
| Optimizer preservation | optimized ≡ `--no-optimize` ≡ SQLite across the whole Phase 5 query surface; estimate quality on multi-join plans | `optimizer-diff` |
| Storage | Any Phase 5 change reaching chunks, pruning, or reconstruction | `storage-verify` |

Fix findings with the Step 5 prompt shape, one agent per auditor that found blockers, pointed
at that auditor's file. Gate (Step 3) after each fix round, then commit and push (Step 8).
Repeat the seam pass up to 5 times, stopping as soon as a pass returns no blockers.

Then stop and report to the user. Week 37 is theirs.

---

## Done criteria for a week

- Verifier returned `VERDICT: GREEN` on the final tree, with real counts
- The README checkpoint for that week is met, not asserted
- New queries present in `compare_against_sqlite.py`; neither harness weakened
- Commits split by layer and pushed to `main`; `git status --short` empty
- State updated, snapshots deleted, deferred items recorded in both state and README

## If you are losing context

The state file plus the on-disk artifacts are a complete resume point. Tell the user which week
and step you are on and that a fresh session can continue from
`scratchpad/phase5-state.md` — do not try to push through a compaction mid-week and guess at
what you already did.
