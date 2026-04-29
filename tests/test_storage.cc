#include <gtest/gtest.h>
#include "storage/csv_loader.h"

// ===== storage tests =====

TEST(CSVLoaderTest, LoadsCorrectRowCount) {
    Schema schema({{"lap_id", TypeId::INT}, {"team", TypeId::STRING}, {"speed", TypeId::DOUBLE}});
    
    // small hand crafted test CSV for this, tests/data/test_laps.csv
    auto rows = CSVLoader::load("../tests/data/test_laps.csv", schema);
    EXPECT_EQ(rows.size(), 3);
}

TEST(CSVLoaderTest, CorrectTypes) {
    Schema schema({{"lap_id", TypeId::INT}, {"team", TypeId::STRING}, {"speed", TypeId::DOUBLE}});
    auto rows = CSVLoader::load("../tests/data/test_laps.csv", schema);

    EXPECT_EQ(rows[0][0].type(), TypeId::INT);
    EXPECT_EQ(rows[0][1].type(), TypeId::STRING);
    EXPECT_EQ(rows[0][2].type(), TypeId::DOUBLE);
}