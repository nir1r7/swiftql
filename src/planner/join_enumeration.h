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
//      fails, so any SPINE containing one is declined whole (containsOuterJoin,
//      Week 29). --explain reports `join-ordering=skipped (outer join)` and no
//      order= line: a decision was available and was refused (unlike the two
//      declines below, where there is none to make), and one outer join costs the
//      query's inner block its ordering too. The outer-join cardinality rule —
//      max(selectivity(ON residual) * matches, left_rows) — lives at the STAMPING
//      site for the same reason the ≥1 floor does: neither term is multiplicative,
//      so the search must never meet it.
//      SPINE, not "tree", since Week 37 (seam audit pass 2, B-2). containsOuterJoin
//      used to walk every child, so it answered about a DIFFERENT query block: a
//      LEFT JOIN sealed inside a DERIVED body or inside a semi/anti join's
//      subquery BODY declined the enclosing fully-inner spine, and reported
//      `(outer join)` for trees whose real and stronger cause is
//      `(semi/anti join)`. It uses countRelations' containment rule now — a
//      derived relation is an opaque leaf, a semi/anti join's children[1] is not
//      a relation of this block — because it is the same rule, and two spellings
//      of it is how the two drifted.
//
// Above 32 relations the pass declines entirely (uint32_t subset masks) and
// --explain shows no order= line, because there is no decision to show. No TPC-H
// query comes close — Q9 and Q21 top out at 6 relations.
//
// It also declines any tree carrying a relation slot outside the range table
// (slotDeclineReason — the function was called hasSlotOutsideRangeTable when this
// paragraph was written, and returns a REASON rather than a bool since the phase
// 5 seam audit). What reaches it, as of Week 34, is exactly two things: an
// unbound key (from_slot -1), and a SEMI/ANTI join, whose join_slot is -1 because
// children[1] is a subquery BODY and not a relation of this block. NOT SILENT
// since pass 1 of that audit: the semi/anti cause is reported as
// `join-ordering=skipped (semi/anti join)`, on the argument that a fully inner
// three-relation spine below a semi join is a decision that WAS available.
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
// !! THE PARAGRAPH THAT STOOD HERE WAS FALSE IN BOTH HALVES, and it is the exact
// text `18af84f` deleted from join_enumeration.cc — swept in the .cc and left
// standing in the HEADER, which is the copy a reader consults first. (Third
// instance of that failure this phase: development.md:855 carries it too, and
// constant_folding.h carried the retracted folding claim.) It said a derived
// relation's missing TableStats make joinCardinality's non-multiplicative
// max(l, r) branch run on a CLI-typable query, and that `method=written-floor`
// was therefore reachable. The seam audit measured both and NEITHER happens:
// `have_ndv` is set when EITHER side supplies an NDV and the non-derived side
// always does, so the MULTIPLICATIVE branch runs; and the DP won outright on
// both queries tested.
//
// What is true is narrower and worth keeping: a derived relation contributes no
// NDV, so a subset containing one is priced from the other side's statistics
// alone. max(l, r) still runs when NEITHER side has any — it is not
// multiplicative, so such a subset has an order-dependent row count and optimal
// substructure does not hold for it. The containment is unchanged and is the
// written-order bound in reorder().
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
        // Reorders EVERY join tree inside `node` and returns it. Never called
        // under --no-optimize (see main.cc): the written order is the benchmark
        // baseline AND the differential oracle.
        //
        // "Every", not "the single one", since Week 37 (seam audit pass 2, B-3).
        // This used to stop at the topmost JOIN, on a comment that named its own
        // expiry — "there is exactly one per statement until subqueries arrive in
        // Week 30" — and was never swept when subqueries (W30), semi joins (W32)
        // and derived tables (W34) each added another. A body's joins were
        // enumerated only when the ENCLOSING block happened to have no join.
        // The descent steps OVER each spine and into its LEAVES, because
        // decompose only accepts a WRITTEN-ORDER tree and a reordered spine is
        // not decomposable.
        static std::unique_ptr<LogicalPlanNode> apply(std::unique_ptr<LogicalPlanNode> node,
                                                      const Catalog& catalog);
};
