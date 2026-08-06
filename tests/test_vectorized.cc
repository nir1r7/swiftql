#include <gtest/gtest.h>
#include "execution/vec_scan_node.h"
#include "execution/vec_filter_node.h"
#include "execution/vec_project_node.h"
#include "execution/vec_limit_node.h"
#include "execution/vec_sort_node.h"
#include "execution/vec_distinct_node.h"
#include "execution/vec_hash_aggregate_node.h"
#include "execution/vec_hash_join_node.h"
#include "execution/vec_types.h"
#include "execution/expression_executor.h"
#include "execution/evaluator.h"
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

// OrderByItem helpers for VecSortNode tests
static OrderByItem orderAsc(std::unique_ptr<Expr> e) { return {std::move(e), false}; }
static OrderByItem orderDesc(std::unique_ptr<Expr> e) { return {std::move(e), true}; }

// Drain a VecPlanNode fully, collect all materialized rows.
// Respects filter_applied so it works correctly with or without a SelectionVector.
static std::vector<Row> drainRows(VecPlanNode& node) {
    node.open();
    std::vector<Row> result;
    while (DataChunk* chunk = node.nextChunk()) {
        int n = chunk->filter_applied
            ? static_cast<int>(chunk->sel.indices.size())
            : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            Row row;
            row.reserve(chunk->columns.size());
            for (const auto& cv : chunk->columns) {
                row.push_back(valueAt(cv, r));   // NULL-aware, mirrors main.cc
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

    // Week 24: exprToString parenthesizes BinaryExpr (name-contract injectivity)
    EXPECT_EQ(filter->explain(), "VecFilter [(season = 2025)]");
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
    // 5 rows can legitimately complete within one clock tick at -O3, so this
    // can't demand > 0 (flaked on Release builds); nonzero timing behavior is
    // pinned by the 5000-row BuildPhaseIsTimed tests instead
    EXPECT_GE(project.stats.elapsed_us, 0.0);
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

// Two VecFilterNodes stacked: second filter sees filter_applied=true from first.
// Only rows passing BOTH predicates should survive.
// id > 1 AND id < 5  →  rows 2, 3, 4  (out of 1..6)
TEST(VecFilter, StackedFiltersCompose) {
    Schema schema = vecSchema({{"id", TypeId::INT}});
    ColumnarTable ct(schema, 6);
    ct.columns["id"] = std::vector<int64_t>{1, 2, 3, 4, 5, 6};

    auto inner = std::make_unique<VecFilterNode>(
        std::make_unique<VecScanNode>("t", ct, schema),
        binOp(">", col("id"), intLit(1)));   // passes rows 2,3,4,5,6

    VecFilterNode outer(std::move(inner),
        binOp("<", col("id"), intLit(5)));   // of those, keeps 2,3,4

    outer.open();
    DataChunk* chunk = outer.nextChunk();
    ASSERT_NE(chunk, nullptr);
    ASSERT_TRUE(chunk->filter_applied);
    ASSERT_EQ(static_cast<int>(chunk->sel.indices.size()), 3);
    EXPECT_EQ(chunk->sel.indices[0], 1);  // physical row 1  → id=2
    EXPECT_EQ(chunk->sel.indices[1], 2);  // physical row 2  → id=3
    EXPECT_EQ(chunk->sel.indices[2], 3);  // physical row 3  → id=4
    outer.close();
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
    // AND cascades the left operand's SelectionVector into the right; the result
    // must be exactly the rows satisfying both.
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

TEST(VecEndToEnd, AndCascade_DifferingSurvivorSets) {
    // Guards the AND cascade (columnar_eval.cc): the left operand narrows the
    // rows, the right is evaluated only over survivors, and the result must be
    // the exact intersection — not the union, not the left, not the right.
    // Left and right keep genuinely different row sets so a broken cascade
    // (e.g. dropping one side) cannot accidentally produce the right answer.
    Schema schema = vecSchema({{"a", TypeId::INT}, {"b", TypeId::INT}});
    auto makeTable = [&]() {
        ColumnarTable ct(schema, 6);
        ct.columns["a"] = std::vector<int64_t>{1, 5, 2, 6, 3, 7};
        ct.columns["b"] = std::vector<int64_t>{10, 40, 20, 25, 50, 15};
        return ct;
    };
    auto makeExprs = [&]() {
        std::vector<std::unique_ptr<Expr>> e;
        e.push_back(col("a"));
        return e;
    };
    Schema out_schema = vecSchema({{"a", TypeId::INT}});

    // a > 2  → rows {1,3,4,5} (a = 5,6,3,7)
    // b < 30 → rows {0,2,3,5} (b = 10,20,25,15)
    // intersection → rows {3,5} → a = 6, 7
    auto pred = [&]() {
        return binOp("AND",
            binOp(">", col("a"), intLit(2)),
            binOp("<", col("b"), intLit(30)));
    };

    auto vol = runVolcano(makeTable(), schema, pred(), makeExprs(), out_schema);
    auto vec = runVec(makeTable(), schema, pred(), makeExprs(), out_schema);
    ASSERT_EQ(vec.size(), 2u);
    std::vector<int64_t> got{vec[0][0].asInt(), vec[1][0].asInt()};
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, (std::vector<int64_t>{6, 7}));
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

// ============================================================
// makeScan: build a multi-column VecScanNode from a Row set
// ============================================================
static std::unique_ptr<VecScanNode> makeScan(const Schema& schema,
                                              const std::vector<Row>& rows) {
    int n = static_cast<int>(rows.size());
    ColumnarTable ct(schema, n);
    for (int c = 0; c < schema.size(); ++c) {
        const std::string& name = schema.column(c).name;
        switch (schema.column(c).type) {
            case TypeId::INT: {
                std::vector<int64_t> cv;
                cv.reserve(n);
                for (const auto& row : rows) cv.push_back(row[c].asInt());
                ct.columns[name] = std::move(cv);
                break;
            }
            case TypeId::DOUBLE: {
                std::vector<double> cv;
                cv.reserve(n);
                for (const auto& row : rows) cv.push_back(row[c].asDouble());
                ct.columns[name] = std::move(cv);
                break;
            }
            case TypeId::STRING: {
                std::vector<std::string> cv;
                cv.reserve(n);
                for (const auto& row : rows) cv.push_back(row[c].asString());
                ct.columns[name] = std::move(cv);
                break;
            }
        }
    }
    return std::make_unique<VecScanNode>("t", std::move(ct), schema);
}

// ============================================================
// VecLimit
// ============================================================

TEST(VecLimit, ExactLimit) {
    auto scan = makeIntScan("id", {1, 2, 3, 4, 5});
    auto lim = std::make_unique<VecLimitNode>(std::move(scan), 5);
    auto rows = drainRows(*lim);
    ASSERT_EQ(rows.size(), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(rows[i][0].asInt(), i + 1);
}

TEST(VecLimit, LimitLessThanChunk) {
    auto scan = makeIntScan("id", {10, 20, 30, 40, 50});
    auto lim = std::make_unique<VecLimitNode>(std::move(scan), 3);
    lim->open();
    DataChunk* chunk = lim->nextChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->num_rows, 3);
    ASSERT_EQ(std::get<std::vector<int64_t>>(chunk->columns[0].data).size(), 3u);
    EXPECT_EQ(lim->nextChunk(), nullptr);
    lim->close();
}

TEST(VecLimit, LimitMoreThanRows) {
    auto scan = makeIntScan("id", {1, 2, 3});
    auto lim = std::make_unique<VecLimitNode>(std::move(scan), 100);
    auto rows = drainRows(*lim);
    ASSERT_EQ(rows.size(), 3u);
}

TEST(VecLimit, LimitZero) {
    auto scan = makeIntScan("id", {1, 2, 3});
    auto lim = std::make_unique<VecLimitNode>(std::move(scan), 0);
    lim->open();
    EXPECT_EQ(lim->nextChunk(), nullptr);
    lim->close();
}

TEST(VecLimit, LimitAcrossChunkBoundary) {
    int total = BATCH_SIZE + 100;
    int limit  = BATCH_SIZE + 50;
    std::vector<int64_t> vals(total);
    std::iota(vals.begin(), vals.end(), 0LL);
    auto lim = std::make_unique<VecLimitNode>(makeIntScan("id", vals), limit);
    auto rows = drainRows(*lim);
    ASSERT_EQ(static_cast<int>(rows.size()), limit);
    EXPECT_EQ(rows[0][0].asInt(), 0);
    EXPECT_EQ(rows[limit - 1][0].asInt(), limit - 1);
}

TEST(VecLimit, EarlyTermination) {
    auto scan = makeIntScan("id", {1, 2, 3, 4, 5});
    auto lim = std::make_unique<VecLimitNode>(std::move(scan), 2);
    lim->open();
    DataChunk* first = lim->nextChunk();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->num_rows, 2);
    EXPECT_EQ(lim->nextChunk(), nullptr);
    EXPECT_EQ(lim->nextChunk(), nullptr);
    lim->close();
}

TEST(VecLimit, OpenResetsState) {
    Schema schema = vecSchema({{"id", TypeId::INT}});
    std::vector<Row> rows;
    for (int i = 1; i <= 5; ++i) rows.push_back({Value(static_cast<int64_t>(i))});
    auto lim = std::make_unique<VecLimitNode>(makeScan(schema, rows), 3);
    auto r1 = drainRows(*lim);
    auto r2 = drainRows(*lim);
    ASSERT_EQ(r1.size(), 3u);
    ASSERT_EQ(r2.size(), 3u);
    for (size_t i = 0; i < r1.size(); ++i)
        EXPECT_EQ(r1[i][0].asInt(), r2[i][0].asInt());
}

// ============================================================
// VecSort
// ============================================================

TEST(VecSort, SingleColumnAscending) {
    auto scan = makeIntScan("id", {5, 3, 1, 4, 2});
    std::vector<OrderByItem> ob; ob.push_back(orderAsc(col("id")));
    auto sort = std::make_unique<VecSortNode>(std::move(scan), std::move(ob));
    auto rows = drainRows(*sort);
    ASSERT_EQ(rows.size(), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(rows[i][0].asInt(), i + 1) << "row " << i;
}

TEST(VecSort, SingleColumnDescending) {
    auto scan = makeIntScan("id", {3, 1, 4, 1, 5});
    std::vector<OrderByItem> ob; ob.push_back(orderDesc(col("id")));
    auto sort = std::make_unique<VecSortNode>(std::move(scan), std::move(ob));
    auto rows = drainRows(*sort);
    ASSERT_EQ(rows.size(), 5u);
    // expected DESC order: 5, 4, 3, 1, 1
    EXPECT_EQ(rows[0][0].asInt(), 5);
    EXPECT_EQ(rows[1][0].asInt(), 4);
    EXPECT_EQ(rows[2][0].asInt(), 3);
    EXPECT_EQ(rows[3][0].asInt(), 1);
    EXPECT_EQ(rows[4][0].asInt(), 1);
}

TEST(VecSort, MultiColumnSort) {
    Schema schema = vecSchema({{"a", TypeId::INT}, {"b", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(1)), Value(int64_t(3))},
        {Value(int64_t(1)), Value(int64_t(1))},
        {Value(int64_t(2)), Value(int64_t(2))},
        {Value(int64_t(2)), Value(int64_t(1))},
    };
    std::vector<OrderByItem> ob;
    ob.push_back(orderAsc(col("a")));
    ob.push_back(orderAsc(col("b")));
    auto sort = std::make_unique<VecSortNode>(makeScan(schema, input), std::move(ob));
    auto rows = drainRows(*sort);
    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0][0].asInt(), 1); EXPECT_EQ(rows[0][1].asInt(), 1);
    EXPECT_EQ(rows[1][0].asInt(), 1); EXPECT_EQ(rows[1][1].asInt(), 3);
    EXPECT_EQ(rows[2][0].asInt(), 2); EXPECT_EQ(rows[2][1].asInt(), 1);
    EXPECT_EQ(rows[3][0].asInt(), 2); EXPECT_EQ(rows[3][1].asInt(), 2);
}

TEST(VecSort, AlreadySorted) {
    auto scan = makeIntScan("id", {1, 2, 3, 4, 5});
    std::vector<OrderByItem> ob; ob.push_back(orderAsc(col("id")));
    auto sort = std::make_unique<VecSortNode>(std::move(scan), std::move(ob));
    auto rows = drainRows(*sort);
    ASSERT_EQ(rows.size(), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(rows[i][0].asInt(), i + 1);
}

TEST(VecSort, ReverseSorted) {
    auto scan = makeIntScan("id", {5, 4, 3, 2, 1});
    std::vector<OrderByItem> ob; ob.push_back(orderAsc(col("id")));
    auto sort = std::make_unique<VecSortNode>(std::move(scan), std::move(ob));
    auto rows = drainRows(*sort);
    ASSERT_EQ(rows.size(), 5u);
    for (int i = 0; i < 5; ++i) EXPECT_EQ(rows[i][0].asInt(), i + 1);
}

TEST(VecSort, MultiChunkInput) {
    int n = BATCH_SIZE + 100;
    std::vector<int64_t> vals(n);
    for (int i = 0; i < n; ++i) vals[i] = static_cast<int64_t>(n - 1 - i);
    std::vector<OrderByItem> ob; ob.push_back(orderAsc(col("id")));
    auto sort = std::make_unique<VecSortNode>(makeIntScan("id", vals), std::move(ob));
    auto rows = drainRows(*sort);
    ASSERT_EQ(static_cast<int>(rows.size()), n);
    for (int i = 0; i < n; ++i) EXPECT_EQ(rows[i][0].asInt(), i) << "i=" << i;
}

TEST(VecSort, EmptyInput) {
    auto scan = makeIntScan("id", {});
    std::vector<OrderByItem> ob; ob.push_back(orderAsc(col("id")));
    auto sort = std::make_unique<VecSortNode>(std::move(scan), std::move(ob));
    sort->open();
    EXPECT_EQ(sort->nextChunk(), nullptr);
    sort->close();
}

TEST(VecSort, OutputSchemaUnchanged) {
    Schema schema = vecSchema({{"x", TypeId::INT}, {"y", TypeId::DOUBLE}});
    std::vector<Row> rows = {{Value(int64_t(1)), Value(2.0)}};
    std::vector<OrderByItem> ob; ob.push_back(orderAsc(col("x")));
    auto sort = std::make_unique<VecSortNode>(makeScan(schema, rows), std::move(ob));
    EXPECT_EQ(sort->outputSchema().size(), 2);
    EXPECT_EQ(sort->outputSchema().column(0).name, "x");
    EXPECT_EQ(sort->outputSchema().column(1).name, "y");
}

TEST(VecSort, OpenResetsState) {
    auto scan = makeIntScan("id", {3, 1, 2});
    std::vector<OrderByItem> ob; ob.push_back(orderAsc(col("id")));
    auto sort = std::make_unique<VecSortNode>(std::move(scan), std::move(ob));
    auto r1 = drainRows(*sort);
    auto r2 = drainRows(*sort);
    ASSERT_EQ(r1.size(), 3u);
    ASSERT_EQ(r2.size(), 3u);
    for (size_t i = 0; i < r1.size(); ++i)
        EXPECT_EQ(r1[i][0].asInt(), r2[i][0].asInt());
}

// ============================================================
// VecDistinct
// ============================================================

TEST(VecDistinct, AllUnique) {
    auto scan = makeIntScan("id", {1, 2, 3, 4, 5});
    auto dist = std::make_unique<VecDistinctNode>(std::move(scan));
    auto rows = drainRows(*dist);
    EXPECT_EQ(rows.size(), 5u);
}

TEST(VecDistinct, AllDuplicate) {
    auto scan = makeIntScan("id", {7, 7, 7, 7, 7});
    auto dist = std::make_unique<VecDistinctNode>(std::move(scan));
    auto rows = drainRows(*dist);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].asInt(), 7);
}

TEST(VecDistinct, PartialDuplicates) {
    auto scan = makeIntScan("id", {1, 2, 2, 3, 3, 3});
    auto dist = std::make_unique<VecDistinctNode>(std::move(scan));
    auto rows = drainRows(*dist);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0].asInt(), 1);
    EXPECT_EQ(rows[1][0].asInt(), 2);
    EXPECT_EQ(rows[2][0].asInt(), 3);
}

TEST(VecDistinct, MultiChunkInput) {
    // 2*BATCH_SIZE rows: [0..BATCH_SIZE-1] then [0..BATCH_SIZE-1] again
    // Duplicates span the chunk boundary; after dedup: BATCH_SIZE unique rows
    int n = BATCH_SIZE;
    std::vector<int64_t> vals;
    vals.reserve(2 * n);
    for (int i = 0; i < n; ++i) vals.push_back(static_cast<int64_t>(i));
    for (int i = 0; i < n; ++i) vals.push_back(static_cast<int64_t>(i));
    auto dist = std::make_unique<VecDistinctNode>(makeIntScan("id", vals));
    auto rows = drainRows(*dist);
    ASSERT_EQ(static_cast<int>(rows.size()), n);
    for (int i = 0; i < n; ++i) EXPECT_EQ(rows[i][0].asInt(), i);
}

TEST(VecDistinct, MultiColumnKey) {
    // (1,1),(1,2),(1,1),(2,1) → distinct on all cols → (1,1),(1,2),(2,1)
    Schema schema = vecSchema({{"a", TypeId::INT}, {"b", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(1)), Value(int64_t(1))},
        {Value(int64_t(1)), Value(int64_t(2))},
        {Value(int64_t(1)), Value(int64_t(1))},
        {Value(int64_t(2)), Value(int64_t(1))},
    };
    auto dist = std::make_unique<VecDistinctNode>(makeScan(schema, input));
    auto rows = drainRows(*dist);
    ASSERT_EQ(rows.size(), 3u);
}

TEST(VecDistinct, EmptyInput) {
    auto scan = makeIntScan("id", {});
    auto dist = std::make_unique<VecDistinctNode>(std::move(scan));
    dist->open();
    EXPECT_EQ(dist->nextChunk(), nullptr);
    dist->close();
}

TEST(VecDistinct, OutputSchemaUnchanged) {
    Schema schema = vecSchema({{"x", TypeId::INT}, {"y", TypeId::STRING}});
    std::vector<Row> rows = {{Value(int64_t(1)), Value(std::string("a"))}};
    auto dist = std::make_unique<VecDistinctNode>(makeScan(schema, rows));
    EXPECT_EQ(dist->outputSchema().size(), 2);
    EXPECT_EQ(dist->outputSchema().column(0).name, "x");
    EXPECT_EQ(dist->outputSchema().column(1).name, "y");
}

// ============================================================
// VecHashAggregate
// ============================================================

TEST(VecHashAggregate, CountStar_NoGroupBy) {
    auto scan = makeIntScan("id", {1, 2, 3, 4, 5});
    Schema out_schema = vecSchema({{"cnt", TypeId::INT}});
    std::vector<AggregateSpec> specs = {{"COUNT", "", true}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        std::move(scan), std::vector<GroupByColumn>{}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].asInt(), 5);
}

TEST(VecHashAggregate, CountStar_WithGroupBy) {
    Schema schema = vecSchema({{"grp", TypeId::STRING}, {"val", TypeId::INT}});
    std::vector<Row> input = {
        {Value(std::string("a")), Value(int64_t(1))},
        {Value(std::string("a")), Value(int64_t(2))},
        {Value(std::string("b")), Value(int64_t(1))},
        {Value(std::string("b")), Value(int64_t(2))},
        {Value(std::string("b")), Value(int64_t(3))},
        {Value(std::string("c")), Value(int64_t(1))},
    };
    Schema out_schema = vecSchema({{"grp", TypeId::STRING}, {"cnt", TypeId::INT}});
    std::vector<AggregateSpec> specs = {{"COUNT", "", true}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{{"", "grp"}}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0].asString(), "a"); EXPECT_EQ(rows[0][1].asInt(), 2);
    EXPECT_EQ(rows[1][0].asString(), "b"); EXPECT_EQ(rows[1][1].asInt(), 3);
    EXPECT_EQ(rows[2][0].asString(), "c"); EXPECT_EQ(rows[2][1].asInt(), 1);
}

TEST(VecHashAggregate, SumGroupBy) {
    Schema schema = vecSchema({{"grp", TypeId::INT}, {"val", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(1)), Value(int64_t(10))},
        {Value(int64_t(1)), Value(int64_t(20))},
        {Value(int64_t(2)), Value(int64_t(30))},
        {Value(int64_t(2)), Value(int64_t(40))},
    };
    Schema out_schema = vecSchema({{"grp", TypeId::INT}, {"total", TypeId::DOUBLE}});
    std::vector<AggregateSpec> specs = {{"SUM", "val", false}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{{"", "grp"}}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][0].asInt(), 1);
    EXPECT_DOUBLE_EQ(rows[0][1].asDouble(), 30.0);
    EXPECT_EQ(rows[1][0].asInt(), 2);
    EXPECT_DOUBLE_EQ(rows[1][1].asDouble(), 70.0);
}

TEST(VecHashAggregate, AvgGroupBy) {
    Schema schema = vecSchema({{"grp", TypeId::INT}, {"val", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(1)), Value(int64_t(10))},
        {Value(int64_t(1)), Value(int64_t(20))},
        {Value(int64_t(2)), Value(int64_t(30))},
        {Value(int64_t(2)), Value(int64_t(40))},
    };
    Schema out_schema = vecSchema({{"grp", TypeId::INT}, {"avg_val", TypeId::DOUBLE}});
    std::vector<AggregateSpec> specs = {{"AVG", "val", false}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{{"", "grp"}}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_DOUBLE_EQ(rows[0][1].asDouble(), 15.0);
    EXPECT_DOUBLE_EQ(rows[1][1].asDouble(), 35.0);
}

TEST(VecHashAggregate, MinMax) {
    Schema schema = vecSchema({{"grp", TypeId::INT}, {"val", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(1)), Value(int64_t(30))},
        {Value(int64_t(1)), Value(int64_t(10))},
        {Value(int64_t(1)), Value(int64_t(20))},
    };
    Schema out_schema = vecSchema({{"grp", TypeId::INT}, {"mn", TypeId::DOUBLE}, {"mx", TypeId::DOUBLE}});
    std::vector<AggregateSpec> specs = {{"MIN", "val", false}, {"MAX", "val", false}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{{"", "grp"}}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0][1].asDouble(), 10.0);
    EXPECT_DOUBLE_EQ(rows[0][2].asDouble(), 30.0);
}

TEST(VecHashAggregate, MultiChunkInput) {
    int n = BATCH_SIZE + 100;
    std::vector<int64_t> vals(n, 0LL);
    Schema out_schema = vecSchema({{"cnt", TypeId::INT}});
    std::vector<AggregateSpec> specs = {{"COUNT", "", true}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeIntScan("id", vals), std::vector<GroupByColumn>{}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].asInt(), n);
}

TEST(VecHashAggregate, EmptyInput) {
    // SQL: a scalar aggregate (no GROUP BY) over empty input emits one row, COUNT = 0
    auto scan = makeIntScan("id", {});
    Schema out_schema = vecSchema({{"cnt", TypeId::INT}});
    std::vector<AggregateSpec> specs = {{"COUNT", "", true}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        std::move(scan), std::vector<GroupByColumn>{}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].asInt(), 0);
}

TEST(VecHashAggregate, InsertionOrderPreserved) {
    Schema schema = vecSchema({{"grp", TypeId::STRING}, {"val", TypeId::INT}});
    std::vector<Row> input = {
        {Value(std::string("c")), Value(int64_t(1))},
        {Value(std::string("a")), Value(int64_t(1))},
        {Value(std::string("b")), Value(int64_t(1))},
    };
    Schema out_schema = vecSchema({{"grp", TypeId::STRING}, {"cnt", TypeId::INT}});
    std::vector<AggregateSpec> specs = {{"COUNT", "", true}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{{"", "grp"}}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0].asString(), "c");
    EXPECT_EQ(rows[1][0].asString(), "a");
    EXPECT_EQ(rows[2][0].asString(), "b");
}

TEST(VecHashAggregate, OutputSchemaCorrect) {
    Schema schema = vecSchema({{"grp", TypeId::INT}, {"val", TypeId::INT}});
    Schema out_schema = vecSchema({{"grp", TypeId::INT}, {"total", TypeId::DOUBLE}});
    std::vector<AggregateSpec> specs = {{"SUM", "val", false}};
    std::vector<Row> rows = {{Value(int64_t(1)), Value(int64_t(10))}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, rows), std::vector<GroupByColumn>{{"", "grp"}}, specs, out_schema);
    EXPECT_EQ(agg->outputSchema().size(), 2);
    EXPECT_EQ(agg->outputSchema().column(0).name, "grp");
    EXPECT_EQ(agg->outputSchema().column(1).name, "total");
}

// ============================================================
// VecHashJoin
// ============================================================

TEST(VecHashJoin, BasicInnerJoin) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}, {"pval", TypeId::STRING}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}, {"bval", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"pval", TypeId::STRING},
                                      {"bid", TypeId::INT}, {"bval", TypeId::STRING}});
    std::vector<Row> probe_rows = {
        {Value(int64_t(1)), Value(std::string("p1"))},
        {Value(int64_t(2)), Value(std::string("p2"))},
        {Value(int64_t(3)), Value(std::string("p3"))},
    };
    std::vector<Row> build_rows = {
        {Value(int64_t(1)), Value(std::string("b1"))},
        {Value(int64_t(2)), Value(std::string("b2"))},
        {Value(int64_t(3)), Value(std::string("b3"))},
    };
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);
    auto rows = drainRows(*join);
    ASSERT_EQ(rows.size(), 3u);
    for (size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i][0].asInt(), static_cast<int64_t>(i + 1));
        EXPECT_EQ(rows[i][2].asInt(), static_cast<int64_t>(i + 1));
    }
}

TEST(VecHashJoin, NoMatch) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    std::vector<Row> probe_rows = {{Value(int64_t(1))}, {Value(int64_t(2))}};
    std::vector<Row> build_rows = {{Value(int64_t(3))}, {Value(int64_t(4))}};
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);
    auto rows = drainRows(*join);
    EXPECT_TRUE(rows.empty());
}

TEST(VecHashJoin, MultipleMatchesPerKey) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}, {"bval", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT},
                                      {"bid", TypeId::INT}, {"bval", TypeId::STRING}});
    std::vector<Row> probe_rows = {{Value(int64_t(1))}};
    std::vector<Row> build_rows = {
        {Value(int64_t(1)), Value(std::string("x"))},
        {Value(int64_t(1)), Value(std::string("y"))},
        {Value(int64_t(1)), Value(std::string("z"))},
    };
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);
    auto rows = drainRows(*join);
    ASSERT_EQ(rows.size(), 3u);
    for (const auto& r : rows) EXPECT_EQ(r[0].asInt(), 1);
}

TEST(VecHashJoin, EmptyBuildSide) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    std::vector<Row> probe_rows = {{Value(int64_t(1))}, {Value(int64_t(2))}};
    std::vector<Row> build_rows;
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);
    auto rows = drainRows(*join);
    EXPECT_TRUE(rows.empty());
}

TEST(VecHashJoin, EmptyProbeSide) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    std::vector<Row> probe_rows;
    std::vector<Row> build_rows = {{Value(int64_t(1))}};
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);
    join->open();
    EXPECT_EQ(join->nextChunk(), nullptr);
    join->close();
}

TEST(VecHashJoin, ProbeColsFirst) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}, {"pname", TypeId::STRING}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}, {"bname", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"pname", TypeId::STRING},
                                      {"bid", TypeId::INT}, {"bname", TypeId::STRING}});
    std::vector<Row> probe_rows = {{Value(int64_t(1)), Value(std::string("probe"))}};
    std::vector<Row> build_rows = {{Value(int64_t(1)), Value(std::string("build"))}};
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);
    const Schema& s = join->outputSchema();
    ASSERT_EQ(s.size(), 4);
    EXPECT_EQ(s.column(0).name, "pid");
    EXPECT_EQ(s.column(1).name, "pname");
    EXPECT_EQ(s.column(2).name, "bid");
    EXPECT_EQ(s.column(3).name, "bname");
}

// When the FROM table is the smaller (build) side, the planner passes
// swapped=true. Output rows must still follow logical [FROM, JOIN] order
// (FROM values first) even though FROM is physically the build side.
TEST(VecHashJoin, SwappedEmitsFromSideFirst) {
    // Physical: probe = JOIN side (fid), build = FROM side (fromid).
    // Logical output schema is [FROM cols, JOIN cols].
    Schema probe_schema = vecSchema({{"jid", TypeId::INT}, {"jname", TypeId::STRING}});
    Schema build_schema = vecSchema({{"fid", TypeId::INT}, {"fname", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"fid", TypeId::INT}, {"fname", TypeId::STRING},
                                      {"jid", TypeId::INT}, {"jname", TypeId::STRING}});
    std::vector<Row> probe_rows = {{Value(int64_t(1)), Value(std::string("joinrow"))}};
    std::vector<Row> build_rows = {{Value(int64_t(1)), Value(std::string("fromrow"))}};
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("jid")}, std::vector<int>{build_schema.indexOf("fid")},
        out_schema, /*swapped=*/true);
    auto rows = drainRows(*join);
    ASSERT_EQ(rows.size(), 1u);
    // FROM (build) columns must come first despite being the physical build side
    EXPECT_EQ(rows[0][1].asString(), "fromrow"); // FROM side fname
    EXPECT_EQ(rows[0][3].asString(), "joinrow"); // JOIN side jname
}

TEST(VecHashJoin, MultiChunkProbe) {
    // probe: ids 0..BATCH_SIZE+4; build: ids 1 and BATCH_SIZE+2
    // id=1 is in the first probe chunk; id=BATCH_SIZE+2 is in the second
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    int n = BATCH_SIZE + 5;
    std::vector<Row> probe_rows;
    probe_rows.reserve(n);
    for (int i = 0; i < n; ++i)
        probe_rows.push_back({Value(static_cast<int64_t>(i))});
    std::vector<Row> build_rows = {
        {Value(int64_t(1))},
        {Value(static_cast<int64_t>(BATCH_SIZE + 2))},
    };
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);
    auto rows = drainRows(*join);
    ASSERT_EQ(rows.size(), 2u);
    std::vector<int64_t> pids;
    for (const auto& r : rows) pids.push_back(r[0].asInt());
    std::sort(pids.begin(), pids.end());
    EXPECT_EQ(pids[0], 1);
    EXPECT_EQ(pids[1], static_cast<int64_t>(BATCH_SIZE + 2));
}

TEST(VecHashJoin, KeyByName) {
    // probe schema: {pval, pid} — pid is at index 1, not 0
    // verifies that VecHashJoinNode uses indexOf("pid"), not position 0
    Schema probe_schema = vecSchema({{"pval", TypeId::INT}, {"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}, {"bdesc", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"pval", TypeId::INT}, {"pid", TypeId::INT},
                                      {"bid", TypeId::INT}, {"bdesc", TypeId::STRING}});
    std::vector<Row> probe_rows = {
        {Value(int64_t(99)),  Value(int64_t(1))},
        {Value(int64_t(100)), Value(int64_t(2))},
        {Value(int64_t(101)), Value(int64_t(99))},  // pid=99 has no build match
    };
    std::vector<Row> build_rows = {
        {Value(int64_t(1)), Value(std::string("one"))},
        {Value(int64_t(2)), Value(std::string("two"))},
    };
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);
    auto rows = drainRows(*join);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][1].asInt(), 1);
    EXPECT_EQ(rows[1][1].asInt(), 2);
}

// T1: LIMIT exactly at one full chunk boundary — available == remaining, non-truncating path.
TEST(VecLimit, LimitAtChunkBoundary) {
    std::vector<int64_t> vals(BATCH_SIZE);
    std::iota(vals.begin(), vals.end(), 0LL);
    auto lim = std::make_unique<VecLimitNode>(makeIntScan("id", vals), BATCH_SIZE);
    lim->open();
    DataChunk* chunk = lim->nextChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_EQ(chunk->num_rows, BATCH_SIZE);
    EXPECT_EQ(lim->nextChunk(), nullptr);
    lim->close();
}

// T4: LIMIT receiving filter_applied=true input — truncation must not resize column vectors.
TEST(VecLimit, FilteredInputTruncation) {
    // scan [0,1,2,3,4]; filter id >= 1 → sel.indices=[1,2,3,4]; limit 2
    // expected: chunk with filter_applied=true, sel.indices=[1,2] (first 2 passing rows)
    auto scan   = makeIntScan("id", {0, 1, 2, 3, 4});
    auto filter = std::make_unique<VecFilterNode>(
        std::move(scan), binOp(">=", col("id"), intLit(1)));
    auto lim = std::make_unique<VecLimitNode>(std::move(filter), 2);
    lim->open();
    DataChunk* chunk = lim->nextChunk();
    ASSERT_NE(chunk, nullptr);
    EXPECT_TRUE(chunk->filter_applied);
    ASSERT_EQ(static_cast<int>(chunk->sel.indices.size()), 2);
    EXPECT_EQ(chunk->sel.size, 2);        // sel.size must stay in sync with indices
    EXPECT_EQ(chunk->sel.indices[0], 1);  // physical row 1 → id=1
    EXPECT_EQ(chunk->sel.indices[1], 2);  // physical row 2 → id=2
    // column vector must still hold all 5 original rows (not resized to 2)
    EXPECT_EQ(std::get<std::vector<int64_t>>(chunk->columns[0].data).size(), 5u);
    EXPECT_EQ(lim->nextChunk(), nullptr);
    lim->close();
}

// T2: VecHashJoin with a filtered build side — regression test for Fix 1.
TEST(VecHashJoin, FilteredBuildSide) {
    // build scan has rows bid=1,2,3; filter bid != 2 → only 1 and 3 enter hash table
    // probe has pid=1,2,3; expected matches: pid=1↔bid=1, pid=3↔bid=3 (pid=2 has no match)
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});

    auto build_scan     = makeScan(build_schema, {{Value(int64_t(1))}, {Value(int64_t(2))}, {Value(int64_t(3))}});
    auto build_filtered = std::make_unique<VecFilterNode>(
        std::move(build_scan), binOp("!=", col("bid"), intLit(2)));
    auto probe_scan     = makeScan(probe_schema, {{Value(int64_t(1))}, {Value(int64_t(2))}, {Value(int64_t(3))}});

    auto join = std::make_unique<VecHashJoinNode>(
        std::move(probe_scan), std::move(build_filtered),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);

    auto rows = drainRows(*join);
    ASSERT_EQ(rows.size(), 2u);
    std::vector<int64_t> pids;
    for (const auto& r : rows) pids.push_back(r[0].asInt());
    std::sort(pids.begin(), pids.end());
    EXPECT_EQ(pids[0], 1);
    EXPECT_EQ(pids[1], 3);
}

// ============================================================
// Coverage-gap tests: filter_applied=true paths into pipeline breakers
// ============================================================

// VecHashAggregateNode must read only sel.indices rows when filter_applied=true.
TEST(VecHashAggregate, FilteredInput) {
    // rows: grp=a(val=1,2), grp=b(val=3,4,5). Filter val > 2 leaves b:3,4,5.
    // COUNT(*) GROUP BY grp → exactly one group "b" with count=3.
    Schema schema = vecSchema({{"grp", TypeId::STRING}, {"val", TypeId::INT}});
    std::vector<Row> input = {
        {Value(std::string("a")), Value(int64_t(1))},
        {Value(std::string("a")), Value(int64_t(2))},
        {Value(std::string("b")), Value(int64_t(3))},
        {Value(std::string("b")), Value(int64_t(4))},
        {Value(std::string("b")), Value(int64_t(5))},
    };
    auto filter = std::make_unique<VecFilterNode>(
        makeScan(schema, input), binOp(">", col("val"), intLit(2)));
    Schema out_schema = vecSchema({{"grp", TypeId::STRING}, {"cnt", TypeId::INT}});
    std::vector<AggregateSpec> specs = {{"COUNT", "", true}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        std::move(filter), std::vector<GroupByColumn>{{"", "grp"}}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].asString(), "b");
    EXPECT_EQ(rows[0][1].asInt(), 3);
}

// ===== Composite (multi-key) join keys, Week 27 =====

// Two keys are one serialized tuple. Both must constrain the match: rows that
// agree on the first key alone must not join.
TEST(VecHashJoin, MultiKeyRequiresEveryKeyToMatch) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}, {"pgrp", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}, {"bgrp", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"pgrp", TypeId::INT},
                                      {"bid", TypeId::INT}, {"bgrp", TypeId::INT}});
    std::vector<Row> probe_rows = {
        {Value(int64_t(1)), Value(int64_t(7))},
        {Value(int64_t(1)), Value(int64_t(8))},   // first key matches, second does not
    };
    std::vector<Row> build_rows = {
        {Value(int64_t(1)), Value(int64_t(7))},
    };
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid"), probe_schema.indexOf("pgrp")},
        std::vector<int>{build_schema.indexOf("bid"), build_schema.indexOf("bgrp")},
        out_schema);
    auto rows = drainRows(*join);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][1].asInt(), 7);
}

// The '\x01' sentinel after EVERY field, not between them: without it the STRING
// tuples ("ab","c") and ("a","bc") serialize identically and share one bucket.
TEST(VecHashJoin, MultiKeyStringTupleBoundariesDoNotCollide) {
    Schema probe_schema = vecSchema({{"p1", TypeId::STRING}, {"p2", TypeId::STRING}});
    Schema build_schema = vecSchema({{"b1", TypeId::STRING}, {"b2", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"p1", TypeId::STRING}, {"p2", TypeId::STRING},
                                      {"b1", TypeId::STRING}, {"b2", TypeId::STRING}});
    std::vector<Row> probe_rows = {{Value(std::string("ab")), Value(std::string("c"))}};
    std::vector<Row> build_rows = {{Value(std::string("a")),  Value(std::string("bc"))}};
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("p1"), probe_schema.indexOf("p2")},
        std::vector<int>{build_schema.indexOf("b1"), build_schema.indexOf("b2")},
        out_schema);
    EXPECT_TRUE(drainRows(*join).empty());
}

// Same hazard on the vectorized side: a STRING key can contain the sentinel,
// because nothing between the CSV cell and the hash key escapes or rejects it.
TEST(VecHashJoin, MultiKeyTupleIsInjectiveWhenAValueContainsTheSentinel) {
    Schema probe_schema = vecSchema({{"p1", TypeId::STRING}, {"p2", TypeId::STRING}});
    Schema build_schema = vecSchema({{"b1", TypeId::STRING}, {"b2", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"p1", TypeId::STRING}, {"p2", TypeId::STRING},
                                      {"b1", TypeId::STRING}, {"b2", TypeId::STRING}});
    std::vector<Row> probe_rows = {{Value(std::string("A\x01" "B")), Value(std::string("C"))}};
    std::vector<Row> build_rows = {{Value(std::string("A")), Value(std::string("B\x01" "C"))}};
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("p1"), probe_schema.indexOf("p2")},
        std::vector<int>{build_schema.indexOf("b1"), build_schema.indexOf("b2")},
        out_schema);
    EXPECT_TRUE(drainRows(*join).empty());
}

TEST(VecHashJoin, MultiKeyTupleStillMatchesWhenAValueContainsTheSentinel) {
    Schema probe_schema = vecSchema({{"p1", TypeId::STRING}, {"p2", TypeId::STRING}});
    Schema build_schema = vecSchema({{"b1", TypeId::STRING}, {"b2", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"p1", TypeId::STRING}, {"p2", TypeId::STRING},
                                      {"b1", TypeId::STRING}, {"b2", TypeId::STRING}});
    std::vector<Row> probe_rows = {{Value(std::string("A\x01" "B")), Value(std::string("C"))}};
    std::vector<Row> build_rows = {{Value(std::string("A\x01" "B")), Value(std::string("C"))}};
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("p1"), probe_schema.indexOf("p2")},
        std::vector<int>{build_schema.indexOf("b1"), build_schema.indexOf("b2")},
        out_schema);
    EXPECT_EQ(drainRows(*join).size(), 1u);
}

// Same encoding, same hazard on the vectorized side: two distinct doubles that
// display alike must not share a bucket, and equal ones (including the two
// zeros) must still match.
TEST(VecHashJoin, DoubleKeysCompareExactlyNotByDisplayText) {
    Schema probe_schema = vecSchema({{"pk", TypeId::DOUBLE}});
    Schema build_schema = vecSchema({{"bk", TypeId::DOUBLE}});
    Schema out_schema   = vecSchema({{"pk", TypeId::DOUBLE}, {"bk", TypeId::DOUBLE}});

    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, {{Value(0.1 + 0.2)}, {Value(-0.0)}}),
        makeScan(build_schema, {{Value(0.3)}, {Value(0.0)}}),
        std::vector<int>{0}, std::vector<int>{0}, out_schema);

    auto rows = drainRows(*join);
    ASSERT_EQ(rows.size(), 1u);          // only the zeros match
    EXPECT_EQ(rows[0][1].asDouble(), 0.0);
}

// explain() renders names from the children's schemas; a one-key join must
// still print exactly the pre-Week-27 string.
TEST(VecHashJoin, ExplainRendersEveryKeyAndKeepsTheSingleKeyForm) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}, {"pgrp", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}, {"bgrp", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"pgrp", TypeId::INT},
                                      {"bid", TypeId::INT}, {"bgrp", TypeId::INT}});
    VecHashJoinNode two(makeScan(probe_schema, {}), makeScan(build_schema, {}),
                        std::vector<int>{0, 1}, std::vector<int>{0, 1}, out_schema);
    EXPECT_EQ(two.explain(), "VecHashJoin [pid = bid AND pgrp = bgrp] (materialize)");

    VecHashJoinNode one(makeScan(probe_schema, {}), makeScan(build_schema, {}),
                        std::vector<int>{0}, std::vector<int>{0}, out_schema);
    EXPECT_EQ(one.explain(), "VecHashJoin [pid = bid] (materialize)");
}

// VecSortNode must extract only sel.indices rows when filter_applied=true.
TEST(VecSort, FilteredInput) {
    // scan [5,3,1,4,2]; filter id > 2 → physical rows 0(5),1(3),3(4) pass.
    // sort ASC → 3, 4, 5.
    auto filter = std::make_unique<VecFilterNode>(
        makeIntScan("id", {5, 3, 1, 4, 2}), binOp(">", col("id"), intLit(2)));
    std::vector<OrderByItem> ob; ob.push_back(orderAsc(col("id")));
    auto sort = std::make_unique<VecSortNode>(std::move(filter), std::move(ob));
    auto rows = drainRows(*sort);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0].asInt(), 3);
    EXPECT_EQ(rows[1][0].asInt(), 4);
    EXPECT_EQ(rows[2][0].asInt(), 5);
}

// VecHashJoinNode output_cursor_ slice path: one probe key matches >BATCH_SIZE build rows.
TEST(VecHashJoin, ProbeOutputOverflow) {
    int n = BATCH_SIZE + 10;
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    std::vector<Row> probe_rows = {{Value(int64_t(1))}};
    std::vector<Row> build_rows;
    for (int i = 0; i < n; ++i) build_rows.push_back({Value(int64_t(1))});
    auto join = std::make_unique<VecHashJoinNode>(
        makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
        std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
        out_schema);
    auto rows = drainRows(*join);
    ASSERT_EQ(static_cast<int>(rows.size()), n);
    for (const auto& r : rows) {
        EXPECT_EQ(r[0].asInt(), 1);
        EXPECT_EQ(r[1].asInt(), 1);
    }
}

// AVG denominator correctness: groups of sizes 1, 2, 3 have different divisors.
TEST(VecHashAggregate, AvgDivisorCorrectness) {
    Schema schema = vecSchema({{"grp", TypeId::INT}, {"val", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(1)), Value(int64_t(6))},
        {Value(int64_t(2)), Value(int64_t(1))}, {Value(int64_t(2)), Value(int64_t(3))},
        {Value(int64_t(3)), Value(int64_t(2))}, {Value(int64_t(3)), Value(int64_t(4))}, {Value(int64_t(3)), Value(int64_t(6))},
    };
    Schema out_schema = vecSchema({{"grp", TypeId::INT}, {"avg_val", TypeId::DOUBLE}});
    std::vector<AggregateSpec> specs = {{"AVG", "val", false}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{{"", "grp"}}, specs, out_schema);
    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_DOUBLE_EQ(rows[0][1].asDouble(), 6.0);   // 6/1
    EXPECT_DOUBLE_EQ(rows[1][1].asDouble(), 2.0);   // (1+3)/2
    EXPECT_DOUBLE_EQ(rows[2][1].asDouble(), 4.0);   // (2+4+6)/3
}

// ============================================================
// VecHaving — HAVING via VecFilterNode on aggregate output
// ============================================================

// GROUP BY grp, COUNT(*) HAVING COUNT(*) > 1
// grp=A: 1 row  → filtered out
// grp=B: 2 rows → kept
// grp=C: 3 rows → kept
TEST(VecHaving, FilterAggregateGroups) {
    Schema in_schema = vecSchema({{"grp", TypeId::STRING}, {"val", TypeId::INT}});
    std::vector<Row> input = {
        {Value(std::string("A")), Value((int64_t)10)},
        {Value(std::string("B")), Value((int64_t)20)},
        {Value(std::string("B")), Value((int64_t)30)},
        {Value(std::string("C")), Value((int64_t)40)},
        {Value(std::string("C")), Value((int64_t)50)},
        {Value(std::string("C")), Value((int64_t)60)},
    };

    Schema agg_schema = vecSchema({{"grp", TypeId::STRING}, {"COUNT(*)", TypeId::INT}});
    std::vector<AggregateSpec> specs = {{"COUNT", "", true}};
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(in_schema, input), std::vector<GroupByColumn>{{"", "grp"}}, specs, agg_schema);

    // HAVING COUNT(*) > 1  →  col index 1 > literal 1
    auto having_pred = std::make_unique<BinaryExpr>();
    having_pred->op = ">";
    auto count_ref = std::make_unique<ColumnRef>(); count_ref->column_name = "COUNT(*)";
    having_pred->left = std::move(count_ref);
    having_pred->right = std::make_unique<Literal>(Value((int64_t)1));

    auto having = std::make_unique<VecFilterNode>(std::move(agg), std::move(having_pred));
    auto rows = drainRows(*having);

    ASSERT_EQ(rows.size(), 2u);
    // grp B (count=2) and grp C (count=3) survive; A (count=1) is filtered
    bool saw_B = false, saw_C = false;
    for (const auto& r : rows) {
        if (r[0].asString() == "B") { saw_B = true; EXPECT_EQ(r[1].asInt(), 2); }
        if (r[0].asString() == "C") { saw_C = true; EXPECT_EQ(r[1].asInt(), 3); }
    }
    EXPECT_TRUE(saw_B);
    EXPECT_TRUE(saw_C);
}
// ===== EXPLAIN ANALYZE timing honesty (audit M6) =====

// The build phase runs in open() and must be timed: with a zero-row probe the
// probe loop and output materialization never run, so any elapsed time can
// only come from the (previously untimed) hash-table build.
TEST(VecHashJoinTiming, BuildPhaseIsTimed) {
    Schema side = vecSchema({{"k", TypeId::INT}});
    std::vector<Row> build_rows;
    build_rows.reserve(5000);
    for (int i = 0; i < 5000; ++i) build_rows.push_back({Value(int64_t(i))});
    Schema merged(std::vector<ColumnDef>{{"k", TypeId::INT, 0}, {"k", TypeId::INT, 1}});

    VecHashJoinNode join(makeScan(side, {}), makeScan(side, build_rows),
                         std::vector<int>{side.indexOf("k")}, std::vector<int>{side.indexOf("k")},
                         merged, /*swapped=*/false);
    join.open();
    while (join.nextChunk()) {}
    join.close();
    EXPECT_GT(join.stats.elapsed_us, 0.0);
}

// ===== Late materialization: filter/limit must not copy chunk data (P5) =====

// A filter stamps its SelectionVector onto the child's chunk and passes the
// pointer through. Proof by address: the scan reuses one internal buffer for
// every chunk, so a pass-through filter must return that same address.
TEST(VecFilterPassThrough, ReturnsChildChunkPointer) {
    Schema schema = vecSchema({{"v", TypeId::INT}});
    std::vector<Row> rows;
    rows.reserve(2000);  // two BATCH_SIZE chunks from one scan buffer
    for (int i = 0; i < 2000; ++i) rows.push_back({Value(int64_t(i))});

    auto scan = makeScan(schema, rows);
    VecScanNode* raw_scan = scan.get();
    VecFilterNode filter(std::move(scan), binOp(">", col("v"), intLit(-1)));

    filter.open();
    DataChunk* through_filter = filter.nextChunk();     // chunk 1 via the filter
    DataChunk* scan_buffer = raw_scan->nextChunk();     // chunk 2, same internal buffer
    ASSERT_NE(through_filter, nullptr);
    ASSERT_NE(scan_buffer, nullptr);
    EXPECT_EQ(through_filter, scan_buffer)
        << "filter must pass the child's chunk through, not copy it";
    filter.close();
}

// Filter-over-filter over multiple batches: the second filter reads the first
// filter's SelectionVector as input_sel on the same pass-through buffer.
TEST(VecFilterPassThrough, CascadeOverMultipleBatchesStaysCorrect) {
    Schema schema = vecSchema({{"v", TypeId::INT}});
    std::vector<Row> rows;
    rows.reserve(3000);
    for (int i = 0; i < 3000; ++i) rows.push_back({Value(int64_t(i))});

    auto scan = makeScan(schema, rows);
    auto f1 = std::make_unique<VecFilterNode>(
        std::move(scan), binOp(">=", col("v"), intLit(1000)));
    VecFilterNode f2(std::move(f1), binOp("<", col("v"), intLit(1500)));

    int total = 0;
    f2.open();
    while (DataChunk* chunk = f2.nextChunk()) {
        ASSERT_TRUE(chunk->filter_applied);
        for (int idx : chunk->sel.indices) {
            int64_t v = std::get<std::vector<int64_t>>(chunk->columns[0].data)[idx];
            EXPECT_GE(v, 1000);
            EXPECT_LT(v, 1500);
            ++total;
        }
    }
    f2.close();
    EXPECT_EQ(total, 500);
}

// Chunks where zero rows pass must not corrupt later chunks (the scan buffer
// is shared, so a stale SelectionVector would leak into the next batch).
TEST(VecFilterPassThrough, ZeroPassChunksMidStream) {
    Schema schema = vecSchema({{"v", TypeId::INT}});
    std::vector<Row> rows;
    rows.reserve(3000);
    for (int i = 0; i < 3000; ++i) rows.push_back({Value(int64_t(i))});

    auto scan = makeScan(schema, rows);
    VecFilterNode filter(std::move(scan), binOp("<", col("v"), intLit(500)));

    int total = 0;
    filter.open();
    while (DataChunk* chunk = filter.nextChunk()) {
        total += static_cast<int>(chunk->sel.indices.size());
    }
    filter.close();
    EXPECT_EQ(total, 500);  // batches 2 and 3 pass zero rows
}

// Limit truncates in place on the pass-through chunk: both the sel-truncation
// path (filtered input) and the column-resize path (unfiltered input).
TEST(VecLimitPassThrough, TruncatesBothPaths) {
    Schema schema = vecSchema({{"v", TypeId::INT}});
    std::vector<Row> rows;
    rows.reserve(2000);
    for (int i = 0; i < 2000; ++i) rows.push_back({Value(int64_t(i))});

    // filtered input: limit truncates sel.indices
    {
        auto scan = makeScan(schema, rows);
        auto filter = std::make_unique<VecFilterNode>(
            std::move(scan), binOp(">=", col("v"), intLit(100)));
        VecLimitNode limit(std::move(filter), 20);
        int total = 0;
        limit.open();
        while (DataChunk* chunk = limit.nextChunk()) {
            total += chunk->filter_applied
                ? static_cast<int>(chunk->sel.indices.size())
                : chunk->num_rows;
        }
        limit.close();
        EXPECT_EQ(total, 20);
    }
    // unfiltered input: limit resizes columns
    {
        auto scan = makeScan(schema, rows);
        VecLimitNode limit(std::move(scan), 30);
        int total = 0;
        limit.open();
        while (DataChunk* chunk = limit.nextChunk()) {
            ASSERT_FALSE(chunk->filter_applied);
            EXPECT_EQ(static_cast<int>(std::get<std::vector<int64_t>>(chunk->columns[0].data).size()),
                      chunk->num_rows);
            total += chunk->num_rows;
        }
        limit.close();
        EXPECT_EQ(total, 30);
    }
}

// ===== evalPredicate AND-cascade (isolated; audit follow-up) =====
// The AND path evaluates the right operand only over the left's survivors
// (columnar_eval.cc): these pin the observable half — exact result sets for
// fast-path, fallback-path, and nested OR-of-AND shapes on one VecFilterNode.

// Two-column fixture: v = 0..n-1, w = f(v).
static std::unique_ptr<VecScanNode> makeVWScan(int n, int64_t (*f)(int64_t)) {
    Schema schema = vecSchema({{"v", TypeId::INT}, {"w", TypeId::INT}});
    std::vector<Row> rows;
    rows.reserve(n);
    for (int64_t i = 0; i < n; ++i) rows.push_back({Value(i), Value(f(i))});
    return makeScan(schema, rows);
}

static int countSurvivors(VecPlanNode& node) {
    node.open();
    int total = 0;
    int last = -1;
    while (DataChunk* chunk = node.nextChunk()) {
        EXPECT_TRUE(chunk->filter_applied);
        last = -1;  // indices are chunk-local; ascending within each chunk
        for (int idx : chunk->sel.indices) {
            EXPECT_GT(idx, last) << "selection indices must stay ascending";
            last = idx;
            ++total;
        }
    }
    node.close();
    return total;
}

TEST(EvalPredicateCascade, AndWithFastPathRightOperand) {
    // w = 2v; v >= 1000 AND w < 3000  ->  v in [1000, 1499]
    auto scan = makeVWScan(3000, [](int64_t v) { return 2 * v; });
    VecFilterNode filter(std::move(scan),
        binOp("AND", binOp(">=", col("v"), intLit(1000)),
                     binOp("<",  col("w"), intLit(3000))));
    EXPECT_EQ(countSurvivors(filter), 500);
}

TEST(EvalPredicateCascade, AndWithFallbackRightOperand) {
    // right operand is col-vs-col (no fast path): the fallback must honor the
    // cascaded input_sel. w = v+1 below 1500 else 0; v >= 1000 AND v < w
    // -> v in [1000, 1499]
    auto scan = makeVWScan(3000, [](int64_t v) { return v < 1500 ? v + 1 : 0; });
    VecFilterNode filter(std::move(scan),
        binOp("AND", binOp(">=", col("v"), intLit(1000)),
                     binOp("<",  col("v"), col("w"))));
    EXPECT_EQ(countSurvivors(filter), 500);
}

TEST(EvalPredicateCascade, OrOfCascadedAndsMergesAscending) {
    // (v < 100 AND w > 0) OR (v >= 2900 AND w > 0), w = v+1 (always > 0)
    // -> v in [0,99] and [2900,2999]; sv_union requires ascending inputs
    auto scan = makeVWScan(3000, [](int64_t v) { return v + 1; });
    VecFilterNode filter(std::move(scan),
        binOp("OR", binOp("AND", binOp("<",  col("v"), intLit(100)),
                                 binOp(">",  col("w"), intLit(0))),
                    binOp("AND", binOp(">=", col("v"), intLit(2900)),
                                 binOp(">",  col("w"), intLit(0)))));
    EXPECT_EQ(countSurvivors(filter), 200);
}


// ===== Week 24: aggregate over an expression argument (vectorized) =====

TEST(VecHashAggregate, SumOverExpressionArgument) {
    Schema schema = vecSchema({{"x", TypeId::INT}, {"y", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(1)), Value(int64_t(10))},
        {Value(int64_t(2)), Value(int64_t(20))},
    };

    // SUM(x * y) = 1*10 + 2*20 = 50
    auto arg = std::make_unique<BinaryExpr>();
    arg->op = "*";
    auto lhs = std::make_unique<ColumnRef>(); lhs->column_name = "x";
    auto rhs = std::make_unique<ColumnRef>(); rhs->column_name = "y";
    arg->left = std::move(lhs);
    arg->right = std::move(rhs);

    AggregateSpec spec;
    spec.function = "SUM";
    spec.is_star = false;
    spec.argument = arg.get();          // non-owning: `arg` outlives the node
    spec.output_name = "SUM((x * y))";

    Schema out_schema = vecSchema({{"SUM((x * y))", TypeId::DOUBLE}});
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{}, std::vector<AggregateSpec>{spec}, out_schema);

    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0][0].asDouble(), 50.0);
}


// ===== NULL values at the materialization point =====
// x/0 evaluates to SQL NULL (SQLite semantics). ColumnVector carries a
// validity mask, so VecProjectNode materializes a real NULL rather than the
// old 0 / 0.0 / "NULL" sentinel, which was indistinguishable from a genuine
// zero and blocked left-outer-join semantics on the vectorized path.

TEST(VecProject, DivisionByZeroMaterializesNull) {
    Schema schema = vecSchema({{"speed", TypeId::DOUBLE}, {"season", TypeId::INT}});
    std::vector<Row> input = {
        {Value(300.5), Value(int64_t(2025))},
        {Value(310.0), Value(int64_t(2024))},
    };

    // SELECT speed / 0, season / 0 — DOUBLE and INT null-producing projections
    auto div_double = std::make_unique<BinaryExpr>();
    div_double->op = "/";
    auto sp = std::make_unique<ColumnRef>(); sp->column_name = "speed";
    div_double->left = std::move(sp);
    div_double->right = std::make_unique<Literal>(Value(int64_t(0)));

    auto div_int = std::make_unique<BinaryExpr>();
    div_int->op = "/";
    auto se = std::make_unique<ColumnRef>(); se->column_name = "season";
    div_int->left = std::move(se);
    div_int->right = std::make_unique<Literal>(Value(int64_t(0)));

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(std::move(div_double));
    exprs.push_back(std::move(div_int));
    Schema out_schema = vecSchema({{"(speed / 0)", TypeId::DOUBLE}, {"(season / 0)", TypeId::INT}});

    VecProjectNode project(makeScan(schema, input), std::move(exprs), out_schema);

    std::vector<Row> rows;
    ASSERT_NO_THROW(rows = drainRows(project));
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[0][0].isNull());   // DOUBLE / 0 -> NULL
    EXPECT_TRUE(rows[0][1].isNull());   // INT / 0    -> NULL
    EXPECT_TRUE(rows[1][0].isNull());
    EXPECT_TRUE(rows[1][1].isNull());
}


// A column whose NULLs start partway through must keep the valid prefix
// intact — appendColumnValue back-fills the all-valid prefix on the first
// NULL, and a mis-sized validity mask would corrupt earlier rows.
TEST(VecProject, NullsMidColumnKeepValidPrefix) {
    Schema schema = vecSchema({{"n", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(4))}, {Value(int64_t(2))},
        {Value(int64_t(0))},                       // divisor 0 -> NULL here
        {Value(int64_t(1))},
    };

    // SELECT 8 / n  -> 2, 4, NULL, 8
    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(binOp("/", intLit(8), col("n")));
    Schema out_schema = vecSchema({{"(8 / n)", TypeId::INT}});

    VecProjectNode project(makeScan(schema, input), std::move(exprs), out_schema);
    std::vector<Row> rows = drainRows(project);

    ASSERT_EQ(rows.size(), 4u);
    EXPECT_EQ(rows[0][0].asInt(), 2);
    EXPECT_EQ(rows[1][0].asInt(), 4);
    EXPECT_TRUE(rows[2][0].isNull());
    EXPECT_EQ(rows[3][0].asInt(), 8);
}


// A null aggregate result (SUM over an all-NULL group; here an empty input)
// must survive materialization as NULL, not as 0. This is the Q13
// left-outer-join precondition: COUNT must be able to skip NULLs that came
// out of an upstream operator.
TEST(VecHashAggregate, NullScalarAggregateMaterializesNull) {
    Schema schema = vecSchema({{"x", TypeId::INT}});
    std::vector<Row> empty;

    AggregateSpec sum_spec;
    sum_spec.function = "SUM";
    sum_spec.is_star = false;
    sum_spec.column = "x";
    sum_spec.output_name = "SUM(x)";

    AggregateSpec count_spec;
    count_spec.function = "COUNT";
    count_spec.is_star = true;
    count_spec.output_name = "COUNT(*)";

    Schema out_schema = vecSchema({{"SUM(x)", TypeId::DOUBLE}, {"COUNT(*)", TypeId::INT}});
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, empty), std::vector<GroupByColumn>{},
        std::vector<AggregateSpec>{sum_spec, count_spec}, out_schema);

    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);          // scalar aggregate always emits one row
    EXPECT_TRUE(rows[0][0].isNull());    // SUM over no rows -> NULL
    EXPECT_EQ(rows[0][1].asInt(), 0);    // COUNT(*) over no rows -> 0
}


// NULL must survive the blocking operators, which round-trip rows through a
// flat Row buffer and re-emit them as chunks.
TEST(VecSort, NullsSurviveMaterializationRoundTrip) {
    Schema schema = vecSchema({{"n", TypeId::INT}});
    std::vector<Row> input = {{Value(int64_t(2))}, {Value(int64_t(0))}, {Value(int64_t(1))}};

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(binOp("/", intLit(6), col("n")));   // 3, NULL, 6
    Schema proj_schema = vecSchema({{"(6 / n)", TypeId::INT}});
    auto project = std::make_unique<VecProjectNode>(
        makeScan(schema, input), std::move(exprs), proj_schema);

    std::vector<OrderByItem> order;
    order.push_back(orderAsc(col("(6 / n)")));
    VecSortNode sort(std::move(project), std::move(order));

    std::vector<Row> rows = drainRows(sort);
    ASSERT_EQ(rows.size(), 3u);
    // NULL sorts first ascending (SQLite), then 3, then 6. This used to assert
    // only survival, on the mistaken premise that NULL placement was merely
    // "arrival position" — in fact comparing with Value::operator< is not a strict
    // weak ordering once a key can be NULL, so the sort was undefined behaviour
    // and reordered the non-NULL keys too. See compareForSort in value.h.
    EXPECT_TRUE(rows[0][0].isNull());
    EXPECT_EQ(rows[1][0].asInt(), 3);
    EXPECT_EQ(rows[2][0].asInt(), 6);
}


// The defect this replaces was not NULL placement: with a NULL in the key, the
// NON-NULL keys came out in the wrong order and rows vanished under LIMIT,
// because a non-transitive equivalence is undefined behaviour in stable_sort.
// This is the shape that failed: keys 1 and 2 arrived inverted.
TEST(VecSort, NullKeyDoesNotDisturbNonNullOrdering) {
    Schema schema = vecSchema({{"n", TypeId::INT}});
    // 6/n -> 2, NULL, 6, 1, 3  (arrival order deliberately unsorted)
    std::vector<Row> input = {{Value(int64_t(3))}, {Value(int64_t(0))},
                              {Value(int64_t(1))}, {Value(int64_t(6))},
                              {Value(int64_t(2))}};

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(binOp("/", intLit(6), col("n")));
    Schema proj_schema = vecSchema({{"(6 / n)", TypeId::INT}});
    auto project = std::make_unique<VecProjectNode>(
        makeScan(schema, input), std::move(exprs), proj_schema);

    std::vector<OrderByItem> order;
    order.push_back(orderAsc(col("(6 / n)")));
    VecSortNode sort(std::move(project), std::move(order));

    std::vector<Row> rows = drainRows(sort);
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_TRUE(rows[0][0].isNull());          // NULL first ascending
    EXPECT_EQ(rows[1][0].asInt(), 1);
    EXPECT_EQ(rows[2][0].asInt(), 2);
    EXPECT_EQ(rows[3][0].asInt(), 3);
    EXPECT_EQ(rows[4][0].asInt(), 6);
}


TEST(VecSort, NullKeySortsLastDescending) {
    Schema schema = vecSchema({{"n", TypeId::INT}});
    std::vector<Row> input = {{Value(int64_t(3))}, {Value(int64_t(0))}, {Value(int64_t(1))}};

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(binOp("/", intLit(6), col("n")));   // 2, NULL, 6
    Schema proj_schema = vecSchema({{"(6 / n)", TypeId::INT}});
    auto project = std::make_unique<VecProjectNode>(
        makeScan(schema, input), std::move(exprs), proj_schema);

    std::vector<OrderByItem> order;
    order.push_back(orderDesc(col("(6 / n)")));
    VecSortNode sort(std::move(project), std::move(order));

    std::vector<Row> rows = drainRows(sort);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][0].asInt(), 6);
    EXPECT_EQ(rows[1][0].asInt(), 2);
    EXPECT_TRUE(rows[2][0].isNull());          // NULL last descending
}


// A NULL in the FIRST key must still let the second key break ties, which only
// works if two NULLs compare equal rather than each being "equivalent to
// everything".
TEST(VecSort, TwoNullsTieAndTheSecondKeyDecides) {
    Schema schema = vecSchema({{"n", TypeId::INT}, {"t", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(0)), Value(int64_t(9))},   // NULL, 9
        {Value(int64_t(2)), Value(int64_t(1))},   // 3,    1
        {Value(int64_t(0)), Value(int64_t(4))},   // NULL, 4
    };

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(binOp("/", intLit(6), col("n")));
    exprs.push_back(col("t"));
    Schema proj_schema = vecSchema({{"(6 / n)", TypeId::INT}, {"t", TypeId::INT}});
    auto project = std::make_unique<VecProjectNode>(
        makeScan(schema, input), std::move(exprs), proj_schema);

    std::vector<OrderByItem> order;
    order.push_back(orderAsc(col("(6 / n)")));
    order.push_back(orderAsc(col("t")));
    VecSortNode sort(std::move(project), std::move(order));

    std::vector<Row> rows = drainRows(sort);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_TRUE(rows[0][0].isNull());
    EXPECT_EQ(rows[0][1].asInt(), 4);           // NULL group, ordered by t
    EXPECT_TRUE(rows[1][0].isNull());
    EXPECT_EQ(rows[1][1].asInt(), 9);
    EXPECT_EQ(rows[2][0].asInt(), 3);
}


// ===== compareForSort is a total order; the SQL operators are not =====

TEST(CompareForSort, NullIsTheMinimumAndNullsAreEqual) {
    EXPECT_EQ(compareForSort(Value::null(), Value::null()), 0);
    EXPECT_LT(compareForSort(Value::null(), Value(int64_t(0))), 0);
    EXPECT_GT(compareForSort(Value(int64_t(0)), Value::null()), 0);
    EXPECT_LT(compareForSort(Value::null(), Value(-1.5)), 0);
    EXPECT_LT(compareForSort(Value::null(), Value(std::string(""))), 0);
}

// The property the old comparator violated: equivalence must be transitive.
// Under Value::operator<, 1 ≡ NULL and NULL ≡ 2 while 1 < 2 — which is what made
// stable_sort's behaviour undefined.
TEST(CompareForSort, EquivalenceIsTransitive) {
    const Value a(int64_t(1)), n = Value::null(), b(int64_t(2));

    // the broken property, stated directly against the SQL operators
    EXPECT_FALSE(a < n); EXPECT_FALSE(n < a);   // 1 "equivalent to" NULL
    EXPECT_FALSE(n < b); EXPECT_FALSE(b < n);   // NULL "equivalent to" 2
    EXPECT_TRUE(a < b);                         // but 1 < 2 — non-transitive

    // compareForSort gives a strict weak ordering instead
    EXPECT_LT(compareForSort(n, a), 0);
    EXPECT_LT(compareForSort(a, b), 0);
    EXPECT_LT(compareForSort(n, b), 0);         // transitive
    EXPECT_EQ(compareForSort(a, a), 0);
    EXPECT_EQ(compareForSort(a, b), -compareForSort(b, a));   // antisymmetric
    EXPECT_EQ(compareForSort(n, a), -compareForSort(a, n));
}

TEST(CompareForSort, CoercesIntAgainstDouble) {
    EXPECT_LT(compareForSort(Value(int64_t(1)), Value(1.5)), 0);
    EXPECT_GT(compareForSort(Value(2.5), Value(int64_t(2))), 0);
    EXPECT_EQ(compareForSort(Value(int64_t(2)), Value(2.0)), 0);
}


TEST(VecDistinct, NullIsItsOwnDistinctKey) {
    Schema schema = vecSchema({{"n", TypeId::INT}});
    // divisors: 0 -> NULL, 0 -> NULL (dup), 1 -> 6, 6 -> 1, 1 -> 6 (dup)
    std::vector<Row> input = {
        {Value(int64_t(0))}, {Value(int64_t(0))}, {Value(int64_t(1))},
        {Value(int64_t(6))}, {Value(int64_t(1))},
    };

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(binOp("/", intLit(6), col("n")));
    Schema proj_schema = vecSchema({{"(6 / n)", TypeId::INT}});
    auto project = std::make_unique<VecProjectNode>(
        makeScan(schema, input), std::move(exprs), proj_schema);

    VecDistinctNode distinct(std::move(project));
    std::vector<Row> rows = drainRows(distinct);

    // {NULL, 6, 1} — NULL dedupes against NULL but never against 0
    ASSERT_EQ(rows.size(), 3u);
    int nulls = 0;
    for (const Row& r : rows) if (r[0].isNull()) ++nulls;
    EXPECT_EQ(nulls, 1);
}


// VecLimitNode truncates a chunk's columns in place; the validity mask has to
// be truncated with them or it desynchronises from the data.
TEST(VecLimit, TruncationKeepsValidityAligned) {
    Schema schema = vecSchema({{"n", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(0))},                       // NULL
        {Value(int64_t(1))}, {Value(int64_t(2))},
    };

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(binOp("/", intLit(6), col("n")));
    Schema proj_schema = vecSchema({{"(6 / n)", TypeId::INT}});
    auto project = std::make_unique<VecProjectNode>(
        makeScan(schema, input), std::move(exprs), proj_schema);

    VecLimitNode limit(std::move(project), 2);
    std::vector<Row> rows = drainRows(limit);

    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[0][0].isNull());
    EXPECT_EQ(rows[1][0].asInt(), 6);
}


// ===== Aggregate result types: MIN/MAX preserve the argument type =====
// Non-COUNT aggregates were typed DOUBLE unconditionally, so MIN/MAX over a
// STRING column declared a DOUBLE output column and then threw
// bad_variant_access in the typed accessor. MIN/MAX are order statistics:
// they return an element of the input domain.

TEST(VecHashAggregate, MinMaxOverStringColumn) {
    Schema schema = vecSchema({{"team", TypeId::STRING}});
    std::vector<Row> input = {
        {Value(std::string("Mercedes"))},
        {Value(std::string("AlphaTauri"))},
        {Value(std::string("Williams"))},
    };

    AggregateSpec min_spec;
    min_spec.function = "MIN";
    min_spec.is_star = false;
    min_spec.column = "team";
    min_spec.output_name = "MIN(team)";

    AggregateSpec max_spec;
    max_spec.function = "MAX";
    max_spec.is_star = false;
    max_spec.column = "team";
    max_spec.output_name = "MAX(team)";

    Schema out_schema = vecSchema({{"MIN(team)", TypeId::STRING}, {"MAX(team)", TypeId::STRING}});
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{},
        std::vector<AggregateSpec>{min_spec, max_spec}, out_schema);

    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].asString(), "AlphaTauri");
    EXPECT_EQ(rows[0][1].asString(), "Williams");
}


TEST(VecHashAggregate, MinMaxOverIntColumnStaysInt) {
    Schema schema = vecSchema({{"season", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(2024))}, {Value(int64_t(2022))}, {Value(int64_t(2025))},
    };

    AggregateSpec min_spec;
    min_spec.function = "MIN";
    min_spec.is_star = false;
    min_spec.column = "season";
    min_spec.output_name = "MIN(season)";

    Schema out_schema = vecSchema({{"MIN(season)", TypeId::INT}});
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{},
        std::vector<AggregateSpec>{min_spec}, out_schema);

    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].asInt(), 2022);
}


// ===== ExpressionExecutor: chunk-at-a-time expression evaluation =====
//
// The executor is a second implementation of evaluate()'s semantics that pays
// dispatch once per chunk instead of once per row. evaluate() stays the Volcano
// correctness baseline, so the only test that matters is differential: for every
// expression shape, the executor must agree with evaluate() row for row,
// including which rows are NULL.

// Pull the single chunk a small row set produces, then assert executor output
// equals evaluate() output element by element.
static void expectExecutorMatchesEvaluate(const Schema& schema,
                                          const std::vector<Row>& input,
                                          const Expr* expr,
                                          const char* label) {
    auto scan = makeScan(schema, input);
    scan->open();
    DataChunk* chunk = scan->nextChunk();
    ASSERT_NE(chunk, nullptr) << label;
    ASSERT_EQ(chunk->num_rows, static_cast<int>(input.size())) << label;

    std::vector<int> sel(input.size());
    std::iota(sel.begin(), sel.end(), 0);

    auto exec = ExpressionExecutor::compile(expr, schema);
    ASSERT_NE(exec, nullptr) << "compile() refused: " << label;

    const ColumnVector& out = exec->execute(*chunk, sel);
    ASSERT_EQ(out.size(), static_cast<int>(input.size())) << label;
    EXPECT_EQ(out.type, exec->type()) << label;

    for (int r = 0; r < static_cast<int>(input.size()); ++r) {
        Value expected = evaluate(expr, input[r], schema);
        Value actual   = valueAt(out, r);
        EXPECT_EQ(expected.isNull(), actual.isNull())
            << label << " row " << r << ": null mismatch";
        if (expected.isNull() || actual.isNull()) continue;
        EXPECT_EQ(expected.type(), actual.type()) << label << " row " << r;
        if (expected.type() == TypeId::STRING) {
            EXPECT_EQ(expected.asString(), actual.asString()) << label << " row " << r;
        } else {
            EXPECT_DOUBLE_EQ(expected.toNumeric(), actual.toNumeric())
                << label << " row " << r;
        }
    }
    scan->close();
}

// i = INT, d = DOUBLE, s = STRING; z carries zeros so every division shape
// exercises the x/0 -> NULL branch. t (Week 25) holds longer strings so LIKE
// wildcards and SUBSTRING ranges have something real to chew on, including a
// string shorter than the substring window and one that is empty.
static Schema exprCorpusSchema() {
    return vecSchema({{"i", TypeId::INT}, {"z", TypeId::INT},
                      {"d", TypeId::DOUBLE}, {"s", TypeId::STRING},
                      {"t", TypeId::STRING}});
}

static std::vector<Row> exprCorpusRows() {
    return {
        {Value(int64_t(7)),  Value(int64_t(2)), Value(1.5),  Value(std::string("b")),
         Value(std::string("PROMO BRUSHED STEEL"))},
        {Value(int64_t(-3)), Value(int64_t(0)), Value(0.0),  Value(std::string("a")),
         Value(std::string("MEDIUM POLISHED BRASS"))},
        {Value(int64_t(0)),  Value(int64_t(5)), Value(-2.5), Value(std::string("c")),
         Value(std::string("ab"))},
        {Value(int64_t(10)), Value(int64_t(0)), Value(4.0),  Value(std::string("a")),
         Value(std::string(""))},
    };
}

#define EXPECT_EXEC_MATCHES(expr) \
    expectExecutorMatchesEvaluate(exprCorpusSchema(), exprCorpusRows(), (expr).get(), #expr)

TEST(ExpressionExecutor, MatchesEvaluateOnColumnsAndLiterals) {
    EXPECT_EXEC_MATCHES(col("i"));
    EXPECT_EXEC_MATCHES(col("d"));
    EXPECT_EXEC_MATCHES(col("s"));
    EXPECT_EXEC_MATCHES(intLit(42));
    EXPECT_EXEC_MATCHES(dblLit(2.5));
}

TEST(ExpressionExecutor, MatchesEvaluateOnIntArithmetic) {
    EXPECT_EXEC_MATCHES(binOp("+", col("i"), col("z")));
    EXPECT_EXEC_MATCHES(binOp("-", col("i"), col("z")));
    EXPECT_EXEC_MATCHES(binOp("*", col("i"), col("z")));
    // INT/INT truncates, and z is 0 on two rows -> NULL there
    EXPECT_EXEC_MATCHES(binOp("/", col("i"), col("z")));
    EXPECT_EXEC_MATCHES(binOp("/", intLit(8), col("z")));
}

TEST(ExpressionExecutor, MatchesEvaluateOnDoubleAndMixedArithmetic) {
    EXPECT_EXEC_MATCHES(binOp("+", col("d"), dblLit(1.25)));
    EXPECT_EXEC_MATCHES(binOp("/", col("d"), col("d")));       // 0.0/0.0 -> NULL
    EXPECT_EXEC_MATCHES(binOp("*", col("i"), col("d")));       // INT * DOUBLE promotes
    EXPECT_EXEC_MATCHES(binOp("-", col("d"), col("i")));
    EXPECT_EXEC_MATCHES(binOp("/", col("i"), col("d")));       // /0.0 -> NULL
}

TEST(ExpressionExecutor, MatchesEvaluateOnNestedTpchRevenueShape) {
    // the TPC-H idiom: d * (1 - i) * (1 + i)
    EXPECT_EXEC_MATCHES(binOp("*", binOp("*", col("d"), binOp("-", intLit(1), col("i"))),
                                   binOp("+", intLit(1), col("i"))));
    // NULL must propagate up through an arithmetic parent
    EXPECT_EXEC_MATCHES(binOp("+", binOp("/", col("i"), col("z")), col("i")));
    EXPECT_EXEC_MATCHES(binOp("*", binOp("/", col("d"), intLit(0)), col("d")));
}

TEST(ExpressionExecutor, MatchesEvaluateOnUnaryMinus) {
    auto neg = std::make_unique<UnaryExpr>();
    neg->op = "-";
    neg->operand = col("i");
    EXPECT_EXEC_MATCHES(neg);

    auto neg_d = std::make_unique<UnaryExpr>();
    neg_d->op = "-";
    neg_d->operand = col("d");
    EXPECT_EXEC_MATCHES(neg_d);

    // -(i / z): NULL must survive negation
    auto neg_null = std::make_unique<UnaryExpr>();
    neg_null->op = "-";
    neg_null->operand = binOp("/", col("i"), col("z"));
    EXPECT_EXEC_MATCHES(neg_null);
}

TEST(ExpressionExecutor, MatchesEvaluateOnComparisonsAndLogic) {
    EXPECT_EXEC_MATCHES(binOp(">",  col("i"), intLit(0)));
    EXPECT_EXEC_MATCHES(binOp("<=", col("d"), dblLit(0.0)));
    EXPECT_EXEC_MATCHES(binOp("=",  col("s"), strLit("a")));
    EXPECT_EXEC_MATCHES(binOp("!=", col("s"), strLit("a")));
    EXPECT_EXEC_MATCHES(binOp("<",  col("i"), col("d")));      // mixed compare promotes
    EXPECT_EXEC_MATCHES(binOp("AND", binOp(">", col("i"), intLit(0)),
                                     binOp("<", col("d"), dblLit(3.0))));
    EXPECT_EXEC_MATCHES(binOp("OR",  binOp("=", col("z"), intLit(0)),
                                     binOp(">", col("i"), intLit(5))));
    // comparison against a NULL operand yields NULL, and AND/OR propagate it
    EXPECT_EXEC_MATCHES(binOp(">", binOp("/", col("i"), col("z")), intLit(0)));
    EXPECT_EXEC_MATCHES(binOp("AND", binOp(">", binOp("/", col("i"), col("z")), intLit(0)),
                                     binOp(">", col("i"), intLit(0))));
}

TEST(ExpressionExecutor, MatchesEvaluateOnIsNull) {
    auto isn = std::make_unique<IsNullExpr>();
    isn->operand = binOp("/", col("i"), col("z"));
    isn->is_not_null = false;
    EXPECT_EXEC_MATCHES(isn);

    auto isnn = std::make_unique<IsNullExpr>();
    isnn->operand = binOp("/", col("i"), col("z"));
    isnn->is_not_null = true;
    EXPECT_EXEC_MATCHES(isnn);
}


// ===== Week 25: differential coverage for the new kernels =====
//
// development.md is explicit that this corpus — not kernel-only tests — is what
// holds ExpressionExecutor and evaluate() in agreement. Every Week 25 node with
// a kernel gets its shapes here.

static std::unique_ptr<Expr> likeOn(std::unique_ptr<Expr> operand,
                                    const std::string& pattern, bool negated) {
    auto n = std::make_unique<LikeExpr>();
    n->operand = std::move(operand);
    n->pattern = pattern;
    n->negated = negated;
    return n;
}

static std::unique_ptr<Expr> inOn(std::unique_ptr<Expr> operand,
                                  std::vector<Value> values, bool negated) {
    auto n = std::make_unique<InExpr>();
    n->operand = std::move(operand);
    n->values = std::move(values);
    n->negated = negated;
    return n;
}

static std::unique_ptr<Expr> substringOn(std::unique_ptr<Expr> operand,
                                         std::unique_ptr<Expr> start,
                                         std::unique_ptr<Expr> length) {
    auto n = std::make_unique<SubstringExpr>();
    n->operand = std::move(operand);
    n->start = std::move(start);
    n->length = std::move(length);
    return n;
}

TEST(ExpressionExecutor, MatchesEvaluateOnLike) {
    EXPECT_EXEC_MATCHES(likeOn(col("t"), "PROMO%", false));       // prefix
    EXPECT_EXEC_MATCHES(likeOn(col("t"), "%BRASS", false));       // suffix
    EXPECT_EXEC_MATCHES(likeOn(col("t"), "%POLISHED%", false));   // contains
    EXPECT_EXEC_MATCHES(likeOn(col("t"), "%", false));            // matches empty too
    EXPECT_EXEC_MATCHES(likeOn(col("t"), "", false));             // only the empty string
    EXPECT_EXEC_MATCHES(likeOn(col("t"), "_b", false));           // single-char wildcard
    EXPECT_EXEC_MATCHES(likeOn(col("t"), "promo%", false));       // ASCII case folding
    EXPECT_EXEC_MATCHES(likeOn(col("t"), "PROMO%", true));        // NOT LIKE
    EXPECT_EXEC_MATCHES(likeOn(col("s"), "a", false));
}

TEST(ExpressionExecutor, MatchesEvaluateOnIn) {
    EXPECT_EXEC_MATCHES(inOn(col("i"), {Value(int64_t(7)), Value(int64_t(0))}, false));
    EXPECT_EXEC_MATCHES(inOn(col("i"), {Value(int64_t(7))}, true));            // NOT IN
    EXPECT_EXEC_MATCHES(inOn(col("i"), {Value(int64_t(99))}, false));          // no match
    EXPECT_EXEC_MATCHES(inOn(col("d"), {Value(1.5), Value(4.0)}, false));
    EXPECT_EXEC_MATCHES(inOn(col("s"), {Value(std::string("a"))}, false));
    // INT column against DOUBLE list values: Value::operator== coerces, and the
    // kernel probes both the exact-INT set and the coerced-DOUBLE set
    EXPECT_EXEC_MATCHES(inOn(col("i"), {Value(7.0)}, false));
    EXPECT_EXEC_MATCHES(inOn(col("i"), {Value(7.0), Value(int64_t(0))}, false));
    // DOUBLE column against INT list values
    EXPECT_EXEC_MATCHES(inOn(col("d"), {Value(int64_t(4))}, false));
    // NULL operand -> NULL, under both polarities
    EXPECT_EXEC_MATCHES(inOn(binOp("/", col("i"), col("z")), {Value(int64_t(0))}, false));
    EXPECT_EXEC_MATCHES(inOn(binOp("/", col("i"), col("z")), {Value(int64_t(0))}, true));
}

TEST(ExpressionExecutor, MatchesEvaluateOnSubstring) {
    EXPECT_EXEC_MATCHES(substringOn(col("t"), intLit(1), intLit(5)));
    EXPECT_EXEC_MATCHES(substringOn(col("t"), intLit(1), intLit(0)));   // empty result
    EXPECT_EXEC_MATCHES(substringOn(col("t"), intLit(3), nullptr));     // to end of string
    EXPECT_EXEC_MATCHES(substringOn(col("t"), intLit(50), intLit(2)));  // entirely past the end
    EXPECT_EXEC_MATCHES(substringOn(col("t"), intLit(2), intLit(100))); // length clamps
    EXPECT_EXEC_MATCHES(substringOn(col("s"), intLit(1), intLit(1)));
    // a NULL length must null the whole result — the third operand's validity
    // is folded in separately, since propagateNulls only takes two
    EXPECT_EXEC_MATCHES(substringOn(col("t"), intLit(1), binOp("/", col("i"), col("z"))));
}

// CaseExpr is deliberately uncompiled: evaluate() short-circuits and a chunk
// kernel cannot. compile() must decline so the caller keeps the correct
// per-row path — an uncovered shape is slow, never wrong.
TEST(ExpressionExecutor, DeclinesCaseSoTheFallbackStaysCorrect) {
    Schema schema = exprCorpusSchema();
    auto c = std::make_unique<CaseExpr>();
    CaseExpr::WhenClause w;
    w.condition = binOp(">", col("i"), intLit(0));
    w.result = intLit(1);
    c->when_clauses.push_back(std::move(w));
    c->else_expr = intLit(0);

    EXPECT_EQ(ExpressionExecutor::compile(c.get(), schema), nullptr);
}

#undef EXPECT_EXEC_MATCHES

// The executor is dense in selection order: result index i corresponds to
// sel[i], not to chunk row sel[i]. Callers index it by position.
TEST(ExpressionExecutor, ResultIsDenseInSelectionOrder) {
    Schema schema = exprCorpusSchema();
    std::vector<Row> input = exprCorpusRows();
    auto scan = makeScan(schema, input);
    scan->open();
    DataChunk* chunk = scan->nextChunk();
    ASSERT_NE(chunk, nullptr);

    auto expr = binOp("*", col("i"), intLit(2));
    auto exec = ExpressionExecutor::compile(expr.get(), schema);
    ASSERT_NE(exec, nullptr);

    std::vector<int> sel = {3, 0};                 // rows 10 and 7 -> 20 and 14
    const ColumnVector& out = exec->execute(*chunk, sel);
    ASSERT_EQ(out.size(), 2);
    EXPECT_EQ(valueAt(out, 0).asInt(), 20);
    EXPECT_EQ(valueAt(out, 1).asInt(), 14);
    scan->close();
}

// The scratch buffers are reused across chunks; a stale length or a leftover
// validity mask from the previous call would corrupt the next one.
TEST(ExpressionExecutor, ScratchIsResetBetweenCalls) {
    Schema schema = exprCorpusSchema();
    std::vector<Row> input = exprCorpusRows();
    auto scan = makeScan(schema, input);
    scan->open();
    DataChunk* chunk = scan->nextChunk();
    ASSERT_NE(chunk, nullptr);

    auto expr = binOp("/", col("i"), col("z"));    // NULL where z == 0
    auto exec = ExpressionExecutor::compile(expr.get(), schema);
    ASSERT_NE(exec, nullptr);

    // first call sees the NULL rows, second sees only valid rows
    const ColumnVector& first = exec->execute(*chunk, {1, 3});
    ASSERT_EQ(first.size(), 2);
    EXPECT_TRUE(valueAt(first, 0).isNull());
    EXPECT_TRUE(valueAt(first, 1).isNull());

    const ColumnVector& second = exec->execute(*chunk, {0, 2});
    ASSERT_EQ(second.size(), 2);
    EXPECT_FALSE(valueAt(second, 0).isNull());     // 7 / 2 = 3
    EXPECT_EQ(valueAt(second, 0).asInt(), 3);
    EXPECT_FALSE(valueAt(second, 1).isNull());     // 0 / 5 = 0
    EXPECT_EQ(valueAt(second, 1).asInt(), 0);
    scan->close();
}

// Compilation declines shapes it has no kernel for, so the caller can keep the
// per-row evaluate() path rather than getting a wrong answer.
TEST(ExpressionExecutor, CompileDeclinesUnresolvableColumn) {
    Schema schema = exprCorpusSchema();
    auto expr = col("nonexistent");
    EXPECT_EQ(ExpressionExecutor::compile(expr.get(), schema), nullptr);
}

TEST(ExpressionExecutor, CompileDeclinesRatherThanThrowingOnBadShapes) {
    Schema schema = exprCorpusSchema();

    // STRING arithmetic: ill-typed, evaluate() raises the error instead
    auto str_arith = binOp("+", col("s"), intLit(1));
    EXPECT_NO_THROW(EXPECT_EQ(ExpressionExecutor::compile(str_arith.get(), schema), nullptr));

    // STRING compared to a numeric: Value's operators throw, so decline and let
    // the fallback raise from the same place it always did
    auto mixed_cmp = binOp("=", col("s"), intLit(1));
    EXPECT_NO_THROW(EXPECT_EQ(ExpressionExecutor::compile(mixed_cmp.get(), schema), nullptr));

    // no NULL literal exists in the grammar; a hand-built one is declined, not
    // broadcast as a typeless constant (Value::type() throws on null)
    auto null_lit = std::make_unique<Literal>(Value::null());
    EXPECT_NO_THROW(EXPECT_EQ(ExpressionExecutor::compile(null_lit.get(), schema), nullptr));

    // unknown operator
    auto bad_op = binOp("%%", col("i"), intLit(2));
    EXPECT_NO_THROW(EXPECT_EQ(ExpressionExecutor::compile(bad_op.get(), schema), nullptr));
}


// Scratch buffers are reused across chunks and the final chunk is partial, so
// resetOut() must shrink correctly and no kernel may read a stale element.
// Row count is deliberately not a multiple of BATCH_SIZE.
TEST(ExpressionExecutor, MultiChunkWithPartialFinalChunk) {
    const int n = BATCH_SIZE * 2 + 37;
    Schema schema = vecSchema({{"x", TypeId::INT}, {"z", TypeId::INT}});
    std::vector<Row> input;
    input.reserve(n);
    int64_t expected_sum = 0;
    int expected_nulls = 0;
    for (int i = 0; i < n; ++i) {
        // every 5th row has divisor 0 -> NULL, which SUM must skip
        int64_t z = (i % 5 == 0) ? 0 : (i % 7) + 1;
        input.push_back({Value(static_cast<int64_t>(i)), Value(z)});
        if (z == 0) ++expected_nulls;
        else expected_sum += static_cast<int64_t>(i) / z;   // INT/INT truncates
    }

    auto arg = binOp("/", col("x"), col("z"));
    AggregateSpec sum_spec;
    sum_spec.function = "SUM";
    sum_spec.is_star = false;
    sum_spec.argument = arg.get();
    sum_spec.output_name = "SUM((x / z))";

    AggregateSpec cnt_spec;
    cnt_spec.function = "COUNT";
    cnt_spec.is_star = false;
    cnt_spec.argument = arg.get();
    cnt_spec.output_name = "COUNT((x / z))";

    Schema out_schema = vecSchema({{"SUM((x / z))", TypeId::DOUBLE},
                                   {"COUNT((x / z))", TypeId::INT}});
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{},
        std::vector<AggregateSpec>{sum_spec, cnt_spec}, out_schema);

    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_DOUBLE_EQ(rows[0][0].asDouble(), static_cast<double>(expected_sum));
    // COUNT(expr) counts non-NULL results only
    EXPECT_EQ(rows[0][1].asInt(), n - expected_nulls);
}


// Expression group keys go through the same executor. A NULL key must form its
// own group rather than colliding with a real value or with the string "NULL".
TEST(VecHashAggregate, NullExpressionGroupKeyIsItsOwnGroup) {
    Schema schema = vecSchema({{"x", TypeId::INT}, {"z", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(6)), Value(int64_t(0))},   // 6/0 -> NULL
        {Value(int64_t(6)), Value(int64_t(3))},   // -> 2
        {Value(int64_t(8)), Value(int64_t(0))},   // -> NULL (same group as row 0)
        {Value(int64_t(4)), Value(int64_t(2))},   // -> 2 (same group as row 1)
    };

    GroupByColumn g;
    g.column_name = "(x / z)";
    g.expr = binOp("/", col("x"), col("z"));

    AggregateSpec cnt;
    cnt.function = "COUNT";
    cnt.is_star = true;
    cnt.output_name = "COUNT(*)";

    Schema out_schema = vecSchema({{"(x / z)", TypeId::INT}, {"COUNT(*)", TypeId::INT}});
    auto agg = std::make_unique<VecHashAggregateNode>(
        makeScan(schema, input), std::vector<GroupByColumn>{g},
        std::vector<AggregateSpec>{cnt}, out_schema);

    auto rows = drainRows(*agg);
    ASSERT_EQ(rows.size(), 2u);   // {NULL: 2, 2: 2}
    for (const Row& r : rows) EXPECT_EQ(r[1].asInt(), 2);
    int nulls = 0;
    for (const Row& r : rows) if (r[0].isNull()) ++nulls;
    EXPECT_EQ(nulls, 1);
}


// A NULL arriving in a source CHUNK column (as opposed to one manufactured by
// x/0 inside the expression) must be gathered and propagated. Not reachable
// from SQL today — ColumnarTable cannot express NULL, so an aggregate's input is
// always all-valid — but it is the shape a left outer join will produce, so the
// gather path is pinned down here with a hand-built chunk.
TEST(ExpressionExecutor, NullInSourceColumnPropagates) {
    DataChunk chunk;
    ColumnVector a = makeColumnVector(TypeId::INT);
    ColumnVector b = makeColumnVector(TypeId::DOUBLE);
    // a = [5, NULL, 7],  b = [2.0, 4.0, NULL]
    appendColumnValue(a, Value(int64_t(5)));
    appendColumnValue(a, Value::null());
    appendColumnValue(a, Value(int64_t(7)));
    appendColumnValue(b, Value(2.0));
    appendColumnValue(b, Value(4.0));
    appendColumnValue(b, Value::null());
    // validity must be index-aligned with the dense data
    ASSERT_FALSE(a.all_valid);
    ASSERT_EQ(a.validity.size(), static_cast<size_t>(a.size()));
    ASSERT_FALSE(b.all_valid);
    ASSERT_EQ(b.validity.size(), static_cast<size_t>(b.size()));

    chunk.columns.push_back(std::move(a));
    chunk.columns.push_back(std::move(b));
    chunk.num_rows = 3;
    Schema schema = vecSchema({{"a", TypeId::INT}, {"b", TypeId::DOUBLE}});

    // a * b -> [10.0, NULL, NULL]
    auto expr = binOp("*", col("a"), col("b"));
    auto exec = ExpressionExecutor::compile(expr.get(), schema);
    ASSERT_NE(exec, nullptr);

    const ColumnVector& out = exec->execute(chunk, {0, 1, 2});
    ASSERT_EQ(out.size(), 3);
    EXPECT_FALSE(out.isNull(0));
    EXPECT_DOUBLE_EQ(valueAt(out, 0).asDouble(), 10.0);
    EXPECT_TRUE(out.isNull(1));    // NULL from the left operand
    EXPECT_TRUE(out.isNull(2));    // NULL from the right operand

    // IS NULL reads the operand mask and is never itself NULL
    auto isn = std::make_unique<IsNullExpr>();
    isn->operand = col("a");
    isn->is_not_null = false;
    auto isn_exec = ExpressionExecutor::compile(isn.get(), schema);
    ASSERT_NE(isn_exec, nullptr);
    const ColumnVector& flags = isn_exec->execute(chunk, {0, 1, 2});
    EXPECT_TRUE(flags.all_valid);
    EXPECT_EQ(valueAt(flags, 0).asInt(), 0);
    EXPECT_EQ(valueAt(flags, 1).asInt(), 1);
    EXPECT_EQ(valueAt(flags, 2).asInt(), 0);

    // a selection that skips the NULL row must produce an all-valid result,
    // proving the gather consults the SOURCE index, not the dense position
    const ColumnVector& subset = exec->execute(chunk, {0});
    ASSERT_EQ(subset.size(), 1);
    EXPECT_TRUE(subset.all_valid);
    EXPECT_DOUBLE_EQ(valueAt(subset, 0).asDouble(), 10.0);
}


// ===== Overflow-checked INT arithmetic =====
// Signed overflow is UB, not wraparound: the compiler may assume it away at -O2
// and a UBSan build aborts. SwiftQL used to compute INT64_MAX + 1 as INT64_MIN by
// accident of the target. Both evaluators now raise instead — and must agree.

static void expectOverflowInBothEvaluators(const Schema& schema,
                                           const std::vector<Row>& input,
                                           const Expr* expr,
                                           const char* label) {
    // scalar evaluator (the Volcano path and the vectorized fallback)
    EXPECT_THROW(evaluate(expr, input[0], schema), std::runtime_error) << label;

    // compiled executor must raise on the same input
    auto scan = makeScan(schema, input);
    scan->open();
    DataChunk* chunk = scan->nextChunk();
    ASSERT_NE(chunk, nullptr) << label;
    std::vector<int> sel(input.size());
    std::iota(sel.begin(), sel.end(), 0);
    auto exec = ExpressionExecutor::compile(expr, schema);
    ASSERT_NE(exec, nullptr) << label;
    EXPECT_THROW(exec->execute(*chunk, sel), std::runtime_error) << label;
    scan->close();
}

TEST(CheckedArithmetic, OverflowRaisesInBothEvaluators) {
    Schema schema = vecSchema({{"big", TypeId::INT}, {"neg", TypeId::INT}});
    std::vector<Row> input = {{Value(INT64_MAX), Value(INT64_MIN)}};

    auto add = binOp("+", col("big"), intLit(1));
    expectOverflowInBothEvaluators(schema, input, add.get(), "INT64_MAX + 1");

    auto sub = binOp("-", col("neg"), intLit(1));
    expectOverflowInBothEvaluators(schema, input, sub.get(), "INT64_MIN - 1");

    auto mul = binOp("*", col("big"), intLit(2));
    expectOverflowInBothEvaluators(schema, input, mul.get(), "INT64_MAX * 2");

    auto div = binOp("/", col("neg"), intLit(-1));
    expectOverflowInBothEvaluators(schema, input, div.get(), "INT64_MIN / -1");

    auto neg = std::make_unique<UnaryExpr>();
    neg->op = "-";
    neg->operand = col("neg");
    expectOverflowInBothEvaluators(schema, input, neg.get(), "-INT64_MIN");
}

// DOUBLE arithmetic is IEEE and must NOT be affected by the INT overflow checks.
TEST(CheckedArithmetic, DoubleArithmeticIsUnaffected) {
    Schema schema = vecSchema({{"d", TypeId::DOUBLE}});
    std::vector<Row> input = {{Value(1.0e308)}};
    auto mul = binOp("*", col("d"), dblLit(10.0));   // overflows to +inf, no throw
    EXPECT_NO_THROW(evaluate(mul.get(), input[0], schema));
    expectExecutorMatchesEvaluate(schema, input, mul.get(), "1e308 * 10");
}

// A row that is already NULL must be SKIPPED, not computed: runColumn gathers the
// real underlying value for a NULL row, and that value can legitimately be
// INT64_MIN. Computing on it would raise an overflow for a row whose result is
// never read — evaluate() short-circuits on NULL before touching either operand,
// so the kernels must too.
TEST(CheckedArithmetic, AlreadyNullRowsAreNotComputed) {
    DataChunk chunk;
    ColumnVector v = makeColumnVector(TypeId::INT);
    appendColumnValue(v, Value(int64_t(1)));
    appendColumnValue(v, Value::null());     // placeholder 0 underneath
    chunk.columns.push_back(std::move(v));

    // deliberately put INT64_MIN under the NULL so a computed row would overflow
    std::get<std::vector<int64_t>>(chunk.columns[0].data)[1] = INT64_MIN;
    chunk.num_rows = 2;
    Schema schema = vecSchema({{"n", TypeId::INT}});

    // n - 1 : row 0 = 0, row 1 = NULL (must not attempt INT64_MIN - 1)
    auto sub = binOp("-", col("n"), intLit(1));
    auto exec = ExpressionExecutor::compile(sub.get(), schema);
    ASSERT_NE(exec, nullptr);
    const ColumnVector* out = nullptr;
    ASSERT_NO_THROW(out = &exec->execute(chunk, {0, 1}));
    EXPECT_EQ(valueAt(*out, 0).asInt(), 0);
    EXPECT_TRUE(out->isNull(1));

    // -n : same, for the unary path
    auto neg = std::make_unique<UnaryExpr>();
    neg->op = "-";
    neg->operand = col("n");
    auto neg_exec = ExpressionExecutor::compile(neg.get(), schema);
    ASSERT_NE(neg_exec, nullptr);
    ASSERT_NO_THROW(out = &neg_exec->execute(chunk, {0, 1}));
    EXPECT_EQ(valueAt(*out, 0).asInt(), -1);
    EXPECT_TRUE(out->isNull(1));
}


// ===== The executor is wired into the filter and projection positions =====
// Both used to reconstruct a Row per row and walk the AST with dynamic_cast.
// Measured on 1M rows, Release: VecFilter 222ms -> 1.7ms, VecProject 162ms ->
// 1.0ms. These tests pin the CORRECTNESS of the rewritten paths; the win itself
// is recorded in development.md.

TEST(VecFilterExecutor, ExpressionPredicateMatchesScalarSemantics) {
    Schema schema = vecSchema({{"x", TypeId::INT}, {"z", TypeId::INT}});
    std::vector<Row> input;
    for (int i = 0; i < BATCH_SIZE + 7; ++i) {
        input.push_back({Value(static_cast<int64_t>(i)),
                         Value(static_cast<int64_t>(i % 4))});   // z == 0 every 4th row
    }

    // WHERE x * 2 > 100 — arithmetic on the left, so no `col op literal` fast path
    VecFilterNode filter(makeScan(schema, input),
                         binOp(">", binOp("*", col("x"), intLit(2)), intLit(100)));
    int expected = 0;
    for (const Row& r : input) if (r[0].asInt() * 2 > 100) ++expected;
    EXPECT_EQ(countSurvivors(filter), expected);
}

TEST(VecFilterExecutor, NullPredicateResultRejectsTheRow) {
    Schema schema = vecSchema({{"x", TypeId::INT}, {"z", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(10)), Value(int64_t(2))},   // 10/2 = 5  > 1 -> passes
        {Value(int64_t(10)), Value(int64_t(0))},   // 10/0 = NULL     -> rejected
        {Value(int64_t(10)), Value(int64_t(5))},   // 10/5 = 2  > 1 -> passes
    };
    // WHERE x / z > 1 : NULL is not true
    VecFilterNode filter(makeScan(schema, input),
                         binOp(">", binOp("/", col("x"), col("z")), intLit(1)));
    EXPECT_EQ(countSurvivors(filter), 2);
}

TEST(VecFilterExecutor, CascadesUnderAndAcrossCompiledAndFastPaths) {
    Schema schema = vecSchema({{"x", TypeId::INT}, {"z", TypeId::INT}});
    std::vector<Row> input;
    for (int i = 0; i < 500; ++i) {
        input.push_back({Value(static_cast<int64_t>(i)), Value(static_cast<int64_t>(i % 3))});
    }
    // (x > 100) AND (x * 2 < 600): a fast-path conjunct cascading into a
    // compiled one — the compiled side must honour the incoming SelectionVector
    VecFilterNode filter(makeScan(schema, input),
        binOp("AND", binOp(">", col("x"), intLit(100)),
                     binOp("<", binOp("*", col("x"), intLit(2)), intLit(600))));
    int expected = 0;
    for (const Row& r : input) {
        int64_t x = r[0].asInt();
        if (x > 100 && x * 2 < 600) ++expected;
    }
    EXPECT_EQ(countSurvivors(filter), expected);
}

TEST(VecProjectExecutor, ExpressionColumnsMatchScalarSemantics) {
    Schema schema = vecSchema({{"x", TypeId::INT}, {"d", TypeId::DOUBLE}});
    std::vector<Row> input;
    for (int i = 0; i < BATCH_SIZE + 13; ++i) {
        input.push_back({Value(static_cast<int64_t>(i)), Value(i * 0.5)});
    }

    // a plain column, a compiled INT expression, and a compiled DOUBLE expression
    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(col("x"));
    exprs.push_back(binOp("*", col("x"), intLit(2)));
    exprs.push_back(binOp("*", col("d"), binOp("-", intLit(1), col("x"))));
    Schema out_schema = vecSchema({{"x", TypeId::INT},
                                   {"(x * 2)", TypeId::INT},
                                   {"(d * (1 - x))", TypeId::DOUBLE}});

    VecProjectNode project(makeScan(schema, input), std::move(exprs), out_schema);
    std::vector<Row> rows = drainRows(project);

    ASSERT_EQ(rows.size(), input.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        int64_t x = input[i][0].asInt();
        double d = input[i][1].asDouble();
        EXPECT_EQ(rows[i][0].asInt(), x) << "row " << i;
        EXPECT_EQ(rows[i][1].asInt(), x * 2) << "row " << i;
        EXPECT_DOUBLE_EQ(rows[i][2].asDouble(), d * (1 - static_cast<double>(x))) << "row " << i;
    }
}

// The whole-column copy path must carry the executor's validity mask across, and
// must respect the incoming SelectionVector rather than the physical row order.
TEST(VecProjectExecutor, CarriesNullsAndHonoursSelectionVector) {
    Schema schema = vecSchema({{"x", TypeId::INT}, {"z", TypeId::INT}});
    std::vector<Row> input = {
        {Value(int64_t(10)), Value(int64_t(0))},   // filtered out below
        {Value(int64_t(20)), Value(int64_t(0))},   // survives; 20/0 -> NULL
        {Value(int64_t(30)), Value(int64_t(3))},   // survives; 30/3 -> 10
    };

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(binOp("/", col("x"), col("z")));
    Schema out_schema = vecSchema({{"(x / z)", TypeId::INT}});

    auto filter = std::make_unique<VecFilterNode>(makeScan(schema, input),
                                                  binOp(">", col("x"), intLit(15)));
    VecProjectNode project(std::move(filter), std::move(exprs), out_schema);
    std::vector<Row> rows = drainRows(project);

    ASSERT_EQ(rows.size(), 2u);
    EXPECT_TRUE(rows[0][0].isNull());        // 20 / 0
    EXPECT_EQ(rows[1][0].asInt(), 10);       // 30 / 3
}


// ===== Three-valued AND/OR =====
// evaluate() propagated NULL through the connectives, so Volcano answered
// `WHERE (season/0) > 0 OR 1 = 1` with zero rows while the vectorized path
// answered with all of them. Both evaluators are now three-valued and agree.

TEST(ThreeValuedLogic, MatchesSqlTruthTables) {
    Schema schema = vecSchema({{"t", TypeId::INT}, {"f", TypeId::INT}, {"z", TypeId::INT}});
    std::vector<Row> input = {{Value(int64_t(1)), Value(int64_t(0)), Value(int64_t(0))}};
    // unknown = 1 / z with z == 0 -> NULL
    auto unknown = [&]{ return binOp("/", intLit(1), col("z")); };
    auto isTrue  = [&]{ return binOp(">", col("t"), intLit(0)); };   // 1 > 0  -> true
    auto isFalse = [&]{ return binOp(">", col("f"), intLit(0)); };   // 0 > 0  -> false

    struct Case { const char* label; std::unique_ptr<Expr> e; int expect; };  // -1 = NULL
    std::vector<Case> cases;
    cases.push_back({"true  AND unknown", binOp("AND", isTrue(),  unknown()), -1});
    cases.push_back({"false AND unknown", binOp("AND", isFalse(), unknown()),  0});
    cases.push_back({"unknown AND false", binOp("AND", unknown(), isFalse()),  0});
    cases.push_back({"unknown AND true",  binOp("AND", unknown(), isTrue()),  -1});
    cases.push_back({"true  OR unknown",  binOp("OR",  isTrue(),  unknown()),  1});
    cases.push_back({"unknown OR true",   binOp("OR",  unknown(), isTrue()),   1});
    cases.push_back({"false OR unknown",  binOp("OR",  isFalse(), unknown()), -1});
    cases.push_back({"unknown OR unknown",binOp("OR",  unknown(), unknown()), -1});
    cases.push_back({"unknown AND unknown", binOp("AND", unknown(), unknown()), -1});

    for (const Case& c : cases) {
        Value v = evaluate(c.e.get(), input[0], schema);
        if (c.expect < 0) {
            EXPECT_TRUE(v.isNull()) << c.label;
        } else {
            ASSERT_FALSE(v.isNull()) << c.label;
            EXPECT_EQ(v.asInt(), c.expect) << c.label;
        }
        // and the compiled executor must agree, row for row
        expectExecutorMatchesEvaluate(schema, input, c.e.get(), c.label);
    }
}
