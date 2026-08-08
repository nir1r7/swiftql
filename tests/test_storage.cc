#include <gtest/gtest.h>
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"
#include "storage/chunk_pruner.h"
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

// ===== Week 35 — the pipe-delimited (.tbl) format =====

static Schema regionSchema() {
    return Schema({{"r_regionkey", TypeId::INT},
                   {"r_name",      TypeId::STRING},
                   {"r_comment",   TypeId::STRING}});
}

// The header assumption was the DANGEROUS half of the pre-Week-35 loader: it
// consumed line 1 unconditionally, so a headerless .tbl silently lost its first
// row and every aggregate came out quietly wrong. Five, not four, and the first
// key is 0 — a count alone would not catch an off-by-one at the other end.
TEST(TblLoaderTest, KeepsTheFirstRowOfAHeaderlessFile) {
    auto rows = CSVLoader::load("../tests/data/test_region.tbl", regionSchema(),
                                FileFormat::tbl());
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_EQ(rows[0][0].asInt(), 0);
    EXPECT_EQ(rows[0][1].asString(), "AFRICA");
    EXPECT_EQ(rows[4][0].asInt(), 4);
}

// .tbl TERMINATES each field, so a correct split yields schema.size()+1 fields
// with an empty last one. Before the trailing-delimiter rule this threw a
// column-count mismatch on every single line.
TEST(TblLoaderTest, DropsTheTrailingDelimitersEmptyField) {
    auto rows = CSVLoader::load("../tests/data/test_region.tbl", regionSchema(),
                                FileFormat::tbl());
    ASSERT_EQ(rows.size(), 5u);
    // The last column survives with its real content rather than being eaten.
    EXPECT_EQ(rows[1][2].asString(), "hs use ironic, even requests.");
}

// The comma inside r_comment is the README's `Commas inside string values not
// supported in CSV input` limitation — sidestepped, not fixed, by the pipe
// delimiter. This is the query-visible reason TPC-H comment columns load at all.
TEST(TblLoaderTest, ACommaInsideAFieldSurvivesThePipeDelimiter) {
    auto rows = CSVLoader::load("../tests/data/test_region.tbl", regionSchema(),
                                FileFormat::tbl());
    ASSERT_EQ(rows.size(), 5u);
    EXPECT_NE(rows[1][2].asString().find(','), std::string::npos);
}

// The defaults ARE the pre-Week-35 behaviour. If this fails, a CSV fixture
// somewhere started being read as something else.
TEST(TblLoaderTest, TheDefaultFormatStillReadsCsvUnchanged) {
    Schema schema({{"lap_id", TypeId::INT}, {"team", TypeId::STRING}, {"speed", TypeId::DOUBLE}});
    auto with_default = CSVLoader::load("../tests/data/test_laps.csv", schema);
    auto with_explicit = CSVLoader::load("../tests/data/test_laps.csv", schema,
                                         FileFormat::csv());
    ASSERT_EQ(with_default.size(), with_explicit.size());
    EXPECT_EQ(with_default.size(), 3u);
}

// The silent-wrong-value guard. std::stod("1996-01-02") returns 1996.0 and stops
// at the first '-', so a DATE column mistyped DOUBLE used to answer every
// predicate on a year-shaped number with no error at any layer. Full consumption
// turns ~61 hand-typed TPC-H columns' worth of transcription risk into a loud
// load-time failure.
TEST(TblLoaderTest, RejectsAPartiallyConsumedNumericField) {
    // r_name mistyped DOUBLE: "AFRICA" is not a number at all.
    Schema mistyped({{"r_regionkey", TypeId::INT},
                     {"r_name",      TypeId::DOUBLE},
                     {"r_comment",   TypeId::STRING}});
    EXPECT_THROW(CSVLoader::load("../tests/data/test_region.tbl", mistyped,
                                 FileFormat::tbl()),
                 std::exception);

    // The date-shaped case, which is the one that used to SUCCEED with 1996.0.
    Schema date_as_double({{"d", TypeId::DOUBLE}});
    try {
        CSVLoader::load("../tests/data/test_dates.tbl", date_as_double,
                        FileFormat::tbl());
        FAIL() << "a date field typed DOUBLE must not load as a year";
    } catch (const std::exception& e) {
        // The message must name the offending text AND the column, or a 61-column
        // catalog typo is unactionable.
        EXPECT_NE(std::string(e.what()).find("1996-01-02"), std::string::npos);
        EXPECT_NE(std::string(e.what()).find("'d'"), std::string::npos);
    }
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
    const auto& name_enc = std::get<DictionaryEncoder>(tbl.columns.at("name"));
    const auto& val_arr  = std::get<std::vector<double>>(tbl.columns.at("val"));

    ASSERT_EQ(id_arr.size(),        3u);
    ASSERT_EQ(name_enc.codes.size(), 3u);
    ASSERT_EQ(val_arr.size(),       3u);

    EXPECT_EQ(id_arr[0], 1); EXPECT_EQ(id_arr[1], 2); EXPECT_EQ(id_arr[2], 3);
    EXPECT_EQ(name_enc.decode(0), "alpha"); EXPECT_EQ(name_enc.decode(1), "beta"); EXPECT_EQ(name_enc.decode(2), "gamma");
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


// ===== Zone Map Construction Tests =====

// CSVToColumnar::convert() populates zone_maps for every column.
TEST(ZoneMapConstruction, BuiltForAllColumns) {
    Schema schema = makeColumnarSchema({
        {"id",    TypeId::INT},
        {"val",   TypeId::DOUBLE},
        {"label", TypeId::STRING}
    });
    std::vector<Row> rows = {
        {Value(int64_t(1)), Value(1.0), Value(std::string("a"))},
        {Value(int64_t(2)), Value(2.0), Value(std::string("b"))},
        {Value(int64_t(3)), Value(3.0), Value(std::string("c"))},
    };
    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    EXPECT_EQ(tbl.zone_maps.size(), 3u);
    EXPECT_EQ(tbl.zone_maps.count("id"),    1u);
    EXPECT_EQ(tbl.zone_maps.count("val"),   1u);
    EXPECT_EQ(tbl.zone_maps.count("label"), 1u);
}

// Single chunk (< CHUNK_SIZE rows) captures the correct min and max.
TEST(ZoneMapConstruction, SingleChunkMinMax) {
    Schema schema = makeColumnarSchema({{"season", TypeId::INT}});
    std::vector<Row> rows = {
        {Value(int64_t(10))},
        {Value(int64_t(5))},
        {Value(int64_t(20))},
        {Value(int64_t(15))},
        {Value(int64_t(30))},
    };
    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    ASSERT_EQ(tbl.zone_maps.at("season").size(), 1u);
    EXPECT_EQ(tbl.zone_maps.at("season")[0].min_val, Value(int64_t(5)));
    EXPECT_EQ(tbl.zone_maps.at("season")[0].max_val, Value(int64_t(30)));
}

// Single chunk has correct start_row=0 and row_count matching the data size.
TEST(ZoneMapConstruction, SingleChunkStartRowAndCount) {
    Schema schema = makeColumnarSchema({{"season", TypeId::INT}});
    std::vector<Row> rows = {
        {Value(int64_t(10))},
        {Value(int64_t(5))},
        {Value(int64_t(20))},
        {Value(int64_t(15))},
        {Value(int64_t(30))},
    };
    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    ASSERT_EQ(tbl.zone_maps.at("season").size(), 1u);
    EXPECT_EQ(tbl.zone_maps.at("season")[0].start_row, 0);
    EXPECT_EQ(tbl.zone_maps.at("season")[0].row_count, 5);
}

// CHUNK_SIZE+10 rows produce exactly 2 chunks; last chunk has the correct partial size.
TEST(ZoneMapConstruction, MultipleChunksCreatedWithCorrectSizes) {
    Schema schema = makeColumnarSchema({{"season", TypeId::INT}});
    std::vector<Row> rows;
    rows.reserve(CHUNK_SIZE + 10);
    for (int i = 0; i < CHUNK_SIZE + 10; ++i)
        rows.push_back({Value(int64_t(i))});  // unique values, not RLE-compressed

    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    ASSERT_EQ(tbl.zone_maps.at("season").size(), 2u);
    EXPECT_EQ(tbl.zone_maps.at("season")[0].start_row,  0);
    EXPECT_EQ(tbl.zone_maps.at("season")[0].row_count,  CHUNK_SIZE);
    EXPECT_EQ(tbl.zone_maps.at("season")[1].start_row,  CHUNK_SIZE);
    EXPECT_EQ(tbl.zone_maps.at("season")[1].row_count,  10);
}

// Dictionary-encoded STRING columns produce zone maps over decoded string values.
TEST(ZoneMapConstruction, DictionaryEncodedColumnZoneMap) {
    Schema schema = makeColumnarSchema({{"name", TypeId::STRING}});
    std::vector<Row> rows = {
        {Value(std::string("gamma"))},
        {Value(std::string("alpha"))},
        {Value(std::string("beta"))},
        {Value(std::string("delta"))},
    };
    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    // STRING columns are always stored as DictionaryEncoder
    ASSERT_TRUE(std::holds_alternative<DictionaryEncoder>(tbl.columns.at("name")));

    ASSERT_EQ(tbl.zone_maps.at("name").size(), 1u);
    EXPECT_EQ(tbl.zone_maps.at("name")[0].min_val, Value(std::string("alpha")));
    EXPECT_EQ(tbl.zone_maps.at("name")[0].max_val, Value(std::string("gamma")));
}

// DOUBLE columns produce zone maps with correct double min/max.
TEST(ZoneMapConstruction, DoubleColumnZoneMap) {
    Schema schema = makeColumnarSchema({{"speed", TypeId::DOUBLE}});
    std::vector<Row> rows = {
        {Value(1.5)},
        {Value(3.5)},
        {Value(2.5)},
    };
    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    ASSERT_EQ(tbl.zone_maps.at("speed").size(), 1u);
    EXPECT_EQ(tbl.zone_maps.at("speed")[0].min_val, Value(1.5));
    EXPECT_EQ(tbl.zone_maps.at("speed")[0].max_val, Value(3.5));
}


// ===== ChunkPruner::canSkipChunk Tests =====
// All use chunk: start=0, count=4, min=2020, max=2023

static ColumnChunk testChunk() {
    return {0, 4, Value(int64_t(2020)), Value(int64_t(2023))};
}

TEST(ChunkPrunerCanSkip, EqualAboveMax) {
    EXPECT_TRUE(ChunkPruner::canSkipChunk("=", Value(int64_t(2025)), testChunk()));
}

TEST(ChunkPrunerCanSkip, EqualBelowMin) {
    EXPECT_TRUE(ChunkPruner::canSkipChunk("=", Value(int64_t(2019)), testChunk()));
}

// Boundary: val==min is inside the range, cannot skip.
TEST(ChunkPrunerCanSkip, EqualAtMin) {
    EXPECT_FALSE(ChunkPruner::canSkipChunk("=", Value(int64_t(2020)), testChunk()));
}

// Boundary: val==max is inside the range, cannot skip.
TEST(ChunkPrunerCanSkip, EqualAtMax) {
    EXPECT_FALSE(ChunkPruner::canSkipChunk("=", Value(int64_t(2023)), testChunk()));
}

TEST(ChunkPrunerCanSkip, EqualInRange) {
    EXPECT_FALSE(ChunkPruner::canSkipChunk("=", Value(int64_t(2021)), testChunk()));
}

// Boundary: val==min for col<val — all values in chunk are >= min == val, none satisfy col<val.
TEST(ChunkPrunerCanSkip, LessThanAtMin) {
    EXPECT_TRUE(ChunkPruner::canSkipChunk("<", Value(int64_t(2020)), testChunk()));
}

// val > min — chunk contains min=2020 which satisfies col < 2021.
TEST(ChunkPrunerCanSkip, LessThanAboveMin) {
    EXPECT_FALSE(ChunkPruner::canSkipChunk("<", Value(int64_t(2021)), testChunk()));
}

// Boundary: val==max for col>val — all values in chunk are <= max == val, none satisfy col>val.
TEST(ChunkPrunerCanSkip, GreaterThanAtMax) {
    EXPECT_TRUE(ChunkPruner::canSkipChunk(">", Value(int64_t(2023)), testChunk()));
}

// val < max — chunk contains max=2023 which satisfies col > 2022.
TEST(ChunkPrunerCanSkip, GreaterThanBelowMax) {
    EXPECT_FALSE(ChunkPruner::canSkipChunk(">", Value(int64_t(2022)), testChunk()));
}

TEST(ChunkPrunerCanSkip, LessThanOrEqualBelowMin) {
    EXPECT_TRUE(ChunkPruner::canSkipChunk("<=", Value(int64_t(2019)), testChunk()));
}

// Boundary: val==min for col<=val — the min row satisfies col<=min, cannot skip.
TEST(ChunkPrunerCanSkip, LessThanOrEqualAtMin) {
    EXPECT_FALSE(ChunkPruner::canSkipChunk("<=", Value(int64_t(2020)), testChunk()));
}

TEST(ChunkPrunerCanSkip, GreaterThanOrEqualAboveMax) {
    EXPECT_TRUE(ChunkPruner::canSkipChunk(">=", Value(int64_t(2024)), testChunk()));
}

// Boundary: val==max for col>=val — the max row satisfies col>=max, cannot skip.
TEST(ChunkPrunerCanSkip, GreaterThanOrEqualAtMax) {
    EXPECT_FALSE(ChunkPruner::canSkipChunk(">=", Value(int64_t(2023)), testChunk()));
}

// != can never be pruned via range metadata.
TEST(ChunkPrunerCanSkip, NotEqualNeverSkips) {
    EXPECT_FALSE(ChunkPruner::canSkipChunk("!=", Value(int64_t(2022)), testChunk()));
}


// ===== ChunkPruner::shouldSkip Tests =====

// The schema the hint's refs are resolved against — since the totality screen
// (parser/expr_totality.h) reached ChunkPruner, shouldSkip needs the scanning
// node's own schema to type a conjunct's operands. `unknown_col` is IN the
// schema and absent from the zone maps on purpose: that keeps
// ColNotInZoneMapNoSkip testing the zone-map lookup rather than the screen.
static Schema prunerSchema() {
    return makeColumnarSchema({{"season", TypeId::INT},
                               {"speed", TypeId::DOUBLE},
                               {"unknown_col", TypeId::INT}});
}

TEST(ChunkPrunerShouldSkip, SinglePredicateProveSkip) {
    std::unordered_map<std::string, std::vector<ColumnChunk>> zone_maps;
    zone_maps["season"] = {{0, 4, Value(int64_t(2020)), Value(int64_t(2023))}};

    auto pred = makeColumnarBinary("=", columnarColRef("season"), columnarLit(Value(int64_t(2025))));
    EXPECT_TRUE(ChunkPruner::shouldSkip(pred.get(), zone_maps, 0, prunerSchema()));
}

TEST(ChunkPrunerShouldSkip, SinglePredicateNoSkip) {
    std::unordered_map<std::string, std::vector<ColumnChunk>> zone_maps;
    zone_maps["season"] = {{0, 4, Value(int64_t(2020)), Value(int64_t(2023))}};

    auto pred = makeColumnarBinary("=", columnarColRef("season"), columnarLit(Value(int64_t(2022))));
    EXPECT_FALSE(ChunkPruner::shouldSkip(pred.get(), zone_maps, 0, prunerSchema()));
}

// AND walk: season sub-predicate alone proves skip; speed not in zone_maps.
TEST(ChunkPrunerShouldSkip, AndWalkOneProvesSkip) {
    std::unordered_map<std::string, std::vector<ColumnChunk>> zone_maps;
    zone_maps["season"] = {{0, 4, Value(int64_t(2020)), Value(int64_t(2023))}};

    auto season_pred = makeColumnarBinary("=",   columnarColRef("season"), columnarLit(Value(int64_t(2025))));
    auto speed_pred  = makeColumnarBinary(">",   columnarColRef("speed"),  columnarLit(Value(300.0)));
    auto and_pred    = makeColumnarBinary("AND", std::move(season_pred),   std::move(speed_pred));

    EXPECT_TRUE(ChunkPruner::shouldSkip(and_pred.get(), zone_maps, 0, prunerSchema()));
}

// AND walk: both sub-predicates fail to prove skip independently.
TEST(ChunkPrunerShouldSkip, AndWalkBothFail) {
    std::unordered_map<std::string, std::vector<ColumnChunk>> zone_maps;
    zone_maps["season"] = {{0, 4, Value(int64_t(2020)), Value(int64_t(2023))}};
    zone_maps["speed"]  = {{0, 4, Value(150.0),         Value(350.0)}};

    // season=2022 in [2020,2023]: cannot skip
    // speed>100.0 with max=350.0: 100.0 >= 350.0 is false: cannot skip
    auto season_pred = makeColumnarBinary("=",   columnarColRef("season"), columnarLit(Value(int64_t(2022))));
    auto speed_pred  = makeColumnarBinary(">",   columnarColRef("speed"),  columnarLit(Value(100.0)));
    auto and_pred    = makeColumnarBinary("AND", std::move(season_pred),   std::move(speed_pred));

    EXPECT_FALSE(ChunkPruner::shouldSkip(and_pred.get(), zone_maps, 0, prunerSchema()));
}

// Predicate column absent from zone_maps: no crash, returns false.
TEST(ChunkPrunerShouldSkip, ColNotInZoneMapNoSkip) {
    std::unordered_map<std::string, std::vector<ColumnChunk>> zone_maps;
    zone_maps["season"] = {{0, 4, Value(int64_t(2020)), Value(int64_t(2023))}};

    auto pred = makeColumnarBinary("=", columnarColRef("unknown_col"), columnarLit(Value(int64_t(2025))));
    EXPECT_FALSE(ChunkPruner::shouldSkip(pred.get(), zone_maps, 0, prunerSchema()));
}

// nullptr WHERE always returns false without crashing.
TEST(ChunkPrunerShouldSkip, NullWhereNoSkip) {
    std::unordered_map<std::string, std::vector<ColumnChunk>> zone_maps;
    zone_maps["season"] = {{0, 4, Value(int64_t(2020)), Value(int64_t(2023))}};

    EXPECT_FALSE(ChunkPruner::shouldSkip(nullptr, zone_maps, 0, prunerSchema()));
}


// ===== SeqScanNode Pruning Integration Tests =====

static Schema singleSeasonSchema() {
    return makeColumnarSchema({{"season", TypeId::INT}});
}

// 3 rows all season=2020; zone map chunk 0 is [2020,2020].
static ColumnarTable makeSingleChunkTable() {
    Schema schema = singleSeasonSchema();
    std::vector<Row> rows = {
        {Value(int64_t(2020))},
        {Value(int64_t(2020))},
        {Value(int64_t(2020))},
    };
    return CSVToColumnar::convert(rows, schema);
}

// Predicate val > max: entire chunk skipped, zero rows returned.
TEST(SeqScanNodePruning, SkipsAllRowsWhenChunkProvedEmpty) {
    Schema schema = singleSeasonSchema();
    auto pred = makeColumnarBinary("=", columnarColRef("season"), columnarLit(Value(int64_t(2025))));
    SeqScanNode scan("test", makeSingleChunkTable(), schema, pred.get());

    EXPECT_EQ(columnarDrainAll(&scan).size(), 0u);
}

// Predicate val in range: chunk not skipped, all rows returned.
TEST(SeqScanNodePruning, NoSkipWhenPredicateInRange) {
    Schema schema = singleSeasonSchema();
    auto pred = makeColumnarBinary("=", columnarColRef("season"), columnarLit(Value(int64_t(2020))));
    SeqScanNode scan("test", makeSingleChunkTable(), schema, pred.get());

    EXPECT_EQ(columnarDrainAll(&scan).size(), 3u);
}

// nullptr pruning predicate: no pruning applied, all rows returned.
TEST(SeqScanNodePruning, NoSkipWithNullPredicate) {
    Schema schema = singleSeasonSchema();
    SeqScanNode scan("test", makeSingleChunkTable(), schema, nullptr);

    EXPECT_EQ(columnarDrainAll(&scan).size(), 3u);
}

// After a scan that skips, explain() reports chunks_skipped=1/1.
TEST(SeqScanNodePruning, ExplainShowsChunksSkippedAfterScan) {
    Schema schema = singleSeasonSchema();
    auto pred = makeColumnarBinary("=", columnarColRef("season"), columnarLit(Value(int64_t(2025))));
    SeqScanNode scan("test", makeSingleChunkTable(), schema, pred.get());

    scan.open();
    while (scan.next()) {}
    scan.close();

    EXPECT_NE(scan.explain().find("chunks_skipped=1/1"), std::string::npos);
}

// After a scan with no skip, explain() reports chunks_skipped=0/1.
TEST(SeqScanNodePruning, ExplainShowsZeroWhenNoSkip) {
    Schema schema = singleSeasonSchema();
    auto pred = makeColumnarBinary("=", columnarColRef("season"), columnarLit(Value(int64_t(2020))));
    SeqScanNode scan("test", makeSingleChunkTable(), schema, pred.get());

    scan.open();
    while (scan.next()) {}
    scan.close();

    EXPECT_NE(scan.explain().find("chunks_skipped=0/1"), std::string::npos);
}

// open() resets skipped_chunks_ so a second pass doesn't accumulate counts.
TEST(SeqScanNodePruning, OpenResetsSkippedChunksCount) {
    Schema schema = singleSeasonSchema();
    auto pred = makeColumnarBinary("=", columnarColRef("season"), columnarLit(Value(int64_t(2025))));
    SeqScanNode scan("test", makeSingleChunkTable(), schema, pred.get());

    // First pass: skips chunk → skipped_chunks_=1
    scan.open();
    while (scan.next()) {}
    scan.close();
    EXPECT_NE(scan.explain().find("chunks_skipped=1/1"), std::string::npos);

    // Second pass: open() must reset to 0; skip increments back to 1 (not 2)
    scan.open();
    while (scan.next()) {}
    scan.close();
    EXPECT_NE(scan.explain().find("chunks_skipped=1/1"), std::string::npos);
    EXPECT_EQ(scan.explain().find("chunks_skipped=2/1"), std::string::npos);
}

// CHUNK_SIZE rows season=2020 then 10 rows season=2025:
// chunk 0 [2020,2020] is skipped; chunk 1 [2025,2025] is not.
// Exactly 10 rows are returned and explain shows chunks_skipped=1/2.
TEST(SeqScanNodePruning, MultiChunkPartialSkip) {
    Schema schema = singleSeasonSchema();
    std::vector<Row> rows;
    rows.reserve(CHUNK_SIZE + 10);
    for (int i = 0; i < CHUNK_SIZE; ++i)
        rows.push_back({Value(int64_t(2020))});
    for (int i = 0; i < 10; ++i)
        rows.push_back({Value(int64_t(2025))});

    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);
    auto pred = makeColumnarBinary("=", columnarColRef("season"), columnarLit(Value(int64_t(2025))));
    SeqScanNode scan("test", std::move(tbl), schema, pred.get());

    auto result = columnarDrainAll(&scan);

    ASSERT_EQ(result.size(), 10u);
    for (const auto& row : result)
        EXPECT_EQ(row[0].asInt(), 2025);

    EXPECT_NE(scan.explain().find("chunks_skipped=1/2"), std::string::npos);
}

// AND predicate: season sub-predicate alone proves skip.
TEST(SeqScanNodePruning, AndPredicatePrunesCorrectly) {
    Schema schema = makeColumnarSchema({
        {"season", TypeId::INT},
        {"speed",  TypeId::DOUBLE}
    });
    std::vector<Row> rows = {
        {Value(int64_t(2020)), Value(100.0)},
        {Value(int64_t(2020)), Value(200.0)},
        {Value(int64_t(2020)), Value(150.0)},
    };
    ColumnarTable tbl = CSVToColumnar::convert(rows, schema);

    // season=2025 AND speed>50: season chunk [2020,2020], 2025 > max → skip
    auto season_pred = makeColumnarBinary("=",   columnarColRef("season"), columnarLit(Value(int64_t(2025))));
    auto speed_pred  = makeColumnarBinary(">",   columnarColRef("speed"),  columnarLit(Value(50.0)));
    auto and_pred    = makeColumnarBinary("AND", std::move(season_pred),   std::move(speed_pred));

    SeqScanNode scan("test", std::move(tbl), schema, and_pred.get());
    EXPECT_EQ(columnarDrainAll(&scan).size(), 0u);
}
// ===== Explain must not print a stale runtime counter pre-execution (P3) =====

#include "execution/vec_scan_node.h"

TEST(ScanPruningExplain, PreExecutionShowsHintNotStaleCounter) {
    Schema schema = singleSeasonSchema();
    auto pred = makeColumnarBinary("=", columnarColRef("season"), columnarLit(Value(int64_t(2025))));

    // never executed: chunks_skipped=0/N would be a runtime value that never ran
    SeqScanNode scan("test", makeSingleChunkTable(), schema, pred.get());
    EXPECT_NE(scan.explain().find("pruning=on"), std::string::npos) << scan.explain();
    EXPECT_EQ(scan.explain().find("chunks_skipped"), std::string::npos) << scan.explain();

    VecScanNode vscan("test", makeSingleChunkTable(), schema, pred.get());
    EXPECT_NE(vscan.explain().find("pruning=on"), std::string::npos) << vscan.explain();
    EXPECT_EQ(vscan.explain().find("chunks_skipped"), std::string::npos) << vscan.explain();

    // after execution the counter is real (whole chunk provably skipped)
    vscan.open();
    while (vscan.nextChunk()) {}
    vscan.close();
    EXPECT_NE(vscan.explain().find("chunks_skipped=1/1"), std::string::npos) << vscan.explain();
}

// The pruner must keep ignoring slot >= 1 refs: under --no-optimize (and for
// residual filters) the FROM-side scan's pruning hint still contains join-side
// conjuncts, and shared column names make them unsafe to act on. Pushed
// join-side conjuncts are re-stamped to slot 0 before they reach a scan.
TEST(ChunkPrunerSlots, JoinSlotRefNeverPrunes) {
    Schema schema = singleSeasonSchema();
    ColumnarTable tbl = makeSingleChunkTable();  // zone map [2020,2020]

    auto ref = columnarColRef("season");
    ref->id = ColumnId::local(1);  // join-side ref reaching a FROM-side hint
    auto pred = makeColumnarBinary("=", std::move(ref), columnarLit(Value(int64_t(2025))));

    // the zone map would prove a skip, but the slot guard must win
    EXPECT_FALSE(ChunkPruner::shouldSkip(pred.get(), tbl.zone_maps, 0, schema));
}

// Week 30 round 3. `relation_slot < 1` is a test on a slot, and since Week 30 a
// slot is a position in the range table of the scope `query_level` blocks out.
// A CORRELATED ref carries (level 1, slot 0), so it read as scan-local here and
// was then matched against the scanned table's zone maps BY NAME — two numbering
// domains. With a shared column name (`team` and `driver_id` are shared on the
// shipped catalog) the wrong relation's zone maps prune the scan: chunks skipped
// silently, no error.
//
// It is NOT protected by the collectSlots/soleSlot `-1` containment that covers
// restampSlots: the vectorized builder hands the whole un-pushed WHERE to the
// FROM-side scan as a hint, so on `--no-optimize` a correlated conjunct arrives
// here without pushdown ever having seen it.
TEST(ChunkPrunerShouldSkip, ACorrelatedRefContributesNoPruningHint) {
    std::unordered_map<std::string, std::vector<ColumnChunk>> zone_maps;
    // this chunk holds only 2020..2023, so `season = 2025` would prove a skip
    zone_maps["season"] = {{0, 4, Value(int64_t(2020)), Value(int64_t(2023))}};

    // the control: a scan-local ref at (level 0, slot 0) still prunes
    auto local = columnarColRef("season");
    local->id = ColumnId::local(0);
    auto local_pred = makeColumnarBinary("=", std::move(local),
                                         columnarLit(Value(int64_t(2025))));
    EXPECT_TRUE(ChunkPruner::shouldSkip(local_pred.get(), zone_maps, 0, prunerSchema()));

    // the finding: the SAME slot one block out must contribute nothing. Declining
    // is correct-and-slower; acting on it prunes another relation's chunks.
    auto correlated = columnarColRef("season");
    correlated->id = ColumnId::outer(1, 0);
    auto corr_pred = makeColumnarBinary("=", std::move(correlated),
                                        columnarLit(Value(int64_t(2025))));
    EXPECT_FALSE(ChunkPruner::shouldSkip(corr_pred.get(), zone_maps, 0, prunerSchema()))
        << "a correlated ref's slot indexes an enclosing block's range table";

    // and a slot >= 1 stays ignored, as it has since Week 26
    auto join_side = columnarColRef("season");
    join_side->id = ColumnId::local(1);
    auto join_pred = makeColumnarBinary("=", std::move(join_side),
                                        columnarLit(Value(int64_t(2025))));
    EXPECT_FALSE(ChunkPruner::shouldSkip(join_pred.get(), zone_maps, 0, prunerSchema()));
}
