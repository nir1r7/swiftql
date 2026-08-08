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
#include <unordered_set>
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
    const auto& fm = cat.getTable(stmt.from.tableName("test loader"));
    tables.emplace(stmt.from.tableName("test loader"),
                   CSVToColumnar::convert(CSVLoader::load(fm.filepath, fm.schema), fm.schema));
    for (const auto& j : stmt.joins) {
        if (tables.count(j.relation.tableName("test loader"))) continue;   // self-join: load once
        const auto& jm = cat.getTable(j.relation.tableName("test loader"));
        tables.emplace(j.relation.tableName("test loader"),
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
            EXPECT_EQ(col->id.slotInOwnScope("test"), 0) << col->column_name;
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
    EXPECT_EQ(col->id.slotInOwnScope("test"), 0) << "pushed join-side refs must be re-stamped to slot 0";
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


// ===== Week 26: routing by relation slot, not by join side =====

// The wrong-answer guard. classify() used to collapse "not slot 0" into
// PushTarget::JOIN and attach those conjuncts to join->children[1]. With two
// relations that was right; with three, children[1] is one specific scan, so a
// conjunct belonging to a different relation was filtered against the WRONG
// table. Each assertion below is paired with its negative form — a test that
// only counted filters would pass with all three misrouted.
TEST(PredicatePushdown, ThreeWayJoinPushesEachConjunctToItsOwnRelation) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id JOIN sj c ON b.grp = c.id "
        "WHERE a.val > 100 AND b.val > 150 AND c.val > 200", cat);

    // no residual left above the join tree
    ASSERT_EQ(plan->type, LogicalNodeType::PROJECT);
    ASSERT_EQ(plan->children[0]->type, LogicalNodeType::JOIN);

    const LogicalPlanNode* top = plan->children[0].get();
    ASSERT_EQ(static_cast<const LogicalJoin*>(top)->join_slot, 2);

    auto predicateOf = [](const LogicalPlanNode* n) {
        return exprToString(static_cast<const LogicalFilter*>(n)->predicate.get());
    };

    // relation 2 -> the top join's right input
    ASSERT_EQ(top->children[1]->type, LogicalNodeType::FILTER);
    std::string rel2 = predicateOf(top->children[1].get());
    EXPECT_NE(rel2.find("200"), std::string::npos) << rel2;
    EXPECT_EQ(rel2.find("150"), std::string::npos) << rel2;   // not b's predicate
    EXPECT_EQ(rel2.find("100"), std::string::npos) << rel2;   // not a's predicate

    // relation 1 -> the inner join's right input
    const LogicalPlanNode* inner = top->children[0].get();
    ASSERT_EQ(inner->type, LogicalNodeType::JOIN);
    ASSERT_EQ(inner->children[1]->type, LogicalNodeType::FILTER);
    std::string rel1 = predicateOf(inner->children[1].get());
    EXPECT_NE(rel1.find("150"), std::string::npos) << rel1;
    EXPECT_EQ(rel1.find("200"), std::string::npos) << rel1;

    // relation 0 -> the bottom of the left spine
    ASSERT_EQ(inner->children[0]->type, LogicalNodeType::FILTER);
    std::string rel0 = predicateOf(inner->children[0].get());
    EXPECT_NE(rel0.find("100"), std::string::npos) << rel0;
    EXPECT_EQ(rel0.find("150"), std::string::npos) << rel0;
    EXPECT_EQ(inner->children[0]->children[0]->type, LogicalNodeType::SCAN);
}

// Conjuncts pushed onto a relation beyond slot 1 must be re-stamped to the
// standalone scan's slot 0, exactly like the two-relation case.
TEST(PredicatePushdown, ThirdRelationConjunctRestampedToScanSlot) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id JOIN sj c ON b.grp = c.id "
        "WHERE c.val > 200", cat);

    const LogicalPlanNode* top = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(top, nullptr);
    ASSERT_EQ(top->children[1]->type, LogicalNodeType::FILTER);
    const auto* filter = static_cast<const LogicalFilter*>(top->children[1].get());
    const auto* bin = dynamic_cast<const BinaryExpr*>(filter->predicate.get());
    ASSERT_NE(bin, nullptr);
    const auto* col = dynamic_cast<const ColumnRef*>(bin->left.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->id.slotInOwnScope("test"), 0);
}

// A conjunct spanning two relations still stays above the whole join tree.
TEST(PredicatePushdown, ThreeWayCrossRelationConjunctStaysResidual) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id JOIN sj c ON b.grp = c.id "
        "WHERE a.val > c.val", cat);

    const LogicalPlanNode* residual = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(residual, nullptr);
    EXPECT_EQ(residual->children[0]->type, LogicalNodeType::JOIN);
}

// ===== Residual ON conjuncts (Week 27) =====
//
// A non-key ON conjunct is folded into the WHERE conjunction by the logical
// planner, so it reaches exactly the assignment machinery below — that fold is
// what these tests are really pinning. A second, stacked LogicalFilter would
// have left the WHERE unpushed instead (apply() only rewrites a FILTER whose
// DIRECT child is a JOIN), so the third test is the regression guard.

TEST(PredicatePushdown, SingleRelationResidualOnConjunctIsPushedToItsScan) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d "
        "ON l.driver_id = d.driver_id AND d.age > 30", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::FILTER);          // d.age > 30 landed here
    EXPECT_EQ(join->children[1]->children[0]->type, LogicalNodeType::SCAN);
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::SCAN);            // laps untouched
}

TEST(PredicatePushdown, CrossRelationResidualOnConjunctStaysAboveTheJoin) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d "
        "ON l.driver_id = d.driver_id AND l.speed > d.age", cat);

    const LogicalPlanNode* residual = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(residual, nullptr);
    EXPECT_EQ(residual->children[0]->type, LogicalNodeType::JOIN);
    // neither side gained a filter: the conjunct belongs to no single relation
    EXPECT_EQ(residual->children[0]->children[0]->type, LogicalNodeType::SCAN);
    EXPECT_EQ(residual->children[0]->children[1]->type, LogicalNodeType::SCAN);
}

TEST(PredicatePushdown, WhereIsStillPushedWhenAResidualOnConjunctExists) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d "
        "ON l.driver_id = d.driver_id AND d.age > 30 WHERE l.season = 2025", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::FILTER);  // WHERE conjunct
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::FILTER);  // ON residual

    // and nothing was left stranded above the join
    const LogicalPlanNode* n = plan.get();
    while (n && n->type != LogicalNodeType::JOIN) {
        EXPECT_NE(n->type, LogicalNodeType::FILTER) << "no filter should remain above the join";
        n = n->children.empty() ? nullptr : n->children[0].get();
    }
}

// ── Week 29: an outer join's null-supplying side is not pushable ─────────────

// σ_p(R ⟕ S) is NOT σ_p(R) ⟕ σ_p(S). Filtering S first makes left rows that HAD
// matches lose them, and the outer join then null-extends exactly the rows the
// WHERE existed to remove — MORE rows, no error. The conjunct must stay above the
// join, where three-valued logic drops the null-extended rows.
TEST(PredicatePushdown, NullSupplyingSidePredicateIsNotPushedThroughALeftJoin) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
        "WHERE l.season = 2025", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    // the null-supplying input is untouched
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::SCAN);
    // and the conjunct is still above the join
    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->children[0]->type, LogicalNodeType::JOIN);
}

// The preserved side keeps its pushdown: σ_p(R) ⟕ S ≡ σ_p(R ⟕ S), because
// null-extension never touches R's columns and never invents an R row. Without
// this control, a guard that declines both sides would look correct here and
// silently cost every outer-join query its pushdown.
TEST(PredicatePushdown, PreservedSidePredicateStillPushesThroughALeftJoin) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
        "WHERE d.age > 30", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::FILTER);
    EXPECT_EQ(join->children[0]->children[0]->type, LogicalNodeType::SCAN);
    // nothing left above the join
    const LogicalPlanNode* n = plan.get();
    while (n && n->type != LogicalNodeType::JOIN) {
        EXPECT_NE(n->type, LogicalNodeType::FILTER);
        n = n->children.empty() ? nullptr : n->children[0].get();
    }
}

// A conjunct owned by a relation an INNER join adds still pushes when the outer
// join is elsewhere in the tree: the test is re-applied per join on the way down,
// not once for the whole tree.
TEST(PredicatePushdown, InnerJoinBelowAnOuterJoinStillPushes) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "LEFT JOIN drivers d2 ON d.team = d2.team "
        "WHERE d.age > 30 AND d2.age > 20", cat);

    const LogicalPlanNode* top = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(top, nullptr);
    // d2 is null-supplied by the LEFT join: not pushed
    EXPECT_EQ(top->children[1]->type, LogicalNodeType::SCAN);
    // d is added by the inner join below: pushed onto its own scan
    const LogicalPlanNode* inner = top->children[0].get();
    ASSERT_EQ(inner->type, LogicalNodeType::JOIN);
    EXPECT_EQ(inner->children[1]->type, LogicalNodeType::FILTER);
    EXPECT_EQ(inner->children[1]->children[0]->type, LogicalNodeType::SCAN);
}

// ── Week 29 round 2: the shared pruning-hint rule ───────────────────────────

// One rule, two callers (VectorizedPlanBuilder's JOIN case and Planner::plan's
// FROM scan). Tested directly, because the two branches that matter are the ones
// no query on the shipped catalog can currently reach.
TEST(PruningHintForPreservedSide, InnerJoinHintIsAlwaysHandedThrough) {
    // an inner join folded its ON residuals into the WHERE, so every conjunct is
    // a legal filter on the join output — withholding would cost the mixed-slot
    // --no-optimize hint the Phase 4 benchmark measures
    Parser parser("SELECT team FROM laps WHERE season = 2024");
    auto stmt = parser.parse();
    const Expr* hint = stmt.where.get();
    EXPECT_EQ(pruningHintForPreservedSide(hint, JoinType::INNER, {}), hint);
    EXPECT_EQ(pruningHintForPreservedSide(nullptr, JoinType::LEFT, {0}), nullptr);
}

TEST(PruningHintForPreservedSide, OuterJoinTestsEveryPreservedSlotNotJustZero) {
    Catalog cat(CATALOG);
    // `d2.age > 30` is slot 1 — preserved in (A JOIN B) LEFT JOIN C, and NOT
    // preserved in A LEFT JOIN B. Testing slot 0 alone withheld it from a scan
    // entitled to it, which measurably disabled zone-map pruning for a relation
    // the outer join has nothing to do with.
    Parser parser("SELECT l.team FROM drivers d JOIN drivers d2 ON d.driver_id = d2.driver_id "
                  "LEFT JOIN laps l ON d.driver_id = l.driver_id WHERE d2.age > 30");
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    const Expr* hint = stmt.where.get();

    EXPECT_EQ(pruningHintForPreservedSide(hint, JoinType::LEFT, {0, 1}), hint);
    EXPECT_EQ(pruningHintForPreservedSide(hint, JoinType::LEFT, {0}), nullptr);
}

// collectSlots is DISPATCH SITE 8: a missed Expr subtype yields an EMPTY slot
// set, and its three callers do NOT agree on what that means (corrected in
// Week 30, along with predicate_pushdown.h/.cc, which said the opposite):
// soleSlot reads empty as -1 and declines to push, which is CONSERVATIVE, while
// classifyJoinCondition reads it as "no forward reference" and ACCEPTS the
// conjunct, which is PERMISSIVE. Here empty would read as "mentions nothing
// unpreserved" and turn the guard OFF, so this caller must fail closed on its
// own account rather than on a property of the others.
TEST(PruningHintForPreservedSide, AnEmptySlotSetFailsClosed) {
    // a constant predicate carries no ColumnRef, so collectSlots yields nothing —
    // the same state an unhandled subtype would produce. Withholding costs
    // nothing: collectSimplePredicates needs a ColumnRef to make a prunable triple.
    Parser parser("SELECT team FROM laps WHERE 1 = 1");
    auto stmt = parser.parse();
    std::unordered_set<int> slots;
    collectSlots(stmt.where.get(), slots);
    ASSERT_TRUE(slots.empty()) << "fixture must actually produce an empty slot set";

    EXPECT_EQ(pruningHintForPreservedSide(stmt.where.get(), JoinType::LEFT, {0}), nullptr);
    // ...and an inner join still hands it through, because the guard does not
    // apply there at all
    EXPECT_EQ(pruningHintForPreservedSide(stmt.where.get(), JoinType::INNER, {0}),
              stmt.where.get());
}

// ===== Week 30: collectSlots and the subquery node (DISPATCH SITE 8) =====

// Not reachable from the CLI this week — Validator refuses a bound subquery
// before any logical plan exists — so these call the walker directly, the way
// PruningHintForPreservedSide.AnEmptySlotSetFailsClosed already does. readme.md's
// site-8 note requires the branch to ship WITH the node, because Week 31 turns
// it live and a miss there is a wrong answer, not a lost optimization.

static SelectStatement bindSql(const std::string& sql, const Catalog& cat) {
    Parser p(sql);
    auto stmt = p.parse();
    Binder::bind(stmt, cat);
    return stmt;
}

// A subquery's OWN refs are positions in a DIFFERENT scope's range table, so
// contributing them here would name this block's relations by another block's
// numbering. An uncorrelated subquery is a constant with respect to this block
// and contributes nothing at all.
TEST(CollectSlots, UncorrelatedSubqueryContributesOnlyItsOperandsSlots) {
    Catalog cat(CATALOG);
    auto stmt = bindSql("SELECT l.team FROM laps l JOIN drivers d "
                        "ON l.driver_id = d.driver_id "
                        "WHERE d.age IN (SELECT season FROM laps l2)", cat);
    std::unordered_set<int> slots;
    collectSlots(stmt.where.get(), slots);
    EXPECT_EQ(slots, (std::unordered_set<int>{1}))
        << "d.age is slot 1; the body's own slot 0 belongs to another scope";
}

// A CORRELATED subquery does reference this block — that is what correlation
// means — so it must contribute something. -1 is this walker's existing
// "references something this block cannot name" value, and it makes every
// caller conservative: soleSlot declines to push, and the pruning-hint guard
// withholds.
TEST(CollectSlots, CorrelatedSubqueryContributesTheConservativeSentinel) {
    Catalog cat(CATALOG);
    auto stmt = bindSql("SELECT l.team FROM laps l JOIN drivers d "
                        "ON l.driver_id = d.driver_id "
                        "WHERE EXISTS (SELECT * FROM laps l2 WHERE l2.team = d.team)", cat);
    std::unordered_set<int> slots;
    collectSlots(stmt.where.get(), slots);
    EXPECT_EQ(slots.count(-1), 1u) << "a correlated conjunct owns no single relation";

    // ...which is exactly what keeps the pruning-hint guard closed over it
    EXPECT_EQ(pruningHintForPreservedSide(stmt.where.get(), JoinType::LEFT, {0, 1}), nullptr);
    // and an inner join is untouched, as always
    EXPECT_EQ(pruningHintForPreservedSide(stmt.where.get(), JoinType::INNER, {0, 1}),
              stmt.where.get());
}

// restampSlots' own SubqueryExpr branch (dispatch site 9) cannot be exercised
// yet and deliberately has no test: the only route to it is a conjunct that
// soleSlot pushes below a join, and no statement containing a subquery reaches
// PredicatePushdown at all this week (Validator refuses first). Its correctness
// is an argument rather than an assertion, and the argument is the test above —
// a CORRELATED subquery yields -1, so soleSlot returns -1, so such a conjunct is
// never pushed; an UNCORRELATED one has nothing inside the body to restamp.

// ===== The totality screen (seam audit pass 3, B3-2) =====
//
// PER-ROW EVALUATION IS NOT TOTAL: `evaluate()` throws on a row whose SUBSTRING
// start is < 1, and on INT overflow. Every conjunct MOVE this pass makes is
// therefore a decision about whether the query ERRORS, not only about how fast
// it runs — and both directions were reproducible from the CLI on the shipped
// `catalog.json` before the screen existed.
//
// THE PROPERTY THESE TESTS PIN is not "predicates evaluate left to right" (SQL
// fixes no such order, so every one of those answers is legal in isolation). It
// is the one this project asserts: a conjunct is evaluated on the SAME ROW SET
// in both legs, so it raises under the optimizer iff it raises under
// `--no-optimize`. See the screen's comment in predicate_pushdown.cc.
//
// EACH TEST FAILS WITHOUT THE FIX, and the assertion that fails is named in its
// comment — they are written against the CONJUNCT ORDER the pass produces, which
// is exactly what changed.

// Written order is [SUBSTRING (raises), speed] and speed is far more selective,
// so before the screen the sort promoted speed and the SUBSTRING was never
// reached: 0 rows optimized, `Error: SUBSTRING: start position must be >= 1`
// under --no-optimize. The raising conjunct must keep index 0.
TEST(PredicatePushdown, RaisingConjunctIsNotDemotedByTheSelectivitySort) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildPushed(
        "SELECT team FROM laps "
        "WHERE SUBSTRING(team, lap_id - lap_id, 2) = 'x' AND speed > 390", cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    const auto* root_and = dynamic_cast<const BinaryExpr*>(
        static_cast<const LogicalFilter*>(filter)->predicate.get());
    ASSERT_NE(root_and, nullptr);
    ASSERT_EQ(root_and->op, "AND");
    // WITHOUT THE FIX this is the `speed > 390` comparison: selectivity ~0.05
    // against the SUBSTRING conjunct's FALLBACK_EQ_SELECTIVITY of 0.1.
    const auto* left = dynamic_cast<const BinaryExpr*>(root_and->left.get());
    ASSERT_NE(left, nullptr);
    EXPECT_NE(dynamic_cast<const SubstringExpr*>(left->left.get()), nullptr)
        << "the conjunct that can raise must keep its written position";
}

// The mirror image, and the one that matters most: the optimizer MANUFACTURING a
// failure on a query that succeeds without it. LIKE deliberately has no
// selectivity rule (FALLBACK_SELECTIVITY 0.5), so the SUBSTRING conjunct's 0.1
// used to sort AHEAD of a predicate that matched nothing.
TEST(PredicatePushdown, RaisingConjunctIsNotPromotedAheadOfAWrittenPredecessor) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildPushed(
        "SELECT team FROM laps "
        "WHERE team LIKE 'zzz%' AND SUBSTRING(team, lap_id - lap_id, 2) = 'x'", cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    const auto* root_and = dynamic_cast<const BinaryExpr*>(
        static_cast<const LogicalFilter*>(filter)->predicate.get());
    ASSERT_NE(root_and, nullptr);
    // WITHOUT THE FIX the left operand is the SUBSTRING comparison.
    EXPECT_NE(dynamic_cast<const LikeExpr*>(root_and->left.get()), nullptr)
        << "a predicate the user wrote FIRST must not be demoted below one that "
           "can raise";
}

// THE CONTROL, and it is what keeps the screen from being a blanket refusal:
// inferExprType decides a CONSTANT SUBSTRING's domain at plan time, in both
// legs, before this pass runs — so `SUBSTRING(team, 1, 2)` cannot raise and is
// sorted like any other conjunct. This is TPC-H Q22's shape.
TEST(PredicatePushdown, ConstantArgumentSubstringIsTotalAndStillSorts) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildPushed(
        "SELECT team FROM laps "
        "WHERE SUBSTRING(team, 1, 2) = 'Fe' AND speed > 390", cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    const auto* root_and = dynamic_cast<const BinaryExpr*>(
        static_cast<const LogicalFilter*>(filter)->predicate.get());
    ASSERT_NE(root_and, nullptr);
    const auto* left = dynamic_cast<const BinaryExpr*>(root_and->left.get());
    ASSERT_NE(left, nullptr);
    const auto* col = dynamic_cast<const ColumnRef*>(left->left.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column_name, "speed")
        << "a SUBSTRING with constant arguments cannot raise, so the sort keeps "
           "working on it";
}

// The SECOND cause, which a fix confined to orderByWork would have missed.
// Pushing a conjunct below the join moves it to a point where it sees EVERY row
// of one relation rather than the join's survivors. Reproduced on the shipped
// catalog: `d.nationality = 'Zzz' AND SUBSTRING(l.team, l.lap_id - l.lap_id, 2)`
// errored optimized and returned 0 rows under --no-optimize.
TEST(PredicatePushdown, RaisingConjunctIsNotPushedBelowTheJoin) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.nationality = 'French' "
        "  AND SUBSTRING(l.team, l.lap_id - l.lap_id, 2) = 'x'", cat);

    // The raising conjunct stays ABOVE the join...
    ASSERT_EQ(plan->type, LogicalNodeType::PROJECT);
    ASSERT_EQ(plan->children[0]->type, LogicalNodeType::FILTER)
        << "WITHOUT THE FIX the SUBSTRING conjunct is single-slot and is pushed "
           "onto the laps scan, leaving no residual filter here at all";
    const auto* residual = static_cast<const LogicalFilter*>(plan->children[0].get());
    EXPECT_NE(dynamic_cast<const SubstringExpr*>(
        dynamic_cast<const BinaryExpr*>(residual->predicate.get())->left.get()), nullptr);

    // ...and the FROM-side scan carries no filter of its own.
    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->children[0]->type, LogicalNodeType::SCAN);
    // The conjunct WRITTEN BEFORE it is total, so it still pushes: the screen
    // freezes a suffix, it does not switch pushdown off.
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::FILTER);
}

// distribute() deliberately leaves a bucket behind when the join at that slot is
// LEFT (Week 29) or semi/anti (Week 32), and pushIntoJoin then lifts it above the
// tree. Appending it there put a WRITTEN-EARLIER conjunct AFTER the raising one,
// which is the same divergence with a third cause — reproduced on the shipped
// catalog as `laps LEFT JOIN drivers WHERE d.nationality='Zzz' AND SUBSTRING(...)`,
// which errored optimized and returned 0 rows under --no-optimize.
TEST(PredicatePushdown, LeftoverBucketIsRestoredToItsWrittenPosition) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);
    auto plan = buildPushed(
        "SELECT l.team FROM laps l LEFT JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE d.nationality = 'French' "
        "  AND SUBSTRING(l.team, l.lap_id - l.lap_id, 2) = 'x'", cat);

    ASSERT_EQ(plan->type, LogicalNodeType::PROJECT);
    ASSERT_EQ(plan->children[0]->type, LogicalNodeType::FILTER);
    const auto* residual = static_cast<const LogicalFilter*>(plan->children[0].get());
    const auto* root_and = dynamic_cast<const BinaryExpr*>(residual->predicate.get());
    ASSERT_NE(root_and, nullptr);
    ASSERT_EQ(root_and->op, "AND");
    // WITHOUT THE FIX the left operand is the SUBSTRING conjunct: the
    // null-supplying-side bucket is declined by distribute and appended LAST.
    const auto* left = dynamic_cast<const BinaryExpr*>(root_and->left.get());
    ASSERT_NE(left, nullptr);
    const auto* col = dynamic_cast<const ColumnRef*>(left->left.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column_name, "nationality")
        << "a leftover bucket must go back to the position the user wrote it in";
}

// ===== Descending past the first rewritable node (seam audit pass 2, B-2) =====

// `apply` used to RETURN from the FILTER-over-JOIN branch, so a derived body's
// own FILTER-over-JOIN was rewritten only when the ENCLOSING block happened to
// have no join. Whether a body is optimized cannot depend on its caller's shape.
TEST(PredicatePushdown, DescendsIntoADerivedBodyUnderAJoiningOuterBlock) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);
    // The outer WHERE is what makes the outer block a FILTER-over-JOIN, which is
    // the branch that used to `return` before the body was ever reached. Without
    // it `apply` falls through to the generic child loop and descends anyway —
    // which is the audit's point: whether a body is optimized depended on a
    // property of its CALLER.
    auto plan = buildPushed(
        "SELECT x.team FROM drivers d0 JOIN "
        "  (SELECT l.team, l.driver_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "   WHERE l.season = 2020 AND d.age > 38) x "
        "ON d0.driver_id = x.driver_id "
        "WHERE d0.nationality = 'French'", cat);

    const LogicalPlanNode* derived = nullptr;
    std::vector<const LogicalPlanNode*> stack{plan.get()};
    while (!stack.empty()) {
        const LogicalPlanNode* n = stack.back(); stack.pop_back();
        if (n->type == LogicalNodeType::DERIVED) { derived = n; break; }
        for (const auto& c : n->children) stack.push_back(c.get());
    }
    ASSERT_NE(derived, nullptr);

    // Inside the body, both conjuncts must have reached their own scans. WITHOUT
    // THE FIX the body still carries one FILTER over its JOIN and both scans are
    // bare.
    const LogicalPlanNode* body_join = findNode(derived, LogicalNodeType::JOIN);
    ASSERT_NE(body_join, nullptr) << "the body's join should be directly reachable "
                                     "— a residual FILTER above it means nothing was pushed";
    EXPECT_EQ(body_join->children[0]->type, LogicalNodeType::FILTER)
        << "l.season = 2020 should have reached the laps scan";
    EXPECT_EQ(body_join->children[1]->type, LogicalNodeType::FILTER)
        << "d.age > 38 should have reached the drivers scan";
}

// ===== Entering a derived body (seam audit pass 3, B3-3) =====
//
// filterOnto WRAPPED a conjunct routed to a derived relation ABOVE the
// LogicalDerived and stopped there, in EVERY shape — including the one with no
// join anywhere — and `--explain` said nothing about it. Measured on the
// simplest exhibiting query (Release, repo `catalog.json`, `Execution:` from
// --explain-analyze, median of 5): 988.6us before, 344.6us after, against 289us
// for the flat equivalent the derived form is semantically identical to. The
// cost was never the filter: it was the body's projection materializing 10000
// rows the filter then discards to 152, and the body's scan losing its zone-map
// hint.

// The whole point: the conjunct must reach the body's SCAN, not stop at the
// relation boundary and not stop at the body's projection.
TEST(PredicatePushdown, ConjunctEntersADerivedBodyAndReachesItsScan) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildPushed(
        "SELECT d.team, d.speed FROM (SELECT team, speed, season FROM laps) d "
        "WHERE d.speed > 390", cat);

    // WITHOUT THE FIX: PROJECT / FILTER / DERIVED / PROJECT / SCAN — the filter
    // sits above the relation and the body materializes every row.
    ASSERT_EQ(plan->type, LogicalNodeType::PROJECT);
    ASSERT_EQ(plan->children[0]->type, LogicalNodeType::DERIVED)
        << "no filter should be left above the derived relation";
    const LogicalPlanNode* body = plan->children[0]->children[0].get();
    ASSERT_EQ(body->type, LogicalNodeType::PROJECT);
    ASSERT_EQ(body->children[0]->type, LogicalNodeType::FILTER)
        << "the conjunct must also descend BELOW the body's projection — that is "
           "where the materialization cost is";
    EXPECT_EQ(body->children[0]->children[0]->type, LogicalNodeType::SCAN);
}

// The relation's schema is derivedRelationSchema(body schema): it may RENAME
// positionally and it re-stamps every slot to 0. So the mapping is by INDEX, and
// a column-alias list is the case a name-to-name mapping would silently get
// wrong.
TEST(PredicatePushdown, DerivedColumnAliasesAreMappedPositionally) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildPushed(
        "SELECT d.b FROM (SELECT team, speed FROM laps) d (a, b) WHERE d.b > 390", cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->children[0]->type, LogicalNodeType::SCAN);
    const auto* cmp = dynamic_cast<const BinaryExpr*>(
        static_cast<const LogicalFilter*>(filter)->predicate.get());
    ASSERT_NE(cmp, nullptr);
    const auto* col = dynamic_cast<const ColumnRef*>(cmp->left.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column_name, "speed") << "d.b is the body's second column";
}

// σ_p(π(R)) ≡ π(σ_p'(R)) needs every column p names to be a PLAIN PASSTHROUGH.
// A computed one cannot be rewritten by substitution here. It must stop above
// the projection — and the conjunct must still have ENTERED the body, because
// entering is safe for any body shape.
TEST(PredicatePushdown, ComputedProjectionStopsTheDescentButNotTheEntry) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildPushed(
        "SELECT d.s2 FROM (SELECT team, speed * 2 AS s2 FROM laps) d WHERE d.s2 > 780", cat);

    ASSERT_EQ(plan->type, LogicalNodeType::PROJECT);
    ASSERT_EQ(plan->children[0]->type, LogicalNodeType::DERIVED);
    const LogicalPlanNode* inside = plan->children[0]->children[0].get();
    ASSERT_EQ(inside->type, LogicalNodeType::FILTER) << "entry is safe for any body";
    EXPECT_EQ(inside->children[0]->type, LogicalNodeType::PROJECT)
        << "and the descent stops at the computed column";
}

// An AGGREGATE or a LIMIT in the body is not a special case in this pass: the
// entry is above the body root either way, and the descent rule only fires on a
// PROJECT of passthroughs. This pins that the filter lands ABOVE the LIMIT — the
// one shape where descending would be a WRONG ANSWER rather than slow.
TEST(PredicatePushdown, DerivedBodyLimitIsNeverDescendedPast) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto plan = buildPushed(
        "SELECT d.team FROM (SELECT team, speed FROM laps LIMIT 10) d WHERE d.speed > 390", cat);

    ASSERT_EQ(plan->children[0]->type, LogicalNodeType::DERIVED);
    const LogicalPlanNode* inside = plan->children[0]->children[0].get();
    ASSERT_EQ(inside->type, LogicalNodeType::FILTER);
    EXPECT_EQ(inside->children[0]->type, LogicalNodeType::LIMIT)
        << "filtering below the cut would change which 10 rows survive";
}

// A refusal at the RELATION boundary is the one --explain could not show: a
// filter drawn above a LogicalDerived looks identical whether there was a
// decision or not, and B3-3 is the third silent decline this phase found. Only a
// refusal is stamped, so every body that takes its conjuncts keeps a
// byte-identical explain string.
TEST(PredicatePushdown, RefusedEntryToADerivedBodyIsReportedInExplain) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    auto refused = buildPushed(
        "SELECT d.team FROM (SELECT team, speed, lap_id FROM laps) d "
        "WHERE SUBSTRING(d.team, d.lap_id - d.lap_id, 2) = 'x'", cat);
    const LogicalPlanNode* derived = findNode(refused.get(), LogicalNodeType::DERIVED);
    ASSERT_NE(derived, nullptr);
    EXPECT_EQ(static_cast<const LogicalDerived*>(derived)->explain(),
              "LogicalDerived [d, 3 columns] pushdown=skipped (predicate can raise)");

    // ...and the CONTROL that keeps the stamp from becoming decoration: a body
    // that took its conjunct says nothing at all.
    auto taken = buildPushed(
        "SELECT d.team FROM (SELECT team, speed, lap_id FROM laps) d WHERE d.speed > 390", cat);
    const LogicalPlanNode* ok = findNode(taken.get(), LogicalNodeType::DERIVED);
    ASSERT_NE(ok, nullptr);
    EXPECT_EQ(static_cast<const LogicalDerived*>(ok)->explain(),
              "LogicalDerived [d, 3 columns]");
}

// A body that JOINS: the conjunct descends through the projection and is then
// routed by the ordinary FILTER-over-JOIN rule onto the relation that owns it.
// This is also the case that would break if the mapping were by name — the
// body's own schema carries two slots.
TEST(PredicatePushdown, ConjunctRoutesToItsOwnRelationInsideAJoiningBody) {
    Catalog cat(CATALOG);
    seedLapsStats(cat);
    seedDriversStats(cat);
    auto plan = buildPushed(
        "SELECT d.n FROM (SELECT dr.name AS n, l.team AS t, l.speed AS sp "
        "                 FROM laps l JOIN drivers dr ON l.driver_id = dr.driver_id) d "
        "WHERE d.sp > 390", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    ASSERT_EQ(join->children[0]->type, LogicalNodeType::FILTER)
        << "sp is laps.speed and belongs on the FROM-side scan";
    EXPECT_EQ(join->children[1]->type, LogicalNodeType::SCAN);
    const auto* cmp = dynamic_cast<const BinaryExpr*>(
        static_cast<const LogicalFilter*>(join->children[0].get())->predicate.get());
    ASSERT_NE(cmp, nullptr);
    const auto* col = dynamic_cast<const ColumnRef*>(cmp->left.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column_name, "speed");
}
