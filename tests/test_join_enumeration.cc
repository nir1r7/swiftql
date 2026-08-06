#include <gtest/gtest.h>
#include "planner/join_enumeration.h"
#include "planner/cardinality_estimator.h"
#include "planner/predicate_pushdown.h"
#include "planner/vectorized_plan_builder.h"
#include "planner/logical_plan.h"
#include "planner/binder.h"
#include "parser/parser.h"
#include "catalog/catalog.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"
#include "common/value.h"
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Tests run from build/, so the catalog is one level up.
static const char* CATALOG = "../tests/data/test_catalog.json";

// ── fixtures ────────────────────────────────────────────────────────────────

// Deterministic stats so the ordering assertions below are exact arithmetic
// rather than fixture-coupled. laps: 1000 rows, driver_id NDV 20, lap_id NDV
// 1000 (a key). drivers: 20 rows, driver_id NDV 20 (a primary key). Widths are
// seeded too — the data-volume term is a Week 28 cost input and an unseeded
// avg_width of 0 would silently switch it off.
static void seedStats(Catalog& cat) {
    TableStats laps;
    laps.row_count = 1000;
    ColumnStats driver_id;
    driver_id.min_val = Value(int64_t(1));
    driver_id.max_val = Value(int64_t(20));
    driver_id.distinct_count = 20;
    driver_id.avg_width = 8.0;
    laps.columns.emplace("driver_id", driver_id);
    ColumnStats lap_id;
    lap_id.min_val = Value(int64_t(1));
    lap_id.max_val = Value(int64_t(1000));
    lap_id.distinct_count = 1000;
    lap_id.avg_width = 8.0;
    laps.columns.emplace("lap_id", lap_id);
    ColumnStats team;
    team.min_val = Value(std::string("Ferrari"));
    team.max_val = Value(std::string("Williams"));
    team.distinct_count = 10;
    team.avg_width = 9.0;
    laps.columns.emplace("team", team);
    ColumnStats season;
    season.min_val = Value(int64_t(2022));
    season.max_val = Value(int64_t(2024));
    season.distinct_count = 3;
    season.avg_width = 8.0;
    laps.columns.emplace("season", season);
    cat.setStats("laps", std::move(laps));

    TableStats drivers;
    drivers.row_count = 20;
    ColumnStats d_id;
    d_id.min_val = Value(int64_t(1));
    d_id.max_val = Value(int64_t(20));
    d_id.distinct_count = 20;
    d_id.avg_width = 8.0;
    drivers.columns.emplace("driver_id", d_id);
    ColumnStats d_team;
    d_team.min_val = Value(std::string("Ferrari"));
    d_team.max_val = Value(std::string("Williams"));
    d_team.distinct_count = 10;
    d_team.avg_width = 9.0;
    drivers.columns.emplace("team", d_team);
    ColumnStats age;
    age.min_val = Value(int64_t(20));
    age.max_val = Value(int64_t(40));
    age.distinct_count = 20;
    age.avg_width = 8.0;
    drivers.columns.emplace("age", age);
    cat.setStats("drivers", std::move(drivers));
}

// The full vectorized optimizer prefix, exactly as main.cc orders it:
// build -> pushdown -> enumeration -> estimation.
static std::unique_ptr<LogicalPlanNode> optimize(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto plan = LogicalPlanBuilder::build(std::move(stmt), cat);
    plan = PredicatePushdown::apply(std::move(plan), cat);
    plan = JoinEnumeration::apply(std::move(plan), cat);
    CardinalityEstimator::estimate(*plan, cat);
    return plan;
}

// The same prefix WITHOUT enumeration — the --no-optimize-shaped join order,
// used as the differential baseline for result preservation.
static std::unique_ptr<LogicalPlanNode> writtenOrder(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto plan = LogicalPlanBuilder::build(std::move(stmt), cat);
    plan = PredicatePushdown::apply(std::move(plan), cat);
    CardinalityEstimator::estimate(*plan, cat);
    return plan;
}

// topmost JOIN down the children[0] spine
static const LogicalJoin* topJoin(const LogicalPlanNode* root) {
    const LogicalPlanNode* n = root;
    while (n && n->type != LogicalNodeType::JOIN) {
        n = n->children.empty() ? nullptr : n->children[0].get();
    }
    return static_cast<const LogicalJoin*>(n);
}

// every JOIN in the tree, top-down along the spine
static void collectJoins(const LogicalPlanNode* n, std::vector<const LogicalJoin*>& out) {
    if (!n) return;
    if (n->type == LogicalNodeType::JOIN) out.push_back(static_cast<const LogicalJoin*>(n));
    for (const auto& c : n->children) collectJoins(c.get(), out);
}

// The chosen join order, as binder slots. The leftmost relation is the leaf at
// the bottom of the spine and carries no join_slot of its own — its identity is
// stamped on the bottom join's merged schema, which is the only place it is
// recorded once enumeration may put a relation other than 0 there.
static std::vector<int> chosenOrder(const LogicalPlanNode* root) {
    std::vector<const LogicalJoin*> joins;
    collectJoins(root, joins);
    if (joins.empty()) return {};
    std::vector<int> slots;
    for (const LogicalJoin* j : joins) slots.push_back(j->join_slot);
    std::reverse(slots.begin(), slots.end());   // bottom-up == join order
    slots.insert(slots.begin(), joins.back()->output_schema.column(0).relation_slot);
    return slots;
}

static std::string decisionOf(const LogicalPlanNode* root) {
    const LogicalJoin* j = topJoin(root);
    return j ? j->order_decision : std::string();
}

// ── the no-op guard ─────────────────────────────────────────────────────────

// Below three relations there is no ordering decision: the hash join is
// symmetric and which side builds is Week 22's decision, made at lowering. The
// pass must leave the tree completely alone — this is what protects every
// pre-existing single-join --explain string, Week 22 build-side assertion and
// Week 23.5 algorithm assertion from a week that never intended to touch them.
TEST(JoinEnumeration, SingleJoinIsLeftUntouched) {
    Catalog cat(CATALOG);
    seedStats(cat);
    auto plan = optimize(
        "SELECT l.lap_id, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id", cat);
    const LogicalJoin* j = topJoin(plan.get());
    ASSERT_NE(j, nullptr);
    EXPECT_EQ(j->join_slot, 1);
    EXPECT_TRUE(j->order_decision.empty());
    // and the explain string is exactly the pre-Week-28 one: no order= suffix
    EXPECT_EQ(j->explain().find("order="), std::string::npos);
}

TEST(JoinEnumeration, NoJoinAtAllIsLeftUntouched) {
    Catalog cat(CATALOG);
    seedStats(cat);
    auto plan = optimize("SELECT team FROM laps WHERE season = 2022", cat);
    EXPECT_EQ(topJoin(plan.get()), nullptr);
}

// ── the search makes a cost-based choice ────────────────────────────────────

// A star centred on l: drivers (20 rows) is adjacent to both laps scans.
// Written order joins the two 1000-row relations first — 1000*1000/20 = 50,000
// rows of intermediate — then joins 20 rows to it. Joining drivers SECOND gives
// 1000*20/20 = 1,000 rows first and reaches the same final cardinality. Same
// answer, one intermediate fifty times smaller, so the search must put drivers
// (slot 2) immediately after the pivot.
TEST(JoinEnumeration, StarGraphJoinsTheSmallPivotRelationSecond) {
    Catalog cat(CATALOG);
    seedStats(cat);
    auto plan = optimize(
        "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id "
        "JOIN drivers d ON l.driver_id = d.driver_id", cat);
    EXPECT_EQ(chosenOrder(plan.get()), (std::vector<int>{0, 2, 1}));
    EXPECT_NE(decisionOf(plan.get()).find("order=laps@0,drivers@2,laps@1"), std::string::npos);
}

// The decision string is only evidence if it carries a comparand. Assert the
// chosen cost is strictly below the written order's — the arithmetic claim the
// reordering rests on, checkable without believing the plan is "good".
TEST(JoinEnumeration, DecisionReportsAChosenCostBelowTheWrittenOne) {
    Catalog cat(CATALOG);
    seedStats(cat);
    auto plan = optimize(
        "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id "
        "JOIN drivers d ON l.driver_id = d.driver_id", cat);
    const std::string d = decisionOf(plan.get());
    ASSERT_NE(d.find("cost="), std::string::npos);
    ASSERT_NE(d.find("(written="), std::string::npos);
    EXPECT_NE(d.find("method=dp"), std::string::npos);
    double chosen  = std::stod(d.substr(d.find("cost=") + 5));
    double written = std::stod(d.substr(d.find("(written=") + 9));
    EXPECT_LT(chosen, written);
}

// The estimator must agree with the search: the intermediate the search chose
// for its cheapness has to be the intermediate the stamped plan reports. If the
// two ever drift, the printed est= is describing a different plan than the one
// that was costed — which is the whole reason joinCardinality is shared.
TEST(JoinEnumeration, ChosenOrderStampsTheSmallerIntermediate) {
    Catalog cat(CATALOG);
    seedStats(cat);
    auto chosen  = optimize(
        "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id "
        "JOIN drivers d ON l.driver_id = d.driver_id", cat);
    auto written = writtenOrder(
        "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id "
        "JOIN drivers d ON l.driver_id = d.driver_id", cat);

    std::vector<const LogicalJoin*> cj, wj;
    collectJoins(chosen.get(), cj);
    collectJoins(written.get(), wj);
    ASSERT_EQ(cj.size(), 2u);
    ASSERT_EQ(wj.size(), 2u);
    // cj/wj are top-down, so back() is the bottom (first-executed) join
    EXPECT_LT(cj.back()->estimated_rows, wj.back()->estimated_rows);
    // and the FINAL cardinality is unchanged — reordering an inner join tree
    // moves work, never rows
    EXPECT_DOUBLE_EQ(cj.front()->estimated_rows, wj.front()->estimated_rows);
}

// A chain l — d — d2, with d in the middle: d2 touches only d and l touches only
// d, so no legal order separates them from d. SwiftQL has no cross-product
// operator, so a disconnected step is unbuildable rather than merely expensive —
// this is a legality guard, not a preference. Either end may lead; what is
// forbidden is d arriving last.
TEST(JoinEnumeration, ChainNeverPlacesARelationBeforeItsOnlyNeighbour) {
    Catalog cat(CATALOG);
    seedStats(cat);
    auto plan = optimize(
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN drivers d2 ON d.team = d2.team", cat);
    std::vector<int> slots = chosenOrder(plan.get());
    ASSERT_EQ(slots.size(), 3u);
    auto pos = [&](int slot) {
        return std::find(slots.begin(), slots.end(), slot) - slots.begin();
    };
    EXPECT_LT(pos(1), 2);   // d (slot 1) is the articulation point: never last
    // every join carries at least one key: a keyless LogicalJoin is a cross product
    std::vector<const LogicalJoin*> joins;
    collectJoins(plan.get(), joins);
    for (const LogicalJoin* j : joins) EXPECT_FALSE(j->keys.empty());
}

// ── structural invariants of the rebuilt tree ───────────────────────────────

// Every edge of the join graph must be consumed exactly once, at the step its
// later-in-order endpoint arrives. That is the correctness argument for
// reordering at all: an inner equi-join is associative and commutative, so any
// order computes the same relation PROVIDED no predicate is dropped or applied
// twice. A triangle is the shape that catches it — every order is legal, and the
// last relation added carries TWO keys where the first carried one.
TEST(JoinEnumeration, EveryJoinEdgeIsUsedExactlyOnce) {
    Catalog cat(CATALOG);
    seedStats(cat);
    const std::string sql =
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN drivers d2 ON d.team = d2.team AND l.driver_id = d2.driver_id";

    auto count_keys = [](const LogicalPlanNode* root) {
        std::vector<const LogicalJoin*> joins;
        collectJoins(root, joins);
        size_t n = 0;
        for (const LogicalJoin* j : joins) n += j->keys.size();
        return n;
    };
    EXPECT_EQ(count_keys(optimize(sql, cat).get()),
              count_keys(writtenOrder(sql, cat).get()));
}

// Left-deep is an invariant three files depend on (rightKeyIndices resolves the
// right input by bare name because it is exactly one relation; rowWidth's
// isSingleRelation split; the whole lowering recursion). A bushy tree here would
// be a silent wrong column, not a compile error.
TEST(JoinEnumeration, RebuiltTreeStaysLeftDeep) {
    Catalog cat(CATALOG);
    seedStats(cat);
    auto plan = optimize(
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN drivers d2 ON d.driver_id = d2.driver_id "
        "JOIN laps l2 ON d2.driver_id = l2.driver_id", cat);
    std::vector<const LogicalJoin*> joins;
    collectJoins(plan.get(), joins);
    ASSERT_EQ(joins.size(), 3u);
    for (const LogicalJoin* j : joins) {
        EXPECT_NE(j->children[1]->type, LogicalNodeType::JOIN);
    }
}

// The two stamping domains, and the boundary between them. A leaf's OWN schema
// stamps slot 0 (a standalone scan has one relation to disambiguate, its pushed
// filter's refs were restamped to 0, and ChunkPruner reads slot < 1 as
// scan-local). The MERGED schema is where a relation acquires its binder slot.
// In written order the leftmost relation IS slot 0, which is the only reason
// LogicalPlanBuilder::build never had to stamp its left block.
TEST(JoinEnumeration, MergedSchemaStampsTheLeftmostRelationsBinderSlot) {
    Catalog cat(CATALOG);
    seedStats(cat);
    // Chain laps(0, 1000 rows) — drivers(1, 20) — drivers(2, 20). Leading with
    // either drivers end joins 20x20/10 = 40 rows before touching laps, against
    // 1000 for the written order — so the search moves a NON-ZERO relation to the
    // bottom of the spine, which is the case the whole stamping generalization
    // exists for and the case no written-order tree can produce.
    auto plan = optimize(
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN drivers d2 ON d.team = d2.team", cat);
    std::vector<const LogicalJoin*> joins;
    collectJoins(plan.get(), joins);
    ASSERT_EQ(joins.size(), 2u);
    const LogicalJoin* bottom = joins.back();

    // The test is only meaningful if the leftmost relation actually moved: with
    // slot 0 at the bottom every stamp below is a no-op and this would pass
    // vacuously.
    const int leftmost = bottom->output_schema.column(0).relation_slot;
    ASSERT_NE(leftmost, 0);

    // the leaf subtree underneath still stamps slot 0 — a standalone scan has one
    // relation to disambiguate, its pushed filter's refs were restamped to 0, and
    // ChunkPruner reads slot < 1 as scan-local
    ASSERT_GT(bottom->children[0]->output_schema.size(), 0);
    for (const ColumnDef& c : bottom->children[0]->output_schema.columns()) {
        EXPECT_EQ(c.relation_slot, 0);
    }
    // ...while the merged schema's left block carries the relation's binder slot
    const int left_cols = bottom->children[0]->output_schema.size();
    for (int i = 0; i < left_cols; ++i) {
        EXPECT_EQ(bottom->output_schema.column(i).relation_slot, leftmost);
    }
    // and the right block carries the join's slot
    for (int i = left_cols; i < bottom->output_schema.size(); ++i) {
        EXPECT_EQ(bottom->output_schema.column(i).relation_slot, bottom->join_slot);
    }
    EXPECT_NE(leftmost, bottom->join_slot);

    // every reference from above still resolves slot-first against the TOP
    // merged schema, which is what the stamping is ultimately for
    const Schema& top = joins.front()->output_schema;
    EXPECT_GE(top.indexOf("driver_id", 0), 0);   // laps.driver_id
    EXPECT_GE(top.indexOf("team", 1), 0);        // d.team
    EXPECT_GE(top.indexOf("team", 2), 0);        // d2.team
}

// JoinKey::from_slot is the relation slot AS PRESENTED BY THE LEFT CHILD'S OWN
// SCHEMA. At the bottom join that schema is a leaf's, stamping 0 —
// leftKeyIndices() resolves against it and THROWS on a miss, so a binder slot
// there would abort the query. Slot 0 is unambiguous: one relation is present.
TEST(JoinEnumeration, BottomJoinKeysAddressTheLeafAsSlotZero) {
    Catalog cat(CATALOG);
    seedStats(cat);
    // same shape as the stamping test: a non-zero relation leads, so from_slot 0
    // on the bottom join is a deliberate rewrite rather than a coincidence
    auto plan = optimize(
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN drivers d2 ON d.team = d2.team", cat);
    std::vector<const LogicalJoin*> joins;
    collectJoins(plan.get(), joins);
    ASSERT_EQ(joins.size(), 2u);
    ASSERT_NE(joins.back()->output_schema.column(0).relation_slot, 0);
    for (const JoinKey& k : joins.back()->keys) {
        EXPECT_EQ(k.from_slot, 0);
        // and it resolves against the leaf's own schema, which is what
        // leftKeyIndices() will do at lowering — a miss there throws
        EXPECT_GE(joins.back()->children[0]->output_schema.indexOf(k.from_col, k.from_slot), 0);
    }
    // the join above it resolves against a merged schema, where binder slots are
    // real and every key must name one that exists there
    for (const JoinKey& k : joins.front()->keys) {
        EXPECT_GE(joins.front()->children[0]->output_schema.indexOf(k.from_col, k.from_slot), 0);
    }
}

// ── the fallback ────────────────────────────────────────────────────────────

// Above MAX_DP_RELATIONS the DP's 2^N table is abandoned for a greedy walk.
// Assert LEGALITY (connected, every relation placed, every join keyed), not
// optimality — greedy is never optimal and a test that demanded it would be
// asserting a belief.
TEST(JoinEnumeration, AboveTheDpLimitFallsBackToGreedyAndStaysLegal) {
    Catalog cat(CATALOG);
    seedStats(cat);
    std::string sql = "SELECT COUNT(*) FROM laps a0";
    for (int i = 1; i <= MAX_DP_RELATIONS; ++i) {   // MAX_DP_RELATIONS + 1 relations
        sql += " JOIN laps a" + std::to_string(i)
             + " ON a0.driver_id = a" + std::to_string(i) + ".driver_id";
    }
    auto plan = optimize(sql, cat);

    std::vector<const LogicalJoin*> joins;
    collectJoins(plan.get(), joins);
    EXPECT_EQ(joins.size(), static_cast<size_t>(MAX_DP_RELATIONS));
    std::unordered_set<int> slots;
    for (const LogicalJoin* j : joins) {
        EXPECT_FALSE(j->keys.empty());               // no cross product
        EXPECT_NE(j->children[1]->type, LogicalNodeType::JOIN);   // still left-deep
        slots.insert(j->join_slot);
    }
    EXPECT_EQ(slots.size(), static_cast<size_t>(MAX_DP_RELATIONS));  // each relation once
    EXPECT_NE(decisionOf(plan.get()).find("method=greedy"), std::string::npos);
}

// ── result preservation ─────────────────────────────────────────────────────

static std::unordered_map<std::string, ColumnarTable> loadColumnar(
        const SelectStatement& stmt, const Catalog& cat) {
    std::unordered_map<std::string, ColumnarTable> tables;
    const auto& fm = cat.getTable(stmt.from_table);
    tables.emplace(stmt.from_table,
                   CSVToColumnar::convert(CSVLoader::load(fm.filepath, fm.schema), fm.schema));
    for (const auto& j : stmt.joins) {
        if (tables.count(j.join_table)) continue;
        const auto& jm = cat.getTable(j.join_table);
        tables.emplace(j.join_table,
                       CSVToColumnar::convert(CSVLoader::load(jm.filepath, jm.schema), jm.schema));
    }
    return tables;
}

static std::vector<std::string> runVec(const std::string& sql, const Catalog& cat,
                                       bool enumerate) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto tables = loadColumnar(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    logical = PredicatePushdown::apply(std::move(logical), cat);
    if (enumerate) logical = JoinEnumeration::apply(std::move(logical), cat);
    CardinalityEstimator::estimate(*logical, cat);
    auto plan = VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);

    plan->open();
    std::vector<std::string> rows;
    while (DataChunk* chunk = plan->nextChunk()) {
        int n = chunk->filter_applied ? static_cast<int>(chunk->sel.indices.size())
                                      : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            std::string s;
            for (const auto& cv : chunk->columns) {
                Value v = valueAt(cv, r);
                s += (v.isNull() ? "NULL" : v.toString()) + "|";
            }
            rows.push_back(std::move(s));
        }
    }
    plan->close();
    std::sort(rows.begin(), rows.end());
    return rows;
}

// The invariant that actually matters: reordering changes plan shape, never
// results. Run each shape through the pipeline with and without the enumeration
// pass and diff the rows — the in-process form of the harness's optimized-vs-
// --no-optimize gate, on the shapes the search actually reorders.
TEST(JoinEnumeration, ReorderedPlansReturnTheWrittenOrdersRows) {
    Catalog cat(CATALOG);
    seedStats(cat);
    const std::vector<std::string> cases = {
        // star: drivers is the pivot's cheap neighbour
        "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id "
        "JOIN drivers d ON l.driver_id = d.driver_id",
        // projected columns from all three relations, so column ORDER is diffed
        // too — the merged schema is rebuilt per ordering
        "SELECT l.lap_id, d.name, l2.speed FROM laps l "
        "JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN laps l2 ON d.driver_id = l2.driver_id",
        // chain with a residual ON conjunct and a pushed WHERE: predicate
        // assignment happens BEFORE the reorder and must survive it
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN drivers d2 ON d.team = d2.team AND d2.age > 25 WHERE l.season = 2022",
        // the shape that moves a NON-ZERO relation to the bottom of the spine —
        // the merged-schema stamping and the from_slot = 0 rewrite are only
        // exercised end-to-end here, and both produce plausible rows when wrong
        "SELECT l.team, d2.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN drivers d2 ON d.team = d2.team",
        // triangle: an ordering changes a one-key join into a two-key one
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN drivers d2 ON d.team = d2.team AND l.driver_id = d2.driver_id",
        // four relations, self-join on both tables
        "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN drivers d2 ON d.driver_id = d2.driver_id "
        "JOIN laps l2 ON d2.driver_id = l2.driver_id",
        // aggregation grouped by a column of a middle relation: the group key
        // has to resolve by slot against a merged schema built in a new order
        "SELECT d.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN laps l2 ON d.driver_id = l2.driver_id GROUP BY d.team",
        // SELECT *: star expressions are synthesized pre-enumeration in written
        // schema order and carry relation slots, so output order must not move
        "SELECT * FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
        "JOIN laps l2 ON d.driver_id = l2.driver_id",
    };
    for (const std::string& sql : cases) {
        EXPECT_EQ(runVec(sql, cat, /*enumerate=*/true),
                  runVec(sql, cat, /*enumerate=*/false)) << sql;
    }
}
