#include <gtest/gtest.h>
#include "planner/binder.h"
#include "planner/planner.h"
#include "planner/plan_nodes.h"
#include "parser/parser.h"
#include "storage/csv_loader.h"
#include "common/schema.h"
#include "common/value.h"
#include <algorithm>
#include <memory>

// The test catalog lives one level up from the build dir (tests run from build/).
static const char* CATALOG = "../tests/data/test_catalog.json";

// Parse + bind a query against the test catalog, return the bound statement.
static SelectStatement bindQuery(const std::string& sql, const Catalog& cat) {
    Parser p(sql);
    auto stmt = p.parse();
    Binder::bind(stmt, cat);
    return stmt;
}

// Find the first ColumnRef in the select list (helper for slot assertions).
static const ColumnRef* firstSelectCol(const SelectStatement& stmt) {
    for (const auto& e : stmt.select_list)
        if (auto* c = dynamic_cast<const ColumnRef*>(e.get())) return c;
    return nullptr;
}

// Run a query fully (Binder -> Planner -> Volcano executor) and collect rows.
static std::vector<Row> runVolcano(const std::string& sql, const Catalog& cat) {
    Parser p(sql);
    auto stmt = p.parse();
    Binder::bind(stmt, cat);

    std::unordered_map<std::string, std::vector<Row>> table_rows;
    const auto& fm = cat.getTable(stmt.from_table);
    table_rows[stmt.from_table] = CSVLoader::load(fm.filepath, fm.schema);
    if (stmt.join.has_value()) {
        const auto& jm = cat.getTable(stmt.join->join_table);
        // self-join keys by name; load once is enough (planner copies internally)
        if (!table_rows.count(stmt.join->join_table))
            table_rows[stmt.join->join_table] = CSVLoader::load(jm.filepath, jm.schema);
    }
    auto plan = Planner::plan(std::move(stmt), cat, std::move(table_rows));
    std::vector<Row> out;
    plan->open();
    while (Row* r = plan->next()) out.push_back(*r);
    plan->close();
    return out;
}

// ===== Schema slot-aware lookup =====

TEST(SchemaSlot, IndexOfBySlotDistinguishesDuplicateNames) {
    Schema s(std::vector<ColumnDef>{
        {"x", TypeId::INT, 0},
        {"y", TypeId::INT, 0},
        {"x", TypeId::INT, 1},  // duplicate name, JOIN side
    });
    EXPECT_EQ(s.indexOf("x"), 0);        // bare: first match
    EXPECT_EQ(s.indexOf("x", 0), 0);     // FROM side
    EXPECT_EQ(s.indexOf("x", 1), 2);     // JOIN side
    EXPECT_EQ(s.indexOf("y", 1), -1);    // y only exists slot 0
    EXPECT_EQ(s.indexOf("z", 0), -1);    // absent
}

// ===== Binder slot assignment =====

TEST(Binder, UnqualifiedSingleRelationGetsSlotZero) {
    Catalog cat(CATALOG);
    auto stmt = bindQuery("SELECT team FROM laps", cat);
    const ColumnRef* c = firstSelectCol(stmt);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->relation_slot, 0);
}

TEST(Binder, QualifiedJoinColumnsGetCorrectSlots) {
    Catalog cat(CATALOG);
    // drivers-only column (nationality) must resolve to JOIN side (slot 1)
    auto stmt = bindQuery(
        "SELECT drivers.nationality FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    const ColumnRef* c = firstSelectCol(stmt);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->relation_slot, 1);
    EXPECT_EQ(c->table_name, "drivers"); // alias/name normalized to canonical
}

TEST(Binder, SharedColumnNameQualifierPicksSide) {
    Catalog cat(CATALOG);
    // both laps and drivers have `team`; drivers.team must bind to slot 1
    auto stmt = bindQuery(
        "SELECT drivers.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    const ColumnRef* c = firstSelectCol(stmt);
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->relation_slot, 1);
}

TEST(Binder, SelfJoinAliasesGetDistinctSlots) {
    Catalog cat(CATALOG);
    auto stmt = bindQuery(
        "SELECT a.id, b.val FROM sj a JOIN sj b ON a.grp = b.grp", cat);
    ASSERT_EQ(stmt.select_list.size(), 2u);
    auto* a_id = dynamic_cast<const ColumnRef*>(stmt.select_list[0].get());
    auto* b_val = dynamic_cast<const ColumnRef*>(stmt.select_list[1].get());
    ASSERT_NE(a_id, nullptr);
    ASSERT_NE(b_val, nullptr);
    EXPECT_EQ(a_id->relation_slot, 0);   // FROM occurrence
    EXPECT_EQ(b_val->relation_slot, 1);  // JOIN occurrence
}

// ===== Binder error cases =====

TEST(Binder, AliaslessSelfJoinRejected) {
    Catalog cat(CATALOG);
    Parser p("SELECT id FROM sj JOIN sj ON sj.id = sj.id");
    auto stmt = p.parse();
    EXPECT_THROW(Binder::bind(stmt, cat), std::runtime_error);
}

TEST(Binder, AmbiguousUnqualifiedInSelfJoinRejected) {
    Catalog cat(CATALOG);
    Parser p("SELECT id FROM sj a JOIN sj b ON a.grp = b.grp");
    auto stmt = p.parse();
    EXPECT_THROW(Binder::bind(stmt, cat), std::runtime_error);
}

TEST(Binder, AmbiguousUnqualifiedAcrossTablesRejected) {
    Catalog cat(CATALOG);
    // team exists in both laps and drivers -> ambiguous unqualified
    Parser p("SELECT team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id");
    auto stmt = p.parse();
    EXPECT_THROW(Binder::bind(stmt, cat), std::runtime_error);
}

TEST(Binder, UnknownQualifierRejected) {
    Catalog cat(CATALOG);
    Parser p("SELECT x.id FROM sj a JOIN sj b ON a.grp = b.grp");
    auto stmt = p.parse();
    EXPECT_THROW(Binder::bind(stmt, cat), std::runtime_error);
}

TEST(Binder, QualifiedColumnNotInTableRejected) {
    Catalog cat(CATALOG);
    Parser p("SELECT a.bogus FROM sj a JOIN sj b ON a.grp = b.grp");
    auto stmt = p.parse();
    EXPECT_THROW(Binder::bind(stmt, cat), std::runtime_error);
}

// ===== Self-join execution (exact values, Volcano E2E) =====

// Sort rows by first two int columns for order-independent comparison.
static void sortByFirstTwo(std::vector<Row>& rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a[0].asInt() != b[0].asInt()) return a[0].asInt() < b[0].asInt();
        return a[1].asInt() < b[1].asInt();
    });
}

TEST(SelfJoin, SymmetricPairs) {
    Catalog cat(CATALOG);
    // sj: (1,1,100),(2,1,200),(3,2,300); grp groups {1,2}x{1,2} + {3}x{3} = 5 pairs
    auto rows = runVolcano(
        "SELECT a.id, b.id FROM sj a JOIN sj b ON a.grp = b.grp", cat);
    ASSERT_EQ(rows.size(), 5u);
    sortByFirstTwo(rows);
    std::vector<std::pair<int64_t,int64_t>> expected = {{1,1},{1,2},{2,1},{2,2},{3,3}};
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(rows[i][0].asInt(), expected[i].first);
        EXPECT_EQ(rows[i][1].asInt(), expected[i].second);
    }
}

TEST(SelfJoin, DistinctSideValuesResolveIndependently) {
    Catalog cat(CATALOG);
    // a.val and b.val must NOT collapse to the same column
    auto rows = runVolcano(
        "SELECT a.id, a.val, b.id, b.val FROM sj a JOIN sj b ON a.grp = b.grp", cat);
    ASSERT_EQ(rows.size(), 5u);
    for (const auto& r : rows) {
        // a.val is a.id*100; b.val is b.id*100 — verifies independent resolution
        EXPECT_EQ(r[1].asInt(), r[0].asInt() * 100);
        EXPECT_EQ(r[3].asInt(), r[2].asInt() * 100);
    }
}

TEST(SelfJoin, AsymmetricConditionRouting) {
    Catalog cat(CATALOG);
    // ON a.grp = b.id : a.grp {1,1,2}, b.id {1,2,3} -> (1,1),(2,1),(3,2)
    auto rows = runVolcano(
        "SELECT a.id, b.id FROM sj a JOIN sj b ON a.grp = b.id", cat);
    ASSERT_EQ(rows.size(), 3u);
    sortByFirstTwo(rows);
    std::vector<std::pair<int64_t,int64_t>> expected = {{1,1},{2,1},{3,2}};
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(rows[i][0].asInt(), expected[i].first);
        EXPECT_EQ(rows[i][1].asInt(), expected[i].second);
    }
}

TEST(SelfJoin, AsymmetricConditionRoutingReversed) {
    Catalog cat(CATALOG);
    // ON b.id = a.grp : same result as above, operands swapped
    auto rows = runVolcano(
        "SELECT a.id, b.id FROM sj a JOIN sj b ON b.id = a.grp", cat);
    ASSERT_EQ(rows.size(), 3u);
    sortByFirstTwo(rows);
    std::vector<std::pair<int64_t,int64_t>> expected = {{1,1},{2,1},{3,2}};
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(rows[i][0].asInt(), expected[i].first);
        EXPECT_EQ(rows[i][1].asInt(), expected[i].second);
    }
}

TEST(SelfJoin, FilterOnJoinSideColumn) {
    Catalog cat(CATALOG);
    // WHERE b.val > 150 keeps b in {200,300}: (1,2),(2,2),(3,3)
    auto rows = runVolcano(
        "SELECT a.id, b.id FROM sj a JOIN sj b ON a.grp = b.grp WHERE b.val > 150", cat);
    ASSERT_EQ(rows.size(), 3u);
    sortByFirstTwo(rows);
    std::vector<std::pair<int64_t,int64_t>> expected = {{1,2},{2,2},{3,3}};
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(rows[i][0].asInt(), expected[i].first);
        EXPECT_EQ(rows[i][1].asInt(), expected[i].second);
    }
}

TEST(SelfJoin, AggregateOverSelfJoin) {
    Catalog cat(CATALOG);
    // GROUP BY a.grp: grp1 -> 2x2=4, grp2 -> 1x1=1
    auto rows = runVolcano(
        "SELECT a.grp, COUNT(*) FROM sj a JOIN sj b ON a.grp = b.grp GROUP BY a.grp", cat);
    ASSERT_EQ(rows.size(), 2u);
    std::sort(rows.begin(), rows.end(),
              [](const Row& x, const Row& y){ return x[0].asInt() < y[0].asInt(); });
    EXPECT_EQ(rows[0][0].asInt(), 1); EXPECT_EQ(rows[0][1].asInt(), 4);
    EXPECT_EQ(rows[1][0].asInt(), 2); EXPECT_EQ(rows[1][1].asInt(), 1);
}

TEST(SelfJoin, AggregateArgResolvesJoinSide) {
    Catalog cat(CATALOG);
    // ON a.grp = b.id : a1->b1(val100), a2->b1(val100), a3->b2(val200)
    // SUM(b.val) GROUP BY a.grp -> grp1=200, grp2=200
    // (SUM(a.val) would wrongly give 300,300 — proves the agg arg resolves b, not a)
    auto rows = runVolcano(
        "SELECT a.grp, SUM(b.val) FROM sj a JOIN sj b ON a.grp = b.id GROUP BY a.grp", cat);
    ASSERT_EQ(rows.size(), 2u);
    std::sort(rows.begin(), rows.end(),
              [](const Row& x, const Row& y){ return x[0].asInt() < y[0].asInt(); });
    EXPECT_EQ(rows[0][0].asInt(), 1); EXPECT_DOUBLE_EQ(rows[0][1].asDouble(), 200.0);
    EXPECT_EQ(rows[1][0].asInt(), 2); EXPECT_DOUBLE_EQ(rows[1][1].asDouble(), 200.0);
}

TEST(SelfJoin, AggregateArgFromSideDistinct) {
    Catalog cat(CATALOG);
    // same join, SUM(a.val) -> grp1=300 (100+200), grp2=300 — the FROM side
    auto rows = runVolcano(
        "SELECT a.grp, SUM(a.val) FROM sj a JOIN sj b ON a.grp = b.id GROUP BY a.grp", cat);
    ASSERT_EQ(rows.size(), 2u);
    std::sort(rows.begin(), rows.end(),
              [](const Row& x, const Row& y){ return x[0].asInt() < y[0].asInt(); });
    EXPECT_EQ(rows[0][0].asInt(), 1); EXPECT_DOUBLE_EQ(rows[0][1].asDouble(), 300.0);
    EXPECT_EQ(rows[1][0].asInt(), 2); EXPECT_DOUBLE_EQ(rows[1][1].asDouble(), 300.0);
}

TEST(SelfJoin, SelectStarEmitsBothSides) {
    Catalog cat(CATALOG);
    // SELECT * on self-join must emit 6 columns (3 from each side), not 3
    auto rows = runVolcano(
        "SELECT * FROM sj a JOIN sj b ON a.grp = b.grp", cat);
    ASSERT_EQ(rows.size(), 5u);
    for (const auto& r : rows) EXPECT_EQ(r.size(), 6u);
}

// ===== NULL over a join, slot-aware (operator level) =====
// CSV cannot express NULLs, so this drives HashJoinNode + FilterNode directly
// with a slot-stamped merged schema and a JOIN-side (slot 1) nullable column.
// Verifies IS NULL resolves against the correct side after the join.
TEST(SelfJoin, IsNullResolvesJoinSideColumn) {
    // FROM side "a": (k, v) ; JOIN side "b": (k, w) where w has a NULL.
    Schema from_schema(std::vector<ColumnDef>{{"k", TypeId::INT, 0}, {"v", TypeId::INT, 0}});
    Schema join_schema(std::vector<ColumnDef>{{"k", TypeId::INT, 0}, {"w", TypeId::INT, 0}});
    // merged output: FROM cols slot 0, JOIN cols slot 1
    Schema merged(std::vector<ColumnDef>{
        {"k", TypeId::INT, 0}, {"v", TypeId::INT, 0},
        {"k", TypeId::INT, 1}, {"w", TypeId::INT, 1}});

    std::vector<Row> from_rows = {{Value(1LL), Value(10LL)}};
    std::vector<Row> join_rows = {
        {Value(1LL), Value::null()},   // w is NULL
        {Value(1LL), Value(99LL)},     // w is 99
    };
    auto from_scan = std::make_unique<SeqScanNode>("a", from_rows, from_schema);
    auto join_scan = std::make_unique<SeqScanNode>("b", join_rows, join_schema);
    // probe = FROM, build = JOIN, not swapped -> output [FROM, JOIN]
    auto join = std::make_unique<HashJoinNode>(
        std::move(from_scan), std::move(join_scan), "k", "k", merged, /*swapped=*/false);

    // WHERE b.w IS NULL  -> operand is the slot-1 "w" column
    auto w_ref = std::make_unique<ColumnRef>();
    w_ref->column_name = "w";
    w_ref->relation_slot = 1;
    auto isnull = std::make_unique<IsNullExpr>();
    isnull->operand = std::move(w_ref);
    isnull->is_not_null = false;
    auto filter = std::make_unique<FilterNode>(std::move(join), std::move(isnull));

    std::vector<Row> out;
    filter->open();
    while (Row* r = filter->next()) out.push_back(*r);
    filter->close();

    // exactly the row where b.w is NULL survives
    ASSERT_EQ(out.size(), 1u);
    EXPECT_TRUE(out[0][3].isNull());     // merged col 3 = JOIN-side w
    EXPECT_EQ(out[0][0].asInt(), 1);     // FROM k
    EXPECT_EQ(out[0][1].asInt(), 10);    // FROM v
}

// ===== JOIN ON condition validation =====
// Phase 4 supports exactly one cross-relation equality (ColumnRef = ColumnRef).
// Anything else must be rejected with a specific error instead of silently
// executing as an equi-join (or worse). Multi-key / non-equality conditions
// are deferred to the multi-way join work (Week 26+).

#include "planner/validator.h"

// Bind then validate, returning the error message ("" when accepted).
static std::string joinOnValidationError(const std::string& sql, const Catalog& cat) {
    Parser p(sql);
    auto stmt = p.parse();
    Binder::bind(stmt, cat);
    try {
        Validator::validate(stmt, cat);
        return "";
    } catch (const std::runtime_error& e) {
        return e.what();
    }
}

TEST(JoinOnValidation, SingleEquiJoinAccepted) {
    Catalog cat(CATALOG);
    EXPECT_EQ(joinOnValidationError(
        "SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat), "");
}

TEST(JoinOnValidation, NonEqualityOperatorRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.grp < b.id", cat);
    EXPECT_NE(err.find("non-equality"), std::string::npos) << err;
}

TEST(JoinOnValidation, NotEqualOperatorRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id != b.id", cat);
    EXPECT_NE(err.find("non-equality"), std::string::npos) << err;
}

TEST(JoinOnValidation, CompoundConditionRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp", cat);
    EXPECT_NE(err.find("compound"), std::string::npos) << err;
}

TEST(JoinOnValidation, LiteralOperandRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = 5", cat);
    EXPECT_NE(err.find("column"), std::string::npos) << err;
}

TEST(JoinOnValidation, SameRelationBothSidesRejected) {
    Catalog cat(CATALOG);
    // Previously executed silently as a.id = b.grp (cross-side reroute).
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = a.grp", cat);
    EXPECT_NE(err.find("each joined table"), std::string::npos) << err;
}

// ===== GROUP BY / aggregate resolution (Phase 4 audit fixes) =====

// C4: self-join aggregates over the same column must not collapse to one value.
TEST(AggregateResolution, SelfJoinDuplicateAggregatesKeepDistinctValues) {
    Catalog cat(CATALOG);
    // join pairs (a,b): (1,1) (2,1) (3,2) -> AVG(a.id)=2.0, AVG(b.id)=4/3
    auto rows = runVolcano(
        "SELECT AVG(a.id), AVG(b.id) FROM sj a JOIN sj b ON a.grp = b.id", cat);
    ASSERT_EQ(rows.size(), 1u);
    ASSERT_EQ(rows[0].size(), 2u);
    EXPECT_DOUBLE_EQ(rows[0][0].asDouble(), 2.0);
    EXPECT_DOUBLE_EQ(rows[0][1].asDouble(), 4.0 / 3.0);
}

// C3: a qualified GROUP BY must group by the named side, not silently by FROM.
TEST(AggregateResolution, QualifiedGroupByGroupsByNamedSide) {
    Catalog cat(CATALOG);
    // join pairs (a,b) on a.id = b.grp: (1,b1) (1,b2) (2,b3)
    // GROUP BY b.grp -> {1: 2 rows, 2: 1 row}; grouping by a.grp gives {1: 3}
    auto rows = runVolcano(
        "SELECT b.grp, COUNT(*) FROM sj a JOIN sj b ON a.id = b.grp GROUP BY b.grp", cat);
    ASSERT_EQ(rows.size(), 2u);
    std::vector<std::pair<int64_t, int64_t>> got;
    for (const auto& r : rows) got.push_back({r[0].asInt(), r[1].asInt()});
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got[0], (std::pair<int64_t, int64_t>{1, 2}));
    EXPECT_EQ(got[1], (std::pair<int64_t, int64_t>{2, 1}));
}

// M2: an unqualified GROUP BY column present on both join sides is ambiguous.
TEST(AggregateResolution, AmbiguousUnqualifiedGroupByRejected) {
    Catalog cat(CATALOG);
    Parser p("SELECT COUNT(*) FROM sj a JOIN sj b ON a.id = b.id GROUP BY val");
    auto stmt = p.parse();
    EXPECT_THROW(Binder::bind(stmt, cat), std::runtime_error);
}

// C3 companion: selecting one side while grouping by the other must be rejected.
TEST(AggregateResolution, SelectedSideMustMatchGroupBySide) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.grp, COUNT(*) FROM sj a JOIN sj b ON a.id = b.id GROUP BY b.grp", cat);
    EXPECT_NE(err.find("GROUP BY"), std::string::npos) << err;
}

// M1: aggregates referenced only in HAVING are computed, not projected.
TEST(AggregateResolution, HavingOnlyAggregateExecutes) {
    Catalog cat(CATALOG);
    // teams: McLaren x2, Ferrari x2, Mercedes x1
    auto rows = runVolcano(
        "SELECT team FROM laps GROUP BY team HAVING COUNT(*) > 1", cat);
    ASSERT_EQ(rows.size(), 2u);
    for (const auto& r : rows) EXPECT_EQ(r.size(), 1u);  // COUNT(*) not projected
}

// M1: aggregates referenced only in ORDER BY are computed, not projected.
TEST(AggregateResolution, OrderByOnlyAggregateExecutes) {
    Catalog cat(CATALOG);
    auto rows = runVolcano(
        "SELECT team FROM laps GROUP BY team ORDER BY COUNT(*) DESC", cat);
    ASSERT_EQ(rows.size(), 3u);
    for (const auto& r : rows) EXPECT_EQ(r.size(), 1u);
    EXPECT_EQ(rows[2][0].asString(), "Mercedes");  // the single-lap team sorts last
}

// ===== Aggregate type checking across joins (N2) =====

TEST(AggregateTypeCheck, SumOnQualifiedStringColumnInJoinRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT SUM(d.name) FROM laps JOIN drivers d ON laps.driver_id = d.driver_id", cat);
    EXPECT_NE(err.find("numeric"), std::string::npos) << err;
}

TEST(AggregateTypeCheck, SumOnUnqualifiedStringColumnInJoinRejected) {
    Catalog cat(CATALOG);
    // name exists only in drivers; binder resolves it to the JOIN side
    std::string err = joinOnValidationError(
        "SELECT SUM(name) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);
    EXPECT_NE(err.find("numeric"), std::string::npos) << err;
}

TEST(AggregateTypeCheck, AvgOnStringColumnInHavingRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT season, COUNT(*) FROM laps GROUP BY season HAVING AVG(team) > 5", cat);
    EXPECT_NE(err.find("numeric"), std::string::npos) << err;
}

// ===== Duplicate alias diagnostics (N3) =====

TEST(Binder, DuplicateAliasAcrossDifferentTablesNamedCorrectly) {
    Catalog cat(CATALOG);
    Parser p("SELECT x.team FROM laps x JOIN drivers x ON x.driver_id = x.driver_id");
    auto stmt = p.parse();
    try {
        Binder::bind(stmt, cat);
        FAIL() << "expected duplicate-alias error";
    } catch (const std::runtime_error& e) {
        std::string err = e.what();
        // not a self-join: the message must talk about the duplicate alias,
        // not claim 'laps' was joined with itself
        EXPECT_NE(err.find("duplicate table alias"), std::string::npos) << err;
        EXPECT_NE(err.find("'x'"), std::string::npos) << err;
    }
}
