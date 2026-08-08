#include <gtest/gtest.h>

#include "common/schema.h"
#include "common/value.h"
#include "execution/key_encoding.h"
#include "execution/vec_distinct_node.h"
#include "execution/vec_hash_aggregate_node.h"
#include "execution/vec_project_node.h"
#include "execution/vec_sort_node.h"
#include "execution/vec_types.h"
#include "parser/ast.h"
#include "planner/logical_plan.h"
#include "planner/plan_nodes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <string>
#include <vector>

// THE INT -> DOUBLE MATERIALIZATION SEAM (seam audit pass 3, E-10).
//
// THE DEFECT. Only ONE of the two engines turns a `Value` back into typed
// columnar storage. `VecProjectNode` and its five siblings rebuild every output
// cell into a `ColumnVector` whose type comes from the SCHEMA, so a value whose
// runtime type is INT under a schema that says DOUBLE was silently converted.
// Volcano has no such step at all — `ProjectNode::next` pushes the `Value` the
// evaluator produced, and the schema never touches it. `vec_types.h` called that
// conversion "lossless". It is not:
//
//   SELECT CASE WHEN round > 10 THEN 9007199254740993 ELSE 0.5 END FROM laps
//     volcano / SQLite : 9007199254740993
//     vectorized       : 9.00719925474099e+15   <- 9007199254740992, another integer
//
// and under DISTINCT it changed a ROW COUNT (3 rows became 2), because two
// distinct inputs landed on one double. Worse, the vectorized path CONTRADICTED
// ITSELF on the same expression: `COUNT(DISTINCT e)` builds its key from the
// pre-conversion `Value` and answered 3 while `SELECT DISTINCT e` answered 2.
//
// WHY REFUSAL AND NOT AGREEMENT. There is no lossless common type. SwiftQL has
// INT (int64_t), DOUBLE and STRING; a mixed-branch CASE genuinely produces both
// numeric types per row, and a chunk column holds exactly one. Reproducing
// SQLite here would mean per-row types inside a ColumnVector. So the fix makes
// the conversion REFUSE at the magnitude where it becomes observable, which is
// the one thing that turns a silent wrong answer into a loud one without moving
// any answer that is right today.
//
// DISCRIMINATION, MEASURED, NOT ASSERTED. Every test below was run in a scratch
// worktree of this same tree with `narrowToDoubleColumn` reverted to a bare
// `return v.toNumeric();` — byte-for-byte the semantics the vectorized path had
// before this change. 8 of the 12 FAILED there, each with the divergence the
// audit recorded, and the observed text is quoted in the comment above each one.
//
// The other 4 pass both before and after ON PURPOSE and are named so:
// `ThresholdIsExactlyTheBoundary` derives the constant rather than testing the
// fix (it cannot fail pre-fix — the constant does not exist there), and the
// three `Guard*` tests pin what the fix must NOT move. Neither kind is offered
// as evidence the fix works. Eight is the number that discriminates.

namespace {

Schema schemaOf(std::initializer_list<std::pair<std::string, TypeId>> cols) {
    std::vector<ColumnDef> defs;
    for (auto& [name, type] : cols) defs.push_back({name, type});
    return Schema(defs);
}

std::unique_ptr<Expr> colRef(std::string name) {
    auto c = std::make_unique<ColumnRef>();
    c->column_name = std::move(name);
    return c;
}

std::unique_ptr<Expr> lit(Value v) {
    return std::make_unique<Literal>(std::move(v));
}

std::unique_ptr<Expr> binOp(std::string op, std::unique_ptr<Expr> l,
                            std::unique_ptr<Expr> r) {
    auto b = std::make_unique<BinaryExpr>();
    b->op = std::move(op);
    b->left = std::move(l);
    b->right = std::move(r);
    return b;
}

// CASE WHEN k = <when[i].first> THEN <when[i].second> ... ELSE <else_val> END
//
// The shape that reaches the seam: inferExprType unifies mixed numeric branches
// to DOUBLE, ExpressionExecutor::compileNode declines CaseExpr outright (it
// cannot reproduce the short-circuit), so the value is produced by the SHARED
// evaluate() — identically in both engines — and only then converted.
std::unique_ptr<Expr> mixedCase(const std::string& key_col,
                                std::vector<std::pair<int64_t, Value>> when,
                                Value else_val) {
    auto c = std::make_unique<CaseExpr>();
    for (auto& [k, result] : when) {
        CaseExpr::WhenClause w;
        w.condition = binOp("=", colRef(key_col), lit(Value(k)));
        w.result = lit(std::move(result));
        c->when_clauses.push_back(std::move(w));
    }
    c->else_expr = lit(std::move(else_val));
    return c;
}

// A source of literal rows for either engine, so the two legs differ only in the
// operators under test. Volcano's leg is a vector<Row> replayed by iterator;
// the vectorized leg is the same rows packed into chunks.
class RowSource : public PlanNode {
public:
    RowSource(Schema schema, std::vector<Row> rows)
        : schema_(std::move(schema)), rows_(std::move(rows)) {}
    void open() override { cursor_ = 0; }
    Row* next() override {
        if (cursor_ >= static_cast<int>(rows_.size())) return nullptr;
        return &rows_[cursor_++];
    }
    void close() override {}
    const Schema& outputSchema() const override { return schema_; }
    std::string explain() const override { return "RowSource"; }
    std::vector<PlanNode*> children() const override { return {}; }

private:
    Schema schema_;
    std::vector<Row> rows_;
    int cursor_ = 0;
};

class VecRowSource : public VecPlanNode {
public:
    VecRowSource(Schema schema, std::vector<Row> rows, int chunk_rows = 1024)
        : schema_(std::move(schema)), rows_(std::move(rows)),
          chunk_rows_(chunk_rows) {}
    void open() override { cursor_ = 0; }
    DataChunk* nextChunk() override {
        if (cursor_ >= static_cast<int>(rows_.size())) return nullptr;
        const int n =
            std::min(chunk_rows_, static_cast<int>(rows_.size()) - cursor_);
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
    std::string explain() const override { return "VecRowSource"; }
    std::vector<VecPlanNode*> children() const override { return {}; }

private:
    Schema schema_;
    std::vector<Row> rows_;
    int chunk_rows_;
    int cursor_ = 0;
    DataChunk chunk_;
};

std::vector<Row> drainVolcano(PlanNode& node) {
    node.open();
    std::vector<Row> out;
    while (Row* r = node.next()) out.push_back(*r);
    node.close();
    return out;
}

std::vector<Row> drainVec(VecPlanNode& node) {
    node.open();
    std::vector<Row> out;
    while (DataChunk* chunk = node.nextChunk()) {
        const int n = chunk->filter_applied
                          ? static_cast<int>(chunk->sel.indices.size())
                          : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            const int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            Row row;
            for (const auto& cv : chunk->columns) row.push_back(valueAt(cv, r));
            out.push_back(std::move(row));
        }
    }
    node.close();
    return out;
}

// What the user actually sees. Comparing rendered text rather than Value is
// deliberate: half of this defect is a value that survives the double intact and
// still PRINTS differently (1000000000000001 -> "1e+15"), and a Value-level
// comparison with numeric coercion would call that pair equal.
std::vector<std::string> asText(const std::vector<Row>& rows) {
    std::vector<std::string> out;
    for (const Row& r : rows) {
        std::string s;
        for (const Value& v : r) { s += v.toString(); s += '|'; }
        out.push_back(std::move(s));
    }
    return out;
}

// A key that this engine's DISTINCT would dedup on, for the self-contradiction
// test — the same encoding VecDistinctNode and the aggregate's COUNT(DISTINCT)
// both call.
size_t distinctGroups(const std::vector<Row>& rows) {
    std::set<std::string> seen;
    for (const Row& r : rows) {
        std::string k;
        for (const Value& v : r) appendGroupKeyField(k, v);
        seen.insert(k);
    }
    return seen.size();
}

// keys 0..n-1 in one INT column named "k"
std::vector<Row> keyRows(int n) {
    std::vector<Row> rows;
    for (int i = 0; i < n; ++i) rows.push_back(Row{Value(static_cast<int64_t>(i))});
    return rows;
}

const int64_t TWO_53      = 9007199254740992LL;   // largest exactly-doubled int
const int64_t TWO_53_PLUS = 9007199254740993LL;   // collapses onto TWO_53
const int64_t TEN_15      = 1000000000000000LL;   // %.15g's exponent cliff
const int64_t TEN_15_PLUS = 1000000000000001LL;   // EXACT as a double, wrong text

}  // namespace


// ===========================================================================
// 1. The boundary itself
// ===========================================================================

// DERIVATION, not discrimination. This test does not fail without the fix — the
// constant does not exist without it. Its job is to keep the constant honest
// against the thing it was derived from: `Value::toString()` formats a DOUBLE
// with `%.15g`, and if that precision ever changes, the safe window changes with
// it and THIS test fails rather than the window silently widening.
TEST(IntDoubleMaterialization, ThresholdIsExactlyTheBoundary) {
    auto survives = [](int64_t i) {
        const double d = static_cast<double>(i);
        return static_cast<int64_t>(d) == i &&
               Value(d).toString() == Value(i).toString();
    };

    // The constant is the FIRST magnitude that fails, in both signs.
    EXPECT_FALSE(survives(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN));
    EXPECT_FALSE(survives(-MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN));
    EXPECT_TRUE(survives(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN - 1));
    EXPECT_TRUE(survives(-(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN - 1)));

    // Nothing below it fails, walked rather than argued: every magnitude in a
    // decade ladder plus the neighbourhood of the boundary.
    for (int64_t p = 1; p < MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN; p *= 10) {
        for (int64_t d = -2; d <= 2; ++d) {
            const int64_t i = p + d;
            if (i <= -MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN ||
                i >= MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN) continue;
            EXPECT_TRUE(survives(i))  << "unexpectedly lossy below the bound: " << i;
            EXPECT_TRUE(survives(-i)) << "unexpectedly lossy below the bound: " << -i;
        }
    }
    for (int64_t i = MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN - 2000;
         i < MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN; ++i) {
        EXPECT_TRUE(survives(i)) << "unexpectedly lossy just below the bound: " << i;
    }

    // And the bound is BELOW 2^53, so the text half is the binding one and a
    // single comparison covers value loss too. (If it were the other way round,
    // a value could survive the text test and still be a different integer.)
    EXPECT_LT(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN, TWO_53);
    EXPECT_NE(static_cast<int64_t>(static_cast<double>(TWO_53_PLUS)), TWO_53_PLUS);

    // narrowToDoubleColumn screens on the DOUBLE's magnitude rather than the
    // int64_t's, to keep one out-of-line call off the hot arm. That is only
    // sound if the two tests agree on EVERY int64_t. The argument is that 1e15
    // is a representable double and rounding to nearest is monotonic; the
    // argument is not the evidence. This is.
    const double bound = static_cast<double>(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN);
    auto agree = [&](int64_t i) {
        const double d = static_cast<double>(i);
        const bool by_int = i >= MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN ||
                            i <= -MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN;
        const bool by_double = !(d < bound && d > -bound);
        return by_int == by_double;
    };
    for (int64_t i = MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN - 5000;
         i <= MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN + 5000; ++i) {
        ASSERT_TRUE(agree(i))  << "proxy disagrees at " <<  i;
        ASSERT_TRUE(agree(-i)) << "proxy disagrees at " << -i;
    }
    uint64_t s = 99;
    for (int n = 0; n < 200000; ++n) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        ASSERT_TRUE(agree(static_cast<int64_t>(s))) << "proxy disagrees at " << s;
    }
    for (int64_t i : {INT64_MIN, INT64_MAX, int64_t(0),
                      MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN,
                      -MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN,
                      MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN - 1}) {
        EXPECT_TRUE(agree(i)) << "proxy disagrees at extremum " << i;
    }
}


// ===========================================================================
// 2. The choke point — appendColumnValue, shared by all seven sites
// ===========================================================================

// FAILS WITHOUT THE FIX: no throw. The old code pushed
// static_cast<double>(9007199254740993) == 9007199254740992.0 and returned
// normally, which is the wrong-VALUE half of E-10.
TEST(IntDoubleMaterialization, AppendRefusesTheIntegerItCannotREPRESENT) {
    ColumnVector cv = makeColumnVector(TypeId::DOUBLE);
    EXPECT_THROW(appendColumnValue(cv, Value(TWO_53_PLUS)), std::runtime_error);
    EXPECT_THROW(appendColumnValue(cv, Value(-TWO_53_PLUS)), std::runtime_error);
}

// FAILS WITHOUT THE FIX: no throw. This is the half that starts LOWER than 2^53
// and that a 2^53 witness alone would miss — the double is EXACT here, and the
// divergence is entirely in the rendering (`%.15g` -> "1e+15" against
// std::to_string -> "1000000000000001"). A user comparing output text sees two
// different answers from a value that never changed.
TEST(IntDoubleMaterialization, AppendRefusesTheIntegerItCannotRENDER) {
    ASSERT_EQ(static_cast<int64_t>(static_cast<double>(TEN_15_PLUS)), TEN_15_PLUS)
        << "premise: this value IS exact as a double";
    ColumnVector cv = makeColumnVector(TypeId::DOUBLE);
    EXPECT_THROW(appendColumnValue(cv, Value(TEN_15_PLUS)), std::runtime_error);
    EXPECT_THROW(appendColumnValue(cv, Value(TEN_15)), std::runtime_error);
}

// FAILS WITHOUT THE UNRENDERED RELAXATION (it does not compile there: the state
// does not exist). What it pins is the boundary of the OTHER bound.
//
// The 1e15 constant above is the TEXT bound, and it applies to a column whose
// text is read. Seam pass 4's E-14 ran a query that reads none of it —
// `SELECT MAX(CASE WHEN c THEN 2000000000000000 ELSE 0.5 END) > 1`, which
// Volcano answers `1` — and the text bound refused it for a rendering that never
// happens. When the plan proves the column is never printed
// (IntNarrowing::UNRENDERED) only the VALUE has to survive, and 2^53 is exactly
// where that stops being true.
TEST(IntDoubleMaterialization, ValueBoundIsExactlyTwoToThe53) {
    // The constant IS the last integer that is still its own double, and the
    // next one is not — derived, not asserted.
    auto roundTrips = [](int64_t i) {
        return static_cast<int64_t>(static_cast<double>(i)) == i;
    };
    EXPECT_TRUE(roundTrips(MAX_EXACT_INT_IN_DOUBLE_COLUMN));
    EXPECT_TRUE(roundTrips(-MAX_EXACT_INT_IN_DOUBLE_COLUMN));
    EXPECT_FALSE(roundTrips(MAX_EXACT_INT_IN_DOUBLE_COLUMN + 1));
    EXPECT_FALSE(roundTrips(-(MAX_EXACT_INT_IN_DOUBLE_COLUMN + 1)));
    EXPECT_EQ(MAX_EXACT_INT_IN_DOUBLE_COLUMN, TWO_53);

    // An UNRENDERED column takes values between the two bounds that a RENDERED
    // one refuses — and stops at 2^53 in both signs.
    ColumnVector u = makeColumnVector(TypeId::DOUBLE);
    u.int_narrowing = IntNarrowing::UNRENDERED;
    ASSERT_NO_THROW(appendColumnValue(u, Value(TEN_15)));
    ASSERT_NO_THROW(appendColumnValue(u, Value(TEN_15_PLUS)));
    ASSERT_NO_THROW(appendColumnValue(u, Value(int64_t(2000000000000000LL))));
    ASSERT_NO_THROW(appendColumnValue(u, Value(TWO_53)));
    ASSERT_NO_THROW(appendColumnValue(u, Value(-TWO_53)));
    EXPECT_THROW(appendColumnValue(u, Value(TWO_53_PLUS)), std::runtime_error);
    EXPECT_THROW(appendColumnValue(u, Value(-TWO_53_PLUS)), std::runtime_error);

    // The same values in the DEFAULT state are refused exactly as before, which
    // is what makes the relaxation a plan-shape decision and not a widening.
    ColumnVector r = makeColumnVector(TypeId::DOUBLE);
    EXPECT_THROW(appendColumnValue(r, Value(TEN_15)), std::runtime_error);
    EXPECT_THROW(appendColumnValue(r, Value(TEN_15_PLUS)), std::runtime_error);
    EXPECT_EQ(static_cast<int>(IntNarrowing::RENDERED),
              static_cast<int>(ColumnVector{}.int_narrowing))
        << "RENDERED must stay the default: a mask that is never set has to "
           "behave as the engine did before either refusal existed";

    // And UNRENDERED does NOT relax the TYPE rule: OBSERVABLE is a third state,
    // not a stronger magnitude.
    ColumnVector o = makeColumnVector(TypeId::DOUBLE);
    o.int_narrowing = IntNarrowing::OBSERVABLE;
    EXPECT_THROW(appendColumnValue(o, Value(int64_t(7))), std::runtime_error);
    ASSERT_NO_THROW(appendColumnValue(u, Value(int64_t(7))));
}

// GUARD — passes before and after. The fix must not move a single answer that is
// right today, and every value any shipped dataset or ordinary query produces is
// in this range. Also pins the three conversions that were never lossy: INT into
// an INT column at any magnitude (including above 2^53, where this operator
// MUST NOT throw), DOUBLE into a DOUBLE column, and STRING into a STRING column.
TEST(IntDoubleMaterialization, GuardEverythingBelowTheBoundaryStillRoundTrips) {
    ColumnVector d = makeColumnVector(TypeId::DOUBLE);
    std::vector<Value> ints;
    for (int64_t p = 1; p < MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN; p *= 10) {
        ints.push_back(Value(p));
        ints.push_back(Value(-p));
        ints.push_back(Value(p + 1));
    }
    ints.push_back(Value(int64_t(0)));
    ints.push_back(Value(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN - 1));
    ints.push_back(Value(-(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN - 1)));
    for (const Value& v : ints) ASSERT_NO_THROW(appendColumnValue(d, v));
    for (size_t i = 0; i < ints.size(); ++i) {
        EXPECT_EQ(valueAt(d, static_cast<int>(i)).toString(), ints[i].toString())
            << "conversion became visible below the bound";
    }

    // INT column: no bound applies, the storage is exact.
    ColumnVector n = makeColumnVector(TypeId::INT);
    ASSERT_NO_THROW(appendColumnValue(n, Value(TWO_53_PLUS)));
    ASSERT_NO_THROW(appendColumnValue(n, Value(int64_t(9223372036854775807LL))));
    ASSERT_NO_THROW(appendColumnValue(n, Value::null()));
    EXPECT_EQ(valueAt(n, 0).asInt(), TWO_53_PLUS);
    EXPECT_EQ(valueAt(n, 1).asInt(), 9223372036854775807LL);
    EXPECT_TRUE(valueAt(n, 2).isNull());

    // DOUBLE and STRING columns receiving their own type: untouched. The large
    // and non-finite entries are the ones the guard could wrongly catch — it
    // screens on the DOUBLE's magnitude for speed, so every DOUBLE above the
    // bound reaches the type check, and every one of them must pass. A DOUBLE
    // is not an INT and this rule is only about INTs.
    ColumnVector dd = makeColumnVector(TypeId::DOUBLE);
    ASSERT_NO_THROW(appendColumnValue(dd, Value(1e300)));
    ASSERT_NO_THROW(appendColumnValue(dd, Value(static_cast<double>(TWO_53_PLUS))));
    ASSERT_NO_THROW(appendColumnValue(dd, Value(
        static_cast<double>(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN))));
    ASSERT_NO_THROW(appendColumnValue(dd, Value(-1e300)));
    ASSERT_NO_THROW(appendColumnValue(dd, Value(
        std::numeric_limits<double>::infinity())));
    ASSERT_NO_THROW(appendColumnValue(dd, Value(
        -std::numeric_limits<double>::infinity())));
    ASSERT_NO_THROW(appendColumnValue(dd, Value(
        std::numeric_limits<double>::quiet_NaN())));
    EXPECT_EQ(valueAt(dd, 0).asDouble(), 1e300);
    EXPECT_EQ(valueAt(dd, 2).asDouble(),
              static_cast<double>(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN));
    EXPECT_TRUE(std::isinf(valueAt(dd, 4).asDouble()));
    EXPECT_TRUE(std::isnan(valueAt(dd, 6).asDouble()));
    ColumnVector s = makeColumnVector(TypeId::STRING);
    ASSERT_NO_THROW(appendColumnValue(s, Value(std::string("9007199254740993"))));
    EXPECT_EQ(valueAt(s, 0).asString(), "9007199254740993");
}

// GUARD — passes before and after. The comment on appendColumnValue claims every
// type disagreement OTHER than INT->DOUBLE is already loud. That claim is the
// reason this fix only had to touch one branch, so it is checked here rather
// than believed. (A silent one would be an eighth site of the same class.)
TEST(IntDoubleMaterialization, GuardEveryOtherTypeDisagreementIsAlreadyLoud) {
    ColumnVector n = makeColumnVector(TypeId::INT);
    EXPECT_ANY_THROW(appendColumnValue(n, Value(0.5)));                    // DOUBLE -> INT
    EXPECT_ANY_THROW(appendColumnValue(n, Value(std::string("x"))));       // STRING -> INT
    ColumnVector d = makeColumnVector(TypeId::DOUBLE);
    EXPECT_ANY_THROW(appendColumnValue(d, Value(std::string("x"))));       // STRING -> DOUBLE
    ColumnVector s = makeColumnVector(TypeId::STRING);
    EXPECT_ANY_THROW(appendColumnValue(s, Value(int64_t(1))));             // INT -> STRING
    EXPECT_ANY_THROW(appendColumnValue(s, Value(0.5)));                    // DOUBLE -> STRING
}


// ===========================================================================
// 3. The operators — the two sites that can actually see a type disagreement
// ===========================================================================

// Site 2: VecProjectNode's Pass-2 append, the one a mixed CASE reaches.
//
// FAILS WITHOUT THE FIX: vectorized returns "9.00719925474099e+15|" where
// Volcano returns "9007199254740993|". Neither engine throws, and the two
// disagree — exactly the run recorded in the audit.
TEST(IntDoubleMaterialization, ProjectNeverReturnsAValueVolcanoWouldNot) {
    const Schema in = schemaOf({{"k", TypeId::INT}});
    auto makeExpr = [&] { return mixedCase("k", {{2, Value(TWO_53_PLUS)}}, Value(0.5)); };

    // The output schema is derived the way the planner derives it, not asserted.
    const Schema out = schemaOf({{"c", inferExprType(makeExpr().get(), in)}});
    ASSERT_EQ(out.column(0).type, TypeId::DOUBLE) << "premise: CASE unifies to DOUBLE";

    std::vector<std::unique_ptr<Expr>> ve;  ve.push_back(makeExpr());
    std::vector<std::unique_ptr<Expr>> re;  re.push_back(makeExpr());

    ProjectNode volcano(std::make_unique<RowSource>(in, keyRows(5)), std::move(re), out);
    const std::vector<std::string> expected = asText(drainVolcano(volcano));
    ASSERT_EQ(expected[2], "9007199254740993|") << "premise: Volcano keeps the integer";

    VecProjectNode vec(std::make_unique<VecRowSource>(in, keyRows(5)), std::move(ve), out);
    try {
        EXPECT_EQ(asText(drainVec(vec)), expected);
    } catch (const std::runtime_error&) {
        SUCCEED() << "refused instead of answering differently";
    }
}

// Same site, the render-only half. FAILS WITHOUT THE FIX: vectorized "1e+15|"
// against Volcano's "1000000000000001|". A witness at 2^53 alone would not
// reach this one.
TEST(IntDoubleMaterialization, ProjectNeverRENDERSAValueVolcanoWouldNot) {
    const Schema in  = schemaOf({{"k", TypeId::INT}});
    const Schema out = schemaOf({{"c", TypeId::DOUBLE}});
    auto makeExpr = [&] { return mixedCase("k", {{2, Value(TEN_15_PLUS)}}, Value(0.5)); };

    std::vector<std::unique_ptr<Expr>> ve;  ve.push_back(makeExpr());
    std::vector<std::unique_ptr<Expr>> re;  re.push_back(makeExpr());

    ProjectNode volcano(std::make_unique<RowSource>(in, keyRows(5)), std::move(re), out);
    const std::vector<std::string> expected = asText(drainVolcano(volcano));
    ASSERT_EQ(expected[2], "1000000000000001|");

    VecProjectNode vec(std::make_unique<VecRowSource>(in, keyRows(5)), std::move(ve), out);
    try {
        EXPECT_EQ(asText(drainVec(vec)), expected);
    } catch (const std::runtime_error&) {
        SUCCEED() << "refused instead of answering differently";
    }
}

// Site 1: VecProjectNode's plain-column GATHER. Reached when the output schema
// declares DOUBLE for a column the child stores as INT — the same conversion,
// through the other branch of the same operator.
//
// FAILS WITHOUT THE FIX: 9007199254740992 comes back out of a column that took
// 9007199254740993 in.
TEST(IntDoubleMaterialization, ProjectColumnGatherRefusesTheSameNarrowing) {
    const Schema in  = schemaOf({{"k", TypeId::INT}});
    const Schema out = schemaOf({{"k", TypeId::DOUBLE}});
    std::vector<Row> rows{Row{Value(TWO_53_PLUS)}};

    std::vector<std::unique_ptr<Expr>> ve;  ve.push_back(colRef("k"));
    VecProjectNode vec(std::make_unique<VecRowSource>(in, rows), std::move(ve), out);
    try {
        const std::vector<Row> got = drainVec(vec);
        ASSERT_EQ(got.size(), 1u);
        EXPECT_EQ(got[0][0].toString(), "9007199254740993")
            << "the gather changed the value on its way through the schema";
    } catch (const std::runtime_error&) {
        SUCCEED() << "refused instead of changing it";
    }
}

// Site 5: VecHashAggregateNode's own materialization, reached WITHOUT
// VecProjectNode — MIN/MAX carry the argument Value verbatim while
// aggregateResultType declares the argument's INFERRED type, so a mixed CASE
// argument puts an INT Value under a DOUBLE column a second, independent way.
//
// FAILS WITHOUT THE FIX: vectorized MAX is 9.00719925474099e+15, Volcano's is
// 9007199254740993. Confirmed end to end at HEAD before the fix:
//   SELECT MAX(CASE WHEN lap_id=2 THEN 9007199254740993 ELSE 0.5 END) FROM laps
TEST(IntDoubleMaterialization, AggregateMaterializationRefusesTheSameNarrowing) {
    const Schema in = schemaOf({{"k", TypeId::INT}});
    std::unique_ptr<Expr> arg = mixedCase("k", {{2, Value(TWO_53_PLUS)}}, Value(0.5));

    AggregateSpec spec;
    spec.function = "MAX";
    spec.is_star = false;
    spec.output_name = "m";
    spec.argument = arg.get();
    const Schema out = schemaOf({{"m", inferExprType(arg.get(), in)}});
    ASSERT_EQ(out.column(0).type, TypeId::DOUBLE) << "premise: MAX takes the arg type";

    HashAggregateNode volcano(std::make_unique<RowSource>(in, keyRows(5)), {},
                              {spec}, out);
    const std::vector<std::string> expected = asText(drainVolcano(volcano));
    ASSERT_EQ(expected, (std::vector<std::string>{"9007199254740993|"}));

    VecHashAggregateNode vec(std::make_unique<VecRowSource>(in, keyRows(5)), {},
                             {spec}, out);
    try {
        EXPECT_EQ(asText(drainVec(vec)), expected);
    } catch (const std::runtime_error&) {
        SUCCEED() << "refused instead of answering differently";
    }
}


// ===========================================================================
// 4. The row count, and the self-contradiction
// ===========================================================================

// FAILS WITHOUT THE FIX: 2 rows against Volcano's 3. Two distinct inputs
// (2^53 and 2^53+1) collapse onto one double before VecDistinctNode ever sees
// them, so the dedup is correct on values that are already wrong.
TEST(IntDoubleMaterialization, DistinctRowCountNeverDisagreesWithVolcano) {
    const Schema in  = schemaOf({{"k", TypeId::INT}});
    const Schema out = schemaOf({{"c", TypeId::DOUBLE}});
    auto makeExpr = [&] {
        return mixedCase("k", {{2, Value(TWO_53_PLUS)}, {8, Value(TWO_53)}}, Value(0.5));
    };

    std::vector<std::unique_ptr<Expr>> ve;  ve.push_back(makeExpr());
    std::vector<std::unique_ptr<Expr>> re;  re.push_back(makeExpr());

    DistinctNode volcano(std::make_unique<ProjectNode>(
        std::make_unique<RowSource>(in, keyRows(10)), std::move(re), out));
    const std::vector<Row> v_rows = drainVolcano(volcano);
    ASSERT_EQ(v_rows.size(), 3u) << "premise: Volcano keeps the two integers apart";

    VecDistinctNode vec(std::make_unique<VecProjectNode>(
        std::make_unique<VecRowSource>(in, keyRows(10)), std::move(ve), out));
    try {
        EXPECT_EQ(asText(drainVec(vec)), asText(v_rows));
    } catch (const std::runtime_error&) {
        SUCCEED() << "refused instead of losing a row";
    }
}

// THE SHARPEST ONE. Not an engine disagreement — a disagreement WITHIN the
// vectorized engine. `SELECT DISTINCT e` deduped values that VecProjectNode had
// already converted, while `COUNT(DISTINCT e)` in the same engine builds its key
// from the pre-conversion Value through appendGroupKeyField. One engine, one
// expression, two group counts.
//
// FAILS WITHOUT THE FIX: 2 against 3. After it, the DISTINCT leg refuses and the
// COUNT(DISTINCT) leg answers 3 — so the engine gives one answer to the question
// or none, never two.
TEST(IntDoubleMaterialization, SelectDistinctAndCountDistinctDoNotContradict) {
    const Schema in  = schemaOf({{"k", TypeId::INT}});
    const Schema out = schemaOf({{"c", TypeId::DOUBLE}});
    auto makeExpr = [&] {
        return mixedCase("k", {{2, Value(TWO_53_PLUS)}, {8, Value(TWO_53)}}, Value(0.5));
    };

    // Leg B first: COUNT(DISTINCT e) on the vectorized engine.
    std::unique_ptr<Expr> arg = makeExpr();
    AggregateSpec spec;
    spec.function = "COUNT";
    spec.is_star = false;
    spec.distinct = true;
    spec.output_name = "n";
    spec.argument = arg.get();
    const Schema agg_out = schemaOf({{"n", TypeId::INT}});
    VecHashAggregateNode counter(std::make_unique<VecRowSource>(in, keyRows(10)),
                                 {}, {spec}, agg_out);
    const std::vector<Row> counted = drainVec(counter);
    ASSERT_EQ(counted.size(), 1u);
    const int64_t count_distinct = counted[0][0].asInt();
    EXPECT_EQ(count_distinct, 3) << "COUNT(DISTINCT) was already right";

    // Leg A: SELECT DISTINCT e on the SAME engine.
    std::vector<std::unique_ptr<Expr>> ve;  ve.push_back(makeExpr());
    VecDistinctNode distinct(std::make_unique<VecProjectNode>(
        std::make_unique<VecRowSource>(in, keyRows(10)), std::move(ve), out));
    try {
        const std::vector<Row> rows = drainVec(distinct);
        EXPECT_EQ(static_cast<int64_t>(distinctGroups(rows)), count_distinct)
            << "the same engine answered the same question two ways";
    } catch (const std::runtime_error&) {
        SUCCEED() << "the DISTINCT leg refuses; nothing left to contradict";
    }
}


// ===========================================================================
// 5. The five sites that re-materialize from a chunk
// ===========================================================================

// GUARD — passes before and after, and that is the verdict it records.
//
// VecSort, VecDistinct, VecHashJoin and VecSimdLoopJoin rebuild their output
// columns from Values they READ OUT of a child chunk, using the CHILD's schema.
// The child's chunk columns are built to that same schema, so the declared type
// already equals the Value's type and the conversion is the identity — those
// four sites are structurally incapable of the loss, whatever passes through
// them. This test is the evidence: an int64_t above 2^53 in an INT column goes
// through sort and distinct and comes out unchanged, and nothing throws.
//
// (If it ever DID throw, that would mean a chunk's column type had drifted from
// its schema, which is a far larger bug than E-10 — so the test is also the
// tripwire for that.)
TEST(IntDoubleMaterialization, GuardChunkRoundTripPreservesLargeIntegers) {
    const Schema in = schemaOf({{"k", TypeId::INT}});
    std::vector<Row> rows{Row{Value(TWO_53_PLUS)}, Row{Value(TWO_53)},
                          Row{Value(TEN_15_PLUS)}, Row{Value(TWO_53_PLUS)}};

    std::vector<OrderByItem> order;
    order.push_back({colRef("k"), false});
    VecSortNode sorter(std::make_unique<VecRowSource>(in, rows, 2), std::move(order));
    std::vector<Row> sorted;
    ASSERT_NO_THROW(sorted = drainVec(sorter));
    EXPECT_EQ(asText(sorted),
              (std::vector<std::string>{"1000000000000001|", "9007199254740992|",
                                        "9007199254740993|", "9007199254740993|"}));

    VecDistinctNode dedup(std::make_unique<VecRowSource>(in, rows, 2));
    std::vector<Row> distinct;
    ASSERT_NO_THROW(distinct = drainVec(dedup));
    EXPECT_EQ(distinct.size(), 3u) << "2^53 and 2^53+1 must stay two groups";
}
