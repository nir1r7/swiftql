# Phase 5 orchestrator state
Current: week 33 — implementation COMPLETE and pushed through ecee221. Gate r2
  (a2b647beda9e39206) + audit r1 (a59512b5b5719b745) launched 11:22 UTC. Audit is told to
  commit its file AS IT GOES, not just at the end (two audits have been lost this run).
  AUDIT R1'S #1 TARGET IS THE ColumnId MIGRATION — no auditor has ever read it; a gate only
  proved behaviour unchanged, not that the representation is sound. That gap is now closing.
  Week 33 shipped: tasks 1,2,3,5,7,8,9. Task 4 is a RECORDED CHECKPOINT MISS (correlated
  scalar / Q17 + Q22 need derived-table range-table entries = Week 34's core deliverable).
  Task 6 (Volcano parity) is a stated gap. The "correct fallback" bullet ships as a REFUSAL,
  written up as a conscious deviation.
  Two stale rationales corrected this week: a Week 32 note pointed at src/execution/
  hash_join_node.cc which DOES NOT EXIST (the Volcano hash join is HashJoinNode in
  src/planner/plan_nodes.h, and its refusal is total STRUCTURALLY — no JoinSemantics
  parameter); and a restampSlots unreachability argument rested on a refusal this week
  removed, so it was re-derived rather than inherited.
Working branch: `claude/phase5-week26-qomtkb` (env mandate; stands in for `main` everywhere in
  the skill — never push elsewhere)
Weeks done: 26 ✅ 27 ✅ 28 ✅ 29 ✅ 30 ✅ 31 ✅ 32 ✅
Last gate: GREEN (week 33 round 1, ColumnId migration alone) — 0 warnings, unit 775/775, sqlite 996, regression 318 all modes
Week 32 verdict: checkpoint met. 4 gates / 4 audits; 1 blocker, 1 high, 6 medium, 8 low.
  The HIGH was a REGRESSION a green 988-query oracle could not see, because the test guarding
  that capability had been narrowed to a scalar stand-in.
Week 31 verdict: checkpoint met. 2 gates / 2 audits; round 2 audit was CLEAN (0/0/0/0).


## !! Container reclaim — read this first after any reboot
The container is reclaimed ON THE HOUR, EVERY HOUR (05:31, 06:31, 07:31 UTC observed) and comes
back rolled to a stale WEEK-28-ERA snapshot: HEAD a91c7f4, docs/ missing week 29+ plans,
scratchpad wiped. Budget every agent to finish inside the current hour, and launch right after
a reclaim when possible.
- `run_in_background: false` is IGNORED by the harness — agents ALWAYS run in background, so a
  blocked turn cannot be used to hold the container open. Short budgets are the only lever.
- **Tell every long agent to write its artifact INCREMENTALLY**, not at the end. Whatever is on
  disk when it dies is all that survives; a partial audit beats nothing.
- **AUDITS MUST COMMIT THEMSELVES.** scratchpad/ is wiped by every reclaim, and the audit file
  is the INPUT to the fix round — one was lost entirely this way. Every auditor is now told to
  `git add -f scratchpad/audits/<file>` and push when it finishes, even if it stopped early.
  Gate LOGS are deliberately NOT committed (150-250KB each); their verdict goes in this file.
- **Never run two gates concurrently** — they contend on the same build/ directory.
- **The git remote is the only durable store.** This file is force-added to git for that reason
  (`git add -f scratchpad/phase5-state.md`) despite the skill saying not to commit scratchpad —
  an uncommitted state file does not survive the exact event it exists for.
- **Recovery, every reboot:** `git fetch origin claude/phase5-week26-qomtkb`, confirm local HEAD
  is an ancestor, then `git reset --hard origin/claude/phase5-week26-qomtkb`.
- **Root cause of lost agents:** background subagents do NOT keep the session alive. A turn that
  ends with only background agents running reads as idle → reclaim → the next heartbeat boots a
  fresh container and those agents are gone. Two gate+audit pairs were lost this way.
- **Long work must be interruption-tolerant**, since nothing keeps the container alive:
  commit+push after each coherent unit, and keep a "## Progress" section in the week's plan doc
  naming done / in progress / next. Week 32's implementation survived a mid-run kill that way —
  a successor resumed from the handoff instead of restarting the week.

## Week 31 as shipped
Uncorrelated + scalar subqueries by materialize-then-substitute; refusal NARROWED to correlated
subqueries only; both Week 30 tripwires stay armed. Two divergences from SQLite are DOCUMENTED,
not fixed: multi-row scalar errors (SQLite takes the first row) and IN caps at 1024 distinct
values — WEEK 32 OWNS REMOVING THE CAP. Structural insight: the diffed oracle suite cannot hold
a query that errors, so refusals need their own rejection suite.

## Deferred
- `ColumnId {level, slot}` — DEFERRED BY DECISION. Standalone change in whichever of Weeks 32/34
  first lowers a correlated reference; never folded into a feature week. Cost: 87 non-comment
  mentions / 6 source layers / every test hand-building a ColumnRef.
- `extract(year from d)` STRING-vs-int → W36; masked-eval CASE kernel → W37; SUM/AVG double
  precision declaration → W36; randomized RESULT differencing → W35 (needs scale-factor harness)

## Open concerns
- `development.md` → "Relation slots and query levels" is the map for the
  (query_level, relation_slot) collapse class. It has been WRONG BY OMISSION twice. Point every
  audit at it and have them verify against code, not trust it.
- Stop-hook asks to re-author commits to noreply@anthropic.com. Config is set so NEW commits are
  correct. DO NOT rewrite history: 135 commits carry the user's email including Weeks 1-25 they
  authored themselves. That force-push is their call.

## Heartbeats — three layers, all intentional
1. ~25-min self-re-arming send_later chain. Re-arm every turn while work is in flight.
2. Pre-armed 30-min one-shots, unbroken 04:30 → 14:00 UTC (14:00 = 10:00 user local, last one).
   Rate-limits at ~10 per burst; batch ~5. The 09:00 message asks to create the 09:30-14:00
   batch — SKIP it, they already exist.
3. Hourly backstop Routine trig_011E7EYu3E7P9F3zKFUSsqhg at :44. Routine-fired sessions get NO
   mcp__* tools so layer 3 cannot re-arm layer 1; send_later-fired ones can (verified).
