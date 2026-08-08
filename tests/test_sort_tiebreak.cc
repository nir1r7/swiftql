#include <gtest/gtest.h>

#include "planner/plan_nodes.h"
#include "execution/vec_sort_node.h"
#include "execution/vec_limit_node.h"
#include "execution/sort_comparator.h"
#include "parser/ast.h"
#include "common/schema.h"
#include "common/value.h"

#include <algorithm>
#include <memory>
#include <vector>

// THE DETERMINISTIC ORDER-BY TIE-BREAK (seam audit pass 2, E-1 / E-1b).
//
// The defect these tests pin: `std::stable_sort` propagates its INPUT order, and
// input order is a function of the PLAN, not of the query — a hash join emits
// probe-major so the build side reverses it, the two engines pick the build side
// by different rules, and `HashAggregateNode` emits groups in the join's output
// order. So `ORDER BY <non-total key> LIMIT n` returned a different ROW SET on
// Volcano than on the optimized vectorized path, and a scalar subquery of that
// shape turned the disagreement into an arithmetic one (977 vs 1536).
//
// The property that fixes it, and the only one worth testing, is PERMUTATION
// INVARIANCE: the sort's output must not depend on the order its input arrived
// in. Every plan-shape difference in the engine reaches the sort as exactly that
// — a permutation of the same rows. So a test that feeds one operator several
// permutations of one row multiset covers every build-side choice, every join
// order, and every group first-encounter order at once, without having to build
// any of them.
//
// DISCRIMINATION, MEASURED, NOT ASSERTED. This file was run against a build of
// this same tree with the tie-break loop removed from `rowLess` — byte-for-byte
// the semantics both engines had before this change. 9 of the 11 tests below
// FAIL there. The two that pass are marked GUARD: they assert the tie-break must
// NOT fire (identical rows stay tied; a declared key still wins), so passing
// pre-fix is what they are for, and neither is offered as evidence the fix
// works. Nine is the number that discriminates.

namespace {

Schema makeSchema(std::initializer_list<std::pair<std::string, TypeId>> cols) {
    std::vector<ColumnDef> defs;
    for (auto& [name, type] : cols) defs.push_back({name, type});
    return Schema(defs);
}

std::unique_ptr<ColumnRef> colRef(std::string name) {
    auto c = std::make_unique<ColumnRef>();
    c->column_name = std::move(name);
    return c;
}

std::vector<OrderByItem> orderBy(std::string col, bool desc = false) {
    std::vector<OrderByItem> items;
    items.push_back({colRef(std::move(col)), desc});
    return items;
}

std::vector<Row> drainVolcano(PlanNode* node) {
    node->open();
    std::vector<Row> out;
    while (Row* r = node->next()) out.push_back(*r);
    node->close();
    return out;
}

// A vectorized source that emits a fixed row multiset in a fixed order, in
// chunks of `chunk_rows`. The chunking is a parameter on purpose: chunk
// boundaries are one of the things the audit named as differing between the
// legs, so the tie-break must be insensitive to them too.
class VecSource : public VecPlanNode {
public:
    VecSource(Schema schema, std::vector<Row> rows, int chunk_rows)
        : schema_(std::move(schema)), rows_(std::move(rows)), chunk_rows_(chunk_rows) {}
    void open() override { cursor_ = 0; }
    DataChunk* nextChunk() override {
        if (cursor_ >= static_cast<int>(rows_.size())) return nullptr;
        int n = std::min(chunk_rows_, static_cast<int>(rows_.size()) - cursor_);
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
    std::string explain() const override { return "VecSource"; }
    std::vector<VecPlanNode*> children() const override { return {}; }

private:
    Schema schema_;
    std::vector<Row> rows_;
    int chunk_rows_;
    int cursor_ = 0;
    DataChunk chunk_;
};

std::vector<Row> drainVec(VecPlanNode& node) {
    node.open();
    std::vector<Row> out;
    while (DataChunk* chunk = node.nextChunk()) {
        int n = chunk->filter_applied ? static_cast<int>(chunk->sel.indices.size())
                                      : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            Row row;
            for (const auto& cv : chunk->columns) row.push_back(valueAt(cv, r));
            out.push_back(std::move(row));
        }
    }
    node.close();
    return out;
}

std::string render(const std::vector<Row>& rows) {
    std::string s;
    for (const Row& r : rows) {
        for (const Value& v : r) { s += v.toString(); s += '|'; }
        s += ';';
    }
    return s;
}

// (group, agg) exactly as `SELECT team, MIN(season) ... GROUP BY team ORDER BY
// MIN(season) LIMIT 3` hands them to the sort: every agg value equal, so the
// declared key is a seven-way tie and the LIMIT cut falls inside it. This is the
// audit's A1-repro reduced to the operator that decides it.
const Schema& tiedSchema() {
    static Schema s = makeSchema({{"team", TypeId::STRING}, {"m", TypeId::INT}});
    return s;
}

// The two orders the audit MEASURED coming out of the aggregate: laps-probe
// first-encounter order (Volcano and vec --no-optimize) and drivers-probe
// first-encounter order (optimized vec). Pre-fix these produced
// {AlphaTauri, Alpine, McLaren} and {RedBull, AlphaTauri, McLaren}.
std::vector<Row> permutedTo(const std::vector<std::string>& order) {
    std::vector<Row> rows;
    for (const std::string& t : order) rows.push_back({Value(t), Value(int64_t(2022))});
    return rows;
}

const std::vector<std::string> kLapsProbeOrder = {
    "AlphaTauri", "Alpine", "McLaren", "Ferrari", "Williams", "Mercedes", "RedBull"};
const std::vector<std::string> kDriversProbeOrder = {
    "RedBull", "AlphaTauri", "McLaren", "Williams", "Ferrari", "Mercedes", "Alpine"};

}  // namespace


// ---------------------------------------------------------------------------
// The comparator itself

TEST(SortTieBreak, WholeRowDecidesWhenEveryDeclaredKeyTies) {
    auto keys = orderBy("m");
    Row a = {Value(std::string("Alpine")), Value(int64_t(2022))};
    Row b = {Value(std::string("RedBull")), Value(int64_t(2022))};
    EXPECT_TRUE(sort_comparator::rowLess(keys, tiedSchema(), a, b));
    EXPECT_FALSE(sort_comparator::rowLess(keys, tiedSchema(), b, a));
}

TEST(SortTieBreak, IdenticalRowsStayTied) {
    // GUARD (passes pre-fix too, by design).
    // The tie-break is a total order on DISTINGUISHABLE rows, not on rows. Two
    // rows equal in every column may still tie — whichever survives a cut, the
    // answer is the same — and the comparator must report that, or it is not a
    // strict weak ordering and stable_sort is undefined.
    auto keys = orderBy("m");
    Row a = {Value(std::string("Alpine")), Value(int64_t(2022))};
    Row b = a;
    EXPECT_FALSE(sort_comparator::rowLess(keys, tiedSchema(), a, b));
    EXPECT_FALSE(sort_comparator::rowLess(keys, tiedSchema(), b, a));
}

TEST(SortTieBreak, RunsAscendingEvenUnderDesc) {
    // The tie-break is not a key the user wrote, so its direction is arbitrary.
    // It is fixed to ascending, and fixing it is the whole point — an arbitrary
    // direction that varied would be a second source of divergence.
    auto keys = orderBy("m", /*desc=*/true);
    Row a = {Value(std::string("Alpine")), Value(int64_t(2022))};
    Row b = {Value(std::string("RedBull")), Value(int64_t(2022))};
    EXPECT_TRUE(sort_comparator::rowLess(keys, tiedSchema(), a, b));
}

TEST(SortTieBreak, DeclaredKeysStillWinOverTheTieBreak) {
    // GUARD (passes pre-fix too, by design).
    // The tie-break must never reorder rows the ORDER BY already separates.
    auto keys = orderBy("m");
    Row a = {Value(std::string("Zzz")), Value(int64_t(2021))};
    Row b = {Value(std::string("Aaa")), Value(int64_t(2022))};
    EXPECT_TRUE(sort_comparator::rowLess(keys, tiedSchema(), a, b));
    EXPECT_FALSE(sort_comparator::rowLess(keys, tiedSchema(), b, a));
}

TEST(SortTieBreak, NullOrdersBeforeAnyValueInTheTieBreak) {
    Schema s = makeSchema({{"k", TypeId::STRING}, {"m", TypeId::INT}});
    auto keys = orderBy("m");
    Row a = {Value::null(), Value(int64_t(1))};
    Row b = {Value(std::string("")), Value(int64_t(1))};
    EXPECT_TRUE(sort_comparator::rowLess(keys, s, a, b));
    EXPECT_FALSE(sort_comparator::rowLess(keys, s, b, a));
}

TEST(SortTieBreak, MixedStringAndNumberDoesNotThrow) {
    // compareForSort refuses STRING against a number, which is right for a
    // user-written key (it is type-checked at plan time) and wrong for a
    // tie-break column, which nobody chose. A comparator that throws inside
    // stable_sort is worse than any ordering it could have produced.
    Schema s = makeSchema({{"k", TypeId::STRING}, {"m", TypeId::INT}});
    auto keys = orderBy("m");
    Row a = {Value(int64_t(5)), Value(int64_t(1))};
    Row b = {Value(std::string("five")), Value(int64_t(1))};
    EXPECT_NO_THROW({
        EXPECT_TRUE(sort_comparator::rowLess(keys, s, a, b));
        EXPECT_FALSE(sort_comparator::rowLess(keys, s, b, a));
    });
}


// ---------------------------------------------------------------------------
// The operator-level property, on both engines

TEST(SortTieBreak, VolcanoSortIsPermutationInvariant) {
    std::vector<Row> laps_order = permutedTo(kLapsProbeOrder);
    std::vector<Row> drivers_order = permutedTo(kDriversProbeOrder);

    SortNode a(std::make_unique<SeqScanNode>("t", laps_order, tiedSchema()), orderBy("m"));
    SortNode b(std::make_unique<SeqScanNode>("t", drivers_order, tiedSchema()), orderBy("m"));

    EXPECT_EQ(render(drainVolcano(&a)), render(drainVolcano(&b)));
}

TEST(SortTieBreak, VectorizedSortIsPermutationInvariantAndChunkInvariant) {
    // chunk sizes differ as well as row order: chunk boundaries are one of the
    // things the audit named as differing between the legs.
    auto a = std::make_unique<VecSource>(tiedSchema(), permutedTo(kLapsProbeOrder), 7);
    auto b = std::make_unique<VecSource>(tiedSchema(), permutedTo(kDriversProbeOrder), 2);
    VecSortNode sa(std::move(a), orderBy("m"));
    VecSortNode sb(std::move(b), orderBy("m"));

    EXPECT_EQ(render(drainVec(sa)), render(drainVec(sb)));
}

TEST(SortTieBreak, TheTwoENGINESAgreeOnTheSameInputMultiset) {
    // The cross-engine claim, stated as a test rather than as a comment: given
    // the same rows in DIFFERENT arrival orders — which is exactly what the two
    // engines' different build-side rules produce — Volcano's sort and the
    // vectorized sort emit the same sequence.
    SortNode volcano(
        std::make_unique<SeqScanNode>("t", permutedTo(kLapsProbeOrder), tiedSchema()),
        orderBy("m"));
    VecSortNode vec(
        std::make_unique<VecSource>(tiedSchema(), permutedTo(kDriversProbeOrder), 3),
        orderBy("m"));

    EXPECT_EQ(render(drainVolcano(&volcano)), render(drainVec(vec)));
}

TEST(SortTieBreak, LimitCutSurvivesTheSameRowsOnBothEngines) {
    // The end-to-end shape of A1-repro: a seven-way tie, a cut of 3. Pre-fix
    // this returned {AlphaTauri, Alpine, McLaren} against
    // {RedBull, AlphaTauri, McLaren} — different row SETS, which no amount of
    // sorting in the harness can repair.
    LimitNode volcano(
        std::make_unique<SortNode>(
            std::make_unique<SeqScanNode>("t", permutedTo(kDriversProbeOrder), tiedSchema()),
            orderBy("m")),
        3);
    VecLimitNode vec(
        std::make_unique<VecSortNode>(
            std::make_unique<VecSource>(tiedSchema(), permutedTo(kLapsProbeOrder), 4),
            orderBy("m")),
        3);

    std::vector<Row> vr = drainVolcano(&volcano);
    std::vector<Row> xr = drainVec(vec);

    ASSERT_EQ(vr.size(), 3u);
    ASSERT_EQ(xr.size(), 3u);
    EXPECT_EQ(render(vr), render(xr));
    // and the value it settles on, written down so a future change to the rule
    // is a visible edit rather than a silent one
    EXPECT_EQ(vr[0][0].asString(), "AlphaTauri");
    EXPECT_EQ(vr[1][0].asString(), "Alpine");
    EXPECT_EQ(vr[2][0].asString(), "Ferrari");
}

TEST(SortTieBreak, EveryPermutationOfATiedInputGivesTheSameCut) {
    // Not two orders — all 5040. Permutation invariance either holds over the
    // whole symmetric group or it is a coincidence about the two orders someone
    // happened to pick.
    std::vector<std::string> teams = kLapsProbeOrder;
    std::sort(teams.begin(), teams.end());
    std::string expected;
    int perms = 0;
    do {
        LimitNode node(
            std::make_unique<SortNode>(
                std::make_unique<SeqScanNode>("t", permutedTo(teams), tiedSchema()),
                orderBy("m")),
            3);
        std::string got = render(drainVolcano(&node));
        if (perms == 0) expected = got;
        ASSERT_EQ(got, expected) << "permutation " << perms << " disagreed";
        ++perms;
    } while (std::next_permutation(teams.begin(), teams.end()));
    EXPECT_EQ(perms, 5040);
}
