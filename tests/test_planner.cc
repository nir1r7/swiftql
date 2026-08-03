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

TEST(PlannerTest, BuildsSeqScan) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team FROM laps");
    auto stmt = p.parse();

    std::unordered_map<std::string, std::vector<Row>> table_rows;
    const auto& meta = catalog.getTable(stmt.from_table);
    table_rows[stmt.from_table] = CSVLoader::load(meta.filepath, meta.schema);

    auto plan = Planner::plan(std::move(stmt), catalog, std::move(table_rows));
    EXPECT_NE(plan, nullptr);
}

TEST(PlannerTest, BuildsJoinPlan) {
    Catalog catalog("../tests/data/test_catalog.json");
    Parser p("SELECT team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id");
    auto stmt = p.parse();

    std::unordered_map<std::string, std::vector<Row>> table_rows;
    const auto& meta = catalog.getTable(stmt.from_table);
    table_rows[stmt.from_table] = CSVLoader::load(meta.filepath, meta.schema);
    if (stmt.join.has_value()) {
        const auto& jmeta = catalog.getTable(stmt.join->join_table);
        table_rows[stmt.join->join_table] = CSVLoader::load(jmeta.filepath, jmeta.schema);
    }

    auto plan = Planner::plan(std::move(stmt), catalog, std::move(table_rows));
    EXPECT_NE(plan, nullptr);
    EXPECT_NO_THROW(plan->open());
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
