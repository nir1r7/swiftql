#include <gtest/gtest.h>
#include "planner/validator.h"
#include "planner/planner.h"
#include "parser/parser.h"
#include "storage/csv_loader.h"


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


// ===== Week 26/27 boundary (Volcano path) =====

// HashJoinNode executes exactly one single-key equi-join. Multi-way and
// multi-key trees bind and plan logically this week but must not execute.
TEST(PlannerTest, ThreeWayJoinRefusedUntilWeek27) {
    Catalog catalog("../tests/data/test_catalog.json");
    try {
        bindAndPlan("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id "
                    "JOIN sj c ON b.grp = c.id", catalog);
        FAIL() << "expected a refusal";
    } catch (const std::runtime_error& e) {
        std::string err = e.what();
        EXPECT_NE(err.find("multi-way joins"), std::string::npos) << err;
    }
}

// Same query as VecPlanBuilder.MultiWayAndMultiKeyReportsTheJoinCountFirst:
// the two engines must agree on which reason they report.
TEST(PlannerTest, MultiWayAndMultiKeyReportsTheJoinCountFirst) {
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

// The multi-key refusal is deferred past the plan-time type checks, because the
// merged schema is built from the two children's schemas and never reads the
// keys. A genuine query defect therefore outranks a temporary engine
// limitation, and both engines report the same thing — the vec path
// type-checks the whole logical plan before checkLowerable refuses.
TEST(PlannerTest, TypeErrorBeatsTheMultiKeyRefusal) {
    Catalog catalog("../tests/data/test_catalog.json");
    try {
        bindAndPlan("SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp "
                    "WHERE a.id + 'x' > 0", catalog);
        FAIL() << "expected a type error";
    } catch (const std::runtime_error& e) {
        std::string err = e.what();
        EXPECT_NE(err.find("numeric operands"), std::string::npos) << err;
        EXPECT_EQ(err.find("multi-key"), std::string::npos) << err;
    }
}

// The multi-WAY guard stays ahead of the type checks: this function builds one
// join, so a third relation's columns are absent from the merged schema and a
// deferred check would report a misleading "column not found".
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

TEST(PlannerTest, MultiKeyJoinRefusedUntilWeek27) {
    Catalog catalog("../tests/data/test_catalog.json");
    try {
        bindAndPlan("SELECT a.val FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp", catalog);
        FAIL() << "expected a refusal";
    } catch (const std::runtime_error& e) {
        std::string err = e.what();
        EXPECT_NE(err.find("multi-key"), std::string::npos) << err;
    }
}
