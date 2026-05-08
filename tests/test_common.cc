#include <gtest/gtest.h>
#include "common/value.h"
#include "common/schema.h"

// ===== value tests =====

TEST(ValueTest, IntConstruction) {
    Value v(int64_t(42));
    EXPECT_EQ(v.type(), TypeId::INT);
    EXPECT_EQ(v.asInt(), 42);
}

TEST(ValueTest, DoubleConstruction) {
    Value v(3.14);
    EXPECT_EQ(v.type(), TypeId::DOUBLE);
    EXPECT_DOUBLE_EQ(v.asDouble(), 3.14);
}

TEST(ValueTest, StringConstruction) {
    Value v(std::string("Ferrari"));
    EXPECT_EQ(v.type(), TypeId::STRING);
    EXPECT_EQ(v.asString(), "Ferrari");
}

TEST(ValueTest, IntComparison) {
    Value a(int64_t(5));
    Value b(int64_t(10));
    EXPECT_TRUE(a < b);
    EXPECT_FALSE(a > b);
    EXPECT_TRUE(a != b);
}

TEST(ValueTest, StringComparison) {
    Value a(std::string("Ferrari"));
    Value b(std::string("McLaren"));
    EXPECT_TRUE(a < b);   // lexicographic
    EXPECT_TRUE(a != b);
}

TEST(ValueTest, TypeMismatchThrows) {
    Value a(int64_t(5));
    Value b(std::string("Ferrari"));
    EXPECT_THROW(a < b, std::runtime_error);
}

TEST(ValueTest, NullDefaultConstruction) {
    Value v;
    EXPECT_TRUE(v.isNull());
}

TEST(ValueTest, NullFactory) {
    Value v = Value::null();
    EXPECT_TRUE(v.isNull());
}

TEST(ValueTest, NonNullIsNotNull) {
    Value v(int64_t(42));
    EXPECT_FALSE(v.isNull());
}

TEST(ValueTest, NullComparisonReturnsFalse) {
    Value null_v;
    Value int_v(int64_t(42));
    EXPECT_FALSE(null_v == int_v);
    EXPECT_FALSE(int_v == null_v);
    EXPECT_FALSE(null_v == null_v);  // NULL = NULL is false in SQL
}

TEST(ValueTest, NullToString) {
    Value v;
    EXPECT_EQ(v.toString(), "NULL");
}

// ===== schema tests =====

TEST(SchemaTest, IndexOf) {
    Schema s({{"lap_id", TypeId::INT},
              {"team",   TypeId::STRING},
              {"speed",  TypeId::DOUBLE}});
    EXPECT_EQ(s.indexOf("team"), 1);
    EXPECT_EQ(s.indexOf("speed"), 2);
    EXPECT_EQ(s.indexOf("missing"), -1);
}

TEST(SchemaTest, HasColumn) {
    Schema s({{"season", TypeId::INT}});
    EXPECT_TRUE(s.hasColumn("season"));
    EXPECT_FALSE(s.hasColumn("bogus"));
}

// ===== row tests =====

TEST(RowTest, ConstructAndAccess) {
    Row row = {Value(int64_t(1)), Value(std::string("Ferrari")), Value(312.4)};
    EXPECT_EQ(row.size(), 3);
    EXPECT_EQ(row[1].asString(), "Ferrari");
}