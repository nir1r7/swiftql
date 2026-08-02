#include <gtest/gtest.h>
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

// With equal widths the comparison reduces to pure row count, which is exactly
// the pre-Week-22 heuristic the --no-optimize path must still reproduce.
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
TEST(CostModel, SimdLoopWinsOnTinyBuild) {
    EXPECT_LT(simdLoopJoinCost(4, 16, 100000), hashJoinCost(4, 16, 100000));
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
