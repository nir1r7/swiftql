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
    for (const auto& j : stmt.joins) {
        // self-join keys by name; load once is enough (planner copies internally)
        if (table_rows.count(j.join_table)) continue;
        const auto& jm = cat.getTable(j.join_table);
        table_rows[j.join_table] = CSVLoader::load(jm.filepath, jm.schema);
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

    std::vector<Row> from_rows = {{Value(int64_t(1)), Value(int64_t(10))}};
    std::vector<Row> join_rows = {
        {Value(int64_t(1)), Value::null()},   // w is NULL
        {Value(int64_t(1)), Value(int64_t(99))},     // w is 99
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
// Week 26 accepts a cross-relation equality or an AND-chain of them (multi-key
// equi-joins, TPC-H Q9). Anything else must be rejected with a specific error
// instead of silently executing as an equi-join (or worse). Non-equality ON
// conjuncts become post-join residuals in Week 27.

#include "planner/validator.h"
#include "planner/join_condition.h"

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

// Validate WITHOUT binding. Site 18 is only reachable this way for a qualified
// reference — in the full pipeline Binder::resolveColumnRef throws first — and
// validator-only callers are exactly the case its unbound fallbacks exist for.
static std::string validateOnlyJoinError(const std::string& sql, const Catalog& cat) {
    Parser p(sql);
    auto stmt = p.parse();
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

// Week 26 lifts exactly this restriction: the AND-chain that used to be
// rejected as "compound" is now a two-key equi-join (required for TPC-H Q9).
TEST(JoinOnValidation, MultiKeyEquiJoinAccepted) {
    Catalog cat(CATALOG);
    EXPECT_EQ(joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp", cat), "");
}

// ...but only when every conjunct is an equality. A mixed compound still
// throws: routing non-equality ON conjuncts as residuals is Week 27.
TEST(JoinOnValidation, MixedCompoundWithNonEqualityRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND a.grp < b.grp", cat);
    EXPECT_NE(err.find("non-equality"), std::string::npos) << err;
}

TEST(JoinOnValidation, OrConditionRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id OR a.grp = b.grp", cat);
    EXPECT_NE(err.find("AND-chain"), std::string::npos) << err;
}

// Only reachable with three relations: `c` is not in the tree when b is joined.
TEST(JoinOnValidation, ForwardReferenceToLaterRelationRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.grp = c.id JOIN sj c ON b.grp = c.id", cat);
    EXPECT_NE(err.find("table being joined"), std::string::npos) << err;
}

// The SAME query with the operands swapped. One side is now the relation being
// joined, so the "references the table being joined" test passes and the other
// half of the rule — the remaining operand must already be in the left tree —
// is what has to reject it. Checking only the first half accepted this and
// rewired keys[0].from_col to whatever column of that name relation 0 had,
// producing a wrong join tree that explain() renders identically to a right one.
TEST(JoinOnValidation, ForwardReferenceOnLeftOperandRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON b.grp = c.id JOIN sj c ON a.id = c.id", cat);
    EXPECT_NE(err.find("joined later"), std::string::npos) << err;
}

// Worse form of the same bug: the borrowed name need not exist in the relation
// the key would silently resolve against.
TEST(JoinOnValidation, ForwardReferenceToColumnAbsentFromLeftTreeRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT l.team FROM laps l JOIN drivers d ON d.driver_id = d2.age "
        "JOIN drivers d2 ON l.driver_id = d2.driver_id", cat);
    EXPECT_NE(err.find("joined later"), std::string::npos) << err;
}

// Dispatch site 18. An unqualified name matching no relation is left
// unresolved by the Binder (it does not throw) and slips past
// classifyJoinCondition's unbound-positional branch, so validateJoinCondition
// is what has to catch it. Before Week 26 the AND-chain was rejected as
// "compound" before that function ever ran.
TEST(JoinOnValidation, UnknownColumnInsideMultiKeyConditionRejected) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND nope = b.grp", cat);
    EXPECT_NE(err.find("nope"), std::string::npos) << err;
    EXPECT_NE(err.find("JOIN ON"), std::string::npos) << err;
}

// Site 18's relation list is keyed by the name a qualified ref can actually
// use — the alias when there is one. Keying it by table name made `b` match no
// entry, so the check silently returned and validated nothing for exactly the
// shapes Week 26 adds (a self-join cannot be written without aliases).
TEST(JoinOnValidation, AliasedQualifierIsCheckedAgainstItsRelation) {
    Catalog cat(CATALOG);
    std::string err = validateOnlyJoinError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.nope", cat);
    EXPECT_NE(err.find("nope"), std::string::npos) << err;
    EXPECT_NE(err.find("'b'"), std::string::npos) << err;
}

TEST(JoinOnValidation, AliasedQualifierWithRealColumnAccepted) {
    Catalog cat(CATALOG);
    EXPECT_EQ(validateOnlyJoinError("SELECT a.id FROM sj a JOIN sj b ON a.id = b.grp", cat), "");
}

// A qualified bad column inside the same AND-chain is caught earlier, by the
// Binder — pinned so the two paths stay distinguishable.
TEST(JoinOnValidation, QualifiedUnknownColumnInsideMultiKeyRejectedByBinder) {
    Catalog cat(CATALOG);
    Parser p("SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND a.nope = b.grp");
    auto stmt = p.parse();
    try {
        Binder::bind(stmt, cat);
        FAIL() << "expected an unknown-column error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("nope"), std::string::npos) << e.what();
    }
}

// Week 25 shapes inside ON are still refused on shape, so site 18's new
// branches stay dormant until Week 27 routes residual ON conjuncts.
TEST(JoinOnValidation, Week25NodeInsideOnRejectedOnShape) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND b.val IN (1, 2)", cat);
    EXPECT_NE(err.find("AND-chain of equalities"), std::string::npos) << err;
}

// classifyJoinCondition normalizes operand order: whichever side carries the
// slot of the relation being joined becomes join_col, the other from_col.
TEST(JoinOnValidation, KeysNormalizeOperandOrder) {
    Catalog cat(CATALOG);
    auto stmt = bindQuery("SELECT a.id FROM sj a JOIN sj b ON b.grp = a.id", cat);
    ASSERT_EQ(stmt.joins.size(), 1u);
    std::vector<JoinKey> keys = classifyJoinCondition(stmt.joins[0].condition.get(), 1);
    ASSERT_EQ(keys.size(), 1u);
    EXPECT_EQ(keys[0].from_col, "id");
    EXPECT_EQ(keys[0].join_col, "grp");
    EXPECT_EQ(keys[0].from_slot, 0);
}

TEST(JoinOnValidation, MultiKeyConditionYieldsOneKeyPerConjunct) {
    Catalog cat(CATALOG);
    auto stmt = bindQuery(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND b.grp = a.grp", cat);
    std::vector<JoinKey> keys = classifyJoinCondition(stmt.joins[0].condition.get(), 1);
    ASSERT_EQ(keys.size(), 2u);
    EXPECT_EQ(keys[0].from_col, "id");
    EXPECT_EQ(keys[0].join_col, "id");
    EXPECT_EQ(keys[1].from_col, "grp");   // normalized from `b.grp = a.grp`
    EXPECT_EQ(keys[1].join_col, "grp");
}

// classifyJoinCondition's operand check fires first, so this is what the user
// sees. validateJoinCondition's AggregateExpr branch is a Week 27 pre-position
// for when residual ON conjuncts make that function the only column check —
// asserting the specific message keeps the two apart. Without it this test
// would pass with the whole branch deleted.
TEST(JoinOnValidation, AggregateInsideJoinConditionRejectedOnShape) {
    Catalog cat(CATALOG);
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = SUM(b.grp)", cat);
    EXPECT_NE(err.find("both sides of the join equality must be column references"),
              std::string::npos) << err;
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


// ===== Week 26: N-relation range table =====

// The range table is positional: FROM = 0, then each JOIN in written order.
// Everything downstream (merged schema stamping, join-key routing, pushdown)
// derives from that, so it is asserted directly.
TEST(Binder, ThreeRelationsGetAscendingSlots) {
    Catalog cat(CATALOG);
    auto stmt = bindQuery(
        "SELECT a.id, b.grp, c.val FROM sj a "
        "JOIN sj b ON a.grp = b.id JOIN sj c ON b.grp = c.id", cat);
    ASSERT_EQ(stmt.select_list.size(), 3u);
    EXPECT_EQ(dynamic_cast<ColumnRef*>(stmt.select_list[0].get())->relation_slot, 0);
    EXPECT_EQ(dynamic_cast<ColumnRef*>(stmt.select_list[1].get())->relation_slot, 1);
    EXPECT_EQ(dynamic_cast<ColumnRef*>(stmt.select_list[2].get())->relation_slot, 2);
}

// The duplicate-name check must compare against every prior entry: this clash
// is between range-table entries 0 and 2, which never form the [0]/[1] pair
// the two-relation check looked at.
TEST(Binder, DuplicateAliasAcrossNonAdjacentRelationsRejected) {
    Catalog cat(CATALOG);
    Parser p("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id JOIN sj a ON b.grp = a.id");
    auto stmt = p.parse();
    try {
        Binder::bind(stmt, cat);
        FAIL() << "expected a duplicate-reference error";
    } catch (const std::runtime_error& e) {
        std::string err = e.what();
        // every relation IS aliased here; the fault is that `a` is used twice,
        // so telling the user to add aliases would be advice they already took
        EXPECT_NE(err.find("duplicate table alias"), std::string::npos) << err;
        EXPECT_NE(err.find("'a'"), std::string::npos) << err;
    }
}

// The sibling diagnostic: nothing is aliased, so "add aliases" is the fix.
TEST(Binder, AliaslessSelfJoinAcrossThreeRelationsAsksForAliases) {
    Catalog cat(CATALOG);
    Parser p("SELECT id FROM sj JOIN drivers ON sj.id = drivers.driver_id "
             "JOIN sj ON drivers.driver_id = sj.grp");
    auto stmt = p.parse();
    try {
        Binder::bind(stmt, cat);
        FAIL() << "expected a duplicate-reference error";
    } catch (const std::runtime_error& e) {
        std::string err = e.what();
        EXPECT_NE(err.find("self-join requires table aliases"), std::string::npos) << err;
        EXPECT_NE(err.find("'sj'"), std::string::npos) << err;
    }
}

// An unqualified name matching several relations is ambiguous at any relation
// count — more likely with three, so it is pinned.
TEST(Binder, UnqualifiedColumnAcrossThreeRelationsIsAmbiguous) {
    Catalog cat(CATALOG);
    Parser p("SELECT val FROM sj a JOIN sj b ON a.grp = b.id JOIN sj c ON b.grp = c.id");
    auto stmt = p.parse();
    EXPECT_THROW(Binder::bind(stmt, cat), std::runtime_error);
}


// ===== Week 24: ORDER BY alias substitution =====

TEST(Binder, OrderByAliasSubstitutesSelectExpression) {
    Catalog cat(CATALOG);
    Parser parser("SELECT speed * 2 AS ds FROM laps ORDER BY ds DESC");
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);

    // the ORDER BY item is no longer the bare ColumnRef 'ds' — it is a clone
    // of the aliased expression, evaluable against the pre-projection schema
    ASSERT_EQ(stmt.order_by.size(), 1u);
    auto* mul = dynamic_cast<BinaryExpr*>(stmt.order_by[0].expr.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, "*");
    auto* col = dynamic_cast<ColumnRef*>(mul->left.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column_name, "speed");
    EXPECT_EQ(col->relation_slot, 0);   // clone carries binder stamps
}

TEST(Binder, OrderByRealColumnNotShadowedWithoutAlias) {
    Catalog cat(CATALOG);
    Parser parser("SELECT speed AS s, team FROM laps ORDER BY team");
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);

    // 'team' matches no alias — stays a plain bound column ref
    auto* col = dynamic_cast<ColumnRef*>(stmt.order_by[0].expr.get());
    ASSERT_NE(col, nullptr);
    EXPECT_EQ(col->column_name, "team");
}


// ===== Week 24: GROUP BY alias fallback =====

TEST(Binder, GroupByAliasSubstitutesExpression) {
    Catalog cat(CATALOG);
    Parser parser("SELECT season - 2000 AS era, COUNT(*) FROM laps GROUP BY era");
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);

    // 'era' names no input column → falls back to the select alias and
    // becomes an expression group key
    ASSERT_EQ(stmt.group_by.size(), 1u);
    ASSERT_NE(stmt.group_by[0].expr, nullptr);
    auto* bin = dynamic_cast<BinaryExpr*>(stmt.group_by[0].expr.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "-");
}

TEST(Binder, GroupByAliasOfPlainColumnKeepsColumnPath) {
    Catalog cat(CATALOG);
    Parser parser("SELECT team AS t, COUNT(*) FROM laps GROUP BY t");
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);

    ASSERT_EQ(stmt.group_by.size(), 1u);
    EXPECT_EQ(stmt.group_by[0].expr, nullptr);
    EXPECT_EQ(stmt.group_by[0].column_name, "team");
}

TEST(Binder, GroupByInputColumnBeatsAlias) {
    Catalog cat(CATALOG);
    // 'speed' aliases team, but a real input column named speed exists —
    // input columns take precedence in GROUP BY (SQLite scoping)
    Parser parser("SELECT team AS speed, COUNT(*) FROM laps GROUP BY speed");
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);

    EXPECT_EQ(stmt.group_by[0].expr, nullptr);
    EXPECT_EQ(stmt.group_by[0].column_name, "speed");
}
