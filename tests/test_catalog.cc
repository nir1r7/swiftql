#include <gtest/gtest.h>
#include "catalog/catalog.h"
#include "catalog/table_metadata.h"

// ===== catalog tests =====

TEST(CatalogTest, LoadsTableMetadata) {
    Catalog catalog("../catalog.json");
    
    EXPECT_TRUE(catalog.hasTable("laps"));
    EXPECT_FALSE(catalog.hasTable("nonexistent"));
}

TEST(CatalogTest, CorrectSchema) {
    Catalog catalog("../catalog.json");
    const auto& meta = catalog.getTable("laps");

    EXPECT_EQ(meta.schema.indexOf("speed"), 3);
    EXPECT_EQ(meta.schema.column(3).type, TypeId::DOUBLE);
}

TEST(CatalogTest, ThrowsOnMissingTable) {
    Catalog catalog("../catalog.json");
    EXPECT_THROW(catalog.getTable("bogus"), std::runtime_error);
}