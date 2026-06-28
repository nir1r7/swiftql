#include <gtest/gtest.h>
#include "execution/vec_scan_node.h"
#include "execution/vec_filter_node.h"
#include "execution/vec_project_node.h"
#include "execution/vec_types.h"
#include "storage/columnar_table.h"
#include "storage/rle_column.h"
#include "storage/dictionary_encoder.h"
#include "planner/plan_nodes.h"
#include "common/schema.h"
#include "common/value.h"
#include "parser/ast.h"
#include <numeric>
#include <vector>

// Helper: build a Schema from an initializer list of (name, type) pairs
static Schema vecSchema(std::initializer_list<std::pair<std::string, TypeId>> cols) {
    std::vector<ColumnDef> defs;
    for (auto& [name, type] : cols) defs.push_back({name, type});
    return Schema(defs);
}

// Helper: drain all chunks from a VecScanNode, returning total row count
static int drainChunks(VecScanNode& scan) {
    scan.open();
    int total = 0;
    while (DataChunk* chunk = scan.nextChunk()) total += chunk->num_rows;
    scan.close();
    return total;
}

// ===== RLEColumn::decodeRange unit tests =====

TEST(DecodeRange, EmptyRange) {
    RLEColumn rle = RLEColumn::encode({1, 1, 2, 2, 3});
    auto result = rle.decodeRange(0, 0);
    EXPECT_TRUE(result.empty());
}

TEST(DecodeRange, SingleRunFullRange) {
    RLEColumn rle = RLEColumn::encode({7, 7, 7, 7, 7});
    auto result = rle.decodeRange(0, 5);
    ASSERT_EQ(result.size(), 5u);
    for (int64_t v : result) EXPECT_EQ(v, 7);
}

TEST(DecodeRange, MultipleRunsFullRange) {
    RLEColumn rle = RLEColumn::encode({1, 1, 2, 2, 3});
    auto result = rle.decodeRange(0, 5);
    ASSERT_EQ(result.size(), 5u);
    EXPECT_EQ(result[0], 1);
    EXPECT_EQ(result[1], 1);
    EXPECT_EQ(result[2], 2);
    EXPECT_EQ(result[3], 2);
    EXPECT_EQ(result[4], 3);
}

TEST(DecodeRange, SubRangeStartingMidRun) {
    // rows: 0=7, 1=7, 2=7, 3=8, 4=8
    RLEColumn rle = RLEColumn::encode({7, 7, 7, 8, 8});
    auto result = rle.decodeRange(1, 3); // rows 1, 2, 3
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0], 7);
    EXPECT_EQ(result[1], 7);
    EXPECT_EQ(result[2], 8);
}

TEST(DecodeRange, SubRangeCrossesRunBoundary) {
    // rows: 0=1, 1=1, 2=2, 3=2, 4=3, 5=3
    RLEColumn rle = RLEColumn::encode({1, 1, 2, 2, 3, 3});
    auto result = rle.decodeRange(1, 4); // rows 1, 2, 3, 4
    ASSERT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0], 1);
    EXPECT_EQ(result[1], 2);
    EXPECT_EQ(result[2], 2);
    EXPECT_EQ(result[3], 3);
}

TEST(DecodeRange, LastRunBoundary) {
    RLEColumn rle = RLEColumn::encode({10, 20, 30}); // each its own run
    auto result = rle.decodeRange(2, 1);             // last row only
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0], 30);
}

TEST(DecodeRange, SpansBatchBoundary) {
    // 1500 rows: 1000 × 2024, then 500 × 2025
    // Simulates a run that spans the VecScan chunk boundary at row 1024
    std::vector<int64_t> raw(1000, 2024LL);
    raw.insert(raw.end(), 500, 2025LL);
    RLEColumn rle = RLEColumn::encode(raw);

    // First batch: rows 0–1023 → 1000 × 2024, then 24 × 2025
    auto batch1 = rle.decodeRange(0, 1024);
    ASSERT_EQ(batch1.size(), 1024u);
    for (int i = 0; i < 1000; ++i) EXPECT_EQ(batch1[i], 2024) << "i=" << i;
    for (int i = 1000; i < 1024; ++i) EXPECT_EQ(batch1[i], 2025) << "i=" << i;

    // Second batch: rows 1024–1499 → 476 × 2025
    auto batch2 = rle.decodeRange(1024, 476);
    ASSERT_EQ(batch2.size(), 476u);
    for (int64_t v : batch2) EXPECT_EQ(v, 2025);
}

// ===== VecScanNode structural tests =====

TEST(VecScan, EmptyTable) {
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 0);
    ct.columns["id"] = std::vector<int64_t>{};

    VecScanNode scan("t", ct, schema);
    scan.open();
    EXPECT_EQ(scan.nextChunk(), nullptr);
    EXPECT_EQ(scan.nextChunk(), nullptr); // idempotent
    scan.close();
}

TEST(VecScan, SmallTable_SingleChunk) {
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 5);
    ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5};

    VecScanNode scan("t", ct, schema);
    scan.open();

    int chunks = 0, total = 0;
    while (DataChunk* chunk = scan.nextChunk()) {
        EXPECT_GT(chunk->num_rows, 0);
        EXPECT_LE(chunk->num_rows, BATCH_SIZE);
        total += chunk->num_rows;
        ++chunks;
    }
    scan.close();

    EXPECT_EQ(total, 5);
    EXPECT_EQ(chunks, 1);
}

TEST(VecScan, MultiChunk_LastChunkSmaller) {
    // 2500 rows → chunks of 1024, 1024, 452
    Schema schema = vecSchema({{"id", TypeId::INT}});
    std::vector<int64_t> vals(2500);
    std::iota(vals.begin(), vals.end(), 0LL);
    ColumnarTable ct(schema, 2500);
    ct.columns["id"] = vals;

    VecScanNode scan("t", ct, schema);
    scan.open();

    std::vector<int> chunk_sizes;
    while (DataChunk* chunk = scan.nextChunk())
        chunk_sizes.push_back(chunk->num_rows);
    scan.close();

    ASSERT_EQ(chunk_sizes.size(), 3u);
    EXPECT_EQ(chunk_sizes[0], 1024);
    EXPECT_EQ(chunk_sizes[1], 1024);
    EXPECT_EQ(chunk_sizes[2], 452);
}

TEST(VecScan, ExactlyOneBatchSize) {
    // Exactly 1024 rows → 1 chunk, then nullptr
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 1024);
    ct.columns["id"] = std::vector<int64_t>(1024, 0LL);

    VecScanNode scan("t", ct, schema);
    scan.open();

    DataChunk* c1 = scan.nextChunk();
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(c1->num_rows, 1024);

    DataChunk* c2 = scan.nextChunk();
    EXPECT_EQ(c2, nullptr);
    scan.close();
}

TEST(VecScan, NullptrAfterExhaustion) {
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 2);
    ct.columns["id"] = std::vector<int64_t>{1, 2};

    VecScanNode scan("t", ct, schema);
    scan.open();
    scan.nextChunk();                          // exhaust
    EXPECT_EQ(scan.nextChunk(), nullptr);
    EXPECT_EQ(scan.nextChunk(), nullptr);
    scan.close();
}

TEST(VecScan, OpenResetsToStart) {
    // open() must reset row_cursor_ so a second pass produces the same row count
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 3);
    ct.columns["id"] = std::vector<int64_t>{10, 20, 30};

    VecScanNode scan("t", ct, schema);

    EXPECT_EQ(drainChunks(scan), 3);
    EXPECT_EQ(drainChunks(scan), 3);
}

TEST(VecScan, ColumnCountMatchesSchema) {
    Schema schema = vecSchema({
        {"a", TypeId::INT}, {"b", TypeId::DOUBLE}, {"c", TypeId::STRING}
    });
    ColumnarTable ct(schema, 2);
    ct.columns["a"] = std::vector<int64_t>{1, 2};
    ct.columns["b"] = std::vector<double>{1.0, 2.0};
    ct.columns["c"] = std::vector<std::string>{"x", "y"};

    VecScanNode scan("t", ct, schema);
    scan.open();
    DataChunk* chunk = scan.nextChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(static_cast<int>(chunk->columns.size()), schema.size());
    scan.close();
}

TEST(VecScan, ColumnTypesMatchSchema) {
    Schema schema = vecSchema({
        {"i", TypeId::INT}, {"d", TypeId::DOUBLE}, {"s", TypeId::STRING}
    });
    ColumnarTable ct(schema, 2);
    ct.columns["i"] = std::vector<int64_t>{1, 2};
    ct.columns["d"] = std::vector<double>{1.0, 2.0};
    ct.columns["s"] = std::vector<std::string>{"a", "b"};

    VecScanNode scan("t", ct, schema);
    scan.open();
    DataChunk* chunk = scan.nextChunk();
    ASSERT_NE(chunk, nullptr);

    EXPECT_EQ(chunk->columns[0].type, TypeId::INT);
    EXPECT_EQ(chunk->columns[1].type, TypeId::DOUBLE);
    EXPECT_EQ(chunk->columns[2].type, TypeId::STRING);
    scan.close();
}

TEST(VecScan, ExplainString) {
    Schema schema = vecSchema({{"id", TypeId::INT}, {"val", TypeId::DOUBLE}});
    ColumnarTable ct(schema, 1);
    ct.columns["id"] = std::vector<int64_t>{1};
    ct.columns["val"] = std::vector<double>{1.0};

    VecScanNode scan("laps", ct, schema);
    EXPECT_EQ(scan.explain(), "VecScan [laps, 2 columns]");
}

// ===== VecScanNode value correctness tests =====

TEST(VecScan, RawIntValues) {
    Schema schema = vecSchema({{"val", TypeId::INT}});
    std::vector<int64_t> expected = {100, 200, 300, 400, 500};
    ColumnarTable ct(schema, 5);
    ct.columns["val"] = expected;

    VecScanNode scan("t", ct, schema);
    scan.open();
    DataChunk* chunk = scan.nextChunk();
    ASSERT_NE(chunk, nullptr);

    const auto& v = std::get<std::vector<int64_t>>(chunk->columns[0].data);
    EXPECT_EQ(v, expected);
    scan.close();
}

TEST(VecScan, RawDoubleValues) {
    Schema schema = vecSchema({{"speed", TypeId::DOUBLE}});
    std::vector<double> expected = {310.5, 305.2, 315.1};
    ColumnarTable ct(schema, 3);
    ct.columns["speed"] = expected;

    VecScanNode scan("t", ct, schema);
    scan.open();
    DataChunk* chunk = scan.nextChunk();
    ASSERT_NE(chunk, nullptr);

    const auto& v = std::get<std::vector<double>>(chunk->columns[0].data);
    ASSERT_EQ(v.size(), 3u);
    for (int i = 0; i < 3; ++i)
        EXPECT_DOUBLE_EQ(v[i], expected[i]) << "i=" << i;
    scan.close();
}

TEST(VecScan, RawStringValues) {
    Schema schema = vecSchema({{"name", TypeId::STRING}});
    std::vector<std::string> expected = {"Alice", "Bob", "Charlie"};
    ColumnarTable ct(schema, 3);
    ct.columns["name"] = expected; // raw vector<string>, no encoding

    VecScanNode scan("t", ct, schema);
    scan.open();
    DataChunk* chunk = scan.nextChunk();
    ASSERT_NE(chunk, nullptr);

    const auto& v = std::get<std::vector<std::string>>(chunk->columns[0].data);
    EXPECT_EQ(v, expected);
    scan.close();
}

TEST(VecScan, DictionaryEncoderValues) {
    Schema schema = vecSchema({{"team", TypeId::STRING}});
    std::vector<std::string> teams = {"Ferrari", "Mercedes", "Ferrari", "McLaren"};
    ColumnarTable ct(schema, 4);
    ct.columns["team"] = DictionaryEncoder::encode(teams);

    VecScanNode scan("t", ct, schema);
    scan.open();
    DataChunk* chunk = scan.nextChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->num_rows, 4);

    const auto& v = std::get<std::vector<std::string>>(chunk->columns[0].data);
    ASSERT_EQ(v.size(), 4u);
    EXPECT_EQ(v[0], "Ferrari");
    EXPECT_EQ(v[1], "Mercedes");
    EXPECT_EQ(v[2], "Ferrari");
    EXPECT_EQ(v[3], "McLaren");
    scan.close();
}

TEST(VecScan, RLEValues_RunSpansChunkBoundary) {
    // 1500 rows: 1000 × 2024, then 500 × 2025
    // Chunk 0 (rows 0–1023):  1000 × 2024, 24 × 2025
    // Chunk 1 (rows 1024–1499): 476 × 2025
    Schema schema = vecSchema({{"season", TypeId::INT}});
    std::vector<int64_t> raw(1000, 2024LL);
    raw.insert(raw.end(), 500, 2025LL);
    ColumnarTable ct(schema, 1500);
    ct.columns["season"] = RLEColumn::encode(raw);

    VecScanNode scan("t", ct, schema);
    scan.open();

    DataChunk* chunk1 = scan.nextChunk();
    ASSERT_NE(chunk1, nullptr);
    EXPECT_EQ(chunk1->num_rows, 1024);
    const auto& v1 = std::get<std::vector<int64_t>>(chunk1->columns[0].data);
    ASSERT_EQ(v1.size(), 1024u);
    for (int i = 0; i < 1000; ++i) EXPECT_EQ(v1[i], 2024) << "i=" << i;
    for (int i = 1000; i < 1024; ++i) EXPECT_EQ(v1[i], 2025) << "i=" << i;

    DataChunk* chunk2 = scan.nextChunk();
    ASSERT_NE(chunk2, nullptr);
    EXPECT_EQ(chunk2->num_rows, 476);
    const auto& v2 = std::get<std::vector<int64_t>>(chunk2->columns[0].data);
    ASSERT_EQ(v2.size(), 476u);
    for (int64_t val : v2) EXPECT_EQ(val, 2025);

    EXPECT_EQ(scan.nextChunk(), nullptr);
    scan.close();
}

TEST(VecScan, MixedEncodings_SchemaOrderPreserved) {
    // Verifies chunk->columns[i] always aligns with schema.column(i)
    // regardless of unordered_map iteration order inside ColumnarTable
    Schema schema = vecSchema({
        {"id", TypeId::INT},
        {"team", TypeId::STRING},
        {"speed", TypeId::DOUBLE}
    });

    std::vector<int64_t> ids = {1, 2, 3};
    std::vector<std::string> teams = {"Ferrari", "Mercedes", "Ferrari"};
    std::vector<double> speeds = {310.5, 305.2, 315.1};

    ColumnarTable ct(schema, 3);
    ct.columns["id"] = ids;
    ct.columns["team"] = DictionaryEncoder::encode(teams);
    ct.columns["speed"] = speeds;

    VecScanNode scan("t", ct, schema);
    scan.open();
    DataChunk* chunk = scan.nextChunk();
    ASSERT_NE(chunk, nullptr);
    ASSERT_EQ(chunk->columns.size(), 3u);

    EXPECT_EQ(chunk->columns[0].type, TypeId::INT);
    const auto& id_v = std::get<std::vector<int64_t>>(chunk->columns[0].data);
    EXPECT_EQ(id_v, ids);

    EXPECT_EQ(chunk->columns[1].type, TypeId::STRING);
    const auto& team_v = std::get<std::vector<std::string>>(chunk->columns[1].data);
    EXPECT_EQ(team_v, teams);

    EXPECT_EQ(chunk->columns[2].type, TypeId::DOUBLE);
    const auto& speed_v = std::get<std::vector<double>>(chunk->columns[2].data);
    ASSERT_EQ(speed_v.size(), 3u);
    for (int i = 0; i < 3; ++i)
        EXPECT_DOUBLE_EQ(speed_v[i], speeds[i]) << "i=" << i;

    scan.close();
}

TEST(VecScan, ValuesMatchGetValue) {
    // Cross-check every chunk value against ColumnarTable::getValue() as ground truth
    Schema schema = vecSchema({
        {"id", TypeId::INT},
        {"speed", TypeId::DOUBLE},
        {"team", TypeId::STRING}
    });

    std::vector<int64_t> ids = {1, 2, 3, 4, 5};
    std::vector<double> speeds = {300.0, 310.5, 295.2, 320.1, 305.8};
    std::vector<std::string> teams = {"Ferrari", "Mercedes", "Ferrari", "McLaren", "Ferrari"};

    ColumnarTable ct(schema, 5);
    ct.columns["id"] = ids;
    ct.columns["speed"] = speeds;
    ct.columns["team"] = DictionaryEncoder::encode(teams);

    VecScanNode scan("t", ct, schema);
    scan.open();

    int global_row = 0;
    while (DataChunk* chunk = scan.nextChunk()) {
        for (int r = 0; r < chunk->num_rows; ++r, ++global_row) {
            for (int c = 0; c < schema.size(); ++c) {
                Value expected = ct.getValue(schema.column(c).name, global_row);
                Value actual = std::visit([r](const auto& vec) {
                    return Value(vec[r]);
                }, chunk->columns[c].data);
                EXPECT_EQ(expected, actual)
                    << "Mismatch at global_row=" << global_row << " col=" << c;
            }
        }
    }

    scan.close();
    EXPECT_EQ(global_row, 5);
}

// ============================================================
// Week 14 helpers
// ============================================================

// AST construction helpers — avoid repetitive unique_ptr boilerplate in tests

static std::unique_ptr<Expr> col(const std::string& name) {
    auto r = std::make_unique<ColumnRef>();
    r->column_name = name;
    return r;
}

static std::unique_ptr<Expr> intLit(int64_t v) {
    return std::make_unique<Literal>(Value(v));
}

static std::unique_ptr<Expr> dblLit(double v) {
    return std::make_unique<Literal>(Value(v));
}

static std::unique_ptr<Expr> strLit(const std::string& v) {
    return std::make_unique<Literal>(Value(std::string(v)));
}

static std::unique_ptr<Expr> binOp(const std::string& op,
                                    std::unique_ptr<Expr> l,
                                    std::unique_ptr<Expr> r) {
    auto b = std::make_unique<BinaryExpr>();
    b->op = op;
    b->left  = std::move(l);
    b->right = std::move(r);
    return b;
}

// Drain a VecPlanNode fully, collect all materialized rows.
// Assumes each ColumnVector in the output chunk holds typed elements
// accessible row-by-row via std::visit.
static std::vector<Row> drainRows(VecPlanNode& node) {
    node.open();
    std::vector<Row> result;
    while (DataChunk* chunk = node.nextChunk()) {
        for (int r = 0; r < chunk->num_rows; ++r) {
            Row row;
            row.reserve(chunk->columns.size());
            for (const auto& cv : chunk->columns) {
                std::visit([&](const auto& vec) {
                    row.push_back(Value(vec[r]));
                }, cv.data);
            }
            result.push_back(std::move(row));
        }
    }
    node.close();
    return result;
}

// Drive a VecFilterNode fully and count total rows passing the predicate.
static int countPassingRows(VecFilterNode& filter) {
    filter.open();
    int total = 0;
    while (DataChunk* chunk = filter.nextChunk()) {
        total += static_cast<int>(chunk->sel.indices.size());
    }
    filter.close();
    return total;
}

// Build a single-INT-column ColumnarTable and VecScanNode.
static std::unique_ptr<VecScanNode> makeIntScan(const std::string& col_name,
                                                 const std::vector<int64_t>& vals) {
    Schema s = vecSchema({{col_name, TypeId::INT}});
    ColumnarTable ct(s, static_cast<int>(vals.size()));
    ct.columns[col_name] = vals;
    return std::make_unique<VecScanNode>("t", std::move(ct), s);
}

// ============================================================
// VecFilterNode unit tests
// ============================================================

TEST(VecFilter, AllRowsPass) {
    // Predicate id > 0 — every row passes.
    auto scan = makeIntScan("id", {1, 2, 3});
    Schema schema = scan->outputSchema();
    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp(">", col("id"), intLit(0)));

    filter->open();
    DataChunk* chunk = filter->nextChunk();
    ASSERT_NE(chunk, nullptr);

    EXPECT_EQ(static_cast<int>(chunk->sel.indices.size()), 3);
    EXPECT_EQ(chunk->sel.indices[0], 0);
    EXPECT_EQ(chunk->sel.indices[1], 1);
    EXPECT_EQ(chunk->sel.indices[2], 2);
    EXPECT_TRUE(chunk->filter_applied);

    filter->close();
}

TEST(VecFilter, NoRowsPass_ReturnsNonNull) {
    // Predicate id > 999 — no row passes.
    // Critical invariant: nextChunk() must NOT return nullptr when the predicate
    // rejects all rows in a chunk. Returning nullptr would prematurely terminate
    // a multi-chunk pipeline that might have passing rows in later chunks.
    auto scan = makeIntScan("id", {1, 2, 3});
    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp(">", col("id"), intLit(999)));

    filter->open();
    DataChunk* chunk = filter->nextChunk();
    ASSERT_NE(chunk, nullptr) << "Must return a chunk (not nullptr) even when 0 rows pass";
    EXPECT_TRUE(chunk->sel.indices.empty());
    EXPECT_TRUE(chunk->filter_applied);

    // Scan is now exhausted — next call must return nullptr.
    EXPECT_EQ(filter->nextChunk(), nullptr);

    filter->close();
}

TEST(VecFilter, NoRowsPass_StatsCorrect) {
    auto scan = makeIntScan("id", {1, 2, 3});
    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp(">", col("id"), intLit(999)));

    EXPECT_EQ(countPassingRows(*filter), 0);

    EXPECT_EQ(filter->stats.rows_in, 3);
    EXPECT_EQ(filter->stats.rows_out, 0);
}

TEST(VecFilter, SelectiveFilter_CorrectIndices) {
    // seasons: {2024, 2025, 2024, 2025} — predicate season = 2025 selects indices 1 and 3.
    Schema schema = vecSchema({{"season", TypeId::INT}});
    ColumnarTable ct(schema, 4);
    ct.columns["season"] = std::vector<int64_t>{2024, 2025, 2024, 2025};
    auto scan = std::make_unique<VecScanNode>("t", ct, schema);

    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp("=", col("season"), intLit(2025)));

    filter->open();
    DataChunk* chunk = filter->nextChunk();
    ASSERT_NE(chunk, nullptr);

    ASSERT_EQ(static_cast<int>(chunk->sel.indices.size()), 2);
    EXPECT_EQ(chunk->sel.indices[0], 1);
    EXPECT_EQ(chunk->sel.indices[1], 3);

    filter->close();
}

TEST(VecFilter, ColumnDataPreservedAtSelectedIndices) {
    // Table: id={10, 20, 30}, speed={1.0, 2.0, 3.0}.
    // Predicate: id > 10 → sel.indices = {1, 2}.
    // The column values at those indices must be accessible and correct.
    Schema schema = vecSchema({{"id", TypeId::INT}, {"speed", TypeId::DOUBLE}});
    ColumnarTable ct(schema, 3);
    ct.columns["id"]    = std::vector<int64_t>{10, 20, 30};
    ct.columns["speed"] = std::vector<double>{1.0, 2.0, 3.0};
    auto scan = std::make_unique<VecScanNode>("t", ct, schema);

    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp(">", col("id"), intLit(10)));

    filter->open();
    DataChunk* chunk = filter->nextChunk();
    ASSERT_NE(chunk, nullptr);
    ASSERT_EQ(static_cast<int>(chunk->sel.indices.size()), 2);

    const auto& ids    = std::get<std::vector<int64_t>>(chunk->columns[0].data);
    const auto& speeds = std::get<std::vector<double>>(chunk->columns[1].data);

    EXPECT_EQ(ids[chunk->sel.indices[0]], 20);
    EXPECT_EQ(ids[chunk->sel.indices[1]], 30);
    EXPECT_DOUBLE_EQ(speeds[chunk->sel.indices[0]], 2.0);
    EXPECT_DOUBLE_EQ(speeds[chunk->sel.indices[1]], 3.0);

    filter->close();
}

TEST(VecFilter, OutputSchemaMatchesChild) {
    // Filter must not modify the schema — outputSchema() == child's schema.
    Schema schema = vecSchema({{"id", TypeId::INT}, {"val", TypeId::DOUBLE}});
    ColumnarTable ct(schema, 2);
    ct.columns["id"]  = std::vector<int64_t>{1, 2};
    ct.columns["val"] = std::vector<double>{1.0, 2.0};
    auto scan = std::make_unique<VecScanNode>("t", ct, schema);

    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp(">", col("id"), intLit(0)));

    const Schema& out = filter->outputSchema();
    ASSERT_EQ(out.size(), 2);
    EXPECT_EQ(out.column(0).name, "id");
    EXPECT_EQ(out.column(0).type, TypeId::INT);
    EXPECT_EQ(out.column(1).name, "val");
    EXPECT_EQ(out.column(1).type, TypeId::DOUBLE);
}

TEST(VecFilter, ExplainString) {
    auto filter = std::make_unique<VecFilterNode>(
        makeIntScan("season", {2025}),
        binOp("=", col("season"), intLit(2025)));

    EXPECT_EQ(filter->explain(), "VecFilter [season = 2025]");
}

TEST(VecFilter, Stats_RowsIn_And_RowsOut) {
    // 4 rows, filter passes 2.
    Schema schema = vecSchema({{"season", TypeId::INT}});
    ColumnarTable ct(schema, 4);
    ct.columns["season"] = std::vector<int64_t>{2024, 2025, 2024, 2025};
    auto scan = std::make_unique<VecScanNode>("t", ct, schema);

    VecFilterNode filter(std::move(scan), binOp("=", col("season"), intLit(2025)));
    countPassingRows(filter);  // drives the pipeline

    EXPECT_EQ(filter.stats.rows_in, 4);
    EXPECT_EQ(filter.stats.rows_out, 2);
}

TEST(VecFilter, MultiChunk_TotalPassingRows) {
    // 2500 rows: first 1000 have season=2024, next 1500 have season=2025.
    // Filter: season = 2025 — must count exactly 1500 across 3 chunks.
    Schema schema = vecSchema({{"season", TypeId::INT}});
    std::vector<int64_t> seasons(1000, 2024LL);
    seasons.insert(seasons.end(), 1500, 2025LL);
    ColumnarTable ct(schema, 2500);
    ct.columns["season"] = seasons;

    VecFilterNode filter(
        std::make_unique<VecScanNode>("t", ct, schema),
        binOp("=", col("season"), intLit(2025)));

    EXPECT_EQ(countPassingRows(filter), 1500);
    EXPECT_EQ(filter.stats.rows_in,  2500);
    EXPECT_EQ(filter.stats.rows_out, 1500);
}

TEST(VecFilter, OpenResetsState) {
    // Re-opening the filter after a full drain must produce the same count.
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 5);
    ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5};

    VecFilterNode filter(
        std::make_unique<VecScanNode>("t", ct, schema),
        binOp(">", col("id"), intLit(2)));  // passes 3, 4, 5

    EXPECT_EQ(countPassingRows(filter), 3);
    EXPECT_EQ(countPassingRows(filter), 3);  // second pass after re-open
}

TEST(VecFilter, DoubleColumnPredicate) {
    // Filter on a DOUBLE column: speed > 310.0 — selects indices 0 and 2.
    Schema schema = vecSchema({{"speed", TypeId::DOUBLE}});
    ColumnarTable ct(schema, 3);
    ct.columns["speed"] = std::vector<double>{315.0, 305.0, 320.0};
    auto scan = std::make_unique<VecScanNode>("t", ct, schema);

    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp(">", col("speed"), dblLit(310.0)));

    filter->open();
    DataChunk* chunk = filter->nextChunk();
    ASSERT_NE(chunk, nullptr);

    ASSERT_EQ(static_cast<int>(chunk->sel.indices.size()), 2);
    EXPECT_EQ(chunk->sel.indices[0], 0);
    EXPECT_EQ(chunk->sel.indices[1], 2);

    filter->close();
}

TEST(VecFilter, StringColumnPredicate) {
    // Filter on a STRING column: team = "Ferrari" — selects indices 0 and 2.
    Schema schema = vecSchema({{"team", TypeId::STRING}});
    ColumnarTable ct(schema, 3);
    ct.columns["team"] = std::vector<std::string>{"Ferrari", "Mercedes", "Ferrari"};
    auto scan = std::make_unique<VecScanNode>("t", ct, schema);

    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp("=", col("team"), strLit("Ferrari")));

    filter->open();
    DataChunk* chunk = filter->nextChunk();
    ASSERT_NE(chunk, nullptr);

    ASSERT_EQ(static_cast<int>(chunk->sel.indices.size()), 2);
    EXPECT_EQ(chunk->sel.indices[0], 0);
    EXPECT_EQ(chunk->sel.indices[1], 2);

    filter->close();
}

// ============================================================
// VecProjectNode unit tests
// ============================================================

TEST(VecProject, IdentityAllColumns_Values) {
    // SELECT id, speed — all columns, verify exact values pass through.
    Schema schema = vecSchema({{"id", TypeId::INT}, {"speed", TypeId::DOUBLE}});
    ColumnarTable ct(schema, 3);
    ct.columns["id"]    = std::vector<int64_t>{1, 2, 3};
    ct.columns["speed"] = std::vector<double>{310.0, 305.0, 320.0};
    auto scan = std::make_unique<VecScanNode>("t", ct, schema);

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("id"));
    exprs.push_back(col("speed"));

    VecProjectNode project(std::move(scan), std::move(exprs), schema);
    auto rows = drainRows(project);

    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0].asInt(), 1);   EXPECT_DOUBLE_EQ(rows[0][1].asDouble(), 310.0);
    EXPECT_EQ(rows[1][0].asInt(), 2);   EXPECT_DOUBLE_EQ(rows[1][1].asDouble(), 305.0);
    EXPECT_EQ(rows[2][0].asInt(), 3);   EXPECT_DOUBLE_EQ(rows[2][1].asDouble(), 320.0);
}

TEST(VecProject, SingleColumnSubset) {
    // SELECT speed from a 3-column table — output has only 1 column.
    Schema input_schema = vecSchema({
        {"id", TypeId::INT}, {"speed", TypeId::DOUBLE}, {"season", TypeId::INT}
    });
    ColumnarTable ct(input_schema, 3);
    ct.columns["id"]     = std::vector<int64_t>{1, 2, 3};
    ct.columns["speed"]  = std::vector<double>{310.0, 305.0, 320.0};
    ct.columns["season"] = std::vector<int64_t>{2025, 2025, 2025};
    auto scan = std::make_unique<VecScanNode>("t", ct, input_schema);

    Schema out_schema = vecSchema({{"speed", TypeId::DOUBLE}});
    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("speed"));

    VecProjectNode project(std::move(scan), std::move(exprs), out_schema);
    auto rows = drainRows(project);

    ASSERT_EQ(rows.size(), 3u);
    ASSERT_EQ(rows[0].size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0][0].asDouble(), 310.0);
    EXPECT_DOUBLE_EQ(rows[1][0].asDouble(), 305.0);
    EXPECT_DOUBLE_EQ(rows[2][0].asDouble(), 320.0);
}

TEST(VecProject, OutputSchemaMatchesConstructorArg) {
    Schema out_schema = vecSchema({{"speed", TypeId::DOUBLE}});
    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("speed"));

    Schema in_schema = vecSchema({{"speed", TypeId::DOUBLE}});
    ColumnarTable ct(in_schema, 1);
    ct.columns["speed"] = std::vector<double>{300.0};

    VecProjectNode project(
        std::make_unique<VecScanNode>("t", ct, in_schema),
        std::move(exprs), out_schema);

    EXPECT_EQ(project.outputSchema().size(), 1);
    EXPECT_EQ(project.outputSchema().column(0).name, "speed");
    EXPECT_EQ(project.outputSchema().column(0).type, TypeId::DOUBLE);
}

TEST(VecProject, ExplainContainsMaterialize) {
    Schema schema = vecSchema({{"id", TypeId::INT}, {"speed", TypeId::DOUBLE}});
    ColumnarTable ct(schema, 1);
    ct.columns["id"]    = std::vector<int64_t>{1};
    ct.columns["speed"] = std::vector<double>{300.0};

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("id"));
    exprs.push_back(col("speed"));

    VecProjectNode project(
        std::make_unique<VecScanNode>("t", ct, schema),
        std::move(exprs), schema);

    EXPECT_NE(project.explain().find("(materialize)"), std::string::npos);
    EXPECT_NE(project.explain().find("VecProject"), std::string::npos);
}

TEST(VecProject, NumRowsMatchesColumnVectorSizes) {
    // After materialization, chunk->num_rows must equal the actual size of
    // every column vector — an internal consistency invariant.
    Schema schema = vecSchema({{"id", TypeId::INT}, {"speed", TypeId::DOUBLE}});
    ColumnarTable ct(schema, 5);
    ct.columns["id"]    = std::vector<int64_t>{1, 2, 3, 4, 5};
    ct.columns["speed"] = std::vector<double>{1.0, 2.0, 3.0, 4.0, 5.0};

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("id"));
    exprs.push_back(col("speed"));

    VecProjectNode project(
        std::make_unique<VecScanNode>("t", ct, schema),
        std::move(exprs), schema);

    project.open();
    while (DataChunk* chunk = project.nextChunk()) {
        for (const auto& cv : chunk->columns) {
            EXPECT_EQ(cv.size(), chunk->num_rows)
                << "Column vector size must equal chunk->num_rows";
        }
    }
    project.close();
}

TEST(VecProject, ReturnsNullptrWhenChildExhausted) {
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 2);
    ct.columns["id"] = std::vector<int64_t>{1, 2};

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("id"));

    VecProjectNode project(
        std::make_unique<VecScanNode>("t", ct, schema),
        std::move(exprs), schema);

    project.open();
    DataChunk* c1 = project.nextChunk();  // returns the 2-row chunk
    ASSERT_NE(c1, nullptr);
    EXPECT_EQ(c1->num_rows, 2);

    DataChunk* c2 = project.nextChunk();  // scan exhausted
    EXPECT_EQ(c2, nullptr);

    project.close();
}

TEST(VecProject, NoFilter_AllRowsMaterialized) {
    // VecScan → VecProject (no filter in between).
    // Chunk from VecScan has filter_applied=false, sel.indices=[].
    // VecProject must treat this as "all rows valid" and materialize all 4 rows.
    Schema schema = vecSchema({{"season", TypeId::INT}});
    ColumnarTable ct(schema, 4);
    ct.columns["season"] = std::vector<int64_t>{2024, 2025, 2024, 2025};

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("season"));

    VecProjectNode project(
        std::make_unique<VecScanNode>("t", ct, schema),
        std::move(exprs), schema);

    auto rows = drainRows(project);
    EXPECT_EQ(rows.size(), 4u);
}

TEST(VecProject, ColumnTypesPreserved) {
    // Output ColumnVectors must carry the type declared in the output schema.
    Schema schema = vecSchema({
        {"id", TypeId::INT}, {"speed", TypeId::DOUBLE}, {"team", TypeId::STRING}
    });
    ColumnarTable ct(schema, 1);
    ct.columns["id"]    = std::vector<int64_t>{1};
    ct.columns["speed"] = std::vector<double>{300.0};
    ct.columns["team"]  = std::vector<std::string>{"Ferrari"};

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("id"));
    exprs.push_back(col("speed"));
    exprs.push_back(col("team"));

    VecProjectNode project(
        std::make_unique<VecScanNode>("t", ct, schema),
        std::move(exprs), schema);

    project.open();
    DataChunk* chunk = project.nextChunk();
    ASSERT_NE(chunk, nullptr);

    EXPECT_EQ(chunk->columns[0].type, TypeId::INT);
    EXPECT_EQ(chunk->columns[1].type, TypeId::DOUBLE);
    EXPECT_EQ(chunk->columns[2].type, TypeId::STRING);

    // Verify the variant holds the right underlying type.
    EXPECT_TRUE(std::holds_alternative<std::vector<int64_t>>(chunk->columns[0].data));
    EXPECT_TRUE(std::holds_alternative<std::vector<double>>(chunk->columns[1].data));
    EXPECT_TRUE(std::holds_alternative<std::vector<std::string>>(chunk->columns[2].data));

    project.close();
}

TEST(VecProject, StatsTracked) {
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 5);
    ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5};

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("id"));

    VecProjectNode project(
        std::make_unique<VecScanNode>("t", ct, schema),
        std::move(exprs), schema);

    drainRows(project);

    EXPECT_EQ(project.stats.rows_in,  5);
    EXPECT_EQ(project.stats.rows_out, 5);
    EXPECT_GT(project.stats.elapsed_us, 0.0);
}

TEST(VecProject, MultiChunk_TotalRowCount) {
    // 2500 rows → 3 chunks; total materialized rows must be 2500.
    Schema schema = vecSchema({{"id", TypeId::INT}});
    std::vector<int64_t> ids(2500);
    std::iota(ids.begin(), ids.end(), 0LL);
    ColumnarTable ct(schema, 2500);
    ct.columns["id"] = ids;

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("id"));

    VecProjectNode project(
        std::make_unique<VecScanNode>("t", ct, schema),
        std::move(exprs), schema);

    auto rows = drainRows(project);
    EXPECT_EQ(rows.size(), 2500u);
}

// ============================================================
// Integration tests — VecScan → VecFilter → VecProject pipeline
// ============================================================

TEST(VecFilterProject, SelectsCorrectValues) {
    // Table: season INT, speed DOUBLE.
    // Filter: season = 2025 → rows at indices 1 and 3.
    // Project: SELECT speed → output is {200.0, 400.0}.
    // This is the canonical late-materialization verification test.
    Schema schema = vecSchema({{"season", TypeId::INT}, {"speed", TypeId::DOUBLE}});
    ColumnarTable ct(schema, 4);
    ct.columns["season"] = std::vector<int64_t>{2024, 2025, 2024, 2025};
    ct.columns["speed"]  = std::vector<double>{100.0, 200.0, 300.0, 400.0};

    auto scan   = std::make_unique<VecScanNode>("t", ct, schema);
    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp("=", col("season"), intLit(2025)));

    Schema out_schema = vecSchema({{"speed", TypeId::DOUBLE}});
    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("speed"));

    VecProjectNode project(std::move(filter), std::move(exprs), out_schema);
    auto rows = drainRows(project);

    ASSERT_EQ(rows.size(), 2u);
    EXPECT_DOUBLE_EQ(rows[0][0].asDouble(), 200.0);
    EXPECT_DOUBLE_EQ(rows[1][0].asDouble(), 400.0);
}

TEST(VecFilterProject, NoMatchesProducesEmptyChunksNotNullptr) {
    // When the filter rejects every row, VecProject must return a chunk with
    // num_rows=0 (not nullptr) until the scan is fully exhausted, then nullptr.
    // Returning nullptr early would break multi-chunk pipelines.
    Schema schema = vecSchema({{"season", TypeId::INT}});
    ColumnarTable ct(schema, 3);
    ct.columns["season"] = std::vector<int64_t>{2024, 2024, 2024};

    auto scan   = std::make_unique<VecScanNode>("t", ct, schema);
    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp("=", col("season"), intLit(2025)));

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("season"));

    VecProjectNode project(std::move(filter), std::move(exprs), schema);
    project.open();

    DataChunk* chunk = project.nextChunk();
    ASSERT_NE(chunk, nullptr) << "Must not return nullptr — scan not yet exhausted";
    EXPECT_EQ(chunk->num_rows, 0);

    EXPECT_EQ(project.nextChunk(), nullptr);  // scan now exhausted
    project.close();
}

TEST(VecFilterProject, MultiChunkCorrectness) {
    // 2500 rows: first 1000 have season=2024, next 1500 have season=2025.
    // Filter: season = 2025.
    // Project: SELECT season.
    // Total output rows must be 1500 and all values must be 2025.
    Schema schema = vecSchema({{"season", TypeId::INT}});
    std::vector<int64_t> seasons(1000, 2024LL);
    seasons.insert(seasons.end(), 1500, 2025LL);
    ColumnarTable ct(schema, 2500);
    ct.columns["season"] = seasons;

    auto scan   = std::make_unique<VecScanNode>("t", ct, schema);
    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp("=", col("season"), intLit(2025)));

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("season"));

    VecProjectNode project(std::move(filter), std::move(exprs), schema);
    auto rows = drainRows(project);

    EXPECT_EQ(rows.size(), 1500u);
    for (size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i][0].asInt(), 2025) << "Row " << i << " has wrong season";
    }
}

TEST(VecFilterProject, FilterAppliedFlagClearedInProjectOutput) {
    // VecProject's output chunk must have filter_applied=false.
    // This ensures downstream Week-15 operators (VecHashAggregate) do not
    // misread a stale flag from a previous filter stage.
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 3);
    ct.columns["id"] = std::vector<int64_t>{1, 2, 3};

    auto scan   = std::make_unique<VecScanNode>("t", ct, schema);
    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp(">", col("id"), intLit(0)));

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("id"));

    VecProjectNode project(std::move(filter), std::move(exprs), schema);
    project.open();
    DataChunk* chunk = project.nextChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_FALSE(chunk->filter_applied);
    project.close();
}

// ============================================================
// SelectionVector.size field — spec compliance
// ============================================================

TEST(VecFilter, SelectionVectorSizeFieldMirrorsIndices) {
    // sel.size must equal sel.indices.size() after every nextChunk() call.
    // This is a spec field used by Week-15 operators; it must be kept in sync.
    Schema schema = vecSchema({{"season", TypeId::INT}});
    ColumnarTable ct(schema, 4);
    ct.columns["season"] = std::vector<int64_t>{2024, 2025, 2024, 2025};

    VecFilterNode filter(
        std::make_unique<VecScanNode>("t", ct, schema),
        binOp("=", col("season"), intLit(2025)));

    filter.open();
    DataChunk* chunk = filter.nextChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->sel.size, static_cast<int>(chunk->sel.indices.size()));
    EXPECT_EQ(chunk->sel.size, 2);  // rows at index 1 and 3 pass

    EXPECT_EQ(filter.nextChunk(), nullptr);
    filter.close();
}

TEST(VecFilter, SelectionVectorSizeZeroWhenNonePass) {
    // When no rows pass, sel.size must be 0 (not a stale value from a prior chunk).
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 3);
    ct.columns["id"] = std::vector<int64_t>{1, 2, 3};

    VecFilterNode filter(
        std::make_unique<VecScanNode>("t", ct, schema),
        binOp(">", col("id"), intLit(999)));

    filter.open();
    DataChunk* chunk = filter.nextChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->sel.size, 0);
    EXPECT_EQ(chunk->sel.size, static_cast<int>(chunk->sel.indices.size()));
    filter.close();
}

// ============================================================
// End-to-end correctness: Volcano vs Vectorized pipeline
// Each test builds the same query on both paths and asserts identical results.
// ============================================================

static std::vector<Row> runVolcano(
        ColumnarTable ct,
        const Schema& scan_schema,
        std::unique_ptr<Expr> pred,
        std::vector<std::unique_ptr<Expr>> proj_exprs,
        const Schema& proj_schema) {
    std::unique_ptr<PlanNode> node =
        std::make_unique<SeqScanNode>("t", std::move(ct), scan_schema);
    if (pred)
        node = std::make_unique<FilterNode>(std::move(node), std::move(pred));
    node = std::make_unique<ProjectNode>(
        std::move(node), std::move(proj_exprs), proj_schema);
    node->open();
    std::vector<Row> rows;
    while (Row* r = node->next()) rows.push_back(*r);
    node->close();
    return rows;
}

static std::vector<Row> runVec(
        ColumnarTable ct,
        const Schema& scan_schema,
        std::unique_ptr<Expr> pred,
        std::vector<std::unique_ptr<Expr>> proj_exprs,
        const Schema& proj_schema) {
    std::unique_ptr<VecPlanNode> node =
        std::make_unique<VecScanNode>("t", std::move(ct), scan_schema);
    if (pred)
        node = std::make_unique<VecFilterNode>(std::move(node), std::move(pred));
    VecProjectNode project(std::move(node), std::move(proj_exprs), proj_schema);
    return drainRows(project);
}

static void expectRowsEqual(const std::vector<Row>& expected,
                             const std::vector<Row>& actual) {
    ASSERT_EQ(expected.size(), actual.size()) << "Row count mismatch";
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(expected[i].size(), actual[i].size())
            << "Column count mismatch at row " << i;
        for (size_t j = 0; j < expected[i].size(); ++j) {
            EXPECT_EQ(expected[i][j], actual[i][j])
                << "Value mismatch at row=" << i << " col=" << j;
        }
    }
}

TEST(VecEndToEnd, SelectStar_NoFilter) {
    // SELECT id, speed FROM t  (no WHERE clause)
    Schema schema = vecSchema({{"id", TypeId::INT}, {"speed", TypeId::DOUBLE}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 3);
        ct.columns["id"]    = std::vector<int64_t>{1, 2, 3};
        ct.columns["speed"] = std::vector<double>{310.0, 305.0, 320.0};
        return ct;
    };

    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id")); e.push_back(col("speed"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema, nullptr, makeExprs(), schema);
    auto vec = runVec(makeTable(), schema, nullptr, makeExprs(), schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, SelectSingleColumn_NoFilter) {
    // SELECT speed FROM t  (column subset, no WHERE)
    Schema in_schema  = vecSchema({{"id", TypeId::INT}, {"speed", TypeId::DOUBLE}});
    Schema out_schema = vecSchema({{"speed", TypeId::DOUBLE}});
    auto makeTable = [&]() {
        ColumnarTable ct(in_schema, 3);
        ct.columns["id"]    = std::vector<int64_t>{1, 2, 3};
        ct.columns["speed"] = std::vector<double>{310.0, 305.0, 320.0};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("speed"));
        return e;
    };

    auto vol = runVolcano(makeTable(), in_schema, nullptr, makeExprs(), out_schema);
    auto vec = runVec(makeTable(), in_schema, nullptr, makeExprs(), out_schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, IntFilter_EqualityPredicate) {
    // SELECT id, speed FROM t WHERE season = 2025
    Schema schema = vecSchema({
        {"id", TypeId::INT}, {"speed", TypeId::DOUBLE}, {"season", TypeId::INT}
    });
    Schema out_schema = vecSchema({{"id", TypeId::INT}, {"speed", TypeId::DOUBLE}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 4);
        ct.columns["id"]     = std::vector<int64_t>{1, 2, 3, 4};
        ct.columns["speed"]  = std::vector<double>{310.0, 305.0, 320.0, 298.0};
        ct.columns["season"] = std::vector<int64_t>{2024, 2025, 2024, 2025};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id")); e.push_back(col("speed"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp("=", col("season"), intLit(2025)), makeExprs(), out_schema);
    auto vec = runVec(makeTable(), schema,
        binOp("=", col("season"), intLit(2025)), makeExprs(), out_schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, IntFilter_GtPredicate) {
    // SELECT id FROM t WHERE id > 2
    Schema schema = vecSchema({{"id", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 5);
        ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp(">", col("id"), intLit(2)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp(">", col("id"), intLit(2)), makeExprs(), schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, IntFilter_LtPredicate) {
    // SELECT id FROM t WHERE id < 3
    Schema schema = vecSchema({{"id", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 5);
        ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp("<", col("id"), intLit(3)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp("<", col("id"), intLit(3)), makeExprs(), schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, DoubleFilter_GtPredicate) {
    // SELECT speed FROM t WHERE speed > 310.0
    Schema schema = vecSchema({{"speed", TypeId::DOUBLE}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 4);
        ct.columns["speed"] = std::vector<double>{315.0, 305.0, 320.0, 308.0};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("speed"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp(">", col("speed"), dblLit(310.0)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp(">", col("speed"), dblLit(310.0)), makeExprs(), schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, StringFilter_EqualityPredicate) {
    // SELECT id FROM t WHERE team = 'Ferrari'
    Schema schema    = vecSchema({{"id", TypeId::INT}, {"team", TypeId::STRING}});
    Schema out_schema = vecSchema({{"id", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 4);
        ct.columns["id"]   = std::vector<int64_t>{1, 2, 3, 4};
        ct.columns["team"] = std::vector<std::string>{"Ferrari", "McLaren", "Ferrari", "Mercedes"};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp("=", col("team"), strLit("Ferrari")), makeExprs(), out_schema);
    auto vec = runVec(makeTable(), schema,
        binOp("=", col("team"), strLit("Ferrari")), makeExprs(), out_schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, FilterRetainsNoRows) {
    // WHERE condition is never true — both paths must return 0 rows.
    Schema schema = vecSchema({{"id", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 3);
        ct.columns["id"] = std::vector<int64_t>{1, 2, 3};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp(">", col("id"), intLit(999)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp(">", col("id"), intLit(999)), makeExprs(), schema);

    EXPECT_EQ(vol.size(), 0u);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, FilterRetainsAllRows) {
    // WHERE id > 0 — all rows pass; output must match SELECT * ordering exactly.
    Schema schema = vecSchema({{"id", TypeId::INT}, {"speed", TypeId::DOUBLE}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 3);
        ct.columns["id"]    = std::vector<int64_t>{1, 2, 3};
        ct.columns["speed"] = std::vector<double>{310.0, 305.0, 320.0};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id")); e.push_back(col("speed"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp(">", col("id"), intLit(0)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp(">", col("id"), intLit(0)), makeExprs(), schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, MixedTypes_MultiColumnProjection_WithFilter) {
    // SELECT id, speed, team FROM t WHERE season = 2025
    Schema schema = vecSchema({
        {"id", TypeId::INT}, {"speed", TypeId::DOUBLE},
        {"team", TypeId::STRING}, {"season", TypeId::INT}
    });
    Schema out_schema = vecSchema({
        {"id", TypeId::INT}, {"speed", TypeId::DOUBLE}, {"team", TypeId::STRING}
    });
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 4);
        ct.columns["id"]     = std::vector<int64_t>{1, 2, 3, 4};
        ct.columns["speed"]  = std::vector<double>{310.0, 305.0, 320.0, 298.0};
        ct.columns["team"]   = std::vector<std::string>{"Ferrari", "McLaren", "Ferrari", "Mercedes"};
        ct.columns["season"] = std::vector<int64_t>{2024, 2025, 2024, 2025};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id")); e.push_back(col("speed")); e.push_back(col("team"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp("=", col("season"), intLit(2025)), makeExprs(), out_schema);
    auto vec = runVec(makeTable(), schema,
        binOp("=", col("season"), intLit(2025)), makeExprs(), out_schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, LargeTable_NoFilter_MultiChunk) {
    // 2500 rows → 3 chunks; total output must match Volcano row-for-row.
    Schema schema = vecSchema({{"id", TypeId::INT}});
    auto makeTable = [&]() {
        std::vector<int64_t> ids(2500);
        std::iota(ids.begin(), ids.end(), 1LL);
        ColumnarTable ct(schema, 2500);
        ct.columns["id"] = ids;
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema, nullptr, makeExprs(), schema);
    auto vec = runVec(makeTable(), schema, nullptr, makeExprs(), schema);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, IntFilter_NotEqualPredicate) {
    // SELECT id FROM t WHERE id != 3
    Schema schema = vecSchema({{"id", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 5);
        ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp("!=", col("id"), intLit(3)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp("!=", col("id"), intLit(3)), makeExprs(), schema);
    ASSERT_EQ(vec.size(), 4u);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, IntFilter_LtEqPredicate) {
    // SELECT id FROM t WHERE id <= 3
    Schema schema = vecSchema({{"id", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 5);
        ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp("<=", col("id"), intLit(3)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp("<=", col("id"), intLit(3)), makeExprs(), schema);
    ASSERT_EQ(vec.size(), 3u);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, IntFilter_GtEqPredicate) {
    // SELECT id FROM t WHERE id >= 3
    Schema schema = vecSchema({{"id", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 5);
        ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp(">=", col("id"), intLit(3)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp(">=", col("id"), intLit(3)), makeExprs(), schema);
    ASSERT_EQ(vec.size(), 3u);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, AndComposition_BothFastPath) {
    // SELECT id FROM t WHERE season = 2025 AND id > 2
    // Both predicates are col-op-literal — both hit the fast path.
    // The AND composition via sv_intersect must return only rows satisfying both.
    Schema schema = vecSchema({{"id", TypeId::INT}, {"season", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 5);
        ct.columns["id"]     = std::vector<int64_t>{1, 2, 3, 4, 5};
        ct.columns["season"] = std::vector<int64_t>{2025, 2025, 2024, 2025, 2024};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };
    Schema out_schema = vecSchema({{"id", TypeId::INT}});

    // season=2025: rows 0,1,3  AND  id>2: rows 2,3,4  → intersection: row 3 (id=4)
    auto pred = [&]() {
        return binOp("AND",
            binOp("=",  col("season"), intLit(2025)),
            binOp(">",  col("id"),     intLit(2)));
    };

    auto vol = runVolcano(makeTable(), schema, pred(), makeExprs(), out_schema);
    auto vec = runVec(makeTable(), schema, pred(), makeExprs(), out_schema);
    ASSERT_EQ(vec.size(), 1u);
    EXPECT_EQ(vec[0][0].asInt(), 4);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, OrComposition_BothFastPath) {
    // SELECT id FROM t WHERE id = 1 OR id = 5
    // Both predicates are fast-path; union must deduplicate correctly.
    Schema schema = vecSchema({{"id", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 5);
        ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id"));
        return e;
    };

    auto pred = [&]() {
        return binOp("OR",
            binOp("=", col("id"), intLit(1)),
            binOp("=", col("id"), intLit(5)));
    };

    auto vol = runVolcano(makeTable(), schema, pred(), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema, pred(), makeExprs(), schema);
    ASSERT_EQ(vec.size(), 2u);
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, DoubleCol_IntLiteral_Coercion) {
    // SELECT speed FROM t WHERE speed > 310
    // Column is DOUBLE, literal is INT (as the parser would produce for "310").
    // The fast path must coerce 310 → 310.0 before comparing.
    Schema schema = vecSchema({{"speed", TypeId::DOUBLE}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 4);
        ct.columns["speed"] = std::vector<double>{315.0, 305.0, 320.0, 308.0};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("speed"));
        return e;
    };

    // Use intLit(310) to simulate the parser producing an INT literal for "310"
    auto vol = runVolcano(makeTable(), schema,
        binOp(">", col("speed"), intLit(310)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp(">", col("speed"), intLit(310)), makeExprs(), schema);
    ASSERT_EQ(vec.size(), 2u);  // 315.0 and 320.0 pass
    expectRowsEqual(vol, vec);
}

TEST(VecEndToEnd, LargeTable_WithFilter_MultiChunk) {
    // 2500 rows, first 1000 season=2024 rest season=2025.
    // WHERE season = 2025 → 1500 rows; must match Volcano exactly.
    Schema schema = vecSchema({{"id", TypeId::INT}, {"season", TypeId::INT}});
    auto makeTable = [&]() {
        std::vector<int64_t> ids(2500), seasons(1000, 2024LL);
        std::iota(ids.begin(), ids.end(), 1LL);
        seasons.insert(seasons.end(), 1500, 2025LL);
        ColumnarTable ct(schema, 2500);
        ct.columns["id"]     = ids;
        ct.columns["season"] = seasons;
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("id")); e.push_back(col("season"));
        return e;
    };

    auto vol = runVolcano(makeTable(), schema,
        binOp("=", col("season"), intLit(2025)), makeExprs(), schema);
    auto vec = runVec(makeTable(), schema,
        binOp("=", col("season"), intLit(2025)), makeExprs(), schema);

    ASSERT_EQ(vec.size(), 1500u);
    expectRowsEqual(vol, vec);
}