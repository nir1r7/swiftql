#include <gtest/gtest.h>
#include "planner/vectorized_plan_builder.h"
#include "planner/logical_plan.h"
#include "planner/binder.h"
#include "planner/planner.h"
#include "parser/parser.h"
#include "catalog/catalog.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"
#include "common/schema.h"
#include "common/value.h"
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The test catalog lives one level up from the build dir (tests run from build/).
static const char* CATALOG = "../tests/data/test_catalog.json";

// Load columnar tables for every table the statement touches (self-join keys
// once by name; VectorizedPlanBuilder copies internally for the extra scan).
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

// Parse + bind + logical plan + lower — the full Week 18 vectorized pipeline.
static std::unique_ptr<VecPlanNode> buildVec(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto tables = loadColumnar(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    return VectorizedPlanBuilder::build(std::move(logical), std::move(tables));
}

// Drain a vectorized plan into materialized rows (same loop as main.cc).
static std::vector<Row> drainVec(VecPlanNode& node) {
    node.open();
    std::vector<Row> rows;
    while (DataChunk* chunk = node.nextChunk()) {
        int n = chunk->filter_applied
            ? static_cast<int>(chunk->sel.indices.size())
            : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            Row row;
            row.reserve(chunk->columns.size());
            for (const auto& cv : chunk->columns) {
                std::visit([&](const auto& vec) {
                    row.push_back(Value(vec[r]));
                }, cv.data);
            }
            rows.push_back(std::move(row));
        }
    }
    node.close();
    return rows;
}

// Run the same query on the independent row-storage Volcano path — the
// correctness baseline the lowered vectorized plan is compared against.
static std::vector<Row> runVolcanoRow(const std::string& sql, const Catalog& cat) {
    Parser p(sql);
    auto stmt = p.parse();
    Binder::bind(stmt, cat);

    std::unordered_map<std::string, std::vector<Row>> table_rows;
    const auto& fm = cat.getTable(stmt.from_table);
    table_rows[stmt.from_table] = CSVLoader::load(fm.filepath, fm.schema);
    if (stmt.join.has_value() && !table_rows.count(stmt.join->join_table)) {
        const auto& jm = cat.getTable(stmt.join->join_table);
        table_rows[stmt.join->join_table] = CSVLoader::load(jm.filepath, jm.schema);
    }
    auto plan = Planner::plan(std::move(stmt), cat, std::move(table_rows));
    std::vector<Row> out;
    plan->open();
    while (Row* r = plan->next()) out.push_back(*r);
    plan->close();
    return out;
}

// Serialize rows for comparison; sorted = order-insensitive (multiset equality).
static std::vector<std::string> serialize(const std::vector<Row>& rows, bool sorted) {
    std::vector<std::string> out;
    for (const auto& row : rows) {
        std::string s;
        for (const auto& v : row) s += v.toString() + "|";
        out.push_back(std::move(s));
    }
    if (sorted) std::sort(out.begin(), out.end());
    return out;
}

// Top-down explain() spine following children()[0].
static std::vector<std::string> vecSpine(const VecPlanNode* root) {
    std::vector<std::string> labels;
    const VecPlanNode* n = root;
    while (n) {
        std::string e = n->explain();
        labels.push_back(e.substr(0, e.find(' ')));
        auto kids = n->children();
        n = kids.empty() ? nullptr : kids[0];
    }
    return labels;
}

// ===== Structural lowering =====

TEST(VecPlanBuilder, SimpleSelectLowersToProjectOverScan) {
    Catalog cat(CATALOG);
    auto plan = buildVec("SELECT team FROM laps", cat);

    EXPECT_EQ(vecSpine(plan.get()),
              (std::vector<std::string>{"VecProject", "VecScan"}));
    ASSERT_EQ(plan->outputSchema().size(), 1);
    EXPECT_EQ(plan->outputSchema().column(0).name, "team");
}

TEST(VecPlanBuilder, FullClauseQueryLowersExactSpine) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT team, COUNT(*) FROM laps WHERE speed > 200 GROUP BY team "
        "HAVING COUNT(*) > 1 ORDER BY team DESC LIMIT 3", cat);

    // WHERE and HAVING both lower to VecFilterNode; position tells them apart.
    EXPECT_EQ(vecSpine(plan.get()), (std::vector<std::string>{
        "VecLimit", "VecProject", "VecSort", "VecFilter",
        "VecHashAggregate", "VecFilter", "VecScan"}));
}

TEST(VecPlanBuilder, JoinLowersToHashJoinWithTwoScanLeavesAndMergedSchema) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);

    // find the join node down the children()[0] chain
    const VecPlanNode* n = plan.get();
    while (n && n->explain().rfind("VecHashJoin", 0) != 0) {
        auto kids = n->children();
        n = kids.empty() ? nullptr : kids[0];
    }
    ASSERT_NE(n, nullptr);
    auto kids = n->children();
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_EQ(vecSpine(kids[0]), (std::vector<std::string>{"VecScan"}));
    EXPECT_EQ(vecSpine(kids[1]), (std::vector<std::string>{"VecScan"}));

    // merged schema stays in fixed [FROM, JOIN] order with slot stamps:
    // narrowed to {lap_id, driver_id} ++ {driver_id, name}
    const Schema& merged = n->outputSchema();
    ASSERT_EQ(merged.size(), 4);
    EXPECT_EQ(merged.column(0).name, "lap_id");
    EXPECT_EQ(merged.column(0).relation_slot, 0);
    EXPECT_EQ(merged.column(3).name, "name");
    EXPECT_EQ(merged.column(3).relation_slot, 1);
}

TEST(VecPlanBuilder, SmallerFromSideBecomesBuildYetSchemaOrderIsLogical) {
    Catalog cat(CATALOG);
    // drivers (3 rows) < laps (5 rows): FROM becomes build, laps becomes probe
    auto plan = buildVec(
        "SELECT drivers.name, laps.lap_id FROM drivers JOIN laps ON drivers.driver_id = laps.driver_id", cat);

    const VecPlanNode* n = plan.get();
    while (n && n->explain().rfind("VecHashJoin", 0) != 0) {
        auto kids = n->children();
        n = kids.empty() ? nullptr : kids[0];
    }
    ASSERT_NE(n, nullptr);

    // children() is {probe, build}: probe must be the larger laps scan
    auto kids = n->children();
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_NE(kids[0]->explain().find("laps"), std::string::npos);
    EXPECT_NE(kids[1]->explain().find("drivers"), std::string::npos);

    // ...while the output schema keeps logical [FROM=drivers, JOIN=laps] order
    const Schema& merged = n->outputSchema();
    EXPECT_EQ(merged.column(0).relation_slot, 0);
    EXPECT_TRUE(merged.hasColumn("name"));
    int name_idx = merged.indexOf("name");
    int lap_id_idx = merged.indexOf("lap_id");
    EXPECT_LT(name_idx, lap_id_idx);  // drivers (FROM) columns come first
}

TEST(VecPlanBuilder, SelfJoinLowersTwoScansOfSameTable) {
    Catalog cat(CATALOG);
    auto plan = buildVec("SELECT l1.id, l2.grp FROM sj l1 JOIN sj l2 ON l1.id = l2.grp", cat);

    const VecPlanNode* n = plan.get();
    while (n && n->explain().rfind("VecHashJoin", 0) != 0) {
        auto kids = n->children();
        n = kids.empty() ? nullptr : kids[0];
    }
    ASSERT_NE(n, nullptr);
    auto kids = n->children();
    ASSERT_EQ(kids.size(), 2u);
    EXPECT_NE(kids[0]->explain().find("sj"), std::string::npos);
    EXPECT_NE(kids[1]->explain().find("sj"), std::string::npos);
}

TEST(VecPlanBuilder, SelectStarJoinProducesFullMergedSchema) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT * FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    // 9 laps columns + 5 drivers columns — nothing narrowed away under SELECT *
    EXPECT_EQ(plan->outputSchema().size(), 14);
}

// ===== Execution equivalence vs the row-storage Volcano baseline =====

TEST(VecPlanBuilder, LoweredPlansMatchVolcanoBaseline) {
    Catalog cat(CATALOG);
    struct Case { std::string sql; bool ordered; };
    const std::vector<Case> corpus = {
        // README benchmark queries
        {"SELECT AVG(speed) FROM laps", false},
        {"SELECT COUNT(*) FROM laps WHERE season = 2025", false},
        {"SELECT team, speed FROM laps WHERE speed > 300", false},
        {"SELECT team, COUNT(*) FROM laps GROUP BY team", false},
        {"SELECT l.team, AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team", false},
        // self-join, both key routings
        {"SELECT l1.id, l2.grp FROM sj l1 JOIN sj l2 ON l1.id = l2.grp", false},
        {"SELECT l1.id FROM sj l1 JOIN sj l2 ON l2.grp = l1.id", false},
        // swapped build side (FROM smaller than JOIN)
        {"SELECT drivers.name, laps.lap_id FROM drivers JOIN laps ON drivers.driver_id = laps.driver_id", false},
        // WHERE + HAVING together (two VecFilters, one pruning hint)
        {"SELECT team, COUNT(*) FROM laps WHERE speed > 200 GROUP BY team HAVING COUNT(*) > 1", false},
        // ORDER BY — output order must match, not just the multiset
        {"SELECT team, COUNT(*) FROM laps GROUP BY team ORDER BY team DESC", true},
        // SELECT * join regression (all 14 columns, values row-identical)
        {"SELECT * FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", false},
        // DISTINCT + LIMIT stack
        {"SELECT DISTINCT team FROM laps LIMIT 2", false},
    };

    for (const auto& c : corpus) {
        auto vec_plan = buildVec(c.sql, cat);
        auto vec_rows = serialize(drainVec(*vec_plan), !c.ordered);
        auto volc_rows = serialize(runVolcanoRow(c.sql, cat), !c.ordered);
        EXPECT_EQ(vec_rows, volc_rows) << c.sql;
    }
}

// ===== Regression: SELECT * + JOIN on the Volcano columnar path =====

// Before Week 18 the join-side scan schema was narrowed by the FROM table's
// column names, silently dropping drivers.name/nationality/age under
// SELECT * — the shared buildScanSchema fixed that.
TEST(VecPlanBuilder, VolcanoColumnarSelectStarJoinKeepsJoinColumns) {
    Catalog cat(CATALOG);
    Parser p("SELECT * FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id");
    auto stmt = p.parse();
    Binder::bind(stmt, cat);

    std::unordered_map<std::string, ColumnarTable> tables = loadColumnar(stmt, cat);
    auto plan = Planner::plan(std::move(stmt), cat, {}, std::move(tables));

    ASSERT_EQ(plan->outputSchema().size(), 14);
    std::vector<Row> rows;
    plan->open();
    while (Row* r = plan->next()) rows.push_back(*r);
    plan->close();
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(rows[0].size(), 14u);
}
