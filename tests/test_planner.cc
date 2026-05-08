#include <gtest/gtest.h>
#include "planner/validator.h"
#include "planner/planner.h"
#include "parser/parser.h"


// ===== validator tests =====

TEST(ValidatorTest, ValidQuery) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team FROM laps WHERE season = 2025");
    auto stmt = p.parse();
    EXPECT_NO_THROW(Validator::validate(stmt, catalog));
}

TEST(ValidatorTest, MissingTable) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team FROM bogus_table");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, MissingColumn) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT bogus_col FROM laps");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, HavingWithoutGroupBy) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team FROM laps HAVING team = 'Ferrari'");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, JoinOnColumnMissingInLeftTable) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team FROM laps JOIN drivers ON laps.bogus_col = drivers.driver_id");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, JoinOnColumnMissingInRightTable) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team FROM laps JOIN drivers ON laps.driver_id = drivers.bogus_col");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, JoinOnColumnValidBothSides) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id");
    auto stmt = p.parse();
    EXPECT_NO_THROW(Validator::validate(stmt, catalog));
}

TEST(ValidatorTest, SelectNonAggColWithoutGroupBy) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team, COUNT(*) FROM laps");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, SelectNonAggColWithWrongGroupBy) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team, COUNT(*) FROM laps GROUP BY season");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, SelectNonAggColWithCorrectGroupBy) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team, COUNT(*) FROM laps GROUP BY team");
    auto stmt = p.parse();
    EXPECT_NO_THROW(Validator::validate(stmt, catalog));
}

TEST(ValidatorTest, SumOnStringColumn) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT SUM(team) FROM laps");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, AvgOnStringColumn) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT AVG(name) FROM drivers");
    auto stmt = p.parse();
    EXPECT_THROW(Validator::validate(stmt, catalog), std::runtime_error);
}

TEST(ValidatorTest, SumOnNumericColumn) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT SUM(speed) FROM laps");
    auto stmt = p.parse();
    EXPECT_NO_THROW(Validator::validate(stmt, catalog));
}

// ===== planner tests =====

TEST(PlannerTest, BuildsSeqScan) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team FROM laps");
    auto stmt = p.parse();
    auto plan = Planner::plan(stmt, catalog);
    EXPECT_NE(plan, nullptr);
}

TEST(PlannerTest, JoinPlanStubbed) {
    Catalog catalog("../catalog.json");
    Parser p("SELECT team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id");
    auto stmt = p.parse();
    auto plan = Planner::plan(stmt, catalog);
    // plan builds successfully
    EXPECT_NE(plan, nullptr);
    // but executing it throws
    plan->open();
    EXPECT_THROW(plan->next(), std::runtime_error);
}