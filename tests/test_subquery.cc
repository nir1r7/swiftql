#include <gtest/gtest.h>
#include "planner/subquery_materialization.h"
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

TEST(SubqueryMaterialization, InBecomesAConstantListAndDedupes) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT name FROM drivers WHERE driver_id IN "
                         "(SELECT driver_id FROM laps)", cat);
    materializeSubqueries(stmt, canned({oneCol("driver_id", TypeId::INT),
                                        {Row{Value(int64_t(3))}, Row{Value(int64_t(3))},
                                         Row{Value(int64_t(7))}}}));
    auto* in = dynamic_cast<const InExpr*>(whereOf(stmt));
    ASSERT_NE(in, nullptr);
    EXPECT_FALSE(in->negated);
    // deduped: evaluate()'s InExpr scans this list LINEARLY per row
    ASSERT_EQ(in->values.size(), 2u);
    EXPECT_EQ(in->values[0].asInt(), 3);
    EXPECT_EQ(in->values[1].asInt(), 7);
}

// THE classic NOT IN defect. `x NOT IN (S ∪ {NULL})` is FALSE where x matches
// and UNKNOWN elsewhere — never TRUE — so the predicate selects nothing.
// Returning rows here is a wrong answer SQLite disagrees with.
TEST(SubqueryMaterialization, NotInWithANullInTheSetIsNeverTrue) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT name FROM drivers WHERE driver_id NOT IN "
                         "(SELECT driver_id FROM laps)", cat);
    materializeSubqueries(stmt, canned({oneCol("driver_id", TypeId::INT),
                                        {Row{Value(int64_t(3))}, Row{Value::null()}}}));
    const Literal* lit = asLiteral(whereOf(stmt));
    ASSERT_NE(lit, nullptr) << "a NOT IN over a NULL-bearing set is a constant, "
                               "not an InExpr over the non-null values";
    EXPECT_EQ(lit->value.asInt(), 0);
}

// The positive form is unaffected: a match is TRUE, and a non-match is UNKNOWN
// rather than FALSE, which every consumer reachable from WHERE/HAVING treats
// identically. So the NULL is simply dropped from the set.
TEST(SubqueryMaterialization, InWithANullInTheSetKeepsTheNonNullValues) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT name FROM drivers WHERE driver_id IN "
                         "(SELECT driver_id FROM laps)", cat);
    materializeSubqueries(stmt, canned({oneCol("driver_id", TypeId::INT),
                                        {Row{Value(int64_t(3))}, Row{Value::null()}}}));
    auto* in = dynamic_cast<const InExpr*>(whereOf(stmt));
    ASSERT_NE(in, nullptr);
    ASSERT_EQ(in->values.size(), 1u);
    EXPECT_EQ(in->values[0].asInt(), 3);
}

// InExpr::values is documented non-empty, so an empty set cannot be represented
// as one. x IN () is FALSE; x NOT IN () is TRUE even for a NULL x.
TEST(SubqueryMaterialization, AnEmptySetBecomesAConstantOfTheRightPolarity) {
    Catalog cat(CATALOG);
    struct Case { const char* sql; int64_t expected; };
    const Case cases[] = {
        {"SELECT name FROM drivers WHERE driver_id IN (SELECT driver_id FROM laps)",     0},
        {"SELECT name FROM drivers WHERE driver_id NOT IN (SELECT driver_id FROM laps)", 1},
    };
    for (const Case& c : cases) {
        auto stmt = bindOnly(c.sql, cat);
        materializeSubqueries(stmt, canned({oneCol("driver_id", TypeId::INT), {}}));
        const Literal* lit = asLiteral(whereOf(stmt));
        ASSERT_NE(lit, nullptr) << c.sql;
        EXPECT_EQ(lit->value.asInt(), c.expected) << c.sql;
    }
}

// A set of only NULLs is empty for the positive form (UNKNOWN, which collapses
// to FALSE) and never-TRUE for the negated one. Both are constants, and neither
// may become an InExpr with a NULL in `values`.
TEST(SubqueryMaterialization, ASetOfOnlyNullsIsAConstantInBothPolarities) {
    Catalog cat(CATALOG);
    for (const char* sql : {"SELECT name FROM drivers WHERE driver_id IN "
                            "(SELECT driver_id FROM laps)",
                            "SELECT name FROM drivers WHERE driver_id NOT IN "
                            "(SELECT driver_id FROM laps)"}) {
        auto stmt = bindOnly(sql, cat);
        materializeSubqueries(stmt, canned({oneCol("driver_id", TypeId::INT),
                                            {Row{Value::null()}}}));
        const Literal* lit = asLiteral(whereOf(stmt));
        ASSERT_NE(lit, nullptr) << sql;
        EXPECT_EQ(lit->value.asInt(), 0) << sql;
    }
}

// evaluate()'s InExpr compares the list LINEARLY per row, so an unbounded
// materialized set turns a subquery into an O(rows x |set|) scan on the
// correctness baseline. Declining loudly names the week that removes the bound.
TEST(SubqueryMaterialization, AnOversizedInSetIsRefusedAndNamesWeek32) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT name FROM drivers WHERE driver_id IN "
                         "(SELECT driver_id FROM laps)", cat);
    std::vector<Row> rows;
    for (int i = 0; i <= MAX_MATERIALIZED_IN_VALUES; ++i) rows.push_back(Row{Value(int64_t(i))});
    try {
        materializeSubqueries(stmt, canned({oneCol("driver_id", TypeId::INT), rows}));
        ADD_FAILURE() << "expected the materialization-limit refusal";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("materialization limit"), std::string::npos) << e.what();
        EXPECT_NE(std::string(e.what()).find("Week 32"), std::string::npos) << e.what();
    }
}

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
    auto stmt = bindOnly("SELECT name FROM drivers WHERE driver_id IN "
                         "(SELECT driver_id FROM laps WHERE speed > "
                         " (SELECT AVG(speed) FROM laps))", cat);
    std::vector<bool> nested_flag;
    materializeSubqueries(stmt, [&](SelectStatement body) {
        nested_flag.push_back(body.has_subquery);
        return SubqueryResult{oneCol("driver_id", TypeId::INT), {Row{Value(int64_t(1))}}};
    });
    ASSERT_EQ(nested_flag.size(), 2u);
    for (bool f : nested_flag) EXPECT_FALSE(f) << "a body must be materialized before it runs";
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

    try {
        Validator::validate(stmt, cat);
        ADD_FAILURE() << "expected the Week 33 correlated-subquery refusal";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("correlated subqueries are not yet executable (Week 33)"),
                  std::string::npos) << e.what();
    }
}

TEST(SubqueryMaterialization, AnUncorrelatedQueryNoLongerCarriesTheFlag) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)", cat);
    EXPECT_TRUE(stmt.has_subquery);
    EXPECT_FALSE(stmt.has_correlated_subquery);
    EXPECT_NO_THROW(Validator::validate(stmt, cat));
}
