#pragma once

#include "catalog/catalog.h"
#include "planner/logical_plan.h"
#include <memory>

// Week 28 — left-deep join ordering.
//
// Pipeline position is forced at both ends:
//   AFTER  PredicatePushdown — each relation's local filters must already sit on
//          its own scan, or every leaf costs at its raw table size and the search
//          orders on base cardinalities instead of filtered ones;
//   BEFORE CardinalityEstimator::estimate — which stamps the tree --explain
//          prints and VectorizedPlanBuilder costs, so it has to see the final
//          shape.
//
// Left-deep only. LogicalJoin::children[1] is always exactly one relation, an
// invariant rightKeyIndices(), rowWidth()'s isSingleRelation() split and the
// whole lowering path are written against.
//
// Four documented approximations, all deliberate:
//   1. The search costs the HASH join only, never the SIMD loop join. SIMD
//      eligibility depends on key count and key type, and an ordering can change
//      a join's key count. Modelling a per-join decision lowering will re-make
//      anyway buys accuracy only where the build side is under ~50 rows — where
//      the absolute cost is negligible and cannot flip an ordering.
//   2. Independence. joinCardinality divides by the NDV product and filters do
//      not narrow column statistics, so errors compound multiplicatively along a
//      spine. Standard System-R; histograms are a README "Possible Extension".
//   3. The DP is exact only where every join key has statistics. Keeping one
//      subplan per subset is sound because rows(S) is the pure product
//      ∏rows / ∏ndv, a function of the SET — which is why the ≥1 floor had to
//      move to the stamping sites (flooredJoinCardinality). joinCardinality's
//      no-statistics branch falls back to max(l, r), which is NOT multiplicative,
//      so a subset containing a stats-less relation has an order-dependent row
//      count and optimal substructure does not hold for it. There is no
//      path-independent estimate to fall back to instead, so the containment is
//      (4) rather than a better formula.
//      Note also that `cost=` and `est=` on the same --explain line are NOT
//      reconcilable, deliberately: the search chains the RAW product from
//      joinCardinality, while every est= on the tree is stamped through
//      flooredJoinCardinality. A sub-1-row intermediate is SEARCHED at 0.9 and
//      PRINTED as est=1, so cost= cannot be re-derived from any est= on the tree.
//      The pass is not inconsistent; a test checking one against the other is
//      testing the wrong thing.
//   4. The search may only IMPROVE on the written order. reorder() costs the
//      written order too and keeps it when the search's pick scores worse, which
//      bounds (3) and any future cost-model change: the plan is never worse than
//      what the user wrote by the model's own metric. `method=` names which of
//      dp / greedy / written-floor / written-fallback produced the printed order.
//   5. Outer joins are not reordered AT ALL. R ⟕ S ≠ S ⟕ R and associativity
//      fails, so any tree containing one is declined whole (containsOuterJoin,
//      Week 29). --explain reports `join-ordering=skipped (outer join)` and no
//      order= line: a decision was available and was refused (unlike the two
//      declines below, where there is none to make), and one outer join costs the
//      query's inner block its ordering too. The outer-join cardinality rule —
//      max(selectivity(ON residual) * matches, left_rows) — lives at the STAMPING
//      site for the same reason the ≥1 floor does: neither term is multiplicative,
//      so the search must never meet it.
//
// Above 32 relations the pass declines entirely (uint32_t subset masks) and
// --explain shows no order= line, because there is no decision to show. No TPC-H
// query comes close — Q9 and Q21 top out at 6 relations.
//
// It also declines, silently, any tree carrying a relation slot outside the
// range table (hasSlotOutsideRangeTable, Week 30). What reaches it, as of Week
// 34, is exactly two things: an unbound key (from_slot -1), and a SEMI/ANTI
// join, whose join_slot is -1 because children[1] is a subquery BODY and not a
// relation of this block.
//
// !! DERIVED TABLES DO NOT REACH IT, contrary to what Weeks 28, 29, 30 and 31
// each predicted for "the week a nested scan genuinely joins the outer one".
// They are ordinary relations of this block's range table with in-range slots:
// decompose takes a LogicalDerived as a leaf, rebuild re-merges its schema, and
// the search reorders it like any other relation (verified at three relations,
// method=dp, optimized == --no-optimize). What was actually wrong was
// countRelations, which counted SCANS and so over-counted a derived body's —
// making `slot >= n` too permissive rather than too strict. It counts the spine
// now. No reported decline was added because no supported query pays a
// plan-quality cost, which is the condition Week 30 set for earning one.
//
// What IS new, and is a different consumer: a derived relation has no
// TableStats, so joinCardinality's non-multiplicative max(l, r) branch runs on a
// query the CLI can type. Optimal substructure does not hold for a subset
// containing one; the containment is the written-order bound in reorder(),
// exactly as Week 28 specified, and this is the first time that bound is
// load-bearing outside a C++ fixture. Week 28 recorded that method=written-floor
// had never executed — it is reachable now.
//
// This USED to throw, which made it the one place the optimized path could fail
// on input `--no-optimize` accepts; declining keeps optimized == --no-optimize,
// which is what makes the fourth harness mode a differential oracle rather than
// a duplicate run.

// No-op below three relations: with one join there is no ordering decision.
// Which side builds is Week 22's decision, made at lowering from the same
// estimates, and reordering two relations would change merged-schema column
// order and every Week 22 / 23.5 steering assertion for zero modelled gain.
constexpr int MIN_ENUMERATED_RELATIONS = 3;

// Search-space cap. Left-deep DP is O(2^N * N): N=10 is ~10k transitions
// (microseconds), N=20 is ~20M (visible in the plan timer, for a shape no TPC-H
// query has — Q9 and Q21 top out at 6 relations). Above the cap, greedy. A named
// constant rather than a CLI flag: nothing needs to vary it per query, and a
// flag would be a knob with no consumer.
constexpr int MAX_DP_RELATIONS = 10;

class JoinEnumeration {
    public:
        // Reorders the single join tree inside `node`, if any, and returns the
        // tree. Never called under --no-optimize (see main.cc): the written
        // order is the benchmark baseline AND the differential oracle.
        static std::unique_ptr<LogicalPlanNode> apply(std::unique_ptr<LogicalPlanNode> node,
                                                      const Catalog& catalog);
};
