# Seam audit — fix round 1, implementer's closing report

HEAD at completion: `ee9c9d7`. Four fix commits: 87c08a2, 17bfcea, 8a23b9d, 18af84f.

## What each fix actually did

1. **Cross-engine wrong answer** — `src/planner/plan_nodes.cc`. `HashAggregateNode::open` now
   records first-encounter order in a vector and materializes from that, instead of iterating the
   accumulator `unordered_map`. Reproduced pre-fix:
   `SELECT team, MIN(season) FROM laps GROUP BY team ORDER BY MIN(season) LIMIT 3`
   returned `Williams/Ferrari/RedBull` on Volcano and `AlphaTauri/Alpine/McLaren` vectorized.
   After: all four modes agree.

2. **Missing `DERIVED` case** — `src/planner/cardinality_estimator.cc`. Recurses into the body,
   adopts its row count, returns an empty context. `--explain-analyze` on the audit's TPC-H query
   now shows est==actual on every node (derived 1000/1000, agg 1000/1000); the fabricated
   `cost=1525 (written=4216)` becomes `6543 (written=7409)`; both physical decisions return
   (`build=derived cost=3516`, `algo=simd`). New test
   `Cardinality.DerivedRelationAdoptsItsBodyRowCount`, confirmed failing pre-fix with five `-1 vs 0`.

3. **F1+F2 coupled pair** — `src/planner/subquery_decorrelation.cc`. `$scalarN`'s group-key columns
   are renamed `$k0..$k{n-1}` (relation-level rename, body untouched); `JoinKey::join_col` points at
   them. Collision impossible by construction, so F1's refusal disappears **without** relaxing the
   duplicate check, and F2's bare-name lookup stops depending on it. Two oracle entries added: the
   formerly-refused query, plus one whose answer changes 20→0 if a key is dropped — so a
   dedup-only "fix" could not have passed.

4. **The four lows** — `vectorized_plan_builder.cc` forwards `on_residual` (test plants the state by
   hand, confirmed failing pre-fix); the semi/anti stamp in `cardinality_estimator.cc` goes through
   `flooredJoinCardinality`; the false `max(l,r)` / written-floor paragraph in `join_enumeration.cc`
   is replaced with what is true; `hasSlotOutsideRangeTable` → `slotDeclineReason`, so
   `FROM a JOIN b JOIN c WHERE x IN (…)` prints `join-ordering=skipped (semi/anti join)` instead of
   nothing.

## Found but NOT fixed — carry into pass 2

- **A stale test that passed for the wrong reason.**
  `JoinEnumeration.DeclinesASemiJoinTreeAsSlotOutsideTheRangeTable` used a 2-relation spine while
  claiming "three SCANs … past the <3-relation guard". `countRelations` has counted the *spine*
  since Week 34, so the query returned at the `<3` guard and never reached the decline it was named
  for. This one was fixed. **Assume there are more comments describing pre-Week-34 `countRelations`
  behaviour** — this is the codebase's recurring "code trusting a refusal that was deleted" class.
- **`VecDerivedNode` re-entry** — still open. `nextChunk` forwards its child's chunk pointer, so a
  derived table on both sides of a self-join would forward the same `DataChunk*` twice. Not traced
  to a reachable plan.
- **`joinCardinality`'s `have_ndv` from either side** — now *documented* as the reason the
  multiplicative branch runs with one stats-less input; the rule itself is unchanged. A modelling
  choice, not a defect; changing it would move plans.

## Where a fix was narrower than the finding

- **Item 1's regression test is not in `QUERIES`.** With a tie at the cut, every choice among the
  tied rows is a legal SQL answer — SQLite returned a third one (`AlphaTauri/Alpine/Ferrari`) — so
  SQLite cannot adjudicate it and a `QUERIES` entry would assert a non-guarantee. Instead: a new
  suite `ENGINE_AGREEMENT_QUERIES` + `run_engine_agreement_suite`, comparing the four SwiftQL modes
  **against each other**, ordered and unsorted. That is the property the fix actually establishes.
  Flagged explicitly because the instruction asked for an oracle entry and this is a different kind.
- **Item 2 returns an empty `StatsContext`**, not the re-stamped one the join-chain audit suggested.
  `PROJECT` returns its *child's* context unchanged, so entries are named by the body's inputs, not
  its outputs, while the relation's columns are the select list after optional aliasing — no mapping
  is computable at that point, and a bare-name match across the boundary would attribute one
  column's NDV to another. "No statistics" is honest and `joinCardinality` already models it.
  Cost: a derived side still supplies no key NDV.
- **F1 was not fixed by deduplicating keys** as the audit's minimal suggestion had it; dedup would
  still need both join keys and would have left F2 resting on the duplicate check. The rename closes
  both at once.

Implementer's claim: nothing here trades a result guarantee for plan quality. Items 2, 3(F2) and all
of 4 left results unaffected; item 1 strictly removes a divergence. **Unverified until the gate
reports** — this file is the implementer's account, not a measurement.
