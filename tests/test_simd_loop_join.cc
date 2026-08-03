#include <gtest/gtest.h>
#include "execution/vec_simd_loop_join_node.h"
#include "execution/vec_hash_join_node.h"
#include "execution/vec_filter_node.h"
#include "execution/vec_scan_node.h"
#include "execution/vec_types.h"
#include "storage/columnar_table.h"
#include "common/schema.h"
#include "common/value.h"
#include "parser/ast.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

// ============================================================
// Helpers (same shapes as test_vectorized.cc)
// ============================================================

static Schema vecSchema(std::initializer_list<std::pair<std::string, TypeId>> cols) {
    std::vector<ColumnDef> defs;
    for (auto& [name, type] : cols) defs.push_back({name, type});
    return Schema(defs);
}

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

// Drain a VecPlanNode fully, collect all materialized rows.
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

// Serialize rows for comparison; sorted = order-insensitive (multiset equality).
static std::vector<std::string> serialize(const std::vector<Row>& rows, bool sorted) {
    std::vector<std::string> out;
    for (const auto& row : rows) {
        std::string s;
        for (const auto& v : row) s += v.toString() + "|";
        out.push_back(std::move(s));
    }
    if (sorted) std::sort(out.begin(), out.end());
    return out;
}

// AST helpers for the VecFilter tests
static std::unique_ptr<Expr> col(const std::string& name) {
    auto r = std::make_unique<ColumnRef>();
    r->column_name = name;
    return r;
}

static std::unique_ptr<Expr> intLit(int64_t v) {
    return std::make_unique<Literal>(Value(v));
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

// Two-INT-column tables for the parametric SIMD-vs-scalar tests: build rows
// (key = i % 7, payload = i), probe rows (key = i % 7, payload = 100 + i).
static std::vector<Row> intKeyRows(int n, int64_t payload_base) {
    std::vector<Row> rows;
    rows.reserve(n);
    for (int i = 0; i < n; ++i)
        rows.push_back({Value(static_cast<int64_t>(i % 7)), Value(payload_base + i)});
    return rows;
}

// ============================================================
// Scalar reference path: equivalence with VecHashJoinNode
// ============================================================

// Both operators implement the same inner equi-join, and both emit probe rows
// in probe order with build matches in build insertion order — so the outputs
// must match row-for-row, not just as multisets.
TEST(VecSimdLoopJoin, ScalarMatchesHashJoinBasic) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}, {"pval", TypeId::STRING}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}, {"bval", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"pval", TypeId::STRING},
                                      {"bid", TypeId::INT}, {"bval", TypeId::STRING}});
    std::vector<Row> probe_rows = {
        {Value(1LL), Value(std::string("p1"))},
        {Value(2LL), Value(std::string("p2"))},
        {Value(3LL), Value(std::string("p3"))},
        {Value(9LL), Value(std::string("p9"))},   // no match
    };
    std::vector<Row> build_rows = {
        {Value(3LL), Value(std::string("b3"))},
        {Value(1LL), Value(std::string("b1"))},
        {Value(2LL), Value(std::string("b2"))},
    };

    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             "pid", "bid", out_schema, /*swapped=*/false, /*use_simd=*/false);
    VecHashJoinNode hash(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                         "pid", "bid", out_schema);

    EXPECT_EQ(serialize(drainRows(loop), false), serialize(drainRows(hash), false));
}

// swapped = true: FROM table is physically the build side, but output rows
// must still lead with the FROM (build) columns — same contract the hash
// join's SwappedEmitsFromSideFirst test pins.
TEST(VecSimdLoopJoin, SwappedEmitsFromSideFirst) {
    Schema probe_schema = vecSchema({{"jid", TypeId::INT}, {"jname", TypeId::STRING}});
    Schema build_schema = vecSchema({{"fid", TypeId::INT}, {"fname", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"fid", TypeId::INT}, {"fname", TypeId::STRING},
                                      {"jid", TypeId::INT}, {"jname", TypeId::STRING}});
    std::vector<Row> probe_rows = {{Value(7LL), Value(std::string("join_side"))}};
    std::vector<Row> build_rows = {{Value(7LL), Value(std::string("from_side"))}};

    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             "jid", "fid", out_schema, /*swapped=*/true, /*use_simd=*/false);
    auto rows = drainRows(loop);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0][0].asInt(), 7);
    EXPECT_EQ(rows[0][1].asString(), "from_side");   // FROM (build) columns first
    EXPECT_EQ(rows[0][3].asString(), "join_side");
}

// Duplicate build keys must each emit one output row per probe match.
TEST(VecSimdLoopJoin, DuplicateBuildKeys) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}, {"bval", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT},
                                      {"bid", TypeId::INT}, {"bval", TypeId::STRING}});
    std::vector<Row> probe_rows = {{Value(1LL)}};
    std::vector<Row> build_rows = {
        {Value(1LL), Value(std::string("x"))},
        {Value(1LL), Value(std::string("y"))},
        {Value(1LL), Value(std::string("z"))},
    };
    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             "pid", "bid", out_schema, false, /*use_simd=*/false);
    auto rows = drainRows(loop);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0][2].asString(), "x");   // build insertion order preserved
    EXPECT_EQ(rows[1][2].asString(), "y");
    EXPECT_EQ(rows[2][2].asString(), "z");
}

TEST(VecSimdLoopJoin, EmptyBuildSide) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    std::vector<Row> probe_rows = {{Value(1LL)}, {Value(2LL)}};
    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, {}),
                             "pid", "bid", out_schema, false, /*use_simd=*/true);
    EXPECT_TRUE(drainRows(loop).empty());
}

TEST(VecSimdLoopJoin, EmptyProbeSide) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    std::vector<Row> build_rows = {{Value(1LL)}};
    VecSimdLoopJoinNode loop(makeScan(probe_schema, {}), makeScan(build_schema, build_rows),
                             "pid", "bid", out_schema, false, /*use_simd=*/true);
    loop.open();
    EXPECT_EQ(loop.nextChunk(), nullptr);
    loop.close();
}

// ============================================================
// SelectionVector handling — the build/probe child may be a VecFilter
// (post-pushdown), whose sel.indices is authoritative (vec_types.h)
// ============================================================

TEST(VecSimdLoopJoin, FilteredBuildChildRespected) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}, {"bval", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT},
                                      {"bid", TypeId::INT}, {"bval", TypeId::INT}});
    std::vector<Row> probe_rows = {{Value(1LL)}, {Value(2LL)}};
    std::vector<Row> build_rows = {
        {Value(1LL), Value(10LL)},   // bval <= 15: filtered out — must never join
        {Value(1LL), Value(20LL)},
        {Value(2LL), Value(30LL)},
    };
    auto filtered_build = std::make_unique<VecFilterNode>(
        makeScan(build_schema, build_rows), binOp(">", col("bval"), intLit(15)));

    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), std::move(filtered_build),
                             "pid", "bid", out_schema, false, /*use_simd=*/true);
    auto rows = drainRows(loop);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][2].asInt(), 20);
    EXPECT_EQ(rows[1][2].asInt(), 30);
}

TEST(VecSimdLoopJoin, FilteredProbeChildRespected) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}, {"pval", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"pval", TypeId::INT},
                                      {"bid", TypeId::INT}});
    std::vector<Row> probe_rows = {
        {Value(1LL), Value(10LL)},   // pval <= 15: filtered out — must never probe
        {Value(1LL), Value(20LL)},
        {Value(2LL), Value(30LL)},
    };
    std::vector<Row> build_rows = {{Value(1LL)}, {Value(2LL)}};
    auto filtered_probe = std::make_unique<VecFilterNode>(
        makeScan(probe_schema, probe_rows), binOp(">", col("pval"), intLit(15)));

    VecSimdLoopJoinNode loop(std::move(filtered_probe), makeScan(build_schema, build_rows),
                             "pid", "bid", out_schema, false, /*use_simd=*/true);
    auto rows = drainRows(loop);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0][1].asInt(), 20);
    EXPECT_EQ(rows[1][1].asInt(), 30);
}

// ============================================================
// SIMD kernel == scalar reference, across vector-width boundaries
// ============================================================

// Build sizes straddle the NEON 2-lane (and AVX2 4-lane) boundaries plus
// BATCH_SIZE, pinning the scalar-tail handling; keys repeat (i % 7) so
// duplicate matches are exercised at every size. Exact row-order equality.
TEST(VecSimdLoopJoin, SimdMatchesScalarAcrossTailSizes) {
    Schema probe_schema = vecSchema({{"pkey", TypeId::INT}, {"pval", TypeId::INT}});
    Schema build_schema = vecSchema({{"bkey", TypeId::INT}, {"bval", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pkey", TypeId::INT}, {"pval", TypeId::INT},
                                      {"bkey", TypeId::INT}, {"bval", TypeId::INT}});
    std::vector<Row> probe_rows = intKeyRows(10, 100);

    for (int n : {0, 1, 2, 3, 5, 8, 1023, 1024, 1025}) {
        std::vector<Row> build_rows = intKeyRows(n, 1000);

        VecSimdLoopJoinNode simd(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                                 "pkey", "bkey", out_schema, false, /*use_simd=*/true);
        VecSimdLoopJoinNode scalar(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                                   "pkey", "bkey", out_schema, false, /*use_simd=*/false);
        EXPECT_EQ(serialize(drainRows(simd), false), serialize(drainRows(scalar), false))
            << "build size " << n;
    }
}

// Multi-chunk on both sides, output larger than one BATCH_SIZE slice —
// multiset equality against the hash join on the same inputs.
TEST(VecSimdLoopJoin, SimdMatchesHashJoinMultiChunk) {
    Schema probe_schema = vecSchema({{"pkey", TypeId::INT}, {"pval", TypeId::INT}});
    Schema build_schema = vecSchema({{"bkey", TypeId::INT}, {"bval", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pkey", TypeId::INT}, {"pval", TypeId::INT},
                                      {"bkey", TypeId::INT}, {"bval", TypeId::INT}});
    std::vector<Row> probe_rows = intKeyRows(2500, 100000);  // 3 chunks
    std::vector<Row> build_rows = intKeyRows(15, 1000);      // ~2 matches per probe row

    VecSimdLoopJoinNode simd(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             "pkey", "bkey", out_schema, false, /*use_simd=*/true);
    VecHashJoinNode hash(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                         "pkey", "bkey", out_schema);
    EXPECT_EQ(serialize(drainRows(simd), true), serialize(drainRows(hash), true));
}

// ============================================================
// Explain + stats
// ============================================================

TEST(VecSimdLoopJoin, ExplainNamesOperatorAndMaterializes) {
    Schema s = vecSchema({{"pid", TypeId::INT}});
    Schema b = vecSchema({{"bid", TypeId::INT}});
    Schema o = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    VecSimdLoopJoinNode loop(makeScan(s, {}), makeScan(b, {}), "pid", "bid", o);
    std::string e = loop.explain();
    EXPECT_EQ(e.rfind("VecSimdLoopJoin", 0), 0u) << e;
    EXPECT_NE(e.find("pid = bid"), std::string::npos) << e;
    EXPECT_NE(e.find("(materialize)"), std::string::npos) << e;
}

TEST(VecSimdLoopJoin, StatsCountProbeRowsInAndJoinedRowsOut) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    std::vector<Row> probe_rows = {{Value(1LL)}, {Value(2LL)}, {Value(9LL)}};
    std::vector<Row> build_rows = {{Value(1LL)}, {Value(2LL)}};
    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             "pid", "bid", out_schema);
    drainRows(loop);
    EXPECT_EQ(loop.stats.rows_in, 3);
    EXPECT_EQ(loop.stats.rows_out, 2);
}

// The build phase runs in open() and must be timed (audit M6): with a zero-row
// probe, elapsed time can only come from consuming the build side.
TEST(VecSimdLoopJoin, BuildPhaseIsTimed) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema(std::vector<ColumnDef>{{"pid", TypeId::INT, 0}, {"bid", TypeId::INT, 1}});
    std::vector<Row> build_rows;
    build_rows.reserve(5000);
    for (int i = 0; i < 5000; ++i) build_rows.push_back({Value(int64_t(i))});

    VecSimdLoopJoinNode loop(makeScan(probe_schema, {}), makeScan(build_schema, build_rows),
                             "pid", "bid", out_schema, false, /*use_simd=*/true);
    loop.open();
    while (loop.nextChunk()) {}
    loop.close();
    EXPECT_GT(loop.stats.elapsed_us, 0.0);
}
