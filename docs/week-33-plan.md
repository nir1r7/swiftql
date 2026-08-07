# Week 33 — Correlated Subqueries

**Checkpoint (README):** Required correlated TPC-H queries execute correctly.
Decorrelate the correlated patterns required by TPC-H; retain a correct fallback
for unsupported patterns.

**The shape of the week.** Week 33 removes `Validator::validate`'s
`has_correlated_subquery` refusal, which means a `ColumnRef` with
`query_level > 0` reaches a plan node for the first time in the project's life.
The README binds a deferred structural change to exactly that trigger, so this
week is two weeks stacked: a standalone mechanical refactor that must land and
verify on its own, and only then the correlation feature.

## Tasks

1. **`ColumnId { level, slot }` — the standalone prerequisite refactor.**
   Its own commit(s), suite green on both sides, no feature work in the diff.
2. **Remove the refusal; disarm the two Week 30 tripwires by replacing them.**
   `ChunkPruner::collectSimplePredicates` and `buildAggregateSchema`.
3. **Decorrelate `EXISTS` / `NOT EXISTS` into the Week 32 semi/anti join.**
   No new operator — only the rewrite that produces the join keys.
4. **Decorrelate correlated scalar subqueries (Q17-shape) into a group-by join.**
5. **The correct fallback for everything not decorrelated.**
   A refusal, message-pinned, plus the harness consequence.
6. **Volcano semi/anti parity — the route back to a four-mode oracle.**
7. **The three surfaces no audit reached.**
   `refuseUnloweredIn` call sites, Volcano `HashJoinNode` refusal totality,
   `setCostDecision`'s consumption of `rowWidth`.
8. **Precise `collectSlots` / `restampSlots` and `buildScanSchema` narrowing.**
9. **Tests, oracle suites, and the changed-test discipline.**

Sections follow in that order.
