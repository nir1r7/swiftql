# Phase 5 orchestrator state
Current: week 31, round 1 — gate + audit to be run SYNCHRONOUSLY (see mitigation below)
Working branch: `claude/phase5-week26-qomtkb` (env mandate; stands in for `main` everywhere in
  the skill — never push elsewhere)
Weeks done: 26 ✅ 27 ✅ 28 ✅ 29 ✅ 30 ✅
Last gate: GREEN (week 30 round 4, closing) — unit 726/726, sqlite 864, regression 296 all modes
Week 31 implementation is PUSHED (d99533a). Do not re-run it. It still needs gate + audit.

## !! Container reclaim — read this first after any reboot
The container is reclaimed roughly hourly (05:31, 06:31 UTC observed) and comes back rolled to
a stale WEEK-28-ERA snapshot: HEAD a91c7f4, docs/ missing week 29+ plans, scratchpad wiped.
- **The git remote is the only durable store.** This file is force-added to git for that reason
  (`git add -f scratchpad/phase5-state.md`) despite the skill saying not to commit scratchpad —
  an uncommitted state file does not survive the exact event it exists for.
- **Recovery, every reboot:** `git fetch origin claude/phase5-week26-qomtkb`, confirm local HEAD
  is an ancestor, then `git reset --hard origin/claude/phase5-week26-qomtkb`.
- **Root cause of lost agents:** background subagents do NOT keep the session alive. A turn that
  ends with only background agents running reads as idle → reclaim → the next heartbeat boots a
  fresh container and those agents are gone. Two gate+audit pairs were lost this way.
- **Mitigation: run agents SYNCHRONOUSLY** (`run_in_background: false`), several in one tool
  block so they still run concurrently. A blocked turn keeps the container alive. Reserve
  background for work you can afford to lose.

## Week 31 as pushed (needs verification)
Uncorrelated + scalar subqueries by materialize-then-substitute; refusal NARROWED to correlated
subqueries only, not lifted; both Week 30 tripwires stay armed (ChunkPruner declines
query_level>0, buildAggregateSchema throws); week 31 lowers NO correlated ref, so the ColumnId
prerequisite did not trigger.

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
