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
// 900, null_count 100; team NDV 10. driver_id NDV is parameterized so join
// tests can pick a value where (l*r)/ndv differs from the max(l,r) fallback.
static void seedLapsStats(Catalog& cat, int64_t driver_id_ndv = 20) {
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
    driver_id.distinct_count = driver_id_ndv;
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

TEST(Cardinality, EqualityOutOfRangeFloorsToOneRow) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE season = 1999", cat);
    CardinalityEstimator::estimate(*plan, cat);
    // selectivity stays a true 0 (out of [min,max]); the stamped estimate is
    // floored to one row so downstream join costing never sees a free input
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 1.0);
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

TEST(Cardinality, RangeClampsBelowMinFloorsToOneRow) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE speed < 200", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 1.0);  // 0-selectivity, ≥1-row floor
}

TEST(Cardinality, RangeAtMaxEdgeIsNotZero) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    // speed max is 400 and min/max come from real rows, so ">= max" is
    // guaranteed to match: it gets the equality mass 1/ndv, not 0
    auto plan = buildLogical("SELECT team FROM laps WHERE speed >= 400", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 1000.0 / 900.0);
}

TEST(Cardinality, RangeAtMinEdgeUsesEqMass) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildLogical("SELECT team FROM laps WHERE speed <= 200", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 1000.0 / 900.0);
}

// laps stats where season is a single-value column (min == max == 2022, NDV 1).
static void seedConstantSeasonStats(Catalog& cat) {
    TableStats ts;
    ts.row_count = 1000;
    ColumnStats season;
    season.min_val = Value(int64_t(2022));
    season.max_val = Value(int64_t(2022));
    season.distinct_count = 1;
    season.null_count = 0;
    ts.columns.emplace("season", season);
    cat.setStats("laps", std::move(ts));
}

TEST(Cardinality, SingleValueColumnRangeIsDecidable) {
    // min == max makes every range predicate decidable from stats: all rows
    // or none (floored to 1), never the blind 1/3 fallback
    struct Case { const char* sql; double expected; };
    const Case cases[] = {
        {"SELECT season FROM laps WHERE season <= 2022", 1000.0},
        {"SELECT season FROM laps WHERE season >= 2022", 1000.0},
        {"SELECT season FROM laps WHERE season < 2022", 1.0},
        {"SELECT season FROM laps WHERE season > 2022", 1.0},
    };
    for (const auto& c : cases) {
        Catalog cat(CATALOG);
        seedConstantSeasonStats(cat);
        auto plan = buildLogical(c.sql, cat);
        CardinalityEstimator::estimate(*plan, cat);
        EXPECT_DOUBLE_EQ(filterEst(plan.get()), c.expected) << c.sql;
    }
}

TEST(Cardinality, RangeAcceptsFlippedOperandOrder) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    // "300 < speed" must equal "speed > 300"
    auto plan = buildLogical("SELECT team FROM laps WHERE 300 < speed", cat);
    CardinalityEstimator::estimate(*plan, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan.get()), 500.0);
}

TEST(Cardinality, IsNullFallbackIsPolarityAware) {
    Catalog cat(CATALOG);  // no stats: both polarities hit the fallback
    auto plan_null = buildLogical("SELECT team FROM laps WHERE speed IS NULL", cat);
    CardinalityEstimator::estimate(*plan_null, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan_null.get()), 100.0);   // 1000 * 0.1

    auto plan_not_null = buildLogical("SELECT team FROM laps WHERE speed IS NOT NULL", cat);
    CardinalityEstimator::estimate(*plan_not_null, cat);
    EXPECT_DOUBLE_EQ(filterEst(plan_not_null.get()), 900.0);  // 1000 * (1 - 0.1)
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
    // laps driver_id NDV 50 makes the formula (1000*20)/50 = 400, a value the
    // max(l,r) = 1000 fallback cannot produce — the two paths stay distinguishable
    seedLapsStats(cat, /*driver_id_ndv=*/50);
    seedDriversStats(cat);
    auto plan = buildLogical(
        "SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    CardinalityEstimator::estimate(*plan, cat);
    const LogicalPlanNode* j = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(j, nullptr);
    EXPECT_DOUBLE_EQ(j->estimated_rows, 400.0);  // 1000 * 20 / max(50,20)
    // both children were estimated (both-children recursion pinned here)
    EXPECT_GT(j->children[0]->estimated_rows, 0.0);
    EXPECT_GT(j->children[1]->estimated_rows, 0.0);
}

TEST(Cardinality, JoinEstimateFloorsToOneRow) {
    Catalog cat(CATALOG);
    // 2-row inputs with key NDV 20: (2*2)/20 = 0.2 — floored to one row
    TableStats laps_ts;
    laps_ts.row_count = 2;
    ColumnStats key;
    key.min_val = Value(int64_t(1));
    key.max_val = Value(int64_t(20));
    key.distinct_count = 20;
    key.null_count = 0;
    laps_ts.columns.emplace("driver_id", key);
    ColumnStats team;
    team.distinct_count = 2;
    laps_ts.columns.emplace("team", team);
    cat.setStats("laps", std::move(laps_ts));

    TableStats drivers_ts;
    drivers_ts.row_count = 2;
    drivers_ts.columns.emplace("driver_id", key);
    cat.setStats("drivers", std::move(drivers_ts));

    auto plan = buildLogical(
        "SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    CardinalityEstimator::estimate(*plan, cat);
    const LogicalPlanNode* j = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(j, nullptr);
    EXPECT_DOUBLE_EQ(j->estimated_rows, 1.0);
}

// An NDV of 1 is a usable statistic, not a missing one: every left row matches
// every right row, so l*r/1 is the exact answer. Testing the product against
// 1.0 instead of tracking "did any key contribute an NDV" sent this to the
// no-statistics fallback and underestimated a constant-key join by the table
// size — the estimate then propagates into every ancestor and into join costs.
TEST(Cardinality, SingleValuedJoinKeyEstimatesTheCrossProduct) {
    Catalog cat(CATALOG);
    seedLapsStats(cat, /*driver_id_ndv=*/1);

    TableStats drivers_ts;
    drivers_ts.row_count = 20;
    ColumnStats key;
    key.min_val = Value(int64_t(7));
    key.max_val = Value(int64_t(7));
    key.distinct_count = 1;
    key.null_count = 0;
    drivers_ts.columns.emplace("driver_id", key);
    cat.setStats("drivers", std::move(drivers_ts));

    auto plan = buildLogical(
        "SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    CardinalityEstimator::estimate(*plan, cat);
    const LogicalPlanNode* j = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(j, nullptr);
    EXPECT_DOUBLE_EQ(j->estimated_rows, 20000.0);   // 1000 * 20 / 1, not max(1000,20)
}

// A join key whose own relation has no statistics must estimate as "no
// statistic", not borrow a same-named column from another relation. A
// stats-less scan contributes no entries at all, so the slot lookup misses —
// and StatsContext::find's bare-name fallback then returns the first `team` it
// sees, which here belongs to `drivers` rather than to `laps`. That is exactly
// the confusion JoinKey::from_slot was introduced to prevent, so the join case
// asks for a slot-exact match.
TEST(Cardinality, JoinKeyOnStatslessRelationDoesNotBorrowAnotherRelationsNdv) {
    Catalog cat(CATALOG);
    seedDriversStats(cat);   // drivers only: team NDV 10, driver_id NDV 20
                             // laps deliberately has NO stats

    auto plan = buildLogical(
        "SELECT d.name FROM drivers d JOIN laps l ON d.driver_id = l.driver_id "
        "JOIN laps l2 ON l.team = l2.team", cat);
    CardinalityEstimator::estimate(*plan, cat);

    // join 1: drivers(20) x laps(fallback 1000) / driver_id NDV 20 = 1000
    const LogicalPlanNode* top = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(top, nullptr);
    ASSERT_EQ(top->children[0]->type, LogicalNodeType::JOIN);
    EXPECT_DOUBLE_EQ(top->children[0]->estimated_rows, 1000.0);

    // join 2 keys on laps.team (slot 1, no stats) — neither side has a usable
    // NDV, so the fallback is max(l, r), not 1000 * 1000 / drivers.team's 10
    EXPECT_DOUBLE_EQ(top->estimated_rows, 1000.0);
}

// The same borrowing hazard one level up the plan: a GROUP BY key whose own
// relation has no statistics must not take its NDV from another relation's
// column of the same name. child_ctx here is the join's MERGED context, so the
// bare-name fallback had `drivers.team` (NDV 10) standing in for `laps.team`.
TEST(Cardinality, GroupKeyOnStatslessRelationDoesNotBorrowAnotherRelationsNdv) {
    Catalog cat(CATALOG);
    seedDriversStats(cat);   // drivers only; laps deliberately unseeded

    auto plan = buildLogical(
        "SELECT l.team, COUNT(*) FROM drivers d JOIN laps l "
        "ON d.driver_id = l.driver_id GROUP BY l.team", cat);
    CardinalityEstimator::estimate(*plan, cat);

    const LogicalPlanNode* agg = findNode(plan.get(), LogicalNodeType::AGGREGATE);
    ASSERT_NE(agg, nullptr);
    // join: 20 * 1000 / 20 = 1000 rows in; laps.team has no NDV, so the group
    // count cannot be reduced below the input — not drivers.team's 10
    EXPECT_DOUBLE_EQ(agg->children[0]->estimated_rows, 1000.0);
    EXPECT_DOUBLE_EQ(agg->estimated_rows, 1000.0);
}

// And in selectivity(), which sees a merged context whenever a WHERE conjunct
// sits above a join (a residual, or any plan estimated without pushdown).
TEST(Cardinality, PredicateOnStatslessRelationDoesNotBorrowAnotherRelationsStats) {
    Catalog cat(CATALOG);
    seedDriversStats(cat);   // drivers only; laps deliberately unseeded

    auto plan = buildLogical(
        "SELECT l.lap_id FROM drivers d JOIN laps l ON d.driver_id = l.driver_id "
        "WHERE l.driver_id = 5", cat);
    CardinalityEstimator::estimate(*plan, cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    // laps.driver_id has no stats, so equality falls back to 0.1 — not to
    // drivers.driver_id's NDV of 20, which would give 0.05 and halve the estimate
    EXPECT_DOUBLE_EQ(filter->estimated_rows, 1000.0 * FALLBACK_EQ_SELECTIVITY);
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

// ===== Duplicate join keys (Week 27) =====

// `ON a.x = b.x AND a.x = b.x` is a legal predicate but two identical JoinKeys.
// The estimator divides by one NDV per key, so the duplicate squared the divisor
// and underestimated the join by a factor of NDV. Deduped in
// classifyJoinCondition, which is why the two forms must now estimate alike.
TEST(Cardinality, DuplicateJoinKeyDoesNotSquareTheNdvDivisor) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);

    auto once  = buildLogical(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id", cat);
    auto twice = buildLogical(
        "SELECT l.team FROM laps l JOIN drivers d "
        "ON l.driver_id = d.driver_id AND l.driver_id = d.driver_id", cat);
    CardinalityEstimator::estimate(*once, cat);
    CardinalityEstimator::estimate(*twice, cat);

    const LogicalPlanNode* j1 = findNode(once.get(), LogicalNodeType::JOIN);
    const LogicalPlanNode* j2 = findNode(twice.get(), LogicalNodeType::JOIN);
    ASSERT_NE(j1, nullptr);
    ASSERT_NE(j2, nullptr);
    EXPECT_DOUBLE_EQ(j1->estimated_rows, j2->estimated_rows);
}

// ===== Week 28: one cardinality rule, shared with join enumeration =====

// joinCardinality was lifted out of estimateNode's JOIN case so the Week 28
// search and the stamped plan cannot hold different models — the search would
// otherwise rank orderings under one arithmetic while --explain printed another,
// and the NDV rule this encodes has already been corrected twice. The anti-drift
// test: estimate a plan, then recompute the same join by hand and demand the two
// agree exactly.
TEST(Cardinality, JoinCardinalityMatchesTheStampedJoinEstimate) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);
    auto plan = buildLogical(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id", cat);
    CardinalityEstimator::estimate(*plan, cat);
    const auto* join = static_cast<const LogicalJoin*>(
        findNode(plan.get(), LogicalNodeType::JOIN));
    ASSERT_NE(join, nullptr);

    StatsContext left  = CardinalityEstimator::estimateSubtree(*join->children[0], cat);
    StatsContext right = CardinalityEstimator::estimateSubtree(*join->children[1], cat);
    EXPECT_DOUBLE_EQ(joinCardinality(join->children[0]->estimated_rows,
                                     join->children[1]->estimated_rows,
                                     join->keys, left, right),
                     join->estimated_rows);
}

// The >=1-row floor is a STAMPING policy, applied by flooredJoinCardinality on
// top of the rule — never inside it. A per-step clamp makes a subset's row count
// depend on the path that reached it, which destroys the optimal substructure the
// Week 28 join DP rests on (see JoinEnumeration.NeverInstallsAnOrderWorseThanThe
// WrittenOne for the plan that measured it).
TEST(Cardinality, TheOneRowFloorBelongsToTheStampNotTheRule) {
    StatsContext empty;
    // no key NDV at all: the FK-like max() fallback, not a cross product
    EXPECT_DOUBLE_EQ(joinCardinality(1000.0, 20.0, {}, empty, empty), 1000.0);
    // the floor fires only when BOTH sides have rows, so a genuinely empty input
    // still reports empty
    EXPECT_DOUBLE_EQ(flooredJoinCardinality(1.0, 1.0, 0.25), 1.0);
    EXPECT_DOUBLE_EQ(flooredJoinCardinality(0.0, 20.0, 0.25), 0.25);
    // and it never lowers a value that already clears the floor
    EXPECT_DOUBLE_EQ(flooredJoinCardinality(1000.0, 20.0, 50.0), 50.0);
}

// Week 28: a leaf's own StatsContext stamps slot 0, because a standalone scan
// has one relation and nothing to disambiguate. Once join enumeration may put a
// relation other than 0 at the bottom of the spine, that stamp is a lie about
// which relation the entries describe, and every later key lookup on it misses —
// have_ndv goes false and the estimate silently degrades. The merge reads the
// truth off the merged schema's first column. Hand-built, because no query can
// produce this shape before the enumeration pass runs.
TEST(Cardinality, LeftmostLeafContextTakesTheMergedSchemaSlot) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);

    // laps sits at the BOTTOM of the spine while carrying binder slot 2
    auto left_scan = std::make_unique<LogicalScan>(
        "laps", Schema({ColumnDef{"driver_id", TypeId::INT, 0, false}}));
    auto right_scan = std::make_unique<LogicalScan>(
        "drivers", Schema({ColumnDef{"driver_id", TypeId::INT, 0, false}}));
    Schema merged({ColumnDef{"driver_id", TypeId::INT, 2, false},
                   ColumnDef{"driver_id", TypeId::INT, 1, false}});
    // bottom join: from_slot 0 addresses the leaf's own schema (see JoinKey)
    std::vector<JoinKey> keys{JoinKey{"driver_id", "driver_id", 0}};
    auto join = std::make_unique<LogicalJoin>(std::move(left_scan), std::move(right_scan),
                                              std::move(keys), 1, merged);

    StatsContext out = CardinalityEstimator::estimateSubtree(*join, cat);

    // laps' entry is addressable at slot 2, and it really is laps' (1000-row
    // table) rather than drivers' 20-row one
    const ColumnStatsEntry* l = out.findExact("driver_id", 2);
    ASSERT_NE(l, nullptr);
    EXPECT_EQ(l->table_rows, 1000);
    const ColumnStatsEntry* r = out.findExact("driver_id", 1);
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->table_rows, 20);
    // and the key lookup that produced the estimate still hit: 1000*20/20
    EXPECT_DOUBLE_EQ(join->estimated_rows, 1000.0);
}

// ===== Week 32: the semi/anti rule =====

// Hand-built, for the reason LeftmostLeafContextTakesTheMergedSchemaSlot is:
// the lowering pass wraps the body in a LogicalProject whose StatsContext is
// empty, so no query this engine can write reaches the branch with a right-side
// NDV in hand. The rule falls back to frac = 1.0 there — conservative and
// invariant-preserving, but it means the arithmetic below is exercised by
// nothing else in the suite.
static std::unique_ptr<LogicalJoin> semiJoinOverLapsAndDrivers(JoinSemantics sem) {
    auto left = std::make_unique<LogicalScan>(
        "laps", Schema({ColumnDef{"driver_id", TypeId::INT, 0, false}}));
    auto right = std::make_unique<LogicalScan>(
        "drivers", Schema({ColumnDef{"driver_id", TypeId::INT, 0, false}}));
    // !! output_schema IS the left child's, NOT a merged schema. That is the
    // containment keeping the body's slot numbering out of the outer plan, and
    // it is what makes `rows <= left` a statement about the same relation.
    Schema out = left->output_schema;
    std::vector<JoinKey> keys{JoinKey{"driver_id", "driver_id", 0}};
    // join_slot -1: children[1] is not a relation of this block's range table.
    auto join = std::make_unique<LogicalJoin>(std::move(left), std::move(right),
                                              std::move(keys), -1, std::move(out));
    join->semantics = sem;
    return join;
}

// drivers with 20 rows but only 10 distinct driver_ids. The gap between the
// right side's ROW COUNT and its NDV is what separates the semi rule from the
// product form: with rows == NDV (seedDriversStats) the two agree numerically
// and the test would pass against the wrong rule.
static void seedDriversStatsWithDuplicateKeys(Catalog& cat) {
    TableStats ts;
    ts.row_count = 20;
    ColumnStats driver_id;
    driver_id.min_val = Value(int64_t(1));
    driver_id.max_val = Value(int64_t(10));
    driver_id.distinct_count = 10;
    driver_id.null_count = 0;
    ts.columns.emplace("driver_id", driver_id);
    cat.setStats("drivers", std::move(ts));
}

// The two NDVs are read SEPARATELY, not max()'d: the semi rule needs the ratio,
// so a shared lookup collapsing them to one number cannot express it. The right
// side contributes ONLY its NDV — never its row count, which is exactly why the
// product form is the wrong shape and is overwritten rather than adjusted.
//
// laps.driver_id NDV 50, drivers.driver_id NDV 10 over 20 rows:
//   semi    = 1000 * min(1, 10/50) = 200
//   product = 1000 * 20 / max(50,10) = 400   <- what joinCardinality returns
TEST(Cardinality, SemiJoinIsALeftSideSelectivityFromTheNdvRatio) {
    Catalog cat(CATALOG);
    seedLapsStats(cat, /*driver_id_ndv=*/50);
    seedDriversStatsWithDuplicateKeys(cat);
    auto join = semiJoinOverLapsAndDrivers(JoinSemantics::SEMI);
    CardinalityEstimator::estimateSubtree(*join, cat);
    EXPECT_DOUBLE_EQ(join->estimated_rows, 200.0);
    EXPECT_NE(join->estimated_rows, 400.0);   // the product form, pinned by name
    EXPECT_LE(join->estimated_rows, join->children[0]->estimated_rows);
}

// Semi + anti = the left side exactly. The two are complements by construction
// (R ▷ S is R − (R ⋉ S)), so any drift between them is a rule that stopped
// being a partition of the left input.
TEST(Cardinality, SemiAndAntiEstimatesPartitionTheLeftSide) {
    Catalog cat(CATALOG);
    seedLapsStats(cat, /*driver_id_ndv=*/50);
    seedDriversStats(cat);
    auto semi = semiJoinOverLapsAndDrivers(JoinSemantics::SEMI);
    auto anti = semiJoinOverLapsAndDrivers(JoinSemantics::ANTI);
    CardinalityEstimator::estimateSubtree(*semi, cat);
    CardinalityEstimator::estimateSubtree(*anti, cat);
    EXPECT_DOUBLE_EQ(anti->estimated_rows, 600.0);
    EXPECT_DOUBLE_EQ(semi->estimated_rows + anti->estimated_rows,
                     semi->children[0]->estimated_rows);
}

// The clamp is not decoration. With more distinct values on the right than the
// left, the ratio exceeds 1 and an unclamped rule would estimate MORE rows than
// the left input holds — for an operator that can only ever emit a subset of
// it. The anti side is where that shows up as a negative row count.
TEST(Cardinality, SemiJoinClampsAtTheLeftRowCountAndAntiNeverGoesNegative) {
    Catalog cat(CATALOG);
    seedLapsStats(cat, /*driver_id_ndv=*/5);   // right NDV 20 > left NDV 5
    seedDriversStats(cat);
    auto semi = semiJoinOverLapsAndDrivers(JoinSemantics::SEMI);
    auto anti = semiJoinOverLapsAndDrivers(JoinSemantics::ANTI);
    CardinalityEstimator::estimateSubtree(*semi, cat);
    CardinalityEstimator::estimateSubtree(*anti, cat);
    EXPECT_DOUBLE_EQ(semi->estimated_rows, 1000.0);   // min(1.0, 20/5) == 1.0
    EXPECT_DOUBLE_EQ(anti->estimated_rows, 0.0);
    EXPECT_GE(anti->estimated_rows, 0.0);
}

// A semi/anti join carries no ON residual — the lowering pass builds none — and
// the estimator asserts that rather than quietly handling a case that cannot
// occur. An assertion that never fires is only worth keeping if something
// proves it fires when violated.
TEST(Cardinality, SemiJoinWithAnOnResidualIsAnInternalError) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);
    auto join = semiJoinOverLapsAndDrivers(JoinSemantics::SEMI);
    auto lit = std::make_unique<Literal>(Value(int64_t(1)));
    join->on_residual = std::move(lit);
    EXPECT_THROW(CardinalityEstimator::estimateSubtree(*join, cat), std::runtime_error);
}
