#include <gtest/gtest.h>

#include "catalog/catalog.h"
#include "common/schema.h"
#include "common/value.h"
#include "parser/parser.h"
#include "planner/binder.h"
#include "planner/cardinality_estimator.h"
#include "planner/join_enumeration.h"
#include "planner/logical_plan.h"
#include "planner/predicate_pushdown.h"
#include "planner/subquery_materialization.h"
#include "planner/vectorized_plan_builder.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// THE INT -> DOUBLE MATERIALIZATION SEAM, SECOND HALF: TYPE, NOT MAGNITUDE.
//
// tests/test_int_double_materialization.cc pins the first half — an integer too
// large to survive the conversion, refused at 1e15. This file pins the half that
// bites at SEVEN.
//
// THE DEFECT. `CASE WHEN c THEN 7 ELSE 0.5 END` infers DOUBLE (inferExprType
// unifies the arms) but evaluate() returns the TAKEN BRANCH VERBATIM, so on the
// rows where the condition holds the runtime Value is the INT 7. Volcano keeps
// it: ProjectNode pushes the Value the evaluator produced. The vectorized path
// cannot — a ColumnVector holds one type — so it stores 7.0. Nothing is lost:
// 7.0 is exactly 7, and %.15g prints it "7". The engines therefore AGREE for as
// long as the column is only rendered.
//
// They stop agreeing the moment another expression reads it, because SwiftQL
// (like SQLite) truncates INT/INT and does not truncate INT/DOUBLE:
//
//   SELECT x / 2 FROM (SELECT CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END AS x
//                      FROM laps WHERE lap_id < 4) t
//     SQLite / Volcano semantics : 0.25, 3,   0.25
//     vectorized, before the fix : 0.25, 3.5, 0.25
//
// and through HAVING it moved a ROW COUNT, where Volcano can adjudicate
// directly: 0 rows in row-Volcano, 0 in SQLite, 1 here.
//
// WHY THE TEST CANNOT LIVE AT appendColumnValue. Refusing every INT that
// narrows into a DOUBLE column would reject `SELECT CASE WHEN c THEN 1 ELSE 0.5
// END`, which is correct today in all four modes and against SQLite. That is a
// passing answer, and moving one is not on offer. The fact that separates the
// two is not about the value at all — it is PLAN SHAPE: is this materialized
// column read by another expression, or is it the query's output? So
// vectorized_plan_builder.cc computes it (collectIntOrigins) and arms the
// column; vec_types.h refuses only on an armed one.
//
// DISCRIMINATION, MEASURED. Every test named ...Refuses... below was run against
// a scratch worktree built from the parent commit (b13a96a) with none of this
// change in it. All SIX failed there, each producing the divergent answer quoted
// in its comment. This file uses nothing but the public planner API precisely so
// that it compiles and runs unchanged against that pre-fix tree — a test that
// cannot be built without the fix cannot be evidence for it.
//
// The tests named Guard... pass BOTH before and after, on purpose. They are not
// evidence the fix works; they are the fence around it, and each one names the
// answer it is keeping still.
//
// EVERY WITNESS IS SMALL. 7 and 0.5 throughout. A large witness would prove the
// wrong thing — the magnitude rule already covers those, and the whole point
// here is that the type is destroyed at a value that survives perfectly.

namespace {

const char* CATALOG = "../tests/data/test_catalog.json";

std::unordered_map<std::string, ColumnarTable> loadColumnar(
        const SelectStatement& stmt, const Catalog& cat) {
    std::unordered_map<std::string, ColumnarTable> tables;
    std::vector<std::string> names;
    collectQueryTables(stmt, names);
    for (const auto& name : names) {
        if (tables.count(name)) continue;
        const auto& m = cat.getTable(name);
        tables.emplace(name, CSVToColumnar::convert(
            CSVLoader::load(m.filepath, m.schema), m.schema));
    }
    return tables;
}

// The CLI's vectorized pipeline. `optimize` mirrors the presence or absence of
// --no-optimize: every claim in this file is made on BOTH, because the standing
// property is `optimized == --no-optimize == SQLite` and an arming pass that
// only fired on one of them would satisfy neither.
std::unique_ptr<VecPlanNode> buildVec(const std::string& sql, const Catalog& cat,
                                      bool optimize) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    auto tables = loadColumnar(stmt, cat);
    auto logical = LogicalPlanBuilder::build(std::move(stmt), cat);
    if (optimize) {
        logical = PredicatePushdown::apply(std::move(logical), cat);
        logical = JoinEnumeration::apply(std::move(logical), cat);
        CardinalityEstimator::estimate(*logical, cat);
    }
    return VectorizedPlanBuilder::build(std::move(logical), std::move(tables), cat);
}

// Rendered text, not Value: what the user sees is the thing the two engines are
// supposed to agree on, and a Value comparison coerces INT against DOUBLE —
// which would call 3 and 3.5 unequal but 7 and 7.0 equal, hiding exactly the
// step this file is about.
std::vector<std::string> runVecText(const std::string& sql, const Catalog& cat,
                                    bool optimize) {
    auto node = buildVec(sql, cat, optimize);
    node->open();
    std::vector<std::string> out;
    while (DataChunk* chunk = node->nextChunk()) {
        const int n = chunk->filter_applied
                          ? static_cast<int>(chunk->sel.indices.size())
                          : chunk->num_rows;
        for (int i = 0; i < n; ++i) {
            const int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
            std::string s;
            for (const auto& cv : chunk->columns) { s += valueAt(cv, r).toString(); s += '|'; }
            out.push_back(std::move(s));
        }
    }
    node->close();
    return out;
}

// Assert the refusal on both optimizer settings. Building alone is not enough —
// the arming decision is made at build time but the refusal fires at
// materialization, so the plan has to be RUN.
void expectRefusedBothWays(const std::string& sql, const Catalog& cat) {
    for (bool optimize : {false, true}) {
        EXPECT_THROW(runVecText(sql, cat, optimize), std::runtime_error)
            << "no refusal with optimize=" << optimize << " for: " << sql;
    }
}

void expectRowsBothWays(const std::string& sql, const Catalog& cat,
                        const std::vector<std::string>& expected) {
    for (bool optimize : {false, true}) {
        std::vector<std::string> got;
        ASSERT_NO_THROW(got = runVecText(sql, cat, optimize))
            << "unexpected refusal with optimize=" << optimize << " for: " << sql;
        EXPECT_EQ(got, expected) << "optimize=" << optimize << " for: " << sql;
    }
}

// tests/data/test_laps_full.csv, laps 1..3: Ferrari(1), McLaren(2), Ferrari(3).
// So `CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END` over `WHERE lap_id < 4` is
// exactly one INT branch among three rows — the smallest input that exercises
// both arms. Every expectation below was taken from SQLite over this same file.
const char* MIXED_BODY =
    "(SELECT CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END AS x "
    " FROM laps WHERE lap_id < 4) t";

class TypeThroughDivision : public ::testing::Test {
protected:
    TypeThroughDivision() : catalog_(CATALOG) {}
    Catalog catalog_;
};

}  // namespace


// ===========================================================================
// 1. The two reproductions
// ===========================================================================

// FAILS WITHOUT THE FIX: no throw, and the middle row reads 3.5.
// Pre-fix run on b13a96a: {"0.25|", "3.5|", "0.25|"}.
// SQLite over the same rows:  0.25, 3, 0.25.
//
// This is the derived-table half. The inner block's VecProject materializes `x`
// through Pass 2's evaluate() — compileNode declines CaseExpr — into a column
// the schema already declared DOUBLE, and the outer block then divides the 7.0
// it reads back out.
TEST_F(TypeThroughDivision, DerivedBodyMixedCaseDividedAboveRefuses) {
    expectRefusedBothWays("SELECT x / 2 FROM " + std::string(MIXED_BODY), catalog_);
}

// FAILS WITHOUT THE FIX: no throw, and the query returns a row.
// Pre-fix run on b13a96a: {"McLaren|7|"} — one row.
// Volcano (row storage) and SQLite: zero rows.
//
// The HAVING half, and the one place Volcano can adjudicate directly, because
// this query needs no derived table. MAX is an order statistic: it returns an
// element of the input domain, so it hands back the argument's own Value — the
// INT 7 — which VecHashAggregateNode::fillChunk then appends into a column
// declared DOUBLE by aggregateResultType. 7/2 is 3 and does not exceed 3; 7.0/2
// is 3.5 and does.
TEST_F(TypeThroughDivision, HavingOverMaxOfAMixedCaseRefusesRatherThanEmittingARow) {
    expectRefusedBothWays(
        "SELECT team, MAX(CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END) AS m "
        "FROM laps WHERE lap_id < 4 GROUP BY team "
        "HAVING MAX(CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END) / 2 > 3",
        catalog_);
}


// ===========================================================================
// 2. One cause, reached four ways
// ===========================================================================

// FAILS WITHOUT THE FIX: no throw; McLaren's value reads 3.5 instead of 3.
// Pre-fix run on b13a96a: {"Ferrari|0.25|", "McLaren|3.5|"}.
// SQLite: Ferrari 0.25, McLaren 3.
//
// Same aggregate origin as the HAVING test, consumed by a PROJECTION instead of
// a filter. If the two reproductions had two causes, this one would need a third
// fix; it needs none, which is the evidence that the aggregate's MIN/MAX arm is a
// single origin and the consumer above it is interchangeable.
TEST_F(TypeThroughDivision, ProjectionDividingAnAggregateOfAMixedCaseRefuses) {
    expectRefusedBothWays(
        "SELECT team, MIN(CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END) / 2 AS m "
        "FROM laps WHERE lap_id < 4 GROUP BY team ORDER BY team",
        catalog_);
}

// FAILS WITHOUT THE FIX: no throw; the middle row reads 0.285714285714286.
// Pre-fix run on b13a96a: {"4|", "0.285714285714286|", "4|"}.
// SQLite: 4.0, 0, 4.0 — `2 / 7` is INTEGER division.
//
// The DIVISOR decides too. A rule that armed only the dividend would leave this
// one silently wrong, which is why taintWalk arms both operands of `/`.
TEST_F(TypeThroughDivision, MixedCaseUsedAsTheDIVISORRefuses) {
    expectRefusedBothWays("SELECT 2 / x FROM " + std::string(MIXED_BODY), catalog_);
}

// FAILS WITHOUT THE FIX: no throw; the middle row reads 3.5.
// Pre-fix run on b13a96a: {"0.25|", "3.5|", "0.25|"}.
// SQLite: 0.25, 3, 0.25 — 7*2 is 14, and 14/4 truncates to 3.
//
// THE TAINT-PROPAGATION TEST, and the reason the arming carries an ORIGIN SET
// rather than a bool. The division is two relations above the CASE, and the
// column it actually reads (`y`) is NOT the one that has to refuse: by the time
// `y` is materialized the value is already the double 14.0 and no INT arrives
// there to refuse. Only `x`, one level further down, ever sees the INT 7. So a
// rule that armed the divided column would fire on nothing and change no answer;
// arming must walk back to the origin.
//
// `*` is deliberately the middle step: it PROPAGATES the type difference (14 vs
// 14.0) without being observable itself — `SELECT x * 2` prints "14" either way,
// and the Guard below pins that it stays legal.
TEST_F(TypeThroughDivision, TaintCrossesAMultiplicationAndANestedDerivedTable) {
    expectRefusedBothWays(
        "SELECT y / 4 FROM (SELECT x * 2 AS y FROM " + std::string(MIXED_BODY) + ") u",
        catalog_);
}

// FAILS WITHOUT THE FIX: no throw, and a row that should not exist survives.
// Pre-fix run on b13a96a: {"7|"} — one row.
// SQLite: zero rows (7/2 is 3, and 3 does not exceed 3).
//
// The second ROW COUNT divergence in this file, and the second one Volcano would
// adjudicate if it could run a derived table.
//
// A WHERE over a derived table, so the consumer is a VecFilterNode rather than a
// project or an aggregate. The refusal still fires because it is armed at the
// PRODUCER — the inner VecProject — and not at whoever reads it. That is what
// makes the rule independent of which operator does the division, including the
// operators this change does not own.
TEST_F(TypeThroughDivision, FilterAboveADerivedTableDividingTheMixedCaseRefuses) {
    expectRefusedBothWays(
        "SELECT x FROM " + std::string(MIXED_BODY) + " WHERE x / 2 > 3", catalog_);
}


// ===========================================================================
// 3. The fence — answers that are right today and must not move
// ===========================================================================

// GUARD (passes before and after). THE named cost of the blanket refusal that
// was rejected: `CASE WHEN c THEN 1 ELSE 0.5 END` is correct in all four modes
// and matches SQLite, because the DOUBLE 1.0 renders "1". A per-value rule at
// appendColumnValue would reject this. This test is the reason the rule is per
// plan shape instead.
TEST_F(TypeThroughDivision, GuardMixedCaseThatIsOnlyProjectedIsUntouched) {
    expectRowsBothWays(
        "SELECT CASE WHEN lap_id = 2 THEN 1 ELSE 0.5 END AS x FROM laps WHERE lap_id < 4",
        catalog_, {"0.5|", "1|", "0.5|"});
    // and at the value this file is named for
    expectRowsBothWays(
        "SELECT CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END AS x FROM laps WHERE lap_id < 4",
        catalog_, {"0.5|", "7|", "0.5|"});
}

// GUARD (passes before and after). The runtime half of the rule. The plan shape
// here is IDENTICAL to the first reproduction — same derived body, same division
// above it — and only the data differs: no row takes the INT branch, so no INT
// ever reaches the column and the query is right today. A plan-time-only refusal
// would reject it. Refusing per (shape AND value) is what keeps it.
TEST_F(TypeThroughDivision, GuardTheSameShapeIsKeptWhenNoRowTakesTheIntBranch) {
    expectRowsBothWays(
        "SELECT x / 2 FROM (SELECT CASE WHEN lap_id = 99 THEN 7 ELSE 0.5 END AS x "
        "FROM laps WHERE lap_id < 4) t",
        catalog_, {"0.25|", "0.25|", "0.25|"});
}

// GUARD (passes before and after). Both arms REAL: nothing narrows, so the
// division is a plain double division and 3.5 is the right answer here — the
// same text the broken engine produced for the INT version. Getting this one
// wrong in either direction (refusing it, or accepting the INT version) is the
// whole difficulty of the bug, so it is pinned next to its twin.
TEST_F(TypeThroughDivision, GuardBothBranchesRealDividesToThreePointFive) {
    expectRowsBothWays(
        "SELECT x / 2 FROM (SELECT CASE WHEN lap_id = 2 THEN 7.0 ELSE 0.5 END AS x "
        "FROM laps WHERE lap_id < 4) t",
        catalog_, {"0.25|", "3.5|", "0.25|"});
}

// GUARD (passes before and after). The operations that are NOT observers, each
// checked against SQLite rather than assumed:
//   +      7+1 is 8 and 7.0+1 is 8.0, and both render "8"
//   >      Value's comparison coerces INT against DOUBLE
//   ORDER BY  7 and 7.0 sort in the same place
// Arming any of these would refuse a query that answers correctly today, which
// is why the rule stops at `/`.
TEST_F(TypeThroughDivision, GuardAdditionComparisonAndOrderByAreNotObservers) {
    expectRowsBothWays("SELECT x + 1 FROM " + std::string(MIXED_BODY),
                       catalog_, {"1.5|", "8|", "1.5|"});
    expectRowsBothWays("SELECT x FROM " + std::string(MIXED_BODY) + " WHERE x > 3",
                       catalog_, {"7|"});
    expectRowsBothWays("SELECT x FROM " + std::string(MIXED_BODY) + " ORDER BY x",
                       catalog_, {"0.5|", "0.5|", "7|"});
}

// GUARD (passes before and after). Nothing about ordinary DOUBLE arithmetic
// moves: no column here can ever hand an INT to a DOUBLE ColumnVector, so
// nothing is armed and the division runs as it always did. This is the shape
// every real query in the suites has, and it is the reason the change is
// invisible to all 1552 oracle and 329 regression entries.
TEST_F(TypeThroughDivision, GuardOrdinaryDoubleDivisionIsUnaffected) {
    expectRowsBothWays(
        "SELECT speed / 2 FROM laps WHERE lap_id < 3", catalog_,
        {"156.225|", "154.455|"});
    expectRowsBothWays(
        "SELECT team, SUM(speed) / COUNT(*) AS manual_avg FROM laps "
        "WHERE lap_id < 4 GROUP BY team ORDER BY team", catalog_,
        {"Ferrari|311.31|", "McLaren|308.91|"});
}
