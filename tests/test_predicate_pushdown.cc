#include <gtest/gtest.h>
#include "planner/predicate_pushdown.h"
#include "planner/logical_plan.h"
#include "planner/vectorized_plan_builder.h"
#include "planner/vec_plan_node.h"
#include "planner/binder.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "catalog/catalog.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"
#include "common/value.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Tests run from build/, so the catalog is one level up.
static const char* CATALOG = "../tests/data/test_catalog.json";

// Parse + bind + build + push down.
static std::unique_ptr<LogicalPlanNode> buildPushed(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    return PredicatePushdown::apply(std::move(logical), cat);
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

// Deterministic stats so the ordering test is exact arithmetic. Mirrors
// test_cardinality.cc: laps 1000 rows; season NDV 5 range [2020,2024];
// speed range [200,400].
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

    cat.setStats("laps", std::move(ts));
}

// drivers: 20 rows; age range [20,40] NDV 20; nationality NDV 5.
static void seedDriversStats(Catalog& cat) {
    TableStats ts;
    ts.row_count = 20;

    ColumnStats age;
    age.min_val = Value(int64_t(20));
    age.max_val = Value(int64_t(40));
    age.distinct_count = 20;
    age.null_count = 0;
    ts.columns.emplace("age", age);

    ColumnStats nationality;
    nationality.min_val = Value(std::string("Australian"));
    nationality.max_val = Value(std::string("Spanish"));
    nationality.distinct_count = 5;
    nationality.null_count = 0;
    ts.columns.emplace("nationality", nationality);

    ColumnStats driver_id;
    driver_id.min_val = Value(int64_t(1));
    driver_id.max_val = Value(int64_t(20));
    driver_id.distinct_count = 20;
    driver_id.null_count = 0;
    ts.columns.emplace("driver_id", driver_id);

    cat.setStats("drivers", std::move(ts));
}

// Load columnar tables the statement touches (mirrors test_vec_plan_builder.cc).
static std::unordered_map<std::string, ColumnarTable> loadColumnar(
        const SelectStatement& stmt, const Catalog& cat) {
    std::unordered_map<std::string, ColumnarTable> tables;
    const auto& fm = cat.getTable(stmt.from_table);
    tables.emplace(stmt.from_table,
                   CSVToColumnar::convert(CSVLoader::load(fm.filepath, fm.schema), fm.schema));
    if (stmt.join.has_value() && !tables.count(stmt.join->join_table)) {
        const auto& jm = cat.getTable(stmt.join->join_table);
        tables.emplace(stmt.join->join_table,
                       CSVToColumnar::convert(CSVLoader::load(jm.filepath, jm.schema), jm.schema));
    }
    return tables;
}

// Parse + bind + logical plan + pushdown + lower — the optimized vectorized path.
static std::unique_ptr<VecPlanNode> buildPushedVec(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto tables = loadColumnar(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    logical = PredicatePushdown::apply(std::move(logical), cat);
    return VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);
}

// Collect every node in the vectorized plan tree.
static void collectVec(const VecPlanNode* node, std::vector<const VecPlanNode*>& out) {
    if (!node) return;
    out.push_back(node);
    for (const VecPlanNode* c : node->children()) collectVec(c, out);
}

// The one VecScan whose explain() names the given table.
static const VecPlanNode* findScan(const VecPlanNode* root, const std::string& table) {
    std::vector<const VecPlanNode*> all;
    collectVec(root, all);
    for (const VecPlanNode* n : all) {
        std::string e = n->explain();
        if (e.rfind("VecScan [" + table, 0) == 0) return n;
    }
    return nullptr;
}

// ===== Task 2: pushdown across a join =====

// Checkpoint: both join inputs are filtered before the join when legal.
TEST(PredicatePushdown, PushesBothSingleRelationPredicatesBelowJoin) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE l.season = 2025 AND d.age > 30", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);

    // both inputs now carry their own filter
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::FILTER);
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::FILTER);
    // and each filter sits directly on its scan
    EXPECT_EQ(join->children[0]->children[0]->type, LogicalNodeType::SCAN);
    EXPECT_EQ(join->children[1]->children[0]->type, LogicalNodeType::SCAN);

    // no residual filter left above the join
    const LogicalPlanNode* n = plan.get();
    while (n && n->type != LogicalNodeType::JOIN) {
        EXPECT_NE(n->type, LogicalNodeType::FILTER) << "no filter should remain above the join";
        n = n->children.empty() ? nullptr : n->children[0].get();
    }
}

TEST(PredicatePushdown, FromOnlyPredicateLeavesJoinSideUnfiltered) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE l.season = 2025", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::FILTER);  // FROM side filtered
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::SCAN);    // JOIN side untouched
}

TEST(PredicatePushdown, MixedPredicateStaysAboveJoinAsResidual) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE l.season = 2025 AND l.speed > d.age", cat);

    // single-relation conjunct pushed; cross-relation conjunct stays above join
    const LogicalPlanNode* residual = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(residual, nullptr);
    EXPECT_EQ(residual->children[0]->type, LogicalNodeType::JOIN);

    const LogicalPlanNode* join = residual->children[0].get();
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::FILTER);  // l.season pushed to FROM
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::SCAN);    // drivers has no pushable conjunct
}

// ===== HAVING must never be pushed =====

TEST(PredicatePushdown, HavingFilterNotPushedIntoJoinScans) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "GROUP BY l.team HAVING COUNT(*) > 5", cat);

    // HAVING filter stays above the aggregate
    const LogicalPlanNode* having = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(having, nullptr);
    EXPECT_EQ(having->children[0]->type, LogicalNodeType::AGGREGATE);

    // with no WHERE, both join inputs remain bare scans
    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::SCAN);
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::SCAN);
}

// ===== Task 3: ordering scan-local conjuncts by expected work =====

// speed > 390 keeps ~5% (range interpolation); season = 2020 keeps 1/5 = 20%.
// The more selective conjunct (speed) must become the left operand so the
// executor's AND cascade evaluates it first.
TEST(PredicatePushdown, OrdersScanLocalConjunctsMostSelectiveFirst) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildPushed("SELECT team FROM laps WHERE season = 2020 AND speed > 390", cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    const auto* f = static_cast<const LogicalFilter*>(filter);

    // predicate is a left-deep AND; the left operand is evaluated first
    const auto* root_and = dynamic_cast<const BinaryExpr*>(f->predicate.get());
    ASSERT_NE(root_and, nullptr);
    ASSERT_EQ(root_and->op, "AND");

    // left operand should be the speed comparison (more selective)
    const auto* left = dynamic_cast<const BinaryExpr*>(root_and->left.get());
    ASSERT_NE(left, nullptr);
    const auto* col = dynamic_cast<const ColumnRef*>(left->left.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column_name, "speed");
}

// ===== no-op cases =====

TEST(PredicatePushdown, SingleTableNoWhereIsUnchanged) {
    Catalog cat(CATALOG);
    auto plan = buildPushed("SELECT team FROM laps", cat);
    // Project over Scan, no filter introduced
    EXPECT_EQ(plan->type, LogicalNodeType::PROJECT);
    EXPECT_EQ(plan->children[0]->type, LogicalNodeType::SCAN);
}

// Conjuncts pushed onto the JOIN side are ordered most-selective-first, just
// like the FROM side. age > 38 keeps ~10% (range); nationality = 'French' keeps
// 1/5 = 20%, so age must become the left operand of the pushed AND.
TEST(PredicatePushdown, OrdersJoinSideConjunctsMostSelectiveFirst) {
    Catalog cat(CATALOG);
    seedDriversStats(cat);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.age > 38 AND d.nationality = 'French'", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    ASSERT_EQ(join->children[1]->type, LogicalNodeType::FILTER);  // drivers side filtered
    const auto* f = static_cast<const LogicalFilter*>(join->children[1].get());

    const auto* root_and = dynamic_cast<const BinaryExpr*>(f->predicate.get());
    ASSERT_NE(root_and, nullptr);
    ASSERT_EQ(root_and->op, "AND");
    const auto* left = dynamic_cast<const BinaryExpr*>(root_and->left.get());
    ASSERT_NE(left, nullptr);
    const auto* col = dynamic_cast<const ColumnRef*>(left->left.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column_name, "age");  // more selective conjunct evaluated first
}

// A join-side WHERE predicate reaches the JOIN-side scan as a zone-map pruning
// hint (previously the join side always got nullptr). VecScan::explain() prints
// "chunks_skipped" only when a hint is present — that's the observable contract.
// The FROM side, with no pushed predicate, must NOT carry a hint.
TEST(PredicatePushdown, JoinSidePredicateReachesScanAsPruningHint) {
    Catalog cat(CATALOG);
    auto vec = buildPushedVec(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.age > 30", cat);

    const VecPlanNode* drivers_scan = findScan(vec.get(), "drivers");
    const VecPlanNode* laps_scan = findScan(vec.get(), "laps");
    ASSERT_NE(drivers_scan, nullptr);
    ASSERT_NE(laps_scan, nullptr);

    EXPECT_NE(drivers_scan->explain().find("chunks_skipped"), std::string::npos)
        << "join-side scan should receive the pushed predicate as a pruning hint";
    EXPECT_EQ(laps_scan->explain().find("chunks_skipped"), std::string::npos)
        << "FROM-side scan has no pushed predicate and should carry no hint";
}
