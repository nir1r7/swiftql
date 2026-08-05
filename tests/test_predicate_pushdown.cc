#include <gtest/gtest.h>
#include "planner/predicate_pushdown.h"
#include "planner/cardinality_estimator.h"
#include "planner/logical_plan.h"
#include "planner/vectorized_plan_builder.h"
#include "planner/vec_plan_node.h"
#include "planner/binder.h"
#include "parser/parser.h"
#include "parser/ast.h"
#include "parser/expr_utils.h"
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
// hint. Pre-execution, explain() reports the hint as "pruning=on" (the counter
// is a runtime value). The FROM side, with no pushed predicate, carries no hint.
TEST(PredicatePushdown, JoinSidePredicateReachesScanAsPruningHint) {
    Catalog cat(CATALOG);
    auto vec = buildPushedVec(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.age > 30", cat);

    const VecPlanNode* drivers_scan = findScan(vec.get(), "drivers");
    const VecPlanNode* laps_scan = findScan(vec.get(), "laps");
    ASSERT_NE(drivers_scan, nullptr);
    ASSERT_NE(laps_scan, nullptr);

    EXPECT_NE(drivers_scan->explain().find("pruning=on"), std::string::npos)
        << "join-side scan should receive the pushed predicate as a pruning hint";
    EXPECT_EQ(laps_scan->explain().find("pruning=on"), std::string::npos)
        << "FROM-side scan has no pushed predicate and should carry no hint";
}

// Conjuncts pushed below the join are re-stamped to the standalone scan's
// slot 0 — that is what lets ChunkPruner (which ignores slot >= 1 refs, a
// guard the FROM-side hint path depends on) act on them.
TEST(PredicatePushdown, JoinSideConjunctSlotsRestampedToScanSlot) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.age > 30", cat);
    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    ASSERT_EQ(join->children[1]->type, LogicalNodeType::FILTER);
    const auto* filter = static_cast<const LogicalFilter*>(join->children[1].get());

    std::vector<const Expr*> stack = {filter->predicate.get()};
    int refs_seen = 0;
    while (!stack.empty()) {
        const Expr* e = stack.back();
        stack.pop_back();
        if (auto* col = dynamic_cast<const ColumnRef*>(e)) {
            ++refs_seen;
            EXPECT_EQ(col->relation_slot, 0) << col->column_name;
        } else if (auto* bin = dynamic_cast<const BinaryExpr*>(e)) {
            stack.push_back(bin->left.get());
            stack.push_back(bin->right.get());
        } else if (auto* isn = dynamic_cast<const IsNullExpr*>(e)) {
            stack.push_back(isn->operand.get());
        }
    }
    EXPECT_GT(refs_seen, 0);
}

// End-to-end: join-side pruning must actually skip chunks AND preserve results.
// Synthetic drivers table spans two zone-map chunks sorted by driver_id, so a
// "driver_id > CHUNK_SIZE" predicate provably excludes chunk 0.
TEST(PredicatePushdown, JoinSidePruningActuallySkipsChunks) {
    Catalog cat(CATALOG);
    const std::string sql =
        "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.driver_id > " + std::to_string(CHUNK_SIZE);

    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);

    // drivers: CHUNK_SIZE + 10 rows, driver_id 1..CHUNK_SIZE+10 in order
    const Schema& dschema = cat.getTable("drivers").schema;
    std::vector<Row> drows;
    drows.reserve(CHUNK_SIZE + 10);
    for (int i = 1; i <= CHUNK_SIZE + 10; ++i) {
        drows.push_back({Value(int64_t(i)), Value(std::string("d")),
                         Value(std::string("n")), Value(std::string("t")),
                         Value(int64_t(30))});
    }
    // laps: two rows land in drivers chunk 1, one in the pruned chunk 0
    const Schema& lschema = cat.getTable("laps").schema;
    std::vector<Row> lrows = {
        {Value(int64_t(1)), Value(int64_t(CHUNK_SIZE + 1)), Value(std::string("t")),
         Value(300.0), Value(1.0), Value(1.0), Value(1.0), Value(int64_t(2024)), Value(int64_t(1))},
        {Value(int64_t(2)), Value(int64_t(CHUNK_SIZE + 2)), Value(std::string("t")),
         Value(300.0), Value(1.0), Value(1.0), Value(1.0), Value(int64_t(2024)), Value(int64_t(1))},
        {Value(int64_t(3)), Value(int64_t(5)), Value(std::string("t")),
         Value(300.0), Value(1.0), Value(1.0), Value(1.0), Value(int64_t(2024)), Value(int64_t(1))},
    };
    std::unordered_map<std::string, ColumnarTable> tables;
    tables.emplace("drivers", CSVToColumnar::convert(drows, dschema));
    tables.emplace("laps", CSVToColumnar::convert(lrows, lschema));

    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    logical = PredicatePushdown::apply(std::move(logical), cat);
    auto vec = VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);

    // drain and collect the projected lap_ids
    vec->open();
    std::vector<int64_t> lap_ids;
    while (DataChunk* chunk = vec->nextChunk()) {
        int n = chunk->filter_applied ? static_cast<int>(chunk->sel.indices.size())
                                      : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            std::visit([&](const auto& v) { lap_ids.push_back(int64_t(Value(v[r]).asInt())); },
                       chunk->columns[0].data);
        }
    }
    vec->close();

    // results correct: laps 1 and 2 survive, lap 3 (driver_id=5) is filtered out
    std::sort(lap_ids.begin(), lap_ids.end());
    EXPECT_EQ(lap_ids, (std::vector<int64_t>{1, 2}));

    // and the join-side scan really skipped drivers chunk 0
    const VecPlanNode* drivers_scan = findScan(vec.get(), "drivers");
    ASSERT_NE(drivers_scan, nullptr);
    EXPECT_NE(drivers_scan->explain().find("chunks_skipped=1/2"), std::string::npos)
        << drivers_scan->explain();
}

// ===== Result correctness under pushdown (audit M8) =====

// Lower WITHOUT the pushdown pass — the baseline plan for result invariance.
static std::unique_ptr<VecPlanNode> buildUnpushedVec(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto tables = loadColumnar(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    return VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);
}

// Drain a vectorized plan into sorted serialized rows (multiset comparison).
static std::vector<std::string> drainSorted(VecPlanNode& node) {
    node.open();
    std::vector<std::string> out;
    while (DataChunk* chunk = node.nextChunk()) {
        int n = chunk->filter_applied ? static_cast<int>(chunk->sel.indices.size())
                                      : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            std::string row;
            for (const auto& cv : chunk->columns) {
                std::visit([&](const auto& v) { row += Value(v[r]).toString() + "|"; }, cv.data);
            }
            out.push_back(std::move(row));
        }
    }
    node.close();
    std::sort(out.begin(), out.end());
    return out;
}

// Pushdown must be result-preserving, not just shape-preserving: execute the
// pushed and unpushed plans and compare row multisets. A regression that
// corrupts or drops a predicate while keeping the plan printable fails here.
TEST(PredicatePushdown, PushedPlansPreserveResults) {
    const char* queries[] = {
        // single-side predicate (pushed to the join side)
        "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.age > 30",
        // both-side predicates
        "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE l.speed > 300 AND d.age > 25",
        // mixed conjunct stays residual above the join
        "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE l.speed > 300 AND l.season = d.age",
    };
    for (const char* sql : queries) {
        Catalog cat(CATALOG);
        auto pushed = buildPushedVec(sql, cat);
        auto rows_pushed = drainSorted(*pushed);

        Catalog cat2(CATALOG);
        auto unpushed = buildUnpushedVec(sql, cat2);
        auto rows_unpushed = drainSorted(*unpushed);

        EXPECT_EQ(rows_pushed, rows_unpushed) << sql;
    }
}

// ===== Untested classification shapes (audit P2) =====

// An IS NULL conjunct over a join-side column is single-slot: it pushes below
// the join like any comparison, and executes correctly (CSV data has no NULLs,
// so zero rows survive).
TEST(PredicatePushdown, IsNullConjunctPushesBelowJoin) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.name IS NULL", cat);
    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    ASSERT_EQ(join->children[1]->type, LogicalNodeType::FILTER);
    const auto* filter = static_cast<const LogicalFilter*>(join->children[1].get());
    EXPECT_NE(dynamic_cast<const IsNullExpr*>(filter->predicate.get()), nullptr);

    Catalog cat2(CATALOG);
    auto vec = buildPushedVec(
        "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.name IS NULL", cat2);
    EXPECT_TRUE(drainSorted(*vec).empty());
}

// A literal-only conjunct references no relation slot: it must stay above the
// join as a residual, never be pushed to either scan.
TEST(PredicatePushdown, LiteralOnlyConjunctStaysAboveJoin) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE 1 = 1 AND d.age > 30", cat);

    // residual filter above the join holds the literal conjunct
    ASSERT_EQ(plan->children[0]->type, LogicalNodeType::FILTER);
    const auto* residual = static_cast<const LogicalFilter*>(plan->children[0].get());
    EXPECT_NE(exprToString(residual->predicate.get()).find("1 = 1"), std::string::npos);

    // and the join-side conjunct still pushed below
    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::FILTER);
}

// A cross-relation OR must not be split or pushed: OR is indivisible, and its
// slots span both sides, so the whole disjunction stays above the join as a
// residual with neither scan filtered.
TEST(PredicatePushdown, CrossRelationOrStaysAboveJoinIntact) {
    Catalog cat(CATALOG);
    const char* sql =
        "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE l.season = 2025 OR d.age > 30";
    auto plan = buildPushed(sql, cat);

    ASSERT_EQ(plan->children[0]->type, LogicalNodeType::FILTER);
    const auto* residual = static_cast<const LogicalFilter*>(plan->children[0].get());
    const auto* pred = dynamic_cast<const BinaryExpr*>(residual->predicate.get());
    ASSERT_NE(pred, nullptr);
    EXPECT_EQ(pred->op, "OR");  // the disjunction survives unsplit

    const LogicalPlanNode* join = residual->children[0].get();
    ASSERT_EQ(join->type, LogicalNodeType::JOIN);
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::SCAN);
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::SCAN);

    // and pushdown is result-preserving for it
    Catalog cat2(CATALOG);
    auto pushed = buildPushedVec(sql, cat2);
    Catalog cat3(CATALOG);
    auto unpushed = buildUnpushedVec(sql, cat3);
    EXPECT_EQ(drainSorted(*pushed), drainSorted(*unpushed));
}

// ===== Week 25: the new nodes must be pushable =====
//
// collectSlots and restampSlots are dispatch sites that development.md's
// checklist does not list, and both fail SILENTLY: a subtype missed in
// collectSlots yields an empty slot set, classify() returns RESIDUAL, and the
// conjunct is evaluated above the join instead of on its own scan. Correct
// answers, silently lost pushdown — which is where TPC-H Q12/Q14/Q16/Q19 live.
TEST(PredicatePushdown, PushesLikeAndInBelowJoin) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE l.team LIKE 'Fer%' AND d.nationality IN ('British', 'German')", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    ASSERT_EQ(join->children[0]->type, LogicalNodeType::FILTER);
    ASSERT_EQ(join->children[1]->type, LogicalNodeType::FILTER);
    EXPECT_EQ(join->children[0]->children[0]->type, LogicalNodeType::SCAN);
    EXPECT_EQ(join->children[1]->children[0]->type, LogicalNodeType::SCAN);

    const LogicalPlanNode* n = plan.get();
    while (n && n->type != LogicalNodeType::JOIN) {
        EXPECT_NE(n->type, LogicalNodeType::FILTER) << "nothing should remain above the join";
        n = n->children.empty() ? nullptr : n->children[0].get();
    }
}

TEST(PredicatePushdown, PushesSubstringAndCaseBelowJoin) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE SUBSTRING(l.team, 1, 3) = 'Fer' "
        "AND CASE WHEN d.age > 30 THEN 1 ELSE 0 END = 1", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::FILTER);
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::FILTER);
}

// restampSlots must stay in lockstep with collectSlots: a conjunct classified
// as JOIN-side is re-stamped to slot 0 below the join, because the standalone
// scan schema stamps every column slot 0. Without it the slot lookup misses and
// ChunkPruner ignores the hint (it deliberately skips slot >= 1 refs).
TEST(PredicatePushdown, RestampsSlotsInsideWeek25Nodes) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.nationality IN ('British')", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    ASSERT_EQ(join->children[1]->type, LogicalNodeType::FILTER);

    const auto* filter = static_cast<const LogicalFilter*>(join->children[1].get());
    const auto* in = dynamic_cast<const InExpr*>(filter->predicate.get());
    ASSERT_NE(in, nullptr) << "the IN conjunct should be the pushed predicate";
    const auto* col = dynamic_cast<const ColumnRef*>(in->operand.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->relation_slot, 0) << "pushed join-side refs must be re-stamped to slot 0";
}

// IN gets a real estimate (k/ndv from statistics); LIKE deliberately does not.
// Guessing low for LIKE measured 1.4-1.7x slower, because orderByWork ranks on
// selectivity alone and promoted an expensive predicate ahead of cheap ones —
// see the comment in cardinality_estimator.cc. This test locks in BOTH halves
// of that decision, so a future "improvement" to LIKE has to confront it.
TEST(CardinalityEstimator, EstimatesInSelectivityButNotLike) {
    Catalog cat(CATALOG);
    seedDriversStats(cat);   // nationality NDV 5, 20 rows

    // both values sit inside nationality's ['Australian', 'Spanish'] range, so
    // both count toward k
    Parser parser("SELECT name FROM drivers WHERE nationality IN ('British', 'German')");
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto plan = LogicalPlanBuilder::build(std::move(stmt), cat);
    CardinalityEstimator::estimate(*plan, cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_NEAR(filter->estimated_rows, 20.0 * (2.0 / 5.0), 1e-9);   // k / ndv

    Parser lp("SELECT name FROM drivers WHERE nationality LIKE 'a%'");
    auto lstmt = lp.parse();
    Binder::bind(lstmt, cat);
    auto lplan = LogicalPlanBuilder::build(std::move(lstmt), cat);
    CardinalityEstimator::estimate(*lplan, cat);

    const LogicalPlanNode* lfilter = findNode(lplan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(lfilter, nullptr);
    // LIKE stays on FALLBACK_SELECTIVITY: no histogram, and a low guess reorders
    // the conjunct cascade against the measured cost
    EXPECT_NEAR(lfilter->estimated_rows, 20.0 * FALLBACK_SELECTIVITY, 1e-9);
}

// Only values the column could actually hold count toward k. Without the
// [min,max] containment check, `NOT IN` inverts catastrophically: k >= ndv
// clamps selectivity to 1.0, the negated branch returns 0, and the >= 1-row
// floor turns a filter that keeps EVERY row into a 1-row estimate. Measured
// consequence before the fix: the join put the 10000-row input on the build
// side (est=1) and ran 1.54x slower than the equivalent positive form.
TEST(CardinalityEstimator, InListIgnoresValuesOutsideTheColumnRange) {
    Catalog cat(CATALOG);
    seedDriversStats(cat);   // age [20, 40], NDV 20, 20 rows

    auto estimateOf = [&](const std::string& sql) {
        Parser parser(sql);
        auto stmt = parser.parse();
        Binder::bind(stmt, cat);
        auto plan = LogicalPlanBuilder::build(std::move(stmt), cat);
        CardinalityEstimator::estimate(*plan, cat);
        const LogicalPlanNode* f = findNode(plan.get(), LogicalNodeType::FILTER);
        return f ? f->estimated_rows : -1.0;
    };

    // every listed value is outside [20, 40], so none can match
    EXPECT_NEAR(estimateOf("SELECT name FROM drivers WHERE age IN (1, 2, 3)"), 1.0, 1e-9);
    // ... and NOT IN of the same list therefore keeps everything. This is the
    // regression: counting all three gave 3/20 -> negated 0.85 before the range
    // check existed only in the equality branch, and a longer out-of-range list
    // (k >= ndv) drove it to a 1-row estimate.
    EXPECT_NEAR(estimateOf("SELECT name FROM drivers WHERE age NOT IN (1, 2, 3)"), 20.0, 1e-9);

    // a list longer than NDV, entirely out of range: the shape that inverted
    EXPECT_NEAR(estimateOf(
        "SELECT name FROM drivers WHERE age NOT IN (1,2,3,4,5,6,7,8,9,10,11,12,"
        "13,14,15,16,17,18,19,100,101)"), 20.0, 1e-9);

    // in-range values still count normally, in both polarities
    EXPECT_NEAR(estimateOf("SELECT name FROM drivers WHERE age IN (25, 30)"),
                20.0 * (2.0 / 20.0), 1e-9);
    EXPECT_NEAR(estimateOf("SELECT name FROM drivers WHERE age NOT IN (25, 30)"),
                20.0 * (1.0 - 2.0 / 20.0), 1e-9);

    // mixed: only the in-range half counts
    EXPECT_NEAR(estimateOf("SELECT name FROM drivers WHERE age IN (25, 30, 1, 999)"),
                20.0 * (2.0 / 20.0), 1e-9);
}

// The [min,max] containment check alone did not close the NOT IN inversion.
// Two paths still drove the negated selectivity to 0 — a 1-row estimate for a
// filter that keeps nearly every row, which put the 10000-row input on the
// join's build side.
TEST(CardinalityEstimator, NegatedInNeverCollapsesToZeroRows) {
    Catalog cat(CATALOG);
    seedDriversStats(cat);   // age [20,40] NDV 20; nationality NDV 5

    auto estimateOf = [&](const std::string& sql) {
        Parser parser(sql);
        auto stmt = parser.parse();
        Binder::bind(stmt, cat);
        auto plan = LogicalPlanBuilder::build(std::move(stmt), cat);
        CardinalityEstimator::estimate(*plan, cat);
        const LogicalPlanNode* f = findNode(plan.get(), LogicalNodeType::FILTER);
        return f ? f->estimated_rows : -1.0;
    };

    // (a) duplicates were counted individually, so k could exceed NDV and clamp
    //     selectivity to 1.0. Five copies of one value is one distinct value.
    EXPECT_NEAR(estimateOf("SELECT name FROM drivers WHERE age IN (25,25,25,25,25)"),
                20.0 * (1.0 / 20.0), 1e-9);
    EXPECT_NEAR(estimateOf("SELECT name FROM drivers WHERE age NOT IN (25,25,25,25,25)"),
                20.0 * (1.0 - 1.0 / 20.0), 1e-9);

    // (b) a list covering every distinct value must still not estimate 0 rows
    //     for NOT IN: NDV is an estimate, so the complement is floored at the
    //     mass of one value rather than allowed to vanish.
    double all_values = estimateOf(
        "SELECT name FROM drivers WHERE nationality NOT IN "
        "('British','German','Dutch','Finnish','Monegasque','Spanish')");
    EXPECT_GE(all_values, 20.0 * (1.0 / 5.0) - 1e-9);

    // (c) no usable statistics (a computed operand) — the flat 0.1 fallback
    //     saturates at ten values, which used to make NOT IN estimate 0
    double no_stats = estimateOf(
        "SELECT name FROM drivers WHERE SUBSTRING(name,1,2) NOT IN "
        "('a','b','c','d','e','f','g','h','i','j')");
    EXPECT_GE(no_stats, 20.0 * FALLBACK_EQ_SELECTIVITY - 1e-9);

    // the positive sense is still allowed to reach 0 — there it is a proof
    // (every listed value lies outside [min,max]), not a guess
    EXPECT_NEAR(estimateOf("SELECT name FROM drivers WHERE age IN (1,2,3)"), 1.0, 1e-9);
}
