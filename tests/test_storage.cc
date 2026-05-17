#include <gtest/gtest.h>
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"
#include "planner/plan_nodes.h"
#include "parser/ast.h"
#include "common/value.h"
#include "common/schema.h"
#include <memory>
#include <vector>

// ===== columnar storage helper methods =====

static Schema makeColumnarSchema(
        std::initializer_list<std::pair<std::string, TypeId>> cols) {
    std::vector<ColumnDef> defs;
    for (auto& [name, type] : cols) defs.push_back({name, type});
    return Schema(defs);
}

static std::unique_ptr<BinaryExpr> makeColumnarBinary(
        std::string op, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r) {
    auto b = std::make_unique<BinaryExpr>();
    b->op = std::move(op);
    b->left  = std::move(l);
    b->right = std::move(r);
    return b;
}

static std::unique_ptr<Literal> columnarLit(Value v) {
    return std::make_unique<Literal>(std::move(v));
}

static std::unique_ptr<ColumnRef> columnarColRef(std::string name) {
    auto c = std::make_unique<ColumnRef>();
    c->column_name = std::move(name);
    return c;
}

// open a node, collect all rows into a vector (copying each), then close.
static std::vector<Row> columnarDrainAll(PlanNode* node) {
    node->open();
    std::vector<Row> result;
    while (Row* r = node->next()) result.push_back(*r);
    node->close();
    return result;
}

static std::unique_ptr<SeqScanNode> makeColumnarScan(ColumnarTable tbl, Schema schema) {
    return std::make_unique<SeqScanNode>("test", std::move(tbl), std::move(schema));
}

// ===== row storage tests =====

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

// ===== CSVToColumnar =====

// CSVToColumnar::convert() produces a ColumnarTable with correct structure and values.
TEST(CSVToColumnar, ConversionCorrectness) {
    Schema schema = makeColumnarSchema({
        {"id",   TypeId::INT},
        {"name", TypeId::STRING},
        {"val",  TypeId::DOUBLE}
    });
    std::vector<Row> rows = {
        {Value(int64_t(1)), Value(std::string("alpha")), Value(1.5)},
        {Value(int64_t(2)), Value(std::string("beta")),  Value(2.5)},
        {Value(int64_t(3)), Value(std::string("gamma")), Value(3.5)},
    };

    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    // num_rows reflects input count
    EXPECT_EQ(tbl.num_rows, 3);

    // each column array holds the correct typed values in order
    const auto& id_arr   = std::get<std::vector<int64_t>>(tbl.columns.at("id"));
    const auto& name_arr = std::get<std::vector<std::string>>(tbl.columns.at("name"));
    const auto& val_arr  = std::get<std::vector<double>>(tbl.columns.at("val"));

    ASSERT_EQ(id_arr.size(),   3u);
    ASSERT_EQ(name_arr.size(), 3u);
    ASSERT_EQ(val_arr.size(),  3u);

    EXPECT_EQ(id_arr[0], 1); EXPECT_EQ(id_arr[1], 2); EXPECT_EQ(id_arr[2], 3);
    EXPECT_EQ(name_arr[0], "alpha"); EXPECT_EQ(name_arr[1], "beta"); EXPECT_EQ(name_arr[2], "gamma");
    EXPECT_DOUBLE_EQ(val_arr[0], 1.5); EXPECT_DOUBLE_EQ(val_arr[1], 2.5); EXPECT_DOUBLE_EQ(val_arr[2], 3.5);

    // getValue() round-trips every cell back to a Value matching the original row
    for (int r = 0; r < 3; ++r) {
        EXPECT_EQ(tbl.getValue("id",   r).asInt(),    rows[r][0].asInt());
        EXPECT_EQ(tbl.getValue("name", r).asString(), rows[r][1].asString());
        EXPECT_DOUBLE_EQ(tbl.getValue("val", r).asDouble(), rows[r][2].asDouble());
    }
}

// ===== SeqScanNode columnar path =====

// SeqScanNode columnar constructor emits rows in order with correct values.
TEST(SeqScanNode, ColumnarPathEmitsCorrectRows) {
    Schema schema = makeColumnarSchema({
        {"x", TypeId::INT},
        {"y", TypeId::DOUBLE},
        {"z", TypeId::STRING}
    });
    std::vector<Row> rows = {
        {Value(int64_t(10)), Value(0.1), Value(std::string("p"))},
        {Value(int64_t(20)), Value(0.2), Value(std::string("q"))},
        {Value(int64_t(30)), Value(0.3), Value(std::string("r"))},
    };
    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    SeqScanNode scan("test", std::move(tbl), schema);

    // first pass: values match original rows in order
    scan.open();
    for (int i = 0; i < 3; ++i) {
        Row* r = scan.next();
        ASSERT_NE(r, nullptr);
        EXPECT_EQ((*r)[0].asInt(),           rows[i][0].asInt());
        EXPECT_DOUBLE_EQ((*r)[1].asDouble(), rows[i][1].asDouble());
        EXPECT_EQ((*r)[2].asString(),        rows[i][2].asString());
    }
    EXPECT_EQ(scan.next(), nullptr);  // exhausted
    scan.close();

    // open() resets cursor; second full pass succeeds
    scan.open();
    int count = 0;
    while (scan.next()) ++count;
    scan.close();
    EXPECT_EQ(count, 3);

    // empty columnar table returns nullptr immediately
    ColumnarTable empty_tbl = CSVToColumnar::convert({}, schema);
    SeqScanNode empty_scan("test", std::move(empty_tbl), schema);
    empty_scan.open();
    EXPECT_EQ(empty_scan.next(), nullptr);
    empty_scan.close();
}

// FilterNode on top of a columnar SeqScanNode correctly filters rows.
TEST(SeqScanNode, ColumnarPlusFilter) {
    Schema schema = makeColumnarSchema({{"v", TypeId::INT}});
    std::vector<Row> rows = {
        {Value(int64_t(5))},
        {Value(int64_t(15))},
        {Value(int64_t(25))},
    };
    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    // predicate: v > 10  ->  rows 15 and 25 pass, 5 is filtered out
    auto pred = makeColumnarBinary(">", columnarColRef("v"), columnarLit(Value(int64_t(10))));
    FilterNode filter(makeColumnarScan(std::move(tbl), schema), std::move(pred));

    auto result = columnarDrainAll(&filter);

    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0][0].asInt(), 15);
    EXPECT_EQ(result[1][0].asInt(), 25);
}

// Columnar and row storage paths produce identical output for the same input.
TEST(SeqScanNode, ColumnarRowParity) {
    Schema schema = makeColumnarSchema({
        {"id",    TypeId::INT},
        {"score", TypeId::DOUBLE},
        {"label", TypeId::STRING}
    });
    std::vector<Row> rows = {
        {Value(int64_t(1)), Value(9.9), Value(std::string("x"))},
        {Value(int64_t(2)), Value(8.8), Value(std::string("y"))},
        {Value(int64_t(3)), Value(7.7), Value(std::string("z"))},
    };

    // row-path scan
    SeqScanNode row_scan("test", rows, schema);
    auto row_result = columnarDrainAll(&row_scan);

    // columnar-path scan over the same data
    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);
    SeqScanNode col_scan("test", std::move(tbl), schema);
    auto col_result = columnarDrainAll(&col_scan);

    ASSERT_EQ(row_result.size(), col_result.size());
    for (size_t i = 0; i < row_result.size(); ++i) {
        EXPECT_EQ(row_result[i][0].asInt(),           col_result[i][0].asInt());
        EXPECT_DOUBLE_EQ(row_result[i][1].asDouble(), col_result[i][1].asDouble());
        EXPECT_EQ(row_result[i][2].asString(),        col_result[i][2].asString());
    }
}