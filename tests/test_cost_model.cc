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
