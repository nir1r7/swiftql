#include <gtest/gtest.h>

#include "planner/plan_nodes.h"
#include "execution/vec_sort_node.h"
#include "execution/vec_limit_node.h"
#include "execution/sort_comparator.h"
#include "parser/ast.h"
#include "parser/parser.h"
#include "planner/binder.h"
#include "planner/logical_plan.h"
#include "planner/predicate_pushdown.h"
#include "planner/join_enumeration.h"
#include "planner/cardinality_estimator.h"
#include "planner/vectorized_plan_builder.h"
#include "catalog/catalog.h"
#include "catalog/table_stats.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"
#include "common/schema.h"
#include "common/value.h"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
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


// ---------------------------------------------------------------------------
// SEAM AUDIT PASS 3 — the two things the rules above did NOT cover.
//
// Pass 3 reached one defect by three independent routes (engine E-8/E-9,
// join-chain B3-1, optimizer B3-1/B3-1b). Both halves are below, and both are
// discriminating, MEASURED and not asserted: this block was copied into a
// worktree at the parent commit (45522ca) and built there. 4 of the 5 tests
// FAIL; the 5th
// (TieBreakOrderIsTheSamePermutationOfIDENTITIESInEitherSchemaOrder) does not
// COMPILE there, because it names `sort_comparator::tieBreakOrder`, which is
// what the fix adds. The one GUARD test below
// (ADeterminedOrderIsNotDisturbedByTheCut) PASSES pre-fix by design -- it exists
// to stop the fix from degenerating into "sort everything", so passing pre-fix
// is what it is for. The tests above pin permutation invariance over ROWS; nothing
// pinned invariance over COLUMNS, and nothing at all pinned a cut with no sort
// beneath it.
//
//   (1) The tie-break was LEXICOGRAPHIC OVER THE ROW IN SCHEMA INDEX ORDER, and
//       `JoinEnumeration::rebuild` builds a merged join schema in the DP's
//       CHOSEN order — its own comment concedes that a slot-sorted canonical
//       order "is not available (invariant 1)". Same values, permuted sequence,
//       and a lexicographic order over a permuted tuple is a DIFFERENT TOTAL
//       ORDER. Both legs ran the tie-break, both were deterministic, and they
//       still cut differently.
//   (2) The tie-break lived in the sort comparator, so it never fired when
//       there was no sort. `LIMIT n` with no `ORDER BY` cut the top hash join's
//       raw probe order.
//
// !! WHY THESE NEED THREE RELATIONS, AND WHY THE ORACLE COULD NOT HAVE CAUGHT
// THEM. `JoinEnumeration` returns its input unchanged below
// MIN_ENUMERATED_RELATIONS = 3, and the oracle catalog has two tables — so not
// one of the 1496 oracle queries or 119 invariant checks has ever reached join
// enumeration at all. tests/data/test_catalog.json has three, and the shape
// below (a self-join on `drivers` through `team`) is the one the search leads
// with a NON-ZERO relation on, which is exactly when the merged schema's
// leading columns change.

namespace {

// The merged schema of `supplier s JOIN nation n`, as the two legs build it.
// Same five columns, same (relation_slot, name) pairs, different SEQUENCE —
// which is all `rebuild` ever changes.
Schema mergedWrittenOrder() {   // supplier@0 ++ nation@1  (the written order)
    return Schema({{"s_suppkey", TypeId::INT, 0},   {"s_nationkey", TypeId::INT, 0},
                   {"n_nationkey", TypeId::INT, 1}, {"n_name", TypeId::STRING, 1},
                   {"n_regionkey", TypeId::INT, 1}});
}
Schema mergedDpOrder() {        // nation@1 ++ supplier@0  (the DP leads with nation)
    return Schema({{"n_nationkey", TypeId::INT, 1}, {"n_name", TypeId::STRING, 1},
                   {"n_regionkey", TypeId::INT, 1}, {"s_suppkey", TypeId::INT, 0},
                   {"s_nationkey", TypeId::INT, 0}});
}

// One logical row, written once and materialized into whichever column order a
// schema asks for — so the test cannot accidentally compare two different row
// sets and call it a permutation.
struct Supplied { int64_t suppkey, nationkey, regionkey; std::string nation_name; };

Row materialize(const Supplied& s, const Schema& schema) {
    Row row;
    for (const ColumnDef& c : schema.columns()) {
        if (c.name == "s_suppkey")        row.push_back(Value(s.suppkey));
        else if (c.name == "s_nationkey") row.push_back(Value(s.nationkey));
        else if (c.name == "n_nationkey") row.push_back(Value(s.nationkey));
        else if (c.name == "n_name")      row.push_back(Value(s.nation_name));
        else                              row.push_back(Value(s.regionkey));
    }
    return row;
}

// Render BY NAME, never positionally: two rows in different column orders are
// the same row, and a positional render would report every permutation as a
// difference.
std::string renderByName(const std::vector<Row>& rows, const Schema& schema) {
    std::vector<int> by_name(static_cast<size_t>(schema.size()));
    for (int i = 0; i < schema.size(); ++i) by_name[static_cast<size_t>(i)] = i;
    std::sort(by_name.begin(), by_name.end(), [&](int a, int b) {
        return schema.column(a).name < schema.column(b).name;
    });
    std::string s;
    for (const Row& r : rows) {
        for (int i : by_name) { s += schema.column(i).name; s += '='; s += r[i].toString(); s += '|'; }
        s += ';';
    }
    return s;
}

// `n_regionkey` is 0 on every row, so `ORDER BY n_regionkey` ties on every pair
// and the tie-break decides the entire order — the same property the TPC-H
// repro rests on (`ORDER BY o_orderstatus` where every surviving row is 'F').
// The two schema orders lead with `s_suppkey` and with `n_nationkey`
// respectively, and those two lexicographic orders disagree.
const std::vector<Supplied>& tiedSuppliers() {
    static const std::vector<Supplied> v = {
        {5, 1, 0, "ALGERIA"}, {2, 3, 0, "CANADA"}, {9, 1, 0, "ALGERIA"}, {1, 3, 0, "CANADA"}};
    return v;
}

std::vector<Row> materializeAll(const Schema& schema) {
    std::vector<Row> rows;
    for (const Supplied& s : tiedSuppliers()) rows.push_back(materialize(s, schema));
    return rows;
}

}  // namespace

TEST(SortTieBreak, SchemaColumnOrderDoesNotChangeTheOrderTheTieBreakProduces) {
    // The property the header used to claim as "a function of the row's VALUES
    // alone", stated as the test that would have caught its falsehood: the same
    // rows, the same column IDENTITIES, a different column POSITION.
    Schema written = mergedWrittenOrder();
    Schema dp = mergedDpOrder();

    SortNode a(std::make_unique<SeqScanNode>("t", materializeAll(written), written),
               orderBy("n_regionkey"));
    SortNode b(std::make_unique<SeqScanNode>("t", materializeAll(dp), dp),
               orderBy("n_regionkey"));

    EXPECT_EQ(renderByName(drainVolcano(&a), written), renderByName(drainVolcano(&b), dp));
}

TEST(SortTieBreak, SchemaColumnOrderDoesNotChangeTheCut) {
    // The same, through a LIMIT — where an order difference becomes a row SET
    // difference and stops being something SQL leaves unspecified in a way the
    // project can live with.
    Schema written = mergedWrittenOrder();
    Schema dp = mergedDpOrder();

    LimitNode a(std::make_unique<SortNode>(
                    std::make_unique<SeqScanNode>("t", materializeAll(written), written),
                    orderBy("n_regionkey")), 2);
    VecLimitNode b(std::make_unique<VecSortNode>(
                       std::make_unique<VecSource>(dp, materializeAll(dp), 3),
                       orderBy("n_regionkey")), 2);

    std::vector<Row> ar = drainVolcano(&a);
    std::vector<Row> br = drainVec(b);
    ASSERT_EQ(ar.size(), 2u);
    ASSERT_EQ(br.size(), 2u);
    EXPECT_EQ(renderByName(ar, written), renderByName(br, dp));
    // and the rows it settles on, written down so a future change to the
    // canonical order is a visible edit rather than a silent one. Canonical
    // order is (relation_slot, name): s_nationkey, s_suppkey, n_name, ...
    EXPECT_EQ(renderByName(ar, written),
              renderByName({materialize({5, 1, 0, "ALGERIA"}, written),
                            materialize({9, 1, 0, "ALGERIA"}, written)}, written));
}

TEST(SortTieBreak, TieBreakOrderIsTheSamePermutationOfIDENTITIESInEitherSchemaOrder) {
    // The mechanism, on its own, so a failure above can be localized without a
    // debugger: the canonical order is a permutation of column IDENTITIES, and
    // it is the same sequence of identities whichever order the schema is in.
    Schema written = mergedWrittenOrder();
    Schema dp = mergedDpOrder();
    std::vector<std::string> a, b;
    for (int i : sort_comparator::tieBreakOrder(written)) a.push_back(written.column(i).name);
    for (int i : sort_comparator::tieBreakOrder(dp)) b.push_back(dp.column(i).name);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, (std::vector<std::string>{"s_nationkey", "s_suppkey",
                                           "n_name", "n_nationkey", "n_regionkey"}));
}


// ---------------------------------------------------------------------------
// END TO END, THROUGH THE REAL PIPELINE, ON THREE RELATIONS.
//
// The operator-level tests above feed hand-built rows to hand-built nodes. That
// is what makes them precise and it is also their limit: they cannot show that
// the PLANNER produces the two column orders, or that a `LIMIT` with no
// `ORDER BY` has a determined input at all. These run the same pipeline the CLI
// runs — Binder, LogicalPlanBuilder, PredicatePushdown, JoinEnumeration,
// CardinalityEstimator, VectorizedPlanBuilder — with and without the
// enumeration pass, which is the in-process form of `optimized` vs
// `--no-optimize`.
//
// Rows are compared IN EMISSION ORDER and NOT sorted. JoinEnumeration's own
// result-preservation test (`ReorderedPlansReturnTheWrittenOrdersRows`) sorts,
// deliberately — it asks whether the same rows come out. These ask which rows
// survive the cut, and sorting is exactly what would hide that.

namespace {

std::unordered_map<std::string, ColumnarTable> loadColumnarFor(const SelectStatement& stmt,
                                                               const Catalog& cat) {
    std::unordered_map<std::string, ColumnarTable> tables;
    auto add = [&](const std::string& name) {
        if (tables.count(name)) return;
        const auto& m = cat.getTable(name);
        tables.emplace(name, CSVToColumnar::convert(CSVLoader::load(m.filepath, m.schema), m.schema));
    };
    add(stmt.from.tableName("tie-break test loader"));
    for (const auto& j : stmt.joins) add(j.relation.tableName("tie-break test loader"));
    return tables;
}

// Stats exactly as the CLI computes them (main.cc: TableStats::compute over the
// loaded rows), so the join order these tests exercise is the one a user gets
// rather than one a fixture seeded.
void computeStatsFor(const SelectStatement& stmt, Catalog& cat) {
    auto add = [&](const std::string& name) {
        const auto& m = cat.getTable(name);
        cat.setStats(name, TableStats::compute(CSVLoader::load(m.filepath, m.schema), m.schema));
    };
    add(stmt.from.tableName("tie-break test stats"));
    for (const auto& j : stmt.joins) add(j.relation.tableName("tie-break test stats"));
}

// Run one SQL string through the whole pipeline. `enumerate` is the
// `--no-optimize` gate: the pass that permutes the merged join schema.
std::string runPipelineUnsorted(const std::string& sql, bool enumerate) {
    Catalog cat("../tests/data/test_catalog.json");
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    computeStatsFor(stmt, cat);
    auto tables = loadColumnarFor(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    if (enumerate) {
        logical = PredicatePushdown::apply(std::move(logical), cat);
        logical = JoinEnumeration::apply(std::move(logical), cat);
    }
    CardinalityEstimator::estimate(*logical, cat);
    auto plan = VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);

    plan->open();
    std::string out;
    while (DataChunk* chunk = plan->nextChunk()) {
        int n = chunk->filter_applied ? static_cast<int>(chunk->sel.indices.size())
                                      : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            for (const auto& cv : chunk->columns) {
                Value v = valueAt(cv, r);
                out += (v.isNull() ? "NULL" : v.toString());
                out += '|';
            }
            out += ';';
        }
    }
    plan->close();
    return out;
}

// Three relations, and the search leads with a NON-ZERO one: `drivers` joins
// both `laps` and the second `drivers`, so the DP puts it at the bottom of the
// spine and `rebuild` emits [drivers@1 ...] ++ [...] instead of [laps@0 ...] ++
// [...]. That is the exact precondition for the schema permutation — the audit
// measured that shapes which keep relation 0 leftmost agree even pre-fix.
const char* kThreeRelationSpine =
    "SELECT l.lap_id, d.name, d2.team FROM laps l "
    "JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team";

// A second three-relation spine, for the cut with no sort under it. The one
// above does NOT discriminate there — verified by running it against the
// pre-fix tree, where it passes — because its top join's probe order happens to
// coincide between the two orderings. This one is the shape that does: written
// leftmost is `drivers`, the DP leads with `laps`, so the top join probes with a
// different relation and the first three rows are a different SET. It is the
// same query `python_tools/test_new_queries.py` records in KNOWN_DIVERGENCES as
// `b31_open_plain_limit_written_leads_drivers`, on the fixture catalog.
const char* kNoSortSpine =
    "SELECT d.name, l.lap_id, l2.lap_id FROM drivers d "
    "JOIN laps l ON d.driver_id = l.driver_id "
    "JOIN laps l2 ON d.driver_id = l2.driver_id";

}  // namespace

TEST(SortTieBreak, ReorderedThreeRelationJoinCutsTheSameRowsUnderATiedOrderBy) {
    // `season` is 2022 on four of the five laps, so `ORDER BY l.season` ties
    // across the cut and the tie-break decides which three survive — the shape
    // of the audit's `ORDER BY n.n_regionkey LIMIT 5` on TPC-H, which returned
    // five ALGERIA suppliers optimized and ALGERIA/MOZAMBIQUE/MOROCCO/MOROCCO/
    // ALGERIA under --no-optimize.
    const std::string sql = std::string(kThreeRelationSpine) + " ORDER BY l.season LIMIT 3";
    EXPECT_EQ(runPipelineUnsorted(sql, /*enumerate=*/true),
              runPipelineUnsorted(sql, /*enumerate=*/false));
}

TEST(SortTieBreak, ReorderedThreeRelationJoinCutsTheSameRowsWithNoOrderByAtAll) {
    // No sort in this query at all, so no edit to sort_comparator.h could ever
    // reach it: the cut lands on the top hash join's raw probe order, and the DP
    // changes which relation probes. Closed by giving the cut a determined input
    // (deterministicCut, planner/logical_plan.cc).
    const std::string sql = std::string(kNoSortSpine) + " LIMIT 3";
    EXPECT_EQ(runPipelineUnsorted(sql, /*enumerate=*/true),
              runPipelineUnsorted(sql, /*enumerate=*/false));
    // pre-fix, the two legs returned (Driver_1,1,1) (Driver_1,1,3) (Driver_2,2,2)
    // and (Driver_1,1,1) (Driver_1,3,1) (Driver_2,2,2) -- a different row SET
    EXPECT_EQ(runPipelineUnsorted(sql, /*enumerate=*/true),
              "Driver_1|1|1|;Driver_1|1|3|;Driver_1|3|1|;");
}

TEST(SortTieBreak, ADeterminedOrderIsNotDisturbedByTheCut) {
    // GUARD. The deterministic cut must not fire where the order is already a
    // function of the query: a TOTAL ORDER BY must still decide, and a plain
    // scan's storage order must still be what a bare LIMIT returns. Both would
    // pass pre-fix, and neither is offered as evidence the fix works — they are
    // what stops the fix from being "sort everything".
    const std::string total = std::string(kThreeRelationSpine) + " ORDER BY l.lap_id LIMIT 3";
    EXPECT_EQ(runPipelineUnsorted(total, true), runPipelineUnsorted(total, false));
    EXPECT_EQ(runPipelineUnsorted(total, false),
              "1|Driver_1|Ferrari|;2|Driver_2|McLaren|;3|Driver_1|Ferrari|;");

    EXPECT_EQ(runPipelineUnsorted("SELECT lap_id, team FROM laps LIMIT 3", false),
              "1|Ferrari|;2|McLaren|;3|Ferrari|;");
}
