#include <gtest/gtest.h>
#include "planner/vectorized_plan_builder.h"
#include "planner/logical_plan.h"
#include "planner/predicate_pushdown.h"
#include "planner/cardinality_estimator.h"
#include "planner/join_enumeration.h"
#include "planner/cost_model.h"
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
    for (const auto& j : stmt.joins) {
        if (tables.count(j.join_table)) continue;   // self-join: load once
        const auto& jm = cat.getTable(j.join_table);
        tables.emplace(j.join_table,
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
    return VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);
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
                // valueAt, never the typed vector directly: a raw read bypasses
                // the validity mask and turns a SQL NULL into the placeholder
                // underneath it (vec_types.h). Harmless until Week 29, since
                // nothing on this path manufactured a NULL — an outer join does,
                // and this loop claims to mirror main.cc, which reads NULL-aware.
                row.push_back(valueAt(cv, r));
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
    for (const auto& j : stmt.joins) {
        if (table_rows.count(j.join_table)) continue;   // self-join: load once
        const auto& jm = cat.getTable(j.join_table);
        table_rows[j.join_table] = CSVLoader::load(jm.filepath, jm.schema);
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

// ===== Week 22: cost-based build-side selection =====

// Full vectorized pipeline WITH the optimizer passes (pushdown + estimate), so
// the join's children carry estimated_rows and the cost model — not raw table
// size — picks the build side. buildVec above deliberately omits these passes.
static std::unique_ptr<VecPlanNode> buildVecOptimized(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto tables = loadColumnar(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    logical = PredicatePushdown::apply(std::move(logical), cat);
    CardinalityEstimator::estimate(*logical, cat);
    return VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);
}

// Descend children()[0] to the join node (either physical join operator —
// Week 23.5 added VecSimdLoopJoin alongside VecHashJoin, Week 29
// VecLeftHashJoin).
static bool isJoinNode(const VecPlanNode* n) {
    const std::string e = n->explain();
    return e.rfind("VecHashJoin", 0) == 0 || e.rfind("VecSimdLoopJoin", 0) == 0
        || e.rfind("VecLeftHashJoin", 0) == 0;
}

static const VecPlanNode* findJoin(const VecPlanNode* root) {
    const VecPlanNode* n = root;
    while (n && !isJoinNode(n)) {
        auto kids = n->children();
        n = kids.empty() ? nullptr : kids[0];
    }
    return n;
}

// Table read by the build child (children()[1]): descend to its leaf scan and
// return its explain() string, which names the table. The build child may be a
// VecFilter over the scan once a predicate is pushed down.
static std::string buildSideScan(const VecPlanNode* join) {
    const VecPlanNode* build = join->children()[1];  // children() == {probe, build}
    while (!build->children().empty()) build = build->children()[0];
    return build->explain();
}

// laps: 1000 rows, speed DOUBLE in [200,400]. Enough for the estimator to size
// a scan-local filter; execution is not run, so real row count is irrelevant.
static void seedCostStats(Catalog& cat) {
    TableStats laps;
    laps.row_count = 1000;
    ColumnStats speed;
    speed.min_val = Value(200.0);
    speed.max_val = Value(400.0);
    speed.distinct_count = 900;
    speed.null_count = 0;
    laps.columns.emplace("speed", speed);
    cat.setStats("laps", std::move(laps));

    TableStats drivers;   // 20 rows; no per-column stats needed for the choice
    drivers.row_count = 20;
    cat.setStats("drivers", std::move(drivers));
}

// Without a filter, laps (1000) > drivers (20): the smaller JOIN side builds.
TEST(VecPlanBuilder, UnfilteredJoinBuildsSmallerRawSide) {
    Catalog cat(CATALOG);
    seedCostStats(cat);
    auto plan = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_NE(buildSideScan(join).find("drivers"), std::string::npos)
        << "unfiltered: smaller drivers table should build";
}

// A selective WHERE on laps (speed > 399 ≈ 0.5% of 1000 ≈ 5 rows) makes the
// FILTERED laps side smaller than drivers (20) — the build side must flip to
// laps, which the raw-row-count heuristic could never do. This is the Week 22
// checkpoint expressed as a plan-shape assertion.
TEST(VecPlanBuilder, FilteredJoinFlipsBuildSideToFilteredTable) {
    Catalog cat(CATALOG);
    seedCostStats(cat);
    auto plan = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id WHERE laps.speed > 399", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_NE(buildSideScan(join).find("laps"), std::string::npos)
        << "filtered laps (~5 rows) is now smaller than drivers (20) and should build";
}

// Requirement #3: output column order is fixed [FROM=laps (slot 0), JOIN=drivers
// (slot 1)] even though the filter flipped laps onto the physical build side.
// The merged schema must still list every FROM column before every JOIN column.
TEST(VecPlanBuilder, FlippedBuildSideKeepsLogicalSchemaOrder) {
    Catalog cat(CATALOG);
    seedCostStats(cat);
    auto plan = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id WHERE laps.speed > 399", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    // sanity: this really is the flipped case (filtered laps is the build side)
    ASSERT_NE(buildSideScan(join).find("laps"), std::string::npos);

    // every slot-0 (FROM=laps) column precedes every slot-1 (JOIN=drivers) column
    const Schema& merged = join->outputSchema();
    int last_from = -1, first_join = merged.size();
    for (int i = 0; i < merged.size(); ++i) {
        if (merged.column(i).relation_slot == 0) last_from = i;
        else if (merged.column(i).relation_slot == 1 && i < first_join) first_join = i;
    }
    EXPECT_LT(last_from, first_join) << "FROM columns must precede JOIN columns";
    EXPECT_LT(merged.indexOf("lap_id"), merged.indexOf("name"));
}

// Gap 3: with EQUAL row counts, the hash-table memory term must break the tie
// toward the physically narrower side (less footprint). Both tables are seeded
// to 100 rows, but drivers' projected column (name) is far wider than laps'
// two INT columns — so laps builds. This fails under the old size()*8 proxy
// (which sees 2 columns each, a tie, and builds the JOIN side), so it pins the
// real-avg_width fix.
TEST(VecPlanBuilder, EqualRowsBuildNarrowerSideByRealWidth) {
    Catalog cat(CATALOG);
    TableStats laps;                 // 100 rows, two 8-byte INT projected columns
    laps.row_count = 100;
    ColumnStats i8; i8.avg_width = 8.0; i8.distinct_count = 100;
    laps.columns.emplace("lap_id", i8);
    laps.columns.emplace("driver_id", i8);
    cat.setStats("laps", std::move(laps));

    TableStats drivers;              // 100 rows, but a very wide string column
    drivers.row_count = 100;
    ColumnStats id; id.avg_width = 8.0; id.distinct_count = 100;
    ColumnStats name; name.avg_width = 100.0; name.distinct_count = 100;
    drivers.columns.emplace("driver_id", id);
    drivers.columns.emplace("name", std::move(name));
    cat.setStats("drivers", std::move(drivers));

    auto plan = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_NE(buildSideScan(join).find("laps"), std::string::npos)
        << "equal rows: the narrower laps side should build, not wide drivers";
}

// ===== Week 23: explainability — estimates survive lowering =====

// Collect every node of a physical tree (any child order).
static void collectAllVec(const VecPlanNode* n, std::vector<const VecPlanNode*>& out) {
    out.push_back(n);
    for (const VecPlanNode* c : n->children()) collectAllVec(c, out);
}

// Lowering consumes the logical tree, so EXPLAIN ANALYZE can only compare
// estimates against actuals if each physical node inherits its logical
// counterpart's estimated_rows. Root must match exactly; every node ≥ 0.
TEST(VecPlanBuilder, EstimatesSurviveLowering) {
    Catalog cat(CATALOG);
    seedCostStats(cat);

    // inline buildVecOptimized so the logical root estimate can be captured
    Parser parser("SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
                  "ON laps.driver_id = drivers.driver_id WHERE laps.speed > 300");
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto tables = loadColumnar(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    logical = PredicatePushdown::apply(std::move(logical), cat);
    CardinalityEstimator::estimate(*logical, cat);
    double root_est = logical->estimated_rows;
    ASSERT_GE(root_est, 0.0) << "estimator must have stamped the logical root";

    auto plan = VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);

    EXPECT_DOUBLE_EQ(plan->estimated_rows, root_est);
    std::vector<const VecPlanNode*> nodes;
    collectAllVec(plan.get(), nodes);
    for (const VecPlanNode* n : nodes) {
        EXPECT_GE(n->estimated_rows, 0.0) << "unstamped node: " << n->explain();
    }
}

// --no-optimize skips the estimator entirely: the -1 sentinel must survive
// lowering unchanged so EXPLAIN hides the est= column instead of printing -1.
TEST(VecPlanBuilder, UnoptimizedLoweringLeavesEstimatesUnset) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id WHERE laps.speed > 300", cat);

    std::vector<const VecPlanNode*> nodes;
    collectAllVec(plan.get(), nodes);
    for (const VecPlanNode* n : nodes) {
        EXPECT_DOUBLE_EQ(n->estimated_rows, -1.0) << n->explain();
    }
}

// ===== Week 23: explainability — the join names its costed decision =====

// The two hashJoinCost values exist only inside lowering; the builder must
// hand the chosen/alternative costs and build side to the join node, and
// explain() must show them. Reuses the Week 22 flip scenario so the shown
// build side is the one only the cost model could pick.
TEST(VecPlanBuilder, JoinExplainShowsCostDecision) {
    Catalog cat(CATALOG);
    seedCostStats(cat);
    auto plan = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id WHERE laps.speed > 399", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    std::string e = join->explain();
    EXPECT_NE(e.find("build=laps"), std::string::npos) << e;
    EXPECT_NE(e.find("cost="), std::string::npos) << e;
    EXPECT_NE(e.find("alt="), std::string::npos) << e;
}

// Without the estimator the builder falls back to raw table sizes — the
// pre-Week-22 heuristic, not an optimizer decision — so explain() must not
// claim a cost-based choice (--no-optimize output stays clean).
TEST(VecPlanBuilder, BareJoinExplainHasNoCostDecision) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->explain().find("cost="), std::string::npos);
}

// ===== Week 23.5: cost-based join algorithm selection =====

// Stats that make the SIMD choice decisive rather than marginal: drivers is a
// 4-row build side, so simdLoopJoinCost beats hashJoinCost for any plausible
// calibration of CPU_SIMD_COMPARE (< 0.25), and the tests survive Stage 2's
// constant tuning.
static void seedSimdStats(Catalog& cat) {
    TableStats laps;
    laps.row_count = 1000;
    ColumnStats speed;
    speed.min_val = Value(200.0);
    speed.max_val = Value(400.0);
    speed.distinct_count = 900;
    speed.null_count = 0;
    laps.columns.emplace("speed", speed);
    cat.setStats("laps", std::move(laps));

    TableStats drivers;   // tiny INT-keyed build side — prime SIMD territory
    drivers.row_count = 4;
    cat.setStats("drivers", std::move(drivers));
}

// The Week 23.5 checkpoint as a plan-shape assertion: a tiny INT-keyed build
// side must lower to the SIMD loop join, and explain() must name both the
// chosen algorithm and the rejected hash alternative.
TEST(VecPlanBuilder, TinyIntKeyBuildSelectsSimdLoopJoin) {
    Catalog cat(CATALOG);
    seedSimdStats(cat);
    auto plan = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id WHERE laps.speed > 300", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    std::string e = join->explain();
    EXPECT_EQ(e.rfind("VecSimdLoopJoin", 0), 0u) << e;
    EXPECT_NE(e.find("algo=simd"), std::string::npos) << e;
    EXPECT_NE(e.find("hash="), std::string::npos) << e;   // rejected alternative shown
}

// A large build side prices the quadratic probe term out — hash must stay,
// and the explain records that the algorithm choice went to hash.
TEST(VecPlanBuilder, LargeIntKeyBuildKeepsHashJoin) {
    Catalog cat(CATALOG);
    TableStats laps;
    laps.row_count = 100000;
    cat.setStats("laps", std::move(laps));
    TableStats drivers;
    drivers.row_count = 5000;
    cat.setStats("drivers", std::move(drivers));

    auto plan = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    std::string e = join->explain();
    EXPECT_EQ(e.rfind("VecHashJoin", 0), 0u) << e;
    EXPECT_NE(e.find("algo=hash"), std::string::npos) << e;
}

// STRING join keys are ineligible regardless of build size — ColumnVector
// carries decoded strings, which the flat int64 key buffer cannot represent —
// so no algorithm choice is live and explain() must not print one.
TEST(VecPlanBuilder, StringKeyKeepsHashJoinEvenWhenTiny) {
    Catalog cat(CATALOG);
    TableStats laps;
    laps.row_count = 1000;
    cat.setStats("laps", std::move(laps));
    TableStats drivers;
    drivers.row_count = 5;
    cat.setStats("drivers", std::move(drivers));

    auto plan = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.team = drivers.team", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    std::string e = join->explain();
    EXPECT_EQ(e.rfind("VecHashJoin", 0), 0u) << e;
    EXPECT_EQ(e.find("algo="), std::string::npos) << e;
}

// SIMD selection is an optimizer decision: without the estimator (--no-optimize)
// even a 3-row build side must keep the pre-Week-22 hash lowering.
TEST(VecPlanBuilder, UnoptimizedLoweringNeverSelectsSimd) {
    Catalog cat(CATALOG);
    auto plan = buildVec(   // buildVec deliberately omits pushdown + estimator
        "SELECT drivers.name, laps.lap_id FROM drivers JOIN laps "
        "ON drivers.driver_id = laps.driver_id", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->explain().rfind("VecHashJoin", 0), 0u) << join->explain();
}

// Result preservation (invariant #11): the SIMD-selected plan returns exactly
// what the row-storage Volcano baseline returns — even though the seeded
// estimates (1000-row laps) wildly disagree with the real 5-row test table.
// Bad estimates may cost performance, never correctness.
TEST(VecPlanBuilder, SimdSelectedPlanMatchesVolcanoBaseline) {
    Catalog cat(CATALOG);
    seedSimdStats(cat);
    const std::string sql =
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id WHERE laps.speed > 300";

    auto plan = buildVecOptimized(sql, cat);
    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    ASSERT_EQ(join->explain().rfind("VecSimdLoopJoin", 0), 0u) << join->explain();

    auto vec_rows  = serialize(drainVec(*plan), true);
    auto volc_rows = serialize(runVolcanoRow(sql, cat), true);
    EXPECT_EQ(vec_rows, volc_rows);
}

// ===== --no-optimize fallback is genuinely row-count-only (audit N5) =====

// Without estimates (buildVec never runs the estimator) the build side must be
// chosen by raw row count alone. A huge avg_width on the smaller table must
// NOT flip the decision — widths are a Week 22 cost-model input, and the
// fallback claims to reproduce the pre-Week-22 row-count heuristic.
TEST(VecPlanBuilder, NoOptimizeFallbackIsRowCountOnly) {
    Catalog cat(CATALOG);

    TableStats drivers_ts;
    drivers_ts.row_count = 3;
    ColumnStats name;
    name.avg_width = 5000.0;   // absurd width; must be ignored by the fallback
    drivers_ts.columns.emplace("name", name);
    cat.setStats("drivers", std::move(drivers_ts));

    auto plan = buildVec(
        "SELECT laps.team, drivers.name FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id",
        cat);
    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    // drivers (3 rows) < laps (5 rows): row count says drivers builds
    EXPECT_NE(buildSideScan(join).find("drivers"), std::string::npos)
        << buildSideScan(join);
}

// A DOUBLE join key is SIMD-ineligible (bitwise equality on doubles is a
// trap): even with a tiny build side that would otherwise favor the loop
// join, lowering must fall back to the hash join. (STRING keys are covered
// by the harness's w23_5_hash_string_key.)
TEST(VecPlanBuilder, DoubleKeyJoinFallsBackToHashJoin) {
    Catalog cat(CATALOG);
    seedCostStats(cat);
    auto plan = buildVecOptimized(
        "SELECT a.lap_id FROM laps a JOIN laps b ON a.speed = b.speed", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->explain().rfind("VecHashJoin", 0), 0u) << join->explain();
}

// rowWidth()'s per-column fallback: a table WITH stats whose referenced
// columns are missing from the stats map must count 8 bytes per missing
// column, not 0. laps' two referenced columns (16B assumed) vs drivers' two
// stated columns (4B) at equal row counts: drivers must build. A 0-byte
// fallback would make laps look free and flip the decision.
TEST(VecPlanBuilder, RowWidthFallsBackPerMissingColumn) {
    Catalog cat(CATALOG);

    TableStats laps;                       // hasStats true, no column entries
    laps.row_count = 20;
    cat.setStats("laps", std::move(laps));

    TableStats drivers;
    drivers.row_count = 20;
    ColumnStats name;
    name.avg_width = 2.0;
    drivers.columns.emplace("name", name);
    ColumnStats driver_id;
    driver_id.avg_width = 2.0;
    drivers.columns.emplace("driver_id", driver_id);
    cat.setStats("drivers", std::move(drivers));

    auto plan = buildVecOptimized(
        "SELECT laps.team, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id", cat);

    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_NE(buildSideScan(join).find("drivers"), std::string::npos)
        << buildSideScan(join);
}


// ===== Multi-way + multi-key lowering (Week 27) =====
//
// The logical layer has built arbitrary join trees since Week 26; Week 27 lowers
// them. Nothing in lowerNode() is 3-relation-specific — the JOIN case recurses
// through children[0] — so these tests exist to prove the recursion and the
// slot-exact key resolution around it, not a special case.

// Count join operators anywhere in the physical tree (a join's probe child is
// another join in a left-deep plan, so the children()[0] spine is not enough).
static int countJoinNodes(const VecPlanNode* n) {
    if (!n) return 0;
    int total = isJoinNode(n) ? 1 : 0;
    for (const VecPlanNode* c : n->children()) total += countJoinNodes(c);
    return total;
}

TEST(VecPlanBuilder, ThreeWayJoinLowersToTwoJoins) {
    Catalog cat(CATALOG);
    auto plan = buildVec("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id "
                         "JOIN sj c ON b.grp = c.id", cat);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(countJoinNodes(plan.get()), 2);
}

// Four relations, to prove the recursion rather than a hardcoded depth.
TEST(VecPlanBuilder, FourWayJoinLowersToThreeJoins) {
    Catalog cat(CATALOG);
    auto plan = buildVec("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id "
                         "JOIN sj c ON b.grp = c.id JOIN sj d ON c.grp = d.id", cat);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(countJoinNodes(plan.get()), 3);
}

TEST(VecPlanBuilder, MultiKeyJoinLowersWithBothKeys) {
    Catalog cat(CATALOG);
    auto plan = buildVec("SELECT a.val FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp", cat);
    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    // both keys rendered, in written order — a dropped key is invisible in the
    // row count whenever the first key already implies the second
    EXPECT_NE(join->explain().find("id = id AND grp = grp"), std::string::npos)
        << join->explain();
}

// A composite key cannot live in the SIMD operator's flat int64 buffer, so the
// planner must decline it there and lower to the hash join — even on INT keys
// with a tiny build side, which is precisely when SIMD would otherwise win.
TEST(VecPlanBuilder, MultiKeyJoinDeclinesSimd) {
    Catalog cat(CATALOG);
    auto plan = buildVecOptimized(
        "SELECT a.val FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp", cat);
    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->explain().rfind("VecHashJoin", 0), 0u) << join->explain();
}

// THE slot hazard. The third join's key is `team` at relation slot 1, and the
// left input's MERGED schema holds `team` at slot 0 first — so a bare-name
// lookup silently joins on the wrong relation's column and returns plausible
// rows. Compared against the row/Volcano baseline for the same shape is
// impossible (Volcano has no multi-way), so the expected count is computed from
// the CSVs directly.
TEST(VecPlanBuilder, ThreeWayJoinResolvesAKeyByRelationSlotNotName) {
    Catalog cat(CATALOG);
    auto plan = buildVecOptimized(
        "SELECT l2.team FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.driver_id "
        "JOIN drivers d ON l2.team = d.team", cat);
    auto rows = drainVec(*plan);

    const auto& lm = cat.getTable("laps");
    const auto& dm = cat.getTable("drivers");
    auto laps = CSVLoader::load(lm.filepath, lm.schema);
    auto drivers = CSVLoader::load(dm.filepath, dm.schema);
    const int lap_id = lm.schema.indexOf("lap_id"), driver_id = lm.schema.indexOf("driver_id");
    const int l_team = lm.schema.indexOf("team"), d_team = dm.schema.indexOf("team");
    size_t expected = 0;
    for (const Row& a : laps)
        for (const Row& b : laps) {
            if (!(a[lap_id] == b[driver_id])) continue;
            for (const Row& d : drivers) if (b[l_team] == d[d_team]) ++expected;
        }
    // joining l1.team instead of l2.team gives a different, plausible count
    EXPECT_EQ(rows.size(), expected);
}

// The defect Task 1 exists to prevent is invisible in a plan that renders a key
// as a bare `column = column`: joining laps.team instead of drivers.team prints
// exactly the same string. --explain is the debugging surface Week 28's
// enumeration will be read through, so an ambiguous name must carry its slot.
TEST(VecPlanBuilder, ExplainQualifiesAJoinKeyThatIsAmbiguousOnTheProbeSchema) {
    Catalog cat(CATALOG);
    auto plan = buildVecOptimized(
        "SELECT l2.team FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.driver_id "
        "JOIN drivers d ON l2.team = d.team", cat);

    // the upper join's probe schema holds `team` at slot 0 (l1) and slot 1 (l2)
    const VecPlanNode* top = findJoin(plan.get());
    ASSERT_NE(top, nullptr);
    EXPECT_NE(top->explain().find("team@1 = team"), std::string::npos) << top->explain();
}

// ...and a name that is unique on the probe schema stays bare, so every existing
// single-join --explain string is byte-identical.
TEST(VecPlanBuilder, ExplainLeavesAnUnambiguousJoinKeyUnqualified) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_NE(join->explain().find("[driver_id = driver_id]"), std::string::npos)
        << join->explain();
}

// Residual ON conjuncts reach the same predicate assignment as WHERE conjuncts:
// a single-relation one lands on its own scan, below the join.
TEST(VecPlanBuilder, SingleRelationResidualOnConjunctIsPushedToItsScan) {
    Catalog cat(CATALOG);
    auto plan = buildVecOptimized(
        "SELECT laps.team FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id AND drivers.age > 30", cat);
    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    // the drivers side is a filter over its scan, not a bare scan
    bool filtered = false;
    for (const VecPlanNode* side : join->children()) {
        if (side->explain().rfind("VecFilter", 0) == 0) filtered = true;
    }
    EXPECT_TRUE(filtered) << join->children()[0]->explain() << " / " << join->children()[1]->explain();
}

// The refusal must not have narrowed what already worked.
TEST(VecPlanBuilder, SingleKeySingleJoinStillLowers) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    ASSERT_NE(plan, nullptr);
}

// ===== Week 28: per-relation row width on a multi-relation join input =====

// Week 27 refused to compute a width for a multi-relation join input and fell
// back to columns * 8.0, because leafScanTable() names relation 0 for a whole
// subtree — so `team` on a laps/drivers subtree took laps' avg_width for BOTH
// copies. Week 28's join enumeration compares orderings whose whole difference
// is intermediate width, which makes that placeholder the thing being measured.
//
// This fixture distinguishes all three answers: the shipped test data gives
// laps.team avg_width 7.2 and drivers.team 7.333, so the real per-relation sum
// differs from the uniform proxy (8.0 each) AND from the pre-Week-27
// attribution error (7.2 twice).
TEST(VecPlanBuilder, MultiRelationRowWidthSumsPerRelationStats) {
    Catalog cat(CATALOG);
    for (const char* t : {"laps", "drivers"}) {
        const auto& m = cat.getTable(t);
        cat.setStats(t, TableStats::compute(CSVLoader::load(m.filepath, m.schema), m.schema));
    }

    const std::string sql =
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.team = d.team "
        "JOIN drivers d2 ON l.driver_id = d2.driver_id";

    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto tables = loadColumnar(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    logical = PredicatePushdown::apply(std::move(logical), cat);
    logical = JoinEnumeration::apply(std::move(logical), cat);
    CardinalityEstimator::estimate(*logical, cat);

    // capture the top join's inputs before lowering consumes the logical tree
    const LogicalPlanNode* n = logical.get();
    while (n && n->type != LogicalNodeType::JOIN) n = n->children[0].get();
    ASSERT_NE(n, nullptr);
    const auto* top = static_cast<const LogicalJoin*>(n);
    const double from_rows = top->children[0]->estimated_rows;
    const double join_rows = top->children[1]->estimated_rows;

    // the width the fix must produce: every column read from ITS OWN relation's
    // stats, keyed by the binder slot stamped on the merged schema
    auto widthOf = [&cat](const Schema& s, const std::unordered_map<int, std::string>& m) {
        double w = 0.0;
        for (const ColumnDef& c : s.columns()) {
            auto it = m.find(c.relation_slot);
            const TableStats& ts = cat.getStats(it->second);
            auto cs = ts.columns.find(c.name);
            w += (cs != ts.columns.end()) ? cs->second.avg_width : 8.0;
        }
        return w;
    };
    // slot 0 = laps (l), slot 1 = drivers (d), slot 2 = drivers (d2)
    const std::unordered_map<int, std::string> all_slots{
        {0, "laps"}, {1, "drivers"}, {2, "drivers"}};
    const double from_w = widthOf(top->children[0]->output_schema, all_slots);
    const double join_w = widthOf(top->children[1]->output_schema, all_slots);

    // the multi-relation side must NOT be the uniform proxy, or this fixture
    // proves nothing
    const LogicalPlanNode* multi = top->children[0]->children.empty()
        ? top->children[1].get() : top->children[0].get();
    ASSERT_NE(multi->output_schema.size() * 8.0,
              (multi == top->children[0].get() ? from_w : join_w));

    auto plan = VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);
    const VecPlanNode* j = plan.get();
    while (j && j->explain().rfind("VecHashJoin", 0) != 0) {
        auto kids = j->children();
        j = kids.empty() ? nullptr : kids[0];
    }
    ASSERT_NE(j, nullptr);
    const std::string d = j->explain();

    // both costed assignments appear as cost= and alt=; each must be the value
    // the real per-relation widths produce
    ASSERT_NE(d.find("cost="), std::string::npos);
    ASSERT_NE(d.find("(alt="), std::string::npos);
    const double cost = std::stod(d.substr(d.find("cost=") + 5));
    const double alt  = std::stod(d.substr(d.find("(alt=") + 5));
    const double from_builds = hashJoinCost(from_rows, from_w, join_rows);
    const double join_builds = hashJoinCost(join_rows, join_w, from_rows);
    const double lo = std::min(from_builds, join_builds);
    const double hi = std::max(from_builds, join_builds);
    // explain prints costs at setprecision(0), so compare within a unit
    EXPECT_NEAR(cost, lo, 1.0);
    EXPECT_NEAR(alt,  hi, 1.0);
}

// ===== Week 28: the pruning hint under a reordered join tree =====

// Collect every node in a lowered plan (both join branches).
static void collectVec(const VecPlanNode* n, std::vector<const VecPlanNode*>& out) {
    if (!n) return;
    out.push_back(n);
    for (const VecPlanNode* c : n->children()) collectVec(c, out);
}

// Build `Filter(lap_id = 1) over Join(laps, drivers)` where the join's MERGED
// schema stamps its left block with `leftmost_slot` — the shape join enumeration
// produces and no written-order tree can. Returns the laps scan's explain string.
static std::string lapsScanExplain(const Catalog& cat, int leftmost_slot) {
    Schema laps_schema({ColumnDef{"lap_id", TypeId::INT, 0, false},
                        ColumnDef{"driver_id", TypeId::INT, 0, false}});
    Schema drivers_schema({ColumnDef{"driver_id", TypeId::INT, 0, false}});
    auto laps = std::make_unique<LogicalScan>("laps", laps_schema);
    auto drivers = std::make_unique<LogicalScan>("drivers", drivers_schema);

    // merged: [left block stamped leftmost_slot] ++ [right block stamped join_slot]
    std::vector<ColumnDef> merged;
    for (ColumnDef c : laps_schema.columns()) { c.relation_slot = leftmost_slot; merged.push_back(c); }
    for (ColumnDef c : drivers_schema.columns()) { c.relation_slot = 1; merged.push_back(c); }
    // bottom join: from_slot 0 addresses the leaf's own schema (JoinKey contract)
    std::vector<JoinKey> keys{JoinKey{"driver_id", "driver_id", 0}};
    auto join = std::make_unique<LogicalJoin>(std::move(laps), std::move(drivers),
                                              std::move(keys), 1, Schema(merged));

    // a prunable scan-local-looking conjunct: ColumnRef(slot 0) op Literal is
    // exactly what ChunkPruner::collectSimplePredicates accepts
    auto col = std::make_unique<ColumnRef>();
    col->column_name = "lap_id";
    col->relation_slot = 0;
    auto pred = std::make_unique<BinaryExpr>();
    pred->op = "=";
    pred->left = std::move(col);
    pred->right = std::make_unique<Literal>(Value(int64_t(1)));
    std::unique_ptr<LogicalPlanNode> root =
        std::make_unique<LogicalFilter>(std::move(join), std::move(pred));

    std::unordered_map<std::string, ColumnarTable> tables;
    for (const char* t : {"laps", "drivers"}) {
        const auto& m = cat.getTable(t);
        tables.emplace(t, CSVToColumnar::convert(CSVLoader::load(m.filepath, m.schema), m.schema));
    }
    auto plan = VectorizedPlanBuilder::build(std::move(root), std::move(tables), cat);

    std::vector<const VecPlanNode*> nodes;
    collectVec(plan.get(), nodes);
    for (const VecPlanNode* n : nodes) {
        if (n->explain().rfind("VecScan [laps", 0) == 0) return n->explain();
    }
    return "";
}

// ChunkPruner treats a relation_slot < 1 ref in a scan hint as scan-local, which
// is only true of the leftmost scan while the leftmost relation IS relation 0.
// Join enumeration can put any relation at the bottom of the spine, and the hint
// only ever descends children[0] — so a slot-0 ref would then prune ANOTHER
// table's chunks on relation 0's value: rows vanish, with no error anywhere.
//
// Unreachable from the query surface (post-pushdown a residual above a join holds
// no ColumnRef-op-Literal conjunct), which is exactly why it needs a hand-built
// test: deleting the guard breaks nothing else in the suite.
TEST(VecPlanBuilder, PruningHintIsWithheldWhenTheLeftmostRelationIsNotSlotZero) {
    Catalog cat(CATALOG);
    // control: relation 0 leads, so the hint is scan-local and must be attached
    EXPECT_NE(lapsScanExplain(cat, /*leftmost_slot=*/0).find("pruning=on"),
              std::string::npos);
    // reordered: relation 2 leads, so a slot-0 ref in the hint does not describe
    // this scan's table and the hint must not reach it
    EXPECT_EQ(lapsScanExplain(cat, /*leftmost_slot=*/2).find("pruning"),
              std::string::npos);
}

// ===== Week 29: outer-join lowering =====

// The SIMD loop join is an INNER equi-join — its probe loop emits matches and has
// no unmatched path — so an outer join must never select it, even in the exact
// configuration that selects it for an inner join. An ineligible algorithm is not
// a fallback; the hash join is always correct.
TEST(VecPlanBuilder, OuterJoinNeverSelectsTheSimdLoopJoin) {
    Catalog cat(CATALOG);
    seedSimdStats(cat);
    auto inner = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers "
        "ON laps.driver_id = drivers.driver_id WHERE laps.speed > 300", cat);
    ASSERT_NE(findJoin(inner.get()), nullptr);
    ASSERT_EQ(findJoin(inner.get())->explain().rfind("VecSimdLoopJoin", 0), 0u);

    auto outer = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps LEFT JOIN drivers "
        "ON laps.driver_id = drivers.driver_id WHERE laps.speed > 300", cat);
    const VecPlanNode* join = findJoin(outer.get());
    ASSERT_NE(join, nullptr);
    const std::string e = join->explain();
    EXPECT_EQ(e.rfind("VecLeftHashJoin", 0), 0u) << e;
    EXPECT_EQ(e.find("algo="), std::string::npos) << e;   // no algorithm choice was made
}

// The build side is forced, not costed, so explain() must not print an (alt=)
// that was never an option — the same rule that keeps cost= off --no-optimize.
TEST(VecPlanBuilder, OuterJoinReportsAForcedBuildSide) {
    Catalog cat(CATALOG);
    seedSimdStats(cat);
    auto plan = buildVecOptimized(
        "SELECT laps.lap_id, drivers.name FROM laps LEFT JOIN drivers "
        "ON laps.driver_id = drivers.driver_id", cat);
    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    const std::string e = join->explain();
    EXPECT_NE(e.find("build=drivers"), std::string::npos) << e;
    EXPECT_NE(e.find("outer: the preserved side must probe"), std::string::npos) << e;
    EXPECT_EQ(e.find("(alt="), std::string::npos) << e;
    // both keys are INT, so SIMD would have been in the running for an inner
    // join. The suffix has to say the algorithm was not a choice either, or a
    // reader cannot tell "ineligible" from "lost on cost" — the `algo=` clause
    // is suppressed for an outer join.
    EXPECT_NE(e.find("hash only"), std::string::npos) << e;
}

// The residual travels from LogicalJoin::on_residual into the operator, and the
// rows prove it filters the match test rather than the result: every preserved
// row survives.
TEST(VecPlanBuilder, OuterJoinCarriesItsOnResidualIntoTheOperator) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT drivers.driver_id, laps.lap_id FROM drivers LEFT JOIN laps "
        "ON drivers.driver_id = laps.driver_id AND laps.speed > 100000", cat);
    const VecPlanNode* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_NE(join->explain().find("residual="), std::string::npos) << join->explain();

    auto rows = drainVec(*plan);
    // no lap can pass speed > 100000, so every driver comes back null-extended
    Catalog cat2(CATALOG);
    const auto& dm = cat2.getTable("drivers");
    const size_t drivers_rows = CSVLoader::load(dm.filepath, dm.schema).size();
    ASSERT_EQ(rows.size(), drivers_rows);
    for (const Row& r : rows) EXPECT_TRUE(r[1].isNull());
}

// The leftmost VecScan, following children()[0] to the bottom of the spine —
// the only scan a pruning hint can reach.
static const VecPlanNode* leftmostScan(const VecPlanNode* root) {
    const VecPlanNode* n = root;
    while (n && !n->children().empty()) n = n->children()[0];
    return n;
}

// Week 29. Predicate pushdown now leaves a single-slot `ColumnRef op Literal`
// conjunct in the residual above an outer join (it may not be pushed onto the
// null-supplying side), and that residual descends to the PRESERVED side's scan
// as its zone-map pruning hint. So a predicate over `laps` reaches `drivers`'
// scan, where the only thing standing between it and the wrong table's zone maps
// is ChunkPruner's `relation_slot < 1` test in another file. The answer is right
// either way today; the guard exists so the safety argument is local, because the
// failure mode is silent row loss on the side the whole feature preserves.
TEST(VecPlanBuilder, OuterJoinWithholdsAPruningHintOverTheNullSupplyingSide) {
    Catalog cat(CATALOG);
    auto plan = buildVecOptimized(
        "SELECT COUNT(*) FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
        "WHERE l.season = 2024", cat);
    const VecPlanNode* scan = leftmostScan(plan.get());
    ASSERT_NE(scan, nullptr);
    EXPECT_NE(scan->explain().find("VecScan [drivers"), std::string::npos) << scan->explain();
    EXPECT_EQ(scan->explain().find("pruning=on"), std::string::npos)
        << "the hint over the null-supplying relation must not reach the preserved "
           "side's scan: " << scan->explain();
}

// ...and the guard is scoped to outer joins, so an INNER join's hint is
// unchanged — including the mixed-slot un-pushed WHERE that the Phase 4
// benchmark measures (buildVec skips pushdown, which is the --no-optimize shape).
TEST(VecPlanBuilder, InnerJoinKeepsItsPruningHintUnchanged) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE l.season = 2024 AND d.age > 30", cat);
    const VecPlanNode* scan = leftmostScan(plan.get());
    ASSERT_NE(scan, nullptr);
    EXPECT_NE(scan->explain().find("VecScan [laps"), std::string::npos) << scan->explain();
    EXPECT_NE(scan->explain().find("pruning=on"), std::string::npos) << scan->explain();
}

// A preserved-side-only hint still descends: withholding it would cost an outer
// join the pruning it is entitled to, and sigma_p(R) LEFTJOIN S is equivalent.
TEST(VecPlanBuilder, OuterJoinKeepsAPreservedSideOnlyPruningHint) {
    Catalog cat(CATALOG);
    auto plan = buildVec(
        "SELECT COUNT(*) FROM laps l LEFT JOIN drivers d ON l.driver_id = d.driver_id "
        "WHERE l.season = 2024", cat);
    const VecPlanNode* scan = leftmostScan(plan.get());
    ASSERT_NE(scan, nullptr);
    EXPECT_NE(scan->explain().find("VecScan [laps"), std::string::npos) << scan->explain();
    EXPECT_NE(scan->explain().find("pruning=on"), std::string::npos) << scan->explain();
}
