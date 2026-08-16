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

// A chunk source that can express NULL and REUSES ONE CHUNK BUFFER, which
// makeScan's ColumnarTable cannot do either of. Both properties are load-bearing
// for the tests below:
//
//   * ColumnarTable has no validity mask, so a scan can never produce a NULL —
//     and "a NULL key matches nothing, on either side" is a rule of this operator
//     that no scan-fed test can reach.
//   * the buffer is CLEARED AND REBUILT on every nextChunk(), which is exactly
//     what vec_types.h says a child may do. Since Week 40 the join reads probe
//     cells at FILL time from a chunk it only holds a pointer to, so a source
//     that quietly kept its old columns alive would hide a stale-pointer bug.
//
// Same shape as the VecRowSource in test_int_double_materialization.cc.
class NullableRowSource : public VecPlanNode {
public:
    NullableRowSource(Schema schema, std::vector<Row> rows, int chunk_rows = 1024)
        : schema_(std::move(schema)), rows_(std::move(rows)), chunk_rows_(chunk_rows) {}
    void open() override { cursor_ = 0; }
    DataChunk* nextChunk() override {
        if (cursor_ >= static_cast<int>(rows_.size())) return nullptr;
        const int n = std::min(chunk_rows_, static_cast<int>(rows_.size()) - cursor_);
        chunk_.columns.clear();
        chunk_.num_rows = n;
        chunk_.filter_applied = false;
        chunk_.sel.indices.clear();
        chunk_.sel.size = 0;
        for (int c = 0; c < schema_.size(); ++c) {
            ColumnVector cv = makeColumnVector(schema_.column(c).type);
            for (int i = 0; i < n; ++i) appendColumnValue(cv, rows_[cursor_ + i][c]);
            chunk_.columns.push_back(std::move(cv));
        }
        cursor_ += n;
        return &chunk_;
    }
    void close() override {}
    const Schema& outputSchema() const override { return schema_; }
    std::string explain() const override { return "NullableRowSource"; }
    std::vector<VecPlanNode*> children() const override { return {}; }

private:
    Schema schema_;
    std::vector<Row> rows_;
    int chunk_rows_;
    int cursor_ = 0;
    DataChunk chunk_;
};

// drainRows() reads cells straight out of the typed vectors, so it cannot see a
// NULL. This one consults the validity mask, which is the only way to tell a
// genuine NULL from the placeholder underneath it.
static std::vector<std::string> drainText(VecPlanNode& node) {
    node.open();
    std::vector<std::string> out;
    while (DataChunk* chunk = node.nextChunk()) {
        const int n = chunk->filter_applied
            ? static_cast<int>(chunk->sel.indices.size()) : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            const int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            std::string s;
            for (const auto& cv : chunk->columns) {
                s += (cv.isNull(r) ? std::string("<null>") : valueAt(cv, r).toString()) + "|";
            }
            out.push_back(std::move(s));
        }
    }
    node.close();
    return out;
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
        {Value(int64_t(1)), Value(std::string("p1"))},
        {Value(int64_t(2)), Value(std::string("p2"))},
        {Value(int64_t(3)), Value(std::string("p3"))},
        {Value(int64_t(9)), Value(std::string("p9"))},   // no match
    };
    std::vector<Row> build_rows = {
        {Value(int64_t(3)), Value(std::string("b3"))},
        {Value(int64_t(1)), Value(std::string("b1"))},
        {Value(int64_t(2)), Value(std::string("b2"))},
    };

    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             probe_schema.indexOf("pid"), build_schema.indexOf("bid"), out_schema, /*swapped=*/false, /*use_simd=*/false);
    VecHashJoinNode hash(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                         std::vector<int>{probe_schema.indexOf("pid")}, std::vector<int>{build_schema.indexOf("bid")},
                         out_schema);

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
    std::vector<Row> probe_rows = {{Value(int64_t(7)), Value(std::string("join_side"))}};
    std::vector<Row> build_rows = {{Value(int64_t(7)), Value(std::string("from_side"))}};

    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             probe_schema.indexOf("jid"), build_schema.indexOf("fid"), out_schema, /*swapped=*/true, /*use_simd=*/false);
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
    std::vector<Row> probe_rows = {{Value(int64_t(1))}};
    std::vector<Row> build_rows = {
        {Value(int64_t(1)), Value(std::string("x"))},
        {Value(int64_t(1)), Value(std::string("y"))},
        {Value(int64_t(1)), Value(std::string("z"))},
    };
    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             probe_schema.indexOf("pid"), build_schema.indexOf("bid"), out_schema, false, /*use_simd=*/false);
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
    std::vector<Row> probe_rows = {{Value(int64_t(1))}, {Value(int64_t(2))}};
    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, {}),
                             probe_schema.indexOf("pid"), build_schema.indexOf("bid"), out_schema, false, /*use_simd=*/true);
    EXPECT_TRUE(drainRows(loop).empty());
}

TEST(VecSimdLoopJoin, EmptyProbeSide) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    std::vector<Row> build_rows = {{Value(int64_t(1))}};
    VecSimdLoopJoinNode loop(makeScan(probe_schema, {}), makeScan(build_schema, build_rows),
                             probe_schema.indexOf("pid"), build_schema.indexOf("bid"), out_schema, false, /*use_simd=*/true);
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
    std::vector<Row> probe_rows = {{Value(int64_t(1))}, {Value(int64_t(2))}};
    std::vector<Row> build_rows = {
        {Value(int64_t(1)), Value(int64_t(10))},   // bval <= 15: filtered out — must never join
        {Value(int64_t(1)), Value(int64_t(20))},
        {Value(int64_t(2)), Value(int64_t(30))},
    };
    auto filtered_build = std::make_unique<VecFilterNode>(
        makeScan(build_schema, build_rows), binOp(">", col("bval"), intLit(15)));

    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), std::move(filtered_build),
                             probe_schema.indexOf("pid"), build_schema.indexOf("bid"), out_schema, false, /*use_simd=*/true);
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
        {Value(int64_t(1)), Value(int64_t(10))},   // pval <= 15: filtered out — must never probe
        {Value(int64_t(1)), Value(int64_t(20))},
        {Value(int64_t(2)), Value(int64_t(30))},
    };
    std::vector<Row> build_rows = {{Value(int64_t(1))}, {Value(int64_t(2))}};
    auto filtered_probe = std::make_unique<VecFilterNode>(
        makeScan(probe_schema, probe_rows), binOp(">", col("pval"), intLit(15)));

    VecSimdLoopJoinNode loop(std::move(filtered_probe), makeScan(build_schema, build_rows),
                             probe_schema.indexOf("pid"), build_schema.indexOf("bid"), out_schema, false, /*use_simd=*/true);
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
                                 probe_schema.indexOf("pkey"), build_schema.indexOf("bkey"), out_schema, false, /*use_simd=*/true);
        VecSimdLoopJoinNode scalar(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                                   probe_schema.indexOf("pkey"), build_schema.indexOf("bkey"), out_schema, false, /*use_simd=*/false);
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
                             probe_schema.indexOf("pkey"), build_schema.indexOf("bkey"), out_schema, false, /*use_simd=*/true);
    VecHashJoinNode hash(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                         std::vector<int>{probe_schema.indexOf("pkey")}, std::vector<int>{build_schema.indexOf("bkey")},
                         out_schema);
    EXPECT_EQ(serialize(drainRows(simd), true), serialize(drainRows(hash), true));
}

// ============================================================
// Week 40 late materialization — the three properties the rewrite could break
// ============================================================
//
// nextChunk() no longer assembles a Row per output row: it appends the pair
// (probe row id, build row id) and fillOutChunk gathers the cells one output
// COLUMN at a time, from the probe chunk and build_rows_. Each test below pins
// one thing that was previously true by construction and is now true only because
// of a specific line.

// THE SWAPPED REORDERING, with the two sides at DIFFERENT WIDTHS and more than
// one row. `swapped_` used to be read per output row while appending Values; it
// is now two base offsets computed once in open(), and an off-by-one there is
// invisible when both sides are the same width or when there is only one row.
// Row-for-row against the hash join, whose own bases are computed independently.
TEST(VecSimdLoopJoin, SwappedAsymmetricWidthsMatchHashJoinRowForRow) {
    // probe = the JOIN table (2 cols), build = the FROM table (3 cols)
    Schema probe_schema = vecSchema({{"jid", TypeId::INT}, {"jname", TypeId::STRING}});
    Schema build_schema = vecSchema({{"fid", TypeId::INT}, {"fname", TypeId::STRING},
                                      {"fage", TypeId::INT}});
    Schema out_schema   = vecSchema({{"fid", TypeId::INT}, {"fname", TypeId::STRING},
                                      {"fage", TypeId::INT},
                                      {"jid", TypeId::INT}, {"jname", TypeId::STRING}});
    std::vector<Row> probe_rows = {
        {Value(int64_t(2)), Value(std::string("j2"))},
        {Value(int64_t(1)), Value(std::string("j1"))},
        {Value(int64_t(1)), Value(std::string("j1b"))},
        {Value(int64_t(8)), Value(std::string("j8"))},   // no match
    };
    std::vector<Row> build_rows = {
        {Value(int64_t(1)), Value(std::string("f1")),  Value(int64_t(31))},
        {Value(int64_t(2)), Value(std::string("f2")),  Value(int64_t(32))},
        {Value(int64_t(1)), Value(std::string("f1b")), Value(int64_t(33))},
    };

    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             probe_schema.indexOf("jid"), build_schema.indexOf("fid"),
                             out_schema, /*swapped=*/true, /*use_simd=*/true);
    VecHashJoinNode hash(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                         std::vector<int>{probe_schema.indexOf("jid")},
                         std::vector<int>{build_schema.indexOf("fid")},
                         out_schema, /*swapped=*/true);
    const std::vector<Row> got = drainRows(loop);
    EXPECT_EQ(serialize(got, false), serialize(drainRows(hash), false));

    // Spelled out too, so a change to BOTH operators cannot pass by agreeing.
    // Probe rows in probe order; within one probe row, build rows in ascending
    // build index — jid=1 matches build rows 0 (f1) and 2 (f1b), so it emits two.
    EXPECT_EQ(serialize(got, false), (std::vector<std::string>{
        "2|f2|32|2|j2|",      // FROM (build) block leads, JOIN (probe) block follows
        "1|f1|31|1|j1|",
        "1|f1b|33|1|j1|",
        "1|f1|31|1|j1b|",
        "1|f1b|33|1|j1b|",
    }));
}

// THE LIFETIME RULE. Cells are read at FILL time from a chunk the join only holds
// a POINTER to, and vec_types.h says the child reuses that buffer on its next
// call — so the pending list must be fully drained before the probe child is
// pulled again. One probe chunk here yields 3072 output rows, three full
// BATCH_SIZE slices, against a source that clears and rebuilds its columns every
// call; if the pointer went stale the later slices would read freed strings.
//
// The payload strings vary in LENGTH on purpose: the output columns are now
// resized rather than rebuilt between slices, so a string cell carries the
// previous slice's buffer into the next one and a short value written over a long
// one must not leave a tail behind.
TEST(VecSimdLoopJoin, PendingOutputDrainsBeforeProbeChunkIsRefilled) {
    Schema probe_schema = vecSchema({{"pkey", TypeId::INT}, {"ptag", TypeId::STRING}});
    Schema build_schema = vecSchema({{"bkey", TypeId::INT}, {"btag", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"pkey", TypeId::INT}, {"ptag", TypeId::STRING},
                                      {"bkey", TypeId::INT}, {"btag", TypeId::STRING}});

    std::vector<Row> probe_rows;
    for (int i = 0; i < 2048; ++i) {   // 2 chunks of 1024
        probe_rows.push_back({Value(int64_t(i % 5)),
                              Value(std::string(1 + (i % 40), 'a' + (i % 26)))});
    }
    std::vector<Row> build_rows;
    for (int i = 0; i < 15; ++i) {     // 3 build rows per probe key
        build_rows.push_back({Value(int64_t(i % 5)),
                              Value(std::string(1 + (i % 30), 'A' + (i % 26)))});
    }

    // The expected sequence, computed independently of the operator.
    std::vector<std::string> expected;
    for (const Row& p : probe_rows) {
        for (const Row& b : build_rows) {
            if (b[0].asInt() != p[0].asInt()) continue;
            expected.push_back(p[0].toString() + "|" + p[1].asString() + "|"
                             + b[0].toString() + "|" + b[1].asString() + "|");
        }
    }
    ASSERT_EQ(expected.size(), 2048u * 3u);

    VecSimdLoopJoinNode loop(
        std::make_unique<NullableRowSource>(probe_schema, probe_rows, 1024),
        std::make_unique<NullableRowSource>(build_schema, build_rows),
        probe_schema.indexOf("pkey"), build_schema.indexOf("bkey"),
        out_schema, /*swapped=*/false, /*use_simd=*/true);
    EXPECT_EQ(drainText(loop), expected);
}

// A NULL KEY MATCHES NOTHING, on either side. Build-side NULLs are skipped in
// open() so they never enter the flat SIMD key buffer (where any sentinel would
// be a legal key); probe-side NULLs are skipped in the probe loop. Neither rule
// is reachable through a scan — ColumnarTable has no validity mask — so this is
// the only test that exercises them, and it checks the non-key columns' NULLs
// survive the gather too.
TEST(VecSimdLoopJoin, NullKeysMatchNothingOnEitherSide) {
    Schema probe_schema = vecSchema({{"pkey", TypeId::INT}, {"pval", TypeId::STRING}});
    Schema build_schema = vecSchema({{"bkey", TypeId::INT}, {"bval", TypeId::STRING}});
    Schema out_schema   = vecSchema({{"pkey", TypeId::INT}, {"pval", TypeId::STRING},
                                      {"bkey", TypeId::INT}, {"bval", TypeId::STRING}});
    std::vector<Row> probe_rows = {
        {Value(int64_t(1)), Value(std::string("p1"))},
        {Value::null(),     Value(std::string("pnull"))},   // NULL key: matches nothing
        {Value(int64_t(2)), Value::null()},                 // NULL payload: must survive
    };
    std::vector<Row> build_rows = {
        {Value(int64_t(1)), Value(std::string("b1"))},
        {Value::null(),     Value(std::string("bnull"))},    // NULL key: never joins
        {Value(int64_t(2)), Value::null()},                  // NULL payload: must survive
    };

    for (bool simd : {false, true}) {
        VecSimdLoopJoinNode loop(
            std::make_unique<NullableRowSource>(probe_schema, probe_rows),
            std::make_unique<NullableRowSource>(build_schema, build_rows),
            probe_schema.indexOf("pkey"), build_schema.indexOf("bkey"),
            out_schema, /*swapped=*/false, simd);
        EXPECT_EQ(drainText(loop),
                  (std::vector<std::string>{"1|p1|1|b1|", "2|<null>|2|<null>|"}))
            << "use_simd=" << simd;
    }

    // And the same answer from the hash join, which reaches NULL by its own route.
    VecHashJoinNode hash(std::make_unique<NullableRowSource>(probe_schema, probe_rows),
                         std::make_unique<NullableRowSource>(build_schema, build_rows),
                         std::vector<int>{probe_schema.indexOf("pkey")},
                         std::vector<int>{build_schema.indexOf("bkey")}, out_schema);
    EXPECT_EQ(drainText(hash),
              (std::vector<std::string>{"1|p1|1|b1|", "2|<null>|2|<null>|"}));
}

// ============================================================
// Explain + stats
// ============================================================

TEST(VecSimdLoopJoin, ExplainNamesOperatorAndMaterializes) {
    Schema s = vecSchema({{"pid", TypeId::INT}});
    Schema b = vecSchema({{"bid", TypeId::INT}});
    Schema o = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    VecSimdLoopJoinNode loop(makeScan(s, {}), makeScan(b, {}),
                             s.indexOf("pid"), b.indexOf("bid"), o);
    std::string e = loop.explain();
    EXPECT_EQ(e.rfind("VecSimdLoopJoin", 0), 0u) << e;
    EXPECT_NE(e.find("pid = bid"), std::string::npos) << e;
    EXPECT_NE(e.find("(materialize)"), std::string::npos) << e;
}

TEST(VecSimdLoopJoin, StatsCountProbeRowsInAndJoinedRowsOut) {
    Schema probe_schema = vecSchema({{"pid", TypeId::INT}});
    Schema build_schema = vecSchema({{"bid", TypeId::INT}});
    Schema out_schema   = vecSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});
    std::vector<Row> probe_rows = {{Value(int64_t(1))}, {Value(int64_t(2))}, {Value(int64_t(9))}};
    std::vector<Row> build_rows = {{Value(int64_t(1))}, {Value(int64_t(2))}};
    VecSimdLoopJoinNode loop(makeScan(probe_schema, probe_rows), makeScan(build_schema, build_rows),
                             probe_schema.indexOf("pid"), build_schema.indexOf("bid"), out_schema);
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
                             probe_schema.indexOf("pid"), build_schema.indexOf("bid"), out_schema, false, /*use_simd=*/true);
    loop.open();
    while (loop.nextChunk()) {}
    loop.close();
    EXPECT_GT(loop.stats.elapsed_us, 0.0);
}
