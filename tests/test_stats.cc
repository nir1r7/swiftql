#include <gtest/gtest.h>
#include "catalog/catalog.h"
#include "catalog/table_stats.h"
#include "storage/csv_loader.h"

// ===== TableStats::compute tests =====

TEST(TableStatsTest, ComputesBasicStats) {
    Schema schema({{"id", TypeId::INT}, {"team", TypeId::STRING}});
    std::vector<Row> rows = {
        {Value(int64_t(1)), Value(std::string("Ferrari"))},
        {Value(int64_t(2)), Value(std::string("McLaren"))},
        {Value(int64_t(2)), Value(std::string("Ferrari"))},
    };
    TableStats ts = TableStats::compute(rows, schema);

    EXPECT_EQ(ts.row_count, 3);
    const ColumnStats& id = ts.columns.at("id");
    EXPECT_EQ(id.min_val.asInt(), 1);
    EXPECT_EQ(id.max_val.asInt(), 2);
    EXPECT_EQ(id.distinct_count, 2);
    EXPECT_EQ(id.null_count, 0);
    EXPECT_DOUBLE_EQ(id.avg_width, 8.0);

    const ColumnStats& team = ts.columns.at("team");
    EXPECT_EQ(team.min_val.asString(), "Ferrari");
    EXPECT_EQ(team.max_val.asString(), "McLaren");
    EXPECT_EQ(team.distinct_count, 2);
    EXPECT_DOUBLE_EQ(team.avg_width, (7.0 + 7.0 + 7.0) / 3.0);
}

TEST(TableStatsTest, NullsExcludedFromMinMaxAndDistinct) {
    Schema schema({{"speed", TypeId::DOUBLE}});
    std::vector<Row> rows = {
        {Value(310.5)}, {Value::null()}, {Value(305.2)}, {Value::null()},
    };
    TableStats ts = TableStats::compute(rows, schema);

    const ColumnStats& cs = ts.columns.at("speed");
    EXPECT_EQ(cs.null_count, 2);
    EXPECT_EQ(cs.distinct_count, 2);
    // fails if min/max were seeded with a null accumulator (comparisons
    // against null Values always return false and never update it)
    EXPECT_DOUBLE_EQ(cs.min_val.asDouble(), 305.2);
    EXPECT_DOUBLE_EQ(cs.max_val.asDouble(), 310.5);
    EXPECT_DOUBLE_EQ(cs.avg_width, 8.0);
}

TEST(TableStatsTest, NullSeedSkippedWhenFirstValueIsNull) {
    Schema schema({{"x", TypeId::INT}});
    std::vector<Row> rows = {
        {Value::null()}, {Value(int64_t(7))}, {Value(int64_t(3))},
    };
    TableStats ts = TableStats::compute(rows, schema);

    const ColumnStats& cs = ts.columns.at("x");
    EXPECT_EQ(cs.null_count, 1);
    EXPECT_EQ(cs.min_val.asInt(), 3);
    EXPECT_EQ(cs.max_val.asInt(), 7);
}

TEST(TableStatsTest, AllNullAndEmptyColumns) {
    Schema schema({{"x", TypeId::INT}});
    TableStats empty = TableStats::compute({}, schema);
    EXPECT_EQ(empty.row_count, 0);
    EXPECT_TRUE(empty.columns.at("x").min_val.isNull());
    EXPECT_TRUE(empty.columns.at("x").max_val.isNull());
    EXPECT_EQ(empty.columns.at("x").distinct_count, 0);
    EXPECT_DOUBLE_EQ(empty.columns.at("x").avg_width, 0.0);

    std::vector<Row> rows = {{Value::null()}, {Value::null()}};
    TableStats all_null = TableStats::compute(rows, schema);
    EXPECT_EQ(all_null.row_count, 2);
    EXPECT_EQ(all_null.columns.at("x").null_count, 2);
    EXPECT_EQ(all_null.columns.at("x").distinct_count, 0);
    EXPECT_TRUE(all_null.columns.at("x").min_val.isNull());
    EXPECT_TRUE(all_null.columns.at("x").max_val.isNull());
}

// ===== Catalog stats storage tests =====

TEST(CatalogStatsTest, SetAndGetRoundTrip) {
    Catalog catalog("../tests/data/test_catalog.json");
    EXPECT_FALSE(catalog.hasStats("laps"));
    EXPECT_THROW(catalog.getStats("laps"), std::runtime_error);

    TableStats ts;
    ts.row_count = 3;
    catalog.setStats("laps", std::move(ts));
    EXPECT_TRUE(catalog.hasStats("laps"));
    EXPECT_EQ(catalog.getStats("laps").row_count, 3);
}

TEST(CatalogStatsTest, RejectsUnknownTable) {
    Catalog catalog("../tests/data/test_catalog.json");
    EXPECT_THROW(catalog.setStats("bogus", TableStats{}), std::runtime_error);
}

// checkpoint: statistics populated for every loaded table via the real loader
TEST(CatalogStatsTest, PopulatedFromRealCSVLoad) {
    Catalog catalog("../tests/data/test_catalog.json");
    const TableMetadata& meta = catalog.getTable("drivers");
    auto rows = CSVLoader::load(meta.filepath, meta.schema);
    catalog.setStats("drivers", TableStats::compute(rows, meta.schema));

    ASSERT_TRUE(catalog.hasStats("drivers"));
    const TableStats& ts = catalog.getStats("drivers");
    EXPECT_EQ(ts.row_count, static_cast<int64_t>(rows.size()));
    EXPECT_EQ(static_cast<int>(ts.columns.size()), meta.schema.size());
    for (int c = 0; c < meta.schema.size(); ++c) {
        const std::string& name = meta.schema.column(c).name;
        ASSERT_TRUE(ts.columns.count(name)) << "missing stats for column " << name;
        // CSV input cannot express NULLs in this engine
        EXPECT_EQ(ts.columns.at(name).null_count, 0);
        EXPECT_FALSE(ts.columns.at(name).min_val.isNull());
        EXPECT_GT(ts.columns.at(name).distinct_count, 0);
    }
}
