#include <gtest/gtest.h>
#include "planner/subquery_materialization.h"
#include "planner/subquery_lowering.h"
#include "planner/logical_plan.h"
#include "planner/binder.h"
#include "planner/validator.h"
#include "parser/parser.h"
#include "parser/expr_utils.h"
#include "catalog/catalog.h"
#include "common/schema.h"
#include <algorithm>
#include <memory>
#include <string>
#include <functional>
#include <vector>

// Week 31 — uncorrelated subqueries, materialized once and substituted.
//
// The rewrite is tested with a FAKE RUNNER: no catalog data, no CSV, no engine.
// That is the whole reason the runner is injected — the semantics under test
// here (cardinality, three-valued IN, the run-once cache) are decisions about a
// result set, not about how the result set was produced.

static const char* CATALOG = "../tests/data/test_catalog.json";

namespace {

SelectStatement bindOnly(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    return stmt;
}

Schema oneCol(const std::string& name, TypeId t) {
    return Schema(std::vector<ColumnDef>{{name, t}});
}

// A runner that always returns the same canned result, counting its calls.
SubqueryRunner canned(SubqueryResult res, int* calls = nullptr) {
    return [res, calls](SelectStatement) {
        if (calls) ++(*calls);
        return res;
    };
}

const Literal* asLiteral(const Expr* e) { return dynamic_cast<const Literal*>(e); }

// The WHERE of `... WHERE <predicate>` after materialization.
const Expr* whereOf(const SelectStatement& stmt) { return stmt.where.get(); }

} // namespace

// ===== scalar =====

TEST(SubqueryMaterialization, ScalarBecomesTheValueItReturned) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)", cat);
    materializeSubqueries(stmt, canned({oneCol("AVG(speed)", TypeId::DOUBLE),
                                        {Row{Value(312.5)}}}));

    auto* bin = dynamic_cast<const BinaryExpr*>(whereOf(stmt));
    ASSERT_NE(bin, nullptr);
    const Literal* lit = asLiteral(bin->right.get());
    ASSERT_NE(lit, nullptr) << "the subquery must become a Literal, "
                               "which is what puts the predicate back into the "
                               "ColumnRef-op-Literal shape three fast paths match on";
    EXPECT_DOUBLE_EQ(lit->value.asDouble(), 312.5);
    EXPECT_FALSE(stmt.has_subquery);
}

// The checkpoint's runtime check. ARITY (more than one COLUMN) is decidable at
// bind time and is the Validator's; CARDINALITY needs data.
TEST(SubqueryMaterialization, ScalarReturningMoreThanOneRowIsAnError) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > (SELECT speed FROM laps)", cat);
    try {
        materializeSubqueries(stmt, canned({oneCol("speed", TypeId::DOUBLE),
                                            {Row{Value(1.0)}, Row{Value(2.0)}}}));
        ADD_FAILURE() << "expected the scalar cardinality error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("scalar subquery returned more than one row"),
                  std::string::npos) << e.what();
    }
}

// Zero rows and one NULL row are DIFFERENT result sets that SQL gives the same
// answer: NULL. A materialization cache that only handles the first returns a
// bogus value for the second.
TEST(SubqueryMaterialization, ScalarWithNoRowsAndScalarWithANullRowBothBecomeTypedNull) {
    Catalog cat(CATALOG);
    const Schema& laps = cat.getTable("laps").schema;

    for (const std::vector<Row>& rows : {std::vector<Row>{},
                                         std::vector<Row>{Row{Value::null()}}}) {
        auto stmt = bindOnly(
            "SELECT team FROM laps WHERE speed > (SELECT speed FROM laps WHERE lap_id = -1)", cat);
        materializeSubqueries(stmt, canned({oneCol("speed", TypeId::DOUBLE), rows}));

        auto* bin = dynamic_cast<const BinaryExpr*>(whereOf(stmt));
        ASSERT_NE(bin, nullptr);
        const Literal* lit = asLiteral(bin->right.get());
        ASSERT_NE(lit, nullptr);
        EXPECT_TRUE(lit->value.isNull());
        // The type comes from the body's output column, because Value has no
        // typed null and inferExprType must answer for every node. Typing it
        // from Value::type() throws; typing it INT unconditionally breaks
        // SUBSTRING over a STRING scalar.
        EXPECT_EQ(lit->null_type, TypeId::DOUBLE);
        EXPECT_NO_THROW(inferExprType(whereOf(stmt), laps));
    }
}

TEST(SubqueryMaterialization, ANullScalarKeepsItsTypeAcrossAClone) {
    // cloneExpr is dispatch site 11 and copies a Literal by value; null_type is
    // part of the node's meaning, so dropping it would retype a cloned NULL as
    // INT and make inferExprType disagree with itself across the clone.
    auto lit = std::make_unique<Literal>(Value::null());
    lit->null_type = TypeId::STRING;
    auto copy = cloneExpr(lit.get());
    ASSERT_NE(asLiteral(copy.get()), nullptr);
    EXPECT_EQ(asLiteral(copy.get())->null_type, TypeId::STRING);
}

// ===== EXISTS =====

TEST(SubqueryMaterialization, ExistsAndNotExistsBecomeTheRightConstant) {
    Catalog cat(CATALOG);
    struct Case { const char* sql; bool any_rows; int64_t expected; };
    const Case cases[] = {
        {"SELECT team FROM laps WHERE EXISTS (SELECT * FROM drivers)",      true,  1},
        {"SELECT team FROM laps WHERE EXISTS (SELECT * FROM drivers)",      false, 0},
        {"SELECT team FROM laps WHERE NOT EXISTS (SELECT * FROM drivers)",  true,  0},
        {"SELECT team FROM laps WHERE NOT EXISTS (SELECT * FROM drivers)",  false, 1},
    };
    for (const Case& c : cases) {
        auto stmt = bindOnly(c.sql, cat);
        std::vector<Row> rows;
        if (c.any_rows) rows.push_back(Row{Value(int64_t(1))});
        materializeSubqueries(stmt, canned({oneCol("x", TypeId::INT), rows}));
        const Literal* lit = asLiteral(whereOf(stmt));
        ASSERT_NE(lit, nullptr) << c.sql;
        EXPECT_EQ(lit->value.asInt(), c.expected) << c.sql << " (rows: " << c.any_rows << ")";
    }
}

// EXISTS never reads a value, so the body is capped at one row. NOT EXISTS is
// the same question negated, so it is capped too.
TEST(SubqueryMaterialization, ExistsCapsTheBodyAtOneRowWithoutWideningAnExistingLimit) {
    Catalog cat(CATALOG);
    std::vector<std::optional<int>> seen;
    auto record = [&seen](SelectStatement body) {
        seen.push_back(body.limit);
        return SubqueryResult{oneCol("x", TypeId::INT), {Row{Value(int64_t(1))}}};
    };

    auto a = bindOnly("SELECT team FROM laps WHERE EXISTS (SELECT * FROM drivers)", cat);
    materializeSubqueries(a, record);
    auto b = bindOnly("SELECT team FROM laps WHERE speed > (SELECT age FROM drivers)", cat);
    materializeSubqueries(b, record);

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[0], std::optional<int>(1)) << "EXISTS needs one row";
    EXPECT_EQ(seen[1], std::optional<int>(2))
        << "a scalar needs TWO: 'more than one row' is the cardinality error, and "
           "LIMIT 1 would silently accept a multi-row scalar";
}

// ===== IN, and the three-valued rule =====
//
// Week 32 DELETED this section's four tests rather than adapting them. They
// asserted the shape of a materialized IN — an InExpr with deduped values, the
// NOT-IN-over-a-NULL-set constant, the empty-set polarity — and materialization
// no longer handles IN at all. The RULES did not go away; they moved into the
// probe loops of VecHashJoinNode and HashJoinNode, where a semi-join can express
// them, and they are asserted there and in the four-mode SQLite diff
// (WEEK32_SEMI_JOIN_VEC_ONLY in python_tools/compare_against_sqlite.py). Keeping
// them here against a canned runner would have tested a code path no query can
// reach. What replaces them at THIS layer is the pair below: the node survives
// the pass, and a large result is no longer a refusal.

// Week 32 — materializeSubqueries no longer consumes a Kind::IN node. The
// set-membership form is the one shape lowered to a PLAN NODE (a semi/anti
// join) rather than substituted with a constant, and a plan node cannot come
// out of an AST rewrite. The node must therefore SURVIVE this pass...
TEST(SubqueryMaterialization, LeavesAnInNodeForSemiJoinLowering) {
    Catalog cat(CATALOG);
    // The canned result is deliberately LARGER than Week 31's removed
    // MAX_MATERIALIZED_IN_VALUES cap of 1024. At this layer "the cap is gone"
    // can only mean "the result is never read at all", which is exactly what
    // runs == 0 says; the cap's removal is covered end to end by
    // WEEK32_SEMI_JOIN_VEC_ONLY's first query (10 000 distinct values), which is
    // the one Week 31 could not answer.
    std::vector<Row> big;
    for (int64_t i = 0; i < 4096; ++i) big.push_back(Row{Value(i)});
    for (const char* sql : {"SELECT name FROM drivers WHERE driver_id IN "
                            "(SELECT driver_id FROM laps)",
                            "SELECT name FROM drivers WHERE driver_id NOT IN "
                            "(SELECT driver_id FROM laps)"}) {
        auto stmt = bindOnly(sql, cat);
        int runs = 0;
        materializeSubqueries(stmt, canned({oneCol("driver_id", TypeId::INT), big},
                                           &runs));
        // ...unrun: the body is executed by the semi-join's build side, not here.
        EXPECT_EQ(runs, 0) << sql;
        const auto* sq = dynamic_cast<const SubqueryExpr*>(whereOf(stmt));
        ASSERT_NE(sq, nullptr) << sql;
        EXPECT_EQ(sq->kind, SubqueryExpr::Kind::IN) << sql;
        // ...and has_subquery must stay SET while it does. The flag means "a
        // SubqueryExpr is still in this tree", and buildScanSchema widens to the
        // full schema while it is set — which is what keeps the IN operand, the
        // semi-join's probe key, from being narrowed away.
        EXPECT_TRUE(stmt.has_subquery) << sql;
    }
}

// Week 31's MAX_MATERIALIZED_IN_VALUES cap had a test here — an EXPECT_NO_THROW
// over an EMPTY canned result, which could not fail for two independent reasons:
// the runner is never invoked for a Kind::IN node, and an empty set was under
// the cap anyway. It has been folded into the test above rather than kept as a
// green line that asserts nothing: the >1024-row canned result plus runs == 0 is
// the strongest statement this layer can make about the removal, and the
// executable proof that the cap is gone is the 10 000-distinct-value query in
// WEEK32_SEMI_JOIN_VEC_ONLY, diffed against SQLite.

// ===== the walker, the cache, and the flag =====

// cloneExpr SHARES the statement (SelectStatement is move-only), and BETWEEN
// clones its left operand before binding — so this is two SubqueryExpr nodes
// over ONE statement. Week 30 handed Week 31 the decision explicitly: one
// subplan or two. One run, two substitutions, keyed on the statement address,
// which is the identity exprKey already uses.
TEST(SubqueryMaterialization, RunsASharedStatementExactlyOnce) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT lap_id FROM laps WHERE "
                         "(SELECT MAX(age) FROM drivers) BETWEEN 1 AND 99", cat);
    int runs = 0;
    materializeSubqueries(stmt, canned({oneCol("MAX(age)", TypeId::INT),
                                        {Row{Value(int64_t(42))}}}, &runs));
    EXPECT_EQ(runs, 1);
    EXPECT_FALSE(stmt.has_subquery);
}

// Innermost first: a body cannot run while it still holds a subquery of its own,
// so the statement handed to the runner must already be free of them.
TEST(SubqueryMaterialization, MaterializesTheInnerBodyBeforeRunningTheOuterOne) {
    Catalog cat(CATALOG);
    // A SCALAR outer form since Week 32: an IN node is no longer materialized,
    // so it would contribute no run and the ordering property would be untested.
    auto stmt = bindOnly("SELECT name FROM drivers WHERE driver_id > "
                         "(SELECT MAX(driver_id) FROM laps WHERE speed > "
                         " (SELECT AVG(speed) FROM laps))", cat);
    std::vector<bool> nested_flag;
    materializeSubqueries(stmt, [&](SelectStatement body) {
        nested_flag.push_back(body.has_subquery);
        return SubqueryResult{oneCol("driver_id", TypeId::INT), {Row{Value(int64_t(1))}}};
    });
    ASSERT_EQ(nested_flag.size(), 2u);
    for (bool f : nested_flag) EXPECT_FALSE(f) << "a body must be materialized before it runs";
}

// The same "innermost first" property for the shape the SCALAR test above
// cannot reach: an IN node is not materialized, but its BODY may hold subqueries
// of its own, and those are still this pass's business. Week 32's first cut
// returned from `visit` on Kind::IN before runOnce, which is where the recursion
// lived — so the body's own SCALAR node survived into planning and
// inferExprType (dispatch site 12) threw an INTERNAL-invariant message at legal
// SQL that Week 31 answered. The IN node itself must still be untouched.
TEST(SubqueryMaterialization, MaterializesASubqueryNestedInsideAnInBody) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT name FROM drivers WHERE driver_id IN "
                         "(SELECT driver_id FROM laps WHERE speed > "
                         " (SELECT AVG(speed) FROM laps))", cat);
    int runs = 0;
    materializeSubqueries(stmt, canned({oneCol("AVG(speed)", TypeId::DOUBLE),
                                        {Row{Value(312.5)}}}, &runs));

    // Exactly one run: the nested SCALAR. The IN body itself is run by the
    // semi-join's build side, not here.
    EXPECT_EQ(runs, 1);

    const auto* sq = dynamic_cast<const SubqueryExpr*>(whereOf(stmt));
    ASSERT_NE(sq, nullptr) << "the IN node must still survive the pass";
    EXPECT_EQ(sq->kind, SubqueryExpr::Kind::IN);
    EXPECT_TRUE(stmt.has_subquery);

    // The body must reach LogicalPlanBuilder free of SubqueryExpr nodes: sites
    // 12/13 throw on one, and a semi-join body is planned like any other block.
    ASSERT_NE(sq->subquery, nullptr) << "the IN body must not have been moved out";
    const auto* body_pred = dynamic_cast<const BinaryExpr*>(sq->subquery->where.get());
    ASSERT_NE(body_pred, nullptr);
    const Literal* lit = asLiteral(body_pred->right.get());
    ASSERT_NE(lit, nullptr) << "the body's nested scalar must have been substituted";
    EXPECT_DOUBLE_EQ(lit->value.asDouble(), 312.5);
    EXPECT_FALSE(sq->subquery->has_subquery);
}

// The walker is dispatch site 19 and must reach a subquery inside every
// container node, not only at the top of a predicate.
TEST(SubqueryMaterialization, TheWalkerReachesASubqueryInsideEveryContainerNode) {
    Catalog cat(CATALOG);
    const char* queries[] = {
        "SELECT team FROM laps WHERE CASE WHEN speed > (SELECT AVG(speed) FROM laps) "
        "THEN 1 ELSE 0 END = 1",
        "SELECT team FROM laps WHERE -(SELECT AVG(speed) FROM laps) < speed",
        "SELECT team FROM laps WHERE (SELECT AVG(speed) FROM laps) IS NOT NULL",
        "SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps) + 1",
        "SELECT team, COUNT(*) FROM laps GROUP BY team "
        "HAVING COUNT(*) > (SELECT COUNT(*) FROM drivers)",
        "SELECT team FROM laps WHERE speed > 1 AND speed < (SELECT AVG(speed) FROM laps)",
    };
    for (const char* sql : queries) {
        auto stmt = bindOnly(sql, cat);
        ASSERT_TRUE(stmt.has_subquery) << sql;
        int runs = 0;
        materializeSubqueries(stmt, canned({oneCol("x", TypeId::DOUBLE), {Row{Value(1.0)}}}, &runs));
        EXPECT_EQ(runs, 1) << sql;
        EXPECT_FALSE(stmt.has_subquery) << sql;
    }
}

// Clearing has_subquery is what returns projection pushdown to a subquery query:
// buildScanSchema widens to the full schema while the flag is set (Week 30).
TEST(SubqueryMaterialization, ClearingTheFlagRestoresScanNarrowing) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)", cat);
    const Schema& full = cat.getTable("laps").schema;
    EXPECT_EQ(buildScanSchema(stmt, full).size(), full.size())
        << "before materialization the widening is the conservative answer";

    materializeSubqueries(stmt, canned({oneCol("AVG(speed)", TypeId::DOUBLE),
                                        {Row{Value(1.0)}}}));
    EXPECT_LT(buildScanSchema(stmt, full).size(), full.size())
        << "after it, only team and speed are needed";
}

// A substituted constant under arithmetic must be folded back into the
// ColumnRef-op-Literal shape, or the query loses zone-map pruning, the tight
// comparison loop and range selectivity at once (constant_folding.h).
TEST(SubqueryMaterialization, RefoldsArithmeticAroundTheSubstitutedConstant) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > "
                         "0.5 * (SELECT AVG(speed) FROM laps)", cat);
    materializeSubqueries(stmt, canned({oneCol("AVG(speed)", TypeId::DOUBLE),
                                        {Row{Value(300.0)}}}));
    auto* bin = dynamic_cast<const BinaryExpr*>(whereOf(stmt));
    ASSERT_NE(bin, nullptr);
    const Literal* lit = asLiteral(bin->right.get());
    ASSERT_NE(lit, nullptr) << "the multiply must have folded away";
    EXPECT_DOUBLE_EQ(lit->value.asDouble(), 150.0);
}

// A statement with no subquery must not pay for the pass, and must not change.
TEST(SubqueryMaterialization, IsANoOpWithoutASubquery) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > 300", cat);
    int runs = 0;
    materializeSubqueries(stmt, canned({oneCol("x", TypeId::INT), {}}, &runs));
    EXPECT_EQ(runs, 0);
    EXPECT_NE(dynamic_cast<const BinaryExpr*>(whereOf(stmt)), nullptr);
}

// ===== the tables a nested query needs =====

// main.cc's loader walked from_table + joins only, so a nested table was never
// loaded and the query died with a raw std::out_of_range from table_rows.at().
TEST(SubqueryMaterialization, CollectQueryTablesFindsNestedAndDeduplicates) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT l.team FROM laps l JOIN laps l2 ON l.lap_id = l2.lap_id "
                         "WHERE l.speed > (SELECT AVG(age) FROM drivers)", cat);
    std::vector<std::string> tables;
    collectQueryTables(stmt, tables);
    ASSERT_EQ(tables.size(), 2u) << "laps must appear once despite the self-join";
    EXPECT_EQ(tables[0], "laps");
    EXPECT_EQ(tables[1], "drivers");
}

// ===== the one slot the walkers used to miss =====

// GroupByColumn::expr carries an arbitrary expression (alias substitution from
// the select list puts one there, binder.cc), and it is the one statement
// expression neither walker reached: the table collector missed its nested
// table, and the substitution walk left the node in place for inferExprType to
// throw on. Unreachable through the parser today only because the Validator
// refuses a subquery in the select list — which is a rule in ANOTHER FILE, and
// not depending silently on one is exactly why forEachStatementExpr walks the
// clauses the position rule already forbids. Constructed here the way alias
// substitution would build it, since no legal SQL text produces it.
TEST(SubqueryMaterialization, AGroupKeyExpressionIsWalkedLikeEveryOtherStatementExpr) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > (SELECT AVG(age) FROM drivers) "
                         "GROUP BY team", cat);
    auto* bin = dynamic_cast<BinaryExpr*>(stmt.where.get());
    ASSERT_NE(bin, nullptr);
    ASSERT_EQ(stmt.group_by.size(), 1u);

    // Move the bound subquery out of the WHERE and into the group key, so the
    // ONLY subquery in the statement is the one under test.
    stmt.group_by[0].expr = std::shared_ptr<Expr>(std::move(bin->right));
    stmt.where.reset();

    std::vector<std::string> tables;
    collectQueryTables(stmt, tables);
    EXPECT_NE(std::find(tables.begin(), tables.end(), "drivers"), tables.end())
        << "main.cc loads exactly what this returns; a missed table is an "
           "out_of_range from table_rows.at(), not a diagnostic";

    int runs = 0;
    materializeSubqueries(stmt, canned({oneCol("AVG(age)", TypeId::DOUBLE),
                                        {Row{Value(31.5)}}}, &runs));
    EXPECT_EQ(runs, 1);
    const Literal* lit = asLiteral(stmt.group_by[0].expr.get());
    ASSERT_NE(lit, nullptr) << "the group key still holds a SubqueryExpr, which "
                               "survives into planning for site 12 to throw on";
    EXPECT_DOUBLE_EQ(lit->value.asDouble(), 31.5);
    EXPECT_FALSE(stmt.has_subquery);
}

// ===== the refusal that replaced the blanket one =====

// Correlation is RELATIVE to a block: in Q20's shape the inner subquery is
// correlated to the MIDDLE block, so the top block's node is uncorrelated. The
// statement flag is propagated upward for exactly that reason — without it the
// top-level refusal accepts the query and a nested block refuses it later, after
// the outer levels have already run.
TEST(SubqueryMaterialization, CorrelationPropagatesToTheOutermostStatement) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT name FROM drivers d WHERE d.driver_id IN "
                         "(SELECT l.driver_id FROM laps l WHERE l.speed > "
                         " (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team))", cat);
    EXPECT_TRUE(stmt.has_correlated_subquery);

    const SubqueryExpr* top = dynamic_cast<const SubqueryExpr*>(stmt.where.get());
    ASSERT_NE(top, nullptr);
    EXPECT_FALSE(top->correlated)
        << "the top node is NOT correlated — which is why the flag has to propagate";

    // Week 33 DELETED the Validator refusal this test used to pin, so the
    // assertion is replaced rather than removed: the flag still has to
    // propagate, and what now consumes it is materializeSubqueries, which must
    // LEAVE a correlated node alone instead of running its body once and
    // substituting a constant.
    EXPECT_NO_THROW(Validator::validate(stmt, cat));

    int calls = 0;
    materializeSubqueries(stmt, canned({oneCol("AVG(l2.speed)", TypeId::DOUBLE),
                                        {Row{Value(300.0)}}}, &calls));
    EXPECT_EQ(calls, 0)
        << "a correlated body must never be RUN here: its rows depend on which "
           "outer row selected them, so there is no single value to substitute";
    EXPECT_TRUE(stmt.has_subquery)
        << "the flag means 'a SubqueryExpr is still in this tree', and two are";

    const SubqueryExpr* in_node = dynamic_cast<const SubqueryExpr*>(stmt.where.get());
    ASSERT_NE(in_node, nullptr);
    ASSERT_NE(in_node->subquery, nullptr);
    const auto* body_pred = dynamic_cast<const BinaryExpr*>(in_node->subquery->where.get());
    ASSERT_NE(body_pred, nullptr);
    EXPECT_NE(dynamic_cast<const SubqueryExpr*>(body_pred->right.get()), nullptr)
        << "the correlated scalar must survive the pass; before the fix it was "
           "replaced by Literal(300.0) computed from a body whose l.team was "
           "resolved by bare name against the body's own schema";
}

// The direct form of the same defect, and the one that made the whole of Week
// 33's decorrelation unreachable from the CLI: a correlated EXISTS was consumed
// by materialization before lowerExistsSubqueries could ever see it.
TEST(SubqueryMaterialization, ACorrelatedExistsSurvivesForDecorrelation) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT name FROM drivers d WHERE EXISTS "
                         "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id)", cat);
    const SubqueryExpr* sq = dynamic_cast<const SubqueryExpr*>(stmt.where.get());
    ASSERT_NE(sq, nullptr);
    ASSERT_TRUE(sq->correlated);

    int calls = 0;
    materializeSubqueries(stmt, canned({oneCol("x", TypeId::INT),
                                        {Row{Value(int64_t(1))}}}, &calls));
    EXPECT_EQ(calls, 0) << "the body must not be run for a value";
    EXPECT_TRUE(stmt.has_subquery);
    EXPECT_NE(dynamic_cast<const SubqueryExpr*>(stmt.where.get()), nullptr)
        << "before the fix this was Literal(1): the body ran with d.driver_id "
           "resolved by bare name against laps, making the predicate the "
           "tautology laps.driver_id = laps.driver_id";
}

TEST(SubqueryMaterialization, AnUncorrelatedQueryNoLongerCarriesTheFlag) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)", cat);
    EXPECT_TRUE(stmt.has_subquery);
    EXPECT_FALSE(stmt.has_correlated_subquery);
    EXPECT_NO_THROW(Validator::validate(stmt, cat));
}

// ===== Week 32: set-membership lowering (subquery_lowering.h) =====

namespace {

// Plans a bound statement the way the CLI does — materialize first (which now
// SKIPS the IN node), then build. The runner is never called for an IN-only
// query, which is itself part of what these tests assert.
std::unique_ptr<LogicalPlanNode> planLowered(const std::string& sql, Catalog& cat) {
    auto stmt = bindOnly(sql, cat);
    Validator::validate(stmt, cat);
    materializeSubqueries(stmt, canned({oneCol("x", TypeId::INT), {}}));
    return LogicalPlanBuilder::build(std::move(stmt), cat);
}

const LogicalJoin* findJoin(const LogicalPlanNode* n) {
    if (n->type == LogicalNodeType::JOIN) return static_cast<const LogicalJoin*>(n);
    for (const auto& c : n->children) if (const auto* j = findJoin(c.get())) return j;
    return nullptr;
}

int countJoins(const LogicalPlanNode* n) {
    int k = (n->type == LogicalNodeType::JOIN) ? 1 : 0;
    for (const auto& c : n->children) k += countJoins(c.get());
    return k;
}

} // namespace

// THE invariant of the week. A SEMI/ANTI join emits only left-side columns, so
// its output schema IS its left child's — not a merged one. That is the whole
// containment for the two slot-numbering domains one plan now holds: nothing
// from the body is in scope above the join, so no indexOf(name, slot) above it
// can hit a body column at a body slot. See development.md -> Relation slots.
TEST(SemiJoinLowering, OutputSchemaIsTheLeftChildsAndTheSlotIsMinusOne) {
    Catalog cat(CATALOG);
    auto plan = planLowered("SELECT lap_id FROM laps WHERE driver_id IN "
                            "(SELECT driver_id FROM drivers)", cat);
    const LogicalJoin* j = findJoin(plan.get());
    ASSERT_NE(j, nullptr);
    EXPECT_EQ(j->semantics, JoinSemantics::SEMI);
    // join_type stays INNER: the two fields are read independently, and setting
    // LEFT to get "preserve the left side" would drag in null-extension, the
    // residual match test and the outer max() cardinality rule.
    EXPECT_EQ(j->join_type, JoinType::INNER);
    // -1 means "children[1] is not a relation of this block's range table".
    EXPECT_EQ(j->join_slot, -1);

    // The per-column loop below is a RESTATEMENT, not the enforcement: the same
    // comparison (plus `type`) is asserted inside lowerInSubqueries at
    // construction, so any divergence throws out of planLowered above and this
    // test reports an uncaught std::runtime_error rather than a schema diff. It
    // is kept because it documents the invariant at the layer that reads it; the
    // assertions here that can actually fail are join_slot, join_type and the
    // no-body-slot check at the end.
    const auto& out = j->output_schema.columns();
    const auto& left = j->children[0]->output_schema.columns();
    ASSERT_EQ(out.size(), left.size());
    for (size_t i = 0; i < out.size(); ++i) {
        EXPECT_EQ(out[i].name, left[i].name);
        EXPECT_EQ(out[i].relation_slot, left[i].relation_slot);
    }
    // and no body column leaked into it
    for (const auto& c : out) EXPECT_NE(c.relation_slot, -1);
}

TEST(SemiJoinLowering, NotInBecomesAnAntiJoin) {
    Catalog cat(CATALOG);
    auto plan = planLowered("SELECT lap_id FROM laps WHERE driver_id NOT IN "
                            "(SELECT driver_id FROM drivers)", cat);
    const LogicalJoin* j = findJoin(plan.get());
    ASSERT_NE(j, nullptr);
    // ANTI_NOT_IN, not ANTI: NOT IN is three-valued, and pinning the exact
    // enumerator is what stops the two negations being merged again.
    EXPECT_EQ(j->semantics, JoinSemantics::ANTI_NOT_IN);
    // This line was `EXPECT_NE(plan->explain().find(""), npos)` — find("")
    // returns 0 for every string, so it read as a check and could not fail. The
    // whole-plan needle it implies does not exist either: LogicalPlanNode's
    // explain() prints ONE node, and the CLI is what walks the tree. What is
    // actually worth pinning here is that the negated conjunct produced exactly
    // one join and left nothing else behind; the node's name is
    // ExplainNamesTheKind's.
    EXPECT_EQ(countJoins(plan.get()), 1);
}

// The node NAME carries the kind, so every pre-existing plan string stays
// byte-identical and the substring "Join" survives for the python harness.
TEST(SemiJoinLowering, ExplainNamesTheKind) {
    Catalog cat(CATALOG);
    EXPECT_NE(findJoin(planLowered(
        "SELECT lap_id FROM laps WHERE driver_id IN (SELECT driver_id FROM drivers)",
        cat).get())->explain().find("LogicalSemiJoin ["), std::string::npos);
    EXPECT_NE(findJoin(planLowered(
        "SELECT lap_id FROM laps WHERE driver_id NOT IN (SELECT driver_id FROM drivers)",
        cat).get())->explain().find("LogicalAntiJoin ["), std::string::npos);
}

// Two conjuncts means two semi-joins, and that is CORRECT — they are two
// separate membership tests. Week 31's statement-keyed result cache is
// deliberately not ported; there is nothing to share.
TEST(SemiJoinLowering, TwoInConjunctsProduceTwoStackedJoins) {
    Catalog cat(CATALOG);
    auto plan = planLowered(
        "SELECT lap_id FROM laps WHERE driver_id IN (SELECT driver_id FROM drivers) "
        "AND season IN (SELECT season FROM laps l2)", cat);
    EXPECT_EQ(countJoins(plan.get()), 2);
}

// The remaining conjuncts stay in the WHERE filter, and the IN conjunct is gone
// from it — which is what keeps inferExprType (dispatch site 12) from throwing.
TEST(SemiJoinLowering, TheOtherConjunctsSurviveInTheFilter) {
    Catalog cat(CATALOG);
    auto plan = planLowered("SELECT lap_id FROM laps WHERE speed > 300 AND driver_id IN "
                            "(SELECT driver_id FROM drivers) AND season = 2024", cat);
    const std::string s = plan->explain();
    bool saw_filter = false;
    std::function<void(const LogicalPlanNode*)> walk = [&](const LogicalPlanNode* n) {
        if (n->type == LogicalNodeType::FILTER) {
            saw_filter = true;
            EXPECT_EQ(n->explain().find("IN"), std::string::npos) << n->explain();
        }
        for (const auto& c : n->children) walk(c.get());
    };
    walk(plan.get());
    EXPECT_TRUE(saw_filter);
}

// An uncorrelated EXISTS is NOT re-routed: its value does not depend on the
// outer row at all, so materialization is strictly better and a semi-join would
// turn a foldable constant into a pipeline breaker. No join is added.
TEST(SemiJoinLowering, ExistsStillMaterializesAndAddsNoJoin) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT lap_id FROM laps WHERE EXISTS "
                         "(SELECT * FROM drivers WHERE age > 30)", cat);
    Validator::validate(stmt, cat);
    int runs = 0;
    materializeSubqueries(stmt, canned({oneCol("driver_id", TypeId::INT),
                                        {Row{Value(int64_t(1))}}}, &runs));
    EXPECT_EQ(runs, 1);
    EXPECT_FALSE(stmt.has_subquery);
    auto plan = LogicalPlanBuilder::build(std::move(stmt), cat);
    EXPECT_EQ(countJoins(plan.get()), 0);
}

// THE FOURTH REFUSAL, and the reason it is asserted here and nowhere else.
// planBody MOVES the statement out of the shared_ptr, so two SubqueryExpr nodes
// naming one body would leave the second planning an emptied statement — a
// wrong answer, not an error. The guard is correct; what it lacked was any
// proof that it fires, because no SQL text reaches it: cloneExpr
// (parser/expr_utils.h) is the only producer of a shared body, and no pass in
// the WHERE path clones a conjunct before lowering runs. That makes it
// unreachable TODAY and reachable the moment one does — exactly the guard that
// silently stops guarding if nothing pins it. So the test drives
// lowerInSubqueries directly, building the sharing cloneExpr would build, and
// deliberately does NOT go through planLowered: there is no SQL to write.
TEST(SemiJoinLowering, RefusesABodySharedByTwoExpressions) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT lap_id FROM laps WHERE driver_id IN "
                         "(SELECT driver_id FROM drivers)", cat);
    Validator::validate(stmt, cat);
    ASSERT_NE(dynamic_cast<SubqueryExpr*>(stmt.where.get()), nullptr);

    // cloneExpr copies the shared_ptr, not the statement — SelectStatement holds
    // unique_ptr members and is move-only. Two Exprs, one body, use_count 2.
    std::vector<std::unique_ptr<Expr>> conjuncts;
    conjuncts.push_back(cloneExpr(stmt.where.get()));
    conjuncts.push_back(std::move(stmt.where));
    ASSERT_EQ(static_cast<SubqueryExpr*>(conjuncts[0].get())->subquery.use_count(), 2u);

    // the spine is irrelevant to this refusal: it fires before any key is built
    auto spine = std::make_unique<LogicalScan>(
        "laps", Schema({ColumnDef{"driver_id", TypeId::INT, 0, false}}));
    try {
        lowerInSubqueries(std::move(spine), conjuncts, cat);
        ADD_FAILURE() << "expected a refusal for a body shared by two expressions";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("shared by two expressions"),
                  std::string::npos) << e.what();
    }
}

// The refusals. Each is a stated README dialect-table row and a rejection-suite
// entry, because the diffed oracle suite cannot hold a query that errors.
TEST(SemiJoinLowering, RefusesTheShapesItCannotExpress) {
    Catalog cat(CATALOG);
    const std::pair<const char*, const char*> cases[] = {
        // JoinKey holds column NAMES: there is no computed-key join here
        {"SELECT lap_id FROM laps WHERE season * 5 IN (SELECT driver_id FROM drivers)",
         "must be a column reference"},
        // a semi-join is a whole-conjunct construct
        {"SELECT lap_id FROM laps WHERE driver_id IN (SELECT driver_id FROM drivers) "
         "OR speed > 340",
         "whole top-level WHERE conjunct"},
        // the join would have to sit above LogicalAggregate
        {"SELECT team, COUNT(*) FROM laps GROUP BY team "
         "HAVING COUNT(*) IN (SELECT driver_id FROM drivers)",
         "whole top-level WHERE conjunct"},
    };
    for (const auto& [sql, needle] : cases) {
        try {
            planLowered(sql, cat);
            ADD_FAILURE() << "expected a refusal for: " << sql;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
                << sql << " -> " << e.what();
        }
    }
}

// ===== Week 33 round 2: the shapes decorrelation must REFUSE, not answer =====

// A correlated ref in the BODY's `JOIN ... ON`. splitCorrelation reads
// body.where only; classifyJoinCondition routes this one to out.residuals and
// LogicalPlanBuilder::build folds inner-join residuals into the body's WHERE
// AFTER splitCorrelation has already run and cleared it. It then resolved by
// BARE NAME against the body's merged schema — `d2.team = d.team` became
// `d2.team = laps.team`.
//
// Not a hypothetical: against the committed data the query below returned 5
// where SQLite returns 20, with no error and an identical --explain. A refusal
// by name beats a plausible wrong answer.
TEST(ExistsDecorrelation, ACorrelatedRefInTheBodysOnClauseIsRefusedNotMisresolved) {
    Catalog cat(CATALOG);
    try {
        planLowered("SELECT COUNT(*) FROM drivers d WHERE EXISTS "
                    "(SELECT * FROM laps l JOIN drivers d2 "
                    " ON l.lap_id = d2.driver_id AND d2.team = d.team "
                    " WHERE d2.driver_id = d.driver_id)", cat);
        ADD_FAILURE() << "expected a refusal, not a plan";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("JOIN ... ON clause"), std::string::npos)
            << e.what();
        EXPECT_NE(std::string(e.what()).find("correlated subquery:"), std::string::npos)
            << e.what();
    }
}

// The same guard over the body's SELECT list. Before it, this reached
// buildProjectSchema, whose isResolved() (not isLocal()) guard made localSlot()
// throw an INTERNAL-defect message for a query SQLite accepts (round 1 M-6).
// Refusing it here is not support, but it is honest about which layer declined.
TEST(ExistsDecorrelation, ACorrelatedRefInTheBodysSelectListIsRefusedByName) {
    Catalog cat(CATALOG);
    try {
        planLowered("SELECT COUNT(*) FROM drivers d WHERE EXISTS "
                    "(SELECT d.name FROM laps l WHERE l.driver_id = d.driver_id)", cat);
        ADD_FAILURE() << "expected a refusal, not a plan";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("SELECT list"), std::string::npos) << e.what();
        EXPECT_EQ(std::string(e.what()).find("internal:"), std::string::npos)
            << "a legal query must not report a planner defect: " << e.what();
    }
}

// H-2 (round 1): the body's key was resolved by NAME against the body's OUTPUT
// schema, which buildProjectSchema names by SELECT ALIAS. `l.speed AS driver_id`
// therefore rebound the key `driver_id` to `speed`, and the semi-join probed
// d.driver_id against laps.speed. Verified against SQLite: 0 rows where SQLite
// returns 20. The body is now projected to its key columns, so the alias cannot
// reach the lookup — there is no lookup.
TEST(ExistsDecorrelation, AnAliasInTheBodysSelectListCannotShadowTheJoinKey) {
    Catalog cat(CATALOG);
    auto plan = planLowered("SELECT d.name FROM drivers d WHERE EXISTS "
                            "(SELECT l.speed AS driver_id FROM laps l "
                            " WHERE l.driver_id = d.driver_id)", cat);
    const LogicalJoin* j = findJoin(plan.get());
    ASSERT_NE(j, nullptr);
    ASSERT_EQ(j->keys.size(), 1u);
    const Schema& body = j->children[1]->output_schema;
    ASSERT_EQ(body.size(), 1) << "the body must be projected to its key columns only";
    EXPECT_EQ(body.column(0).name, "driver_id")
        << "and it must be the KEY column, not the aliased one";
    // !! THE TYPE IS WHAT MAKES THIS TEST ABLE TO FAIL (round 2 R2-M1). The alias
    // in the query IS `driver_id`, so on the pre-fix code buildProjectSchema
    // produced a size-1 schema named `driver_id` too and BOTH assertions above
    // passed while the semi-join probed d.driver_id against laps.speed. Only the
    // type separates the two states: the key column l.driver_id is INT, the
    // aliased l.speed is DOUBLE.
    EXPECT_EQ(body.column(0).type, TypeId::INT)
        << "the aliased column is l.speed (DOUBLE); INT proves this is the key";
}

// The interaction round 2 flagged as unaudited: a body ORDER BY must survive the
// select-list replacement. It does, and it is correct rather than lucky --
// LogicalPlanBuilder places Sort BELOW Project, so the sort key resolves against
// the pre-projection schema, and buildScanSchema keeps ORDER BY columns in
// `required`. It could only change the ANSWER together with a LIMIT (a different
// prefix of a different order), and requireDecorrelatableBody refuses a body
// LIMIT outright. All three shapes below match SQLite (20).
TEST(ExistsDecorrelation, ABodyOrderByOutlivesTheSelectListReplacement) {
    Catalog cat(CATALOG);
    for (const char* sql : {"SELECT COUNT(*) FROM drivers d WHERE EXISTS "
                            "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id "
                            " ORDER BY l.speed)",
                            "SELECT COUNT(*) FROM drivers d WHERE EXISTS "
                            "(SELECT l.lap_id FROM laps l WHERE l.driver_id = d.driver_id "
                            " ORDER BY l.speed)"}) {
        std::unique_ptr<LogicalPlanNode> plan;
        ASSERT_NO_THROW(plan = planLowered(sql, cat)) << sql;
        const LogicalJoin* j = findJoin(plan.get());
        ASSERT_NE(j, nullptr) << sql;
        EXPECT_EQ(j->children[1]->output_schema.size(), 1) << sql;
        EXPECT_EQ(j->children[1]->output_schema.column(0).name, "driver_id") << sql;
    }
}

// M-3 (round 1): the correlated conjunct is removed from body.where BEFORE the
// body is planned, so buildScanSchema never saw the key column and narrowed it
// away. `EXISTS (SELECT 1 FROM ...)` — the most idiomatic EXISTS body there is —
// died with "join key 'driver_id' not found on the joined relation".
TEST(ExistsDecorrelation, ABodyThatIsNotSelectStarStillKeepsItsKeyColumn) {
    Catalog cat(CATALOG);
    for (const char* body_sql : {"SELECT 1 FROM drivers d WHERE d.driver_id = l.driver_id",
                                 "SELECT d.name FROM drivers d WHERE d.driver_id = l.driver_id",
                                 "SELECT * FROM drivers d WHERE d.driver_id = l.driver_id"}) {
        const std::string sql =
            std::string("SELECT l.lap_id FROM laps l WHERE EXISTS (") + body_sql + ")";
        std::unique_ptr<LogicalPlanNode> plan;
        ASSERT_NO_THROW(plan = planLowered(sql, cat)) << sql;
        const LogicalJoin* j = findJoin(plan.get());
        ASSERT_NE(j, nullptr) << sql;
        ASSERT_EQ(j->children[1]->output_schema.size(), 1) << sql;
        EXPECT_EQ(j->children[1]->output_schema.column(0).name, "driver_id") << sql;
    }
}

// H-1 (round 1): a body that is a JOIN has a MERGED output schema, and invariant
// 3 makes duplicate column names legal there. indexOf(join_col) took the first
// match, so the probe could run against the wrong relation's column. The body is
// now projected to the key columns before the join is built, so the merged
// schema never reaches a name lookup.
TEST(ExistsDecorrelation, AJoinBodyIsProjectedToItsKeyBeforeTheMergedSchemaCanShadowIt) {
    Catalog cat(CATALOG);
    auto plan = planLowered("SELECT d.name FROM drivers d WHERE EXISTS "
                            "(SELECT * FROM laps l JOIN drivers t "
                            " ON l.lap_id = t.driver_id WHERE t.team = d.team)", cat);
    const LogicalJoin* semi = findJoin(plan.get());
    ASSERT_NE(semi, nullptr);
    EXPECT_EQ(semi->semantics, JoinSemantics::SEMI);
    ASSERT_EQ(semi->children[1]->output_schema.size(), 1)
        << "`team` lives in BOTH body relations; a merged schema here is the bug";
    EXPECT_EQ(semi->children[1]->output_schema.column(0).name, "team");
}

// ===== Week 33 round 2: a correlated IN must not be lowered as if it were not =====

// R2-C1. lowerInSubqueries ran on the WHERE conjuncts THIRTEEN LINES BEFORE
// refuseUnloweredCorrelated, so it consumed a correlated IN the tripwire never
// saw. The semi-join it built carried ONE key -- the IN operand -- and the
// body's correlated equality was planned inside the body, where the outer ref
// fell back to bare name: `l.team = d.team` became `laps.team = laps.team`. The
// correlation was silently discarded and the semi-join degenerated to "does the
// body have any row at all": 20 rows where SQLite says 6.
//
// The assertion is that NO join is built -- a plan here IS the bug -- and that
// the refusal names the IN, not a position.
TEST(InLowering, ACorrelatedInIsRefusedRatherThanLoweredWithoutItsCorrelation) {
    Catalog cat(CATALOG);
    for (const char* sql : {"SELECT COUNT(*) FROM drivers d WHERE d.driver_id IN "
                            "(SELECT l.lap_id FROM laps l WHERE l.team = d.team)",
                            "SELECT COUNT(*) FROM drivers d WHERE d.driver_id NOT IN "
                            "(SELECT l.lap_id FROM laps l WHERE l.team = d.team)"}) {
        try {
            auto plan = planLowered(sql, cat);
            ADD_FAILURE() << "a plan was built for " << sql << "; before the fix it "
                          << "was a one-key semi-join that answered 20 where SQLite "
                          << "says 6 (and 0 where SQLite says 14)"
                          << (findJoin(plan.get()) ? " [and it contains a join]" : "");
        } catch (const std::runtime_error& e) {
            const std::string what = e.what();
            EXPECT_NE(what.find("correlated IN / NOT IN is not lowered"), std::string::npos)
                << what;
            EXPECT_EQ(what.find("whole top-level WHERE conjunct"), std::string::npos)
                << "position is not the reason and must not be named as it: " << what;
        }
    }
}

// The control, and it is what makes the fix a routing change rather than a
// blanket refusal: an UNCORRELATED IN is still lowered to a semi-join.
TEST(InLowering, AnUncorrelatedInIsStillLoweredAfterTheCorrelatedOneIsDeclined) {
    Catalog cat(CATALOG);
    auto plan = planLowered("SELECT lap_id FROM laps WHERE driver_id IN "
                            "(SELECT driver_id FROM drivers)", cat);
    const LogicalJoin* j = findJoin(plan.get());
    ASSERT_NE(j, nullptr);
    EXPECT_EQ(j->semantics, JoinSemantics::SEMI);
}
