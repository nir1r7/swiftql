#include <gtest/gtest.h>
#include "planner/cardinality_estimator.h"
#include "planner/logical_plan.h"
#include "planner/binder.h"
#include "parser/parser.h"
#include "catalog/catalog.h"
#include "common/value.h"
#include <memory>
#include <string>
#include <vector>

// Tests run from build/, so the catalog is one level up.
static const char* CATALOG = "../tests/data/test_catalog.json";

// Parse + bind + build the logical plan for a query against the test catalog.
static std::unique_ptr<LogicalPlanNode> buildLogical(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    return LogicalPlanBuilder::build(std::move(stmt), cat);
}

// Walk down children[0] to the first node of a given type, or nullptr.
static const LogicalPlanNode* findNode(const LogicalPlanNode* root, LogicalNodeType t) {
    const LogicalPlanNode* n = root;
    while (n) {
        if (n->type == t) return n;
        n = n->children.empty() ? nullptr : n->children[0].get();
    }
    return nullptr;
}

// Recursively collect every node (both join branches), for propagation checks.
static void collectAllNodes(const LogicalPlanNode* node, std::vector<const LogicalPlanNode*>& out) {
    if (!node) return;
    out.push_back(node);
    for (const auto& child : node->children) collectAllNodes(child.get(), out);
}

// Deterministic stats so assertions below are exact arithmetic, not
// fixture-coupled. Column names match tests/data/test_catalog.json.
// laps: 1000 rows; season NDV 5 range [2020,2024]; speed range [200,400] NDV
// 900, null_count 100; team NDV 10.
static void seedLapsStats(Catalog& cat) {
    TableStats ts;
    ts.row_count = 1000;

    ColumnStats season;
    season.min_val = Value(int64_t(2020));
    season.max_val = Value(int64_t(2024));
    season.distinct_count = 5;
    season.null_count = 0;
    ts.columns.emplace("season", season);

    ColumnStats speed;
    speed.min_val = Value(200.0);
    speed.max_val = Value(400.0);
    speed.distinct_count = 900;
    speed.null_count = 100;
    ts.columns.emplace("speed", speed);

    ColumnStats team;
    team.min_val = Value(std::string("Alpine"));
    team.max_val = Value(std::string("Williams"));
    team.distinct_count = 10;
    team.null_count = 0;
    ts.columns.emplace("team", team);

    ColumnStats driver_id;
    driver_id.min_val = Value(int64_t(1));
    driver_id.max_val = Value(int64_t(20));
    driver_id.distinct_count = 20;
    driver_id.null_count = 0;
    ts.columns.emplace("driver_id", driver_id);

    cat.setStats("laps", std::move(ts));
}

// drivers: 20 rows; driver_id NDV 20 (a primary key).
static void seedDriversStats(Catalog& cat) {
    TableStats ts;
    ts.row_count = 20;

    ColumnStats driver_id;
    driver_id.min_val = Value(int64_t(1));
    driver_id.max_val = Value(int64_t(20));
    driver_id.distinct_count = 20;
    driver_id.null_count = 0;
    ts.columns.emplace("driver_id", driver_id);

    ColumnStats team;
    team.min_val = Value(std::string("Alpine"));
    team.max_val = Value(std::string("Williams"));
    team.distinct_count = 10;
    team.null_count = 0;
    ts.columns.emplace("team", team);

    cat.setStats("drivers", std::move(ts));
}

// Estimate for the first FILTER node down the spine.
static double filterEst(const LogicalPlanNode* root) {
    const LogicalPlanNode* f = findNode(root, LogicalNodeType::FILTER);
    return f ? f->estimated_rows : -999.0;
}

// ===== Task 1: scan + fallback =====

TEST(Cardinality, ScanUsesTableRowCount) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(findNode(plan.get(), LogicalNodeType::SCAN)->estimated_rows, 1000.0);
    EXPECT_DOUBLE_EQ(plan->estimated_rows, 1000.0);  // project passes through
}

TEST(Cardinality, MissingStatsFallsBackToDefaultRowCount) {
    Catalog cat(CATALOG);  // no setStats
    auto plan = buildLogical("SELECT team FROM laps", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(findNode(plan.get(), LogicalNodeType::SCAN)->estimated_rows,
                     static_cast<double>(FALLBACK_ROW_COUNT));
}

// ===== Task 2: single-predicate selectivity =====

TEST(Cardinality, EqualityUsesNdv) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE season = 2022", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 200.0);  // 1000 * 1/5
}

TEST(Cardinality, EqualityOutOfRangeIsZero) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE season = 1999", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 0.0);
}

TEST(Cardinality, NotEqualIsComplement) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE season != 2022", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 800.0);  // 1000 * 4/5
}

TEST(Cardinality, RangeInterpolates) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE speed > 300", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 500.0);  // 1000 * (400-300)/(400-200)
}

TEST(Cardinality, RangeClampsBelowMin) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE speed < 200", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 0.0);
}

TEST(Cardinality, RangeAcceptsFlippedOperandOrder) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    // "300 < speed" must equal "speed > 300"
    auto plan = buildLogical("SELECT team FROM laps WHERE 300 < speed", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 500.0);
}

TEST(Cardinality, IsNotNullUsesNullFraction) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE speed IS NOT NULL", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 900.0);  // 1000 * (1 - 100/1000)
}

TEST(Cardinality, EqualityFallbackWithoutStats) {
    Catalog cat(CATALOG);  // no laps stats
    auto plan = buildLogical("SELECT team FROM laps WHERE team = 'Ferrari'", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 100.0);  // 1000-fallback * 0.1
}

TEST(Cardinality, FilterNeverExceedsChild) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE speed > 300", cat);
    CardinalityEstimator::estimate(*plan, cat);
    const LogicalPlanNode* f = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(f, nullptr);
    EXPECT_LE(f->estimated_rows, f->children[0]->estimated_rows);
}

// ===== Task 3: conjunction / disjunction =====

TEST(Cardinality, ConjunctionMultipliesSelectivities) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE season = 2022 AND speed > 300", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 100.0);  // 1000 * 0.2 * 0.5
}

TEST(Cardinality, DisjunctionUsesInclusionExclusion) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE season = 2022 OR speed > 300", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 600.0);  // 1000 * (0.2 + 0.5 - 0.1)
}

// ===== Task 4: join =====

TEST(Cardinality, JoinDividesByMaxNdv) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);
    auto plan = buildLogical(
        "SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    CardinalityEstimator::estimate(*plan, cat);
    const LogicalPlanNode* j = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(j, nullptr);
    EXPECT_DOUBLE_EQ(j->estimated_rows, 1000.0);  // 1000 * 20 / max(20,20)
    // both children were estimated (both-children recursion pinned here)
    EXPECT_GT(j->children[0]->estimated_rows, 0.0);
    EXPECT_GT(j->children[1]->estimated_rows, 0.0);
}

TEST(Cardinality, JoinFallsBackToMaxWithoutStats) {
    Catalog cat(CATALOG);  // no stats on either table
    auto plan = buildLogical(
        "SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    CardinalityEstimator::estimate(*plan, cat);
    const LogicalPlanNode* j = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(j, nullptr);
    EXPECT_DOUBLE_EQ(j->estimated_rows,
                     static_cast<double>(FALLBACK_ROW_COUNT));  // max(1000,1000)
}

// ===== Task 5: aggregate + HAVING =====

TEST(Cardinality, GlobalAggregateIsOneRow) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT COUNT(*) FROM laps", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(findNode(plan.get(), LogicalNodeType::AGGREGATE)->estimated_rows, 1.0);
}

TEST(Cardinality, GroupByUsesNdvClampedToChild) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team, COUNT(*) FROM laps GROUP BY team", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(findNode(plan.get(), LogicalNodeType::AGGREGATE)->estimated_rows,
                     10.0);  // min(10, 1000)
}

TEST(Cardinality, HavingEstimatesWithoutCrashOverAggregate) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    // HAVING predicate references COUNT(*), which has no base-table stats:
    // must hit the fallback path, not crash.
    auto plan = buildLogical(
        "SELECT team, COUNT(*) FROM laps GROUP BY team HAVING COUNT(*) > 5", cat);
    CardinalityEstimator::estimate(*plan, cat);
    // topmost FILTER is the HAVING filter, over a 10-group aggregate
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 5.0);  // 10 * FALLBACK_SELECTIVITY (0.5)
}

// ===== Task 6: end-to-end propagation checkpoint =====

TEST(Cardinality, EveryNodeEstimatedOnFullQuery) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);
    auto plan = buildLogical(
        "SELECT laps.team, COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id "
        "WHERE season = 2022 GROUP BY laps.team HAVING COUNT(*) > 5 ORDER BY laps.team LIMIT 3",
        cat);
    CardinalityEstimator::estimate(*plan, cat);

    std::vector<const LogicalPlanNode*> all;
    collectAllNodes(plan.get(), all);
    ASSERT_FALSE(all.empty());
    for (const LogicalPlanNode* n : all) {
        EXPECT_GE(n->estimated_rows, 0.0)
            << "unestimated node type " << static_cast<int>(n->type);
    }
}
