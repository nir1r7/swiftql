#include <gtest/gtest.h>
#include "planner/validator.h"
#include "planner/planner.h"
#include "parser/parser.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"


// ===== validator tests =====

TEST(ValidatorTest, ValidQuery) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team FROM laps WHERE season = 2025");
    auto stmt = p.parse();
    EXPECT_NO_THROW(Validator::validate(stmt, catalog));
}

TEST(ValidatorTest, MissingTable) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team FROM bogus_table");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, MissingColumn) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT bogus_col FROM laps");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, HavingWithoutGroupBy) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team FROM laps HAVING team = 'Ferrari'");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, JoinOnColumnMissingInLeftTable) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team FROM laps JOIN drivers ON laps.bogus_col = drivers.driver_id");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, JoinOnColumnMissingInRightTable) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team FROM laps JOIN drivers ON laps.driver_id = drivers.bogus_col");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, JoinOnColumnValidBothSides) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id");
    auto stmt = p.parse();
    EXPECT_NO_THROW(Validator::validate(stmt, catalog));
}

TEST(ValidatorTest, SelectNonAggColWithoutGroupBy) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team, COUNT(*) FROM laps");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, SelectNonAggColWithWrongGroupBy) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team, COUNT(*) FROM laps GROUP BY season");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, SelectNonAggColWithCorrectGroupBy) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team, COUNT(*) FROM laps GROUP BY team");
    auto stmt = p.parse();
    EXPECT_NO_THROW(Validator::validate(stmt, catalog));
}

TEST(ValidatorTest, SumOnStringColumn) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT SUM(team) FROM laps");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, AvgOnStringColumn) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT AVG(name) FROM drivers");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, SumOnNumericColumn) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT SUM(speed) FROM laps");
    auto stmt = p.parse();
    EXPECT_NO_THROW(Validator::validate(stmt, catalog));
}

// ===== planner tests =====

#include "planner/binder.h"

// Bind + plan, mirroring the real pipeline (main.cc binds before planning).
static std::unique_ptr<PlanNode> bindAndPlan(const std::string& sql, const Catalog& catalog) {
    Parser p(sql);
    auto stmt = p.parse();
    Binder::bind(stmt, catalog);

    std::unordered_map<std::string, std::vector<Row>> table_rows;
    const auto& meta = catalog.getTable(stmt.from_table);
    table_rows[stmt.from_table] = CSVLoader::load(meta.filepath, meta.schema);
    for (const auto& j : stmt.joins) {
        if (table_rows.count(j.join_table)) continue;   // self-join: load once
        const auto& jmeta = catalog.getTable(j.join_table);
        table_rows[j.join_table] = CSVLoader::load(jmeta.filepath, jmeta.schema);
    }
    return Planner::plan(std::move(stmt), catalog, std::move(table_rows));
}

// Top-down explain() spine following children()[0].
static std::vector<std::string> planSpine(const PlanNode* root) {
    std::vector<std::string> labels;
    for (const PlanNode* n = root; n; ) {
        std::string e = n->explain();
        labels.push_back(e.substr(0, e.find(' ')));
        auto kids = n->children();
        n = kids.empty() ? nullptr : kids[0];
    }
    return labels;
}

TEST(PlannerTest, BuildsSeqScan) {
    Catalog catalog("../tests/data/test_catalog.json");
    auto plan = bindAndPlan("SELECT team FROM laps", catalog);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(planSpine(plan.get()), (std::vector<std::string>{"Project", "SeqScan"}));
    ASSERT_EQ(plan->outputSchema().size(), 1);
    EXPECT_EQ(plan->outputSchema().column(0).name, "team");
}

TEST(PlannerTest, BuildsJoinPlan) {
    Catalog catalog("../tests/data/test_catalog.json");
    auto plan = bindAndPlan(
        "SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", catalog);
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(planSpine(plan.get()), (std::vector<std::string>{"Project", "HashJoin", "SeqScan"}));

    // join keys routed by binder slot, one per side
    const PlanNode* join = plan->children()[0];
    EXPECT_NE(join->explain().find("driver_id = driver_id"), std::string::npos);
    ASSERT_EQ(join->children().size(), 2u);

    // projected output is just laps.team
    ASSERT_EQ(plan->outputSchema().size(), 1);
    EXPECT_EQ(plan->outputSchema().column(0).name, "team");
    EXPECT_EQ(plan->outputSchema().column(0).relation_slot, 0);

    // the plan actually executes: 5 laps x matching drivers
    plan->open();
    int rows = 0;
    while (plan->next()) ++rows;
    plan->close();
    EXPECT_GT(rows, 0);
}
// ===== Phase 4 audit fixes: validation gaps =====

// N1: a HAVING column that exists nowhere must fail validation, not execution.
TEST(ValidatorTest, HavingUnknownColumnRejected) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team, COUNT(*) FROM laps GROUP BY team HAVING bogus > 5");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

// M1 guard: an ORDER BY aggregate with no GROUP BY and no SELECT aggregate.
TEST(ValidatorTest, OrderByAggregateWithoutGroupByRejected) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team FROM laps ORDER BY COUNT(*)");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}


// ===== Week 24: grouped-reference validation over expressions =====

TEST(ValidatorTest, NonGroupedColumnInsideExpressionRejected) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT speed + AVG(speed) FROM laps GROUP BY team");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, GroupedColumnInsideExpressionAllowed) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT season + 1, COUNT(*) FROM laps GROUP BY season");
    auto stmt = p.parse();
    EXPECT_NO_THROW(Validator::validate(stmt, catalog));
}

TEST(ValidatorTest, BareColumnWithGroupByButNoAggregatesRejected) {
    // pre-Week-24 this slipped validation and died at runtime with
    // "Column not found"; now it is a clean plan-time error
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT lap_id FROM laps GROUP BY team");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}


// ===== Multi-way vs multi-key on the Volcano path (Week 27) =====

// First node whose explain() starts with `prefix`, in preorder.
static const PlanNode* findNode(const PlanNode* node, const std::string& prefix) {
    if (!node) return nullptr;
    if (node->explain().rfind(prefix, 0) == 0) return node;
    for (const PlanNode* child : node->children()) {
        if (const PlanNode* hit = findNode(child, prefix)) return hit;
    }
    return nullptr;
}

// Expected row count of `sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp`,
// computed straight off the CSV so the test states the answer independently of
// the engine that is being tested.
static int countMultiKeyMatches() {
    Catalog catalog("../tests/data/test_catalog.json");
    const auto& meta = catalog.getTable("sj");
    auto rows = CSVLoader::load(meta.filepath, meta.schema);
    const int id = meta.schema.indexOf("id"), grp = meta.schema.indexOf("grp");
    int n = 0;
    for (const Row& a : rows)
        for (const Row& b : rows)
            if (a[id] == b[id] && a[grp] == b[grp]) ++n;
    return n;
}

// Volcano builds exactly ONE join and is not planned to gain more: it is the
// correctness baseline, not the feature-complete path. The refusal must name the
// path rather than promise a later week, and it must not be deleted — the single
// HashJoinNode would otherwise build one join out of two clauses and silently
// drop a relation.
TEST(PlannerTest, ThreeWayJoinRefusedOnVolcano) {
    Catalog catalog("../tests/data/test_catalog.json");
    try {
        bindAndPlan("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id "
                    "JOIN sj c ON b.grp = c.id", catalog);
        FAIL() << "expected a refusal";
    } catch (const std::runtime_error& e) {
        std::string err = e.what();
        EXPECT_NE(err.find("multi-way joins"), std::string::npos) << err;
        // names the capable path: this is a mode difference, not a missing feature
        EXPECT_NE(err.find("--execution vectorized"), std::string::npos) << err;
    }
}

// Multi-KEY is a different axis and executes here: only the relation count is
// vectorized-only. A query with both properties must therefore still report the
// relation count, which is the half Volcano cannot do.
TEST(PlannerTest, MultiWayAndMultiKeyReportsTheJoinCount) {
    Catalog catalog("../tests/data/test_catalog.json");
    try {
        bindAndPlan("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id "
                    "JOIN sj c ON b.grp = c.id AND b.val = c.val", catalog);
        FAIL() << "expected a refusal";
    } catch (const std::runtime_error& e) {
        std::string err = e.what();
        EXPECT_NE(err.find("multi-way joins"), std::string::npos) << err;
        EXPECT_EQ(err.find("multi-key"), std::string::npos) << err;
    }
}

// The multi-WAY guard stays ahead of the plan-time type checks: this function
// builds one join, so a third relation's columns are absent from the merged
// schema and a deferred check would report a misleading "column not found".
TEST(PlannerTest, MultiWayRefusalStillPrecedesTypeChecks) {
    Catalog catalog("../tests/data/test_catalog.json");
    try {
        bindAndPlan("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id "
                    "JOIN sj c ON b.grp = c.id WHERE a.id + 'x' > 0", catalog);
        FAIL() << "expected a refusal";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("multi-way joins"), std::string::npos) << e.what();
    }
}

// The plan-time type checks that used to outrank the deferred multi-key refusal
// must still fire on a multi-key query — the refusal is gone, the typing is not.
TEST(PlannerTest, TypeErrorInWhereStillCaughtOnAMultiKeyJoin) {
    Catalog catalog("../tests/data/test_catalog.json");
    try {
        bindAndPlan("SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp "
                    "WHERE a.id + 'x' > 0", catalog);
        FAIL() << "expected a type error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("numeric operands"), std::string::npos) << e.what();
    }
}

// The projection's check is the LAST one Planner::plan performs
// (buildProjectSchema calls inferExprType per select item).
TEST(PlannerTest, TypeErrorInSelectListStillCaughtOnAMultiKeyJoin) {
    Catalog catalog("../tests/data/test_catalog.json");
    try {
        bindAndPlan("SELECT a.id + 'x' FROM sj a JOIN sj b "
                    "ON a.id = b.id AND a.grp = b.grp", catalog);
        FAIL() << "expected a type error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("numeric operands"), std::string::npos) << e.what();
    }
}

// Week 27: a multi-key equi-join executes on Volcano — the composite key is one
// serialized tuple, so both keys must actually constrain the match.
TEST(PlannerTest, MultiKeyJoinExecutesOnVolcano) {
    Catalog catalog("../tests/data/test_catalog.json");
    auto plan = bindAndPlan(
        "SELECT a.val FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp", catalog);
    ASSERT_NE(plan, nullptr);

    // both keys in the operator's explain string, in written order
    const PlanNode* join = findNode(plan.get(), "HashJoin");
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->explain(), "HashJoin [id = id AND grp = grp]");

    // and both actually constrain the match: sj rows sharing id but not grp
    // must not join. Dropping the second key would emit more rows than this.
    plan->open();
    int rows = 0;
    while (plan->next()) ++rows;
    plan->close();
    EXPECT_EQ(rows, countMultiKeyMatches());
}

// Residual ON conjuncts execute here too, so the two engines accept exactly the
// same ON clauses and differ only in how many relations they can join.
TEST(PlannerTest, ResidualOnConjunctPlansOnVolcano) {
    Catalog catalog("../tests/data/test_catalog.json");
    auto plan = bindAndPlan(
        "SELECT a.val FROM sj a JOIN sj b ON a.id = b.id AND a.grp < b.grp", catalog);
    ASSERT_NE(plan, nullptr);

    // the residual became a Filter above the join, and the join kept one key
    const PlanNode* join = findNode(plan.get(), "HashJoin");
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->explain(), "HashJoin [id = id]");
    EXPECT_NE(findNode(plan.get(), "Filter"), nullptr);
}

// The FROM SeqScanNode is handed `stmt.where` as its zone-map pruning hint, and
// residual ON conjuncts are folded into that predicate — so the fold has to
// happen BEFORE the scan is constructed. Folding afterwards left the hint
// pointing at the pre-fold tree: the vectorized path pruned on a relation-0
// residual and this one did not, with identical results and nothing to catch the
// difference. Asserting the skip counter is the only way to see it.
TEST(PlannerTest, ResidualOnConjunctReachesTheFromScanAsAPruningHint) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT COUNT(*) FROM laps l JOIN drivers d "
             "ON l.driver_id = d.driver_id AND l.season = 1999");
    auto stmt = p.parse();
    Binder::bind(stmt, catalog);

    std::unordered_map<std::string, ColumnarTable> columnar;
    for (const std::string& t : {std::string("laps"), std::string("drivers")}) {
        const auto& m = catalog.getTable(t);
        columnar.emplace(t, CSVToColumnar::convert(CSVLoader::load(m.filepath, m.schema), m.schema));
    }
    auto plan = Planner::plan(std::move(stmt), catalog, {}, std::move(columnar));

    plan->open();
    while (plan->next()) {}
    plan->close();

    // season 1999 is outside the chunk's min/max, so the whole chunk is skipped
    const PlanNode* scan = findNode(plan.get(), "SeqScan [laps");
    ASSERT_NE(scan, nullptr) << "expected the FROM scan to be reachable";
    EXPECT_NE(scan->explain().find("chunks_skipped=1/1"), std::string::npos)
        << scan->explain();
}
