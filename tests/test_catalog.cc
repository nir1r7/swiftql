#include <gtest/gtest.h>
#include "catalog/catalog.h"
#include "catalog/table_metadata.h"
#include <cstdio>
#include <fstream>
#include <string>

// ===== catalog tests =====

namespace {
// Written rather than shipped as a fixture: this catalog is DELIBERATELY
// malformed, and a malformed .json sitting in tests/data is one careless
// --catalog away from being read as a real one.
struct TempCatalog {
    std::string path;
    explicit TempCatalog(const std::string& body, const char* name)
        : path(std::string("./") + name) {
        std::ofstream(path) << body;
    }
    ~TempCatalog() { std::remove(path.c_str()); }
};
}  // namespace

TEST(CatalogTest, LoadsTableMetadata) {
    Catalog catalog("../tests/data/test_catalog.json");
    
    EXPECT_TRUE(catalog.hasTable("laps"));
    EXPECT_FALSE(catalog.hasTable("nonexistent"));
}

TEST(CatalogTest, CorrectSchema) {
    Catalog catalog("../tests/data/test_catalog.json");
    const auto& meta = catalog.getTable("laps");

    EXPECT_EQ(meta.schema.indexOf("speed"), 3);
    EXPECT_EQ(meta.schema.column(3).type, TypeId::DOUBLE);
}

TEST(CatalogTest, ThrowsOnMissingTable) {
    Catalog catalog("../tests/data/test_catalog.json");
    EXPECT_THROW(catalog.getTable("bogus"), std::runtime_error);
}

// !! Week 37, seam-storage pass 2 finding S-8. Two columns with the same name
// was a SILENT WRONG ANSWER in columnar storage and only there — measured on
// t(k INT, k INT) over `k,k / 1,100 / 2,200 / 3,300`:
//
//   SELECT k FROM t          row 1,2,3   columnar 1,100,2
//   SELECT COUNT(*),SUM(k)   row 3,6     columnar 3,103
//
// ColumnarTable::columns is keyed by name, so both schema entries share one
// vector and CSVToColumnar interleaves them into it; row storage is positional
// and picks the first. This was the only input two audit passes found that made
// the two storage formats disagree, and nothing at any layer said a word.
//
// Establishing that it fails without the fix: with the throw removed this test
// reaches EXPECT_EQ and reports `Catalog(dup.path) doesn't throw`, and the CLI
// prints the table above. Both were run.
TEST(CatalogTest, RejectsADuplicateColumnName) {
    TempCatalog dup(
        R"({"tables":[{"name":"t","file":"data/t.csv","columns":[)"
        R"({"name":"k","type":"INT"},{"name":"k","type":"INT"}]}]})",
        "swiftql_test_dup_column_catalog.json");

    EXPECT_THROW({
        try {
            Catalog catalog(dup.path);
        } catch (const std::runtime_error& e) {
            // matches the shape the engine already uses for the identical shape
            // one layer up (logical_plan.cc, "column '<c>' is produced twice")
            EXPECT_STREQ(e.what(),
                "catalog: table 't': column 'k' is declared twice; "
                "give one of them a distinct name");
            throw;
        }
    }, std::runtime_error);
}

// The non-regression that stops the check over-matching: two columns of the
// same name in DIFFERENT tables are ordinary and must still load, and so must a
// table whose column names merely share a prefix.
TEST(CatalogTest, DistinctNamesAcrossTablesStillLoad) {
    TempCatalog ok(
        R"({"tables":[)"
        R"({"name":"a","file":"data/a.csv","columns":[)"
        R"({"name":"k","type":"INT"},{"name":"kk","type":"INT"}]},)"
        R"({"name":"b","file":"data/b.csv","columns":[)"
        R"({"name":"k","type":"INT"}]}]})",
        "swiftql_test_ok_column_catalog.json");

    EXPECT_NO_THROW({
        Catalog catalog(ok.path);
        EXPECT_TRUE(catalog.hasTable("a"));
        EXPECT_TRUE(catalog.hasTable("b"));
        EXPECT_EQ(catalog.getTable("a").schema.indexOf("kk"), 1);
    });
}