#include <gtest/gtest.h>
#include "execution/vec_scan_node.h"
#include "execution/vec_types.h"
#include "storage/columnar_table.h"
#include "storage/rle_column.h"
#include "storage/dictionary_encoder.h"
#include "common/schema.h"
#include "common/value.h"
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