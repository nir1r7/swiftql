#include <gtest/gtest.h>
#include <algorithm>
#include "planner/cost_model.h"

// ===== Build-side rule as arithmetic =====

// The smaller input must be the cheaper build side: building 10 rows and
// probing with 1000 costs less than building 1000 and probing with 10. This is
// the whole justification for putting the smaller (filtered) side on build.
TEST(CostModel, SmallerBuildSideIsCheaper) {
    double small_builds = hashJoinCost(/*build=*/10,   /*width=*/16, /*probe=*/1000);
    double large_builds = hashJoinCost(/*build=*/1000, /*width=*/16, /*probe=*/10);
    EXPECT_LT(small_builds, large_builds);
}

// With equal widths the comparison reduces to pure row count — the builder's
// --no-optimize fallback pins widths to a uniform constant for exactly this
// reason, so it reproduces the pre-Week-22 row-count heuristic.
TEST(CostModel, EqualWidthReducesToRowCount) {
    // from=200, join=20: join is smaller, so building join is cheaper
    double from_builds = hashJoinCost(200, 8, 20);
    double join_builds = hashJoinCost(20, 8, 200);
    EXPECT_GT(from_builds, join_builds);
}

// ===== Degenerate inputs =====

// Estimates can be 0 (empty filtered side) — costs must stay finite and
// non-negative, never divide or blow up.
TEST(CostModel, ZeroRowsAreFiniteAndNonNegative) {
    EXPECT_GE(hashJoinCost(0, 16, 0), 0.0);
    EXPECT_GE(hashJoinCost(0, 16, 1000), 0.0);
    EXPECT_DOUBLE_EQ(hashJoinCost(0, 16, 0), 0.0);   // no rows either side, no work
}

// ===== Week 23.5: SIMD small-build loop join vs hash join =====

// A tiny build side is the loop join's whole reason to exist: with a handful
// of build keys the flat SIMD scan must cost less than hashing every probe key.
// The crossover the model expresses must be the crossover the CONSTANT encodes,
// so this test derives the boundary from CPU_SIMD_COMPARE instead of hardcoding a
// build size. It used to assert that a 4-row build side favours the loop join,
// which was true against the Week 23.5 hash join (measured crossover 52-57 rows)
// and became FALSE in Week 37, when VecHashJoinNode was rewritten and the
// re-measured crossover fell below 2 rows. A hardcoded size makes the test a
// record of one calibration; deriving it makes the test track the calibration.
TEST(CostModel, SimdLoopWinsOnlyBelowTheCalibratedCrossover) {
    // For a large probe the modelled crossover is B* ~ 1 / CPU_SIMD_COMPARE.
    const double b_star = 1.0 / CPU_SIMD_COMPARE;
    const double probe = 100000;

    // Strictly below B*, the loop join must win.
    const double below = std::max(1.0, b_star - 1.0);
    EXPECT_LT(simdLoopJoinCost(below, 16, probe), hashJoinCost(below, 16, probe));

    // Comfortably above it, the quadratic probe term must price it out.
    const double above = b_star * 4.0;
    EXPECT_GT(simdLoopJoinCost(above, 16, probe), hashJoinCost(above, 16, probe));
}

// The probe term is quadratic (every probe row scans every build key), so a
// large build side must price the loop join out — this is the "small build
// only" restriction expressed as arithmetic, not as a hard row cap.
TEST(CostModel, HashWinsOnLargeBuild) {
    EXPECT_GT(simdLoopJoinCost(100000, 16, 100000), hashJoinCost(100000, 16, 100000));
}

// The compare work is build_rows * probe_rows: doubling the probe side must
// double the probe-dependent share of the cost exactly.
TEST(CostModel, SimdProbeTermIsQuadratic) {
    double base   = simdLoopJoinCost(64, 16, 0);
    double p_gap  = simdLoopJoinCost(64, 16, 1000)  - base;
    double p2_gap = simdLoopJoinCost(64, 16, 2000)  - base;
    EXPECT_GT(p_gap, 0.0);
    EXPECT_DOUBLE_EQ(p2_gap, 2.0 * p_gap);
}

// Same degenerate-input contract as hashJoinCost.
TEST(CostModel, SimdZeroRowsAreFiniteAndNonNegative) {
    EXPECT_GE(simdLoopJoinCost(0, 16, 1000), 0.0);
    EXPECT_DOUBLE_EQ(simdLoopJoinCost(0, 16, 0), 0.0);
}

// Widths DO decide at equal row counts — which is exactly why the
// --no-optimize fallback must feed uniform widths into the comparison to
// reproduce the pure row-count heuristic (see VectorizedPlanBuilder).
TEST(CostModel, UnequalWidthBreaksRowCountEquivalence) {
    EXPECT_NE(hashJoinCost(100, 8, 100), hashJoinCost(100, 800, 100));
}

// ===== Week 28: the data-volume (bytes-materialized) term =====

// The reason joinOutputCost lives OUTSIDE hashJoinCost/simdLoopJoinCost: output
// rows and output width do not depend on which input builds, so the term is a
// constant added to both sides of every Week 22 build-side comparison. Folding
// it in would change no decision while inflating every cost= string --explain
// has printed since Week 23 and invalidating CPU_SIMD_COMPARE's measured
// calibration. Pin the property rather than the intention.
TEST(CostModel, OutputCostIsSymmetricUnderBuildSideSwap) {
    const double a_rows = 1000, a_w = 24, b_rows = 20, b_w = 16;
    const double out_rows = 1000, out_w = a_w + b_w;

    double a_builds = hashJoinCost(a_rows, a_w, b_rows);
    double b_builds = hashJoinCost(b_rows, b_w, a_rows);
    double a_total  = a_builds + joinOutputCost(out_rows, out_w);
    double b_total  = b_builds + joinOutputCost(out_rows, out_w);

    // the side decision is untouched: the same delta, the same winner
    EXPECT_DOUBLE_EQ(a_total - b_total, a_builds - b_builds);
    EXPECT_EQ(a_total < b_total, a_builds < b_builds);
}

// The term exists to make a WIDE intermediate cost more than a narrow one at
// equal cardinality — the discrimination Week 22 deferred to Week 28 because
// only differing join orderings can produce it.
TEST(CostModel, OutputCostGrowsWithWidthAndRows) {
    EXPECT_LT(joinOutputCost(1000, 16), joinOutputCost(1000, 64));
    EXPECT_LT(joinOutputCost(1000, 16), joinOutputCost(4000, 16));
    EXPECT_DOUBLE_EQ(joinOutputCost(1000, 40), 1000.0 * 40.0 * CPU_MATERIALIZE_BYTE);
}

// Derivation check: one ~40-byte output row is anchored to cost the same as one
// probe (CPU_HASH_PROBE). If the constant is retuned, this is the statement in
// the header that has to be retuned with it.
TEST(CostModel, OutputCostAnchorsAFortyByteRowToOneProbe) {
    EXPECT_DOUBLE_EQ(joinOutputCost(1, 40), CPU_HASH_PROBE);
}

// Same degenerate-input contract as the two algorithm costs.
TEST(CostModel, OutputCostZeroRowsAreFiniteAndNonNegative) {
    EXPECT_GE(joinOutputCost(0, 40), 0.0);
    EXPECT_DOUBLE_EQ(joinOutputCost(0, 0), 0.0);
    EXPECT_GE(joinOutputCost(-5, 40), 0.0);
}
