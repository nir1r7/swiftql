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
#include "planner/validator.h"
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
// DISCRIMINATION, MEASURED. Every test named ...Refuses... in sections 1 and 2
// below was run against a scratch worktree built from the parent commit
// (b13a96a) with none of this change in it. All SIX failed there, each producing
// the divergent answer quoted in its comment. This file uses nothing but the
// public planner API precisely so that it compiles and runs unchanged against
// that pre-fix tree — a test that cannot be built without the fix cannot be
// evidence for it.
//
// FIX ROUND 4 ADDED SECTIONS 4 TO 7 and re-measured them the same way, against
// 689ea9a: the five cross-cut reproductions and the four over-firing answers all
// failed there, and every Guard passed on both sides. Sections 4 and 5 need the
// `int_type_observable` argument, which does not exist pre-fix, so their
// counterfactual run adapted exactly two call sites (the runner's second
// parameter and build()'s fourth argument) and left the ENGINE untouched. The
// row counts quoted in each comment are from that run, and every expectation in
// this file was re-checked against SQLite over the same five rows.
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

// The CLI's pipeline for a query that CONTAINS A SUBQUERY, which is a different
// pipeline: Validator, then materializeSubqueries (which runs each uncorrelated
// body as its OWN plan through its own VectorizedPlanBuilder::build), then the
// outer build. The two builds share no logical tree, which is the whole subject
// of section 4 below — buildVec above cannot express it, because it never
// materializes.
std::unique_ptr<VecPlanNode> buildVecWithSubqueries(const std::string& sql,
                                                   const Catalog& cat, bool optimize) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    Validator::validate(stmt, cat);
    auto tables = loadColumnar(stmt, cat);
    if (needsSubqueryMaterialization(stmt)) {
        materializeSubqueries(stmt, [&](SelectStatement body, bool int_type_observable) {
            std::unordered_map<std::string, ColumnarTable> body_tables;
            std::vector<std::string> names;
            collectQueryTables(body, names);
            for (const auto& n : names) body_tables.emplace(n, tables.at(n));
            auto logical = LogicalPlanBuilder::build(std::move(body), cat);
            if (optimize) {
                logical = PredicatePushdown::apply(std::move(logical), cat);
                logical = JoinEnumeration::apply(std::move(logical), cat);
                CardinalityEstimator::estimate(*logical, cat);
            }
            auto node = VectorizedPlanBuilder::build(std::move(logical),
                                                     std::move(body_tables), cat,
                                                     int_type_observable);
            node->open();
            std::vector<Row> rows;
            while (DataChunk* chunk = node->nextChunk()) {
                const int n = chunk->filter_applied
                                  ? static_cast<int>(chunk->sel.indices.size())
                                  : chunk->num_rows;
                for (int i = 0; i < n; ++i) {
                    const int r = chunk->filter_applied ? chunk->sel.indices[i] : i;
                    Row row;
                    for (const auto& cv : chunk->columns) row.push_back(valueAt(cv, r));
                    rows.push_back(std::move(row));
                }
            }
            SubqueryResult out{node->outputSchema(), std::move(rows)};
            node->close();
            return out;
        }, &cat);
    }
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
std::vector<std::string> drainText(std::unique_ptr<VecPlanNode> node) {
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

// The same two, for a query whose subquery has to be materialized first.
void expectSubqueryRefusedBothWays(const std::string& sql, const Catalog& cat) {
    for (bool optimize : {false, true}) {
        EXPECT_THROW(drainText(buildVecWithSubqueries(sql, cat, optimize)),
                     std::runtime_error)
            << "no refusal with optimize=" << optimize << " for: " << sql;
    }
}

void expectSubqueryRowsBothWays(const std::string& sql, const Catalog& cat,
                                const std::vector<std::string>& expected) {
    for (bool optimize : {false, true}) {
        std::vector<std::string> got;
        ASSERT_NO_THROW(got = drainText(buildVecWithSubqueries(sql, cat, optimize)))
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
// one silently wrong, which is why taintWalk arms both operands of `/` — once it
// has decided the division is INTEGER at all. The literal `2` here is what makes
// it one; `2.0 / x` is not armed, and section 6 pins that.
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


// ===========================================================================
// 4. The materialization cut — the boundary the walk could not cross
//
// `materializeSubqueries` runs an uncorrelated body as its OWN plan and
// substitutes the value it returned. Two builds, and `collectIntOrigins` runs
// once per build: the body's plan holds no `/`, so nothing armed and the INT was
// flattened into a DOUBLE column — correct for that plan alone — and the outer
// plan then saw a DOUBLE Literal and correctly found nothing to arm. Neither
// walk was wrong; no walk spanned both. Measured on data/laps.csv: 10000 rows
// where both Volcano modes and SQLite return 0 (seam subquery pass 4, B-1).
//
// What crosses is the REQUEST, not the taint: materializeSubqueries knows, from
// the outer AST it still holds, that the body's value is about to be divided by
// an INTEGER, and hands that to the runner as `int_type_observable`. The body's
// own root columns are then armed exactly as an in-plan division would arm them,
// and the refusal stays VALUE-driven — which is what keeps the guards in
// section 5 answering.
//
// EVERY WITNESS HERE IS 7 OR 2, over the five rows of test_laps_full.csv.
// ===========================================================================

// FAILS WITHOUT THE FIX: no throw, and five rows come back.
// Pre-fix run on 689ea9a: 5 rows (lap_id 1..5).
// Volcano and SQLite: zero rows — MAX(...) is the INT 2, `7 / 2` is 3, and 3
// does not exceed 3. Flattened to 2.0 it is 3.5, and every row passes.
//
// The AGGREGATE materialization site, reached across the cut.
TEST_F(TypeThroughDivision, MaterializedScalarDividedByAnIntegerRefuses) {
    expectSubqueryRefusedBothWays(
        "SELECT lap_id FROM laps WHERE 7 / "
        "(SELECT MAX(CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END) FROM laps l2) > 3",
        catalog_);
}

// FAILS WITHOUT THE FIX: no throw, five rows.
// Pre-fix run on 689ea9a: 5 rows. Volcano/SQLite: zero.
//
// The PROJECT materialization site — no aggregate anywhere — so the cut is not
// a property of VecHashAggregateNode.
TEST_F(TypeThroughDivision, MaterializedProjectionDividedByAnIntegerRefuses) {
    expectSubqueryRefusedBothWays(
        "SELECT lap_id FROM laps WHERE 7 / "
        "(SELECT CASE WHEN lap_id = 2 THEN 2 ELSE 0.5 END FROM laps WHERE lap_id = 2) > 3",
        catalog_);
}

// FAILS WITHOUT THE FIX: no throw, five rows.
// Pre-fix run on 689ea9a: 5 rows. Volcano/SQLite: zero (`7 / 2` is 3).
//
// The subquery as the DIVIDEND rather than the divisor. Both polarities, one
// rule — the same reason MixedCaseUsedAsTheDIVISORRefuses exists in section 2.
TEST_F(TypeThroughDivision, MaterializedScalarAsTheDIVIDENDRefuses) {
    expectSubqueryRefusedBothWays(
        "SELECT lap_id FROM laps WHERE "
        "(SELECT MAX(CASE WHEN l2.lap_id = 2 THEN 7 ELSE 0.5 END) FROM laps l2) / 2 > 3",
        catalog_);
}

// FAILS WITHOUT THE FIX: no throw, five rows where Volcano returns two.
// Pre-fix on 689ea9a: 5 rows; row-Volcano and SQLite: 2 (lap_id 3 and 4).
//
// NOT IN THE AUDIT, and the reason the other operand is TYPED rather than
// assumed. `round` is an INT column, so `round / 2` truncates in Volcano —
// 1/2 = 0, 2/2 = 1 — and `round / 2.0` does not. A rule that only recognised an
// INTEGER LITERAL beside the `/` would leave this one silently wrong, and a rule
// that armed every operand would refuse the REAL-column twin in section 5 that
// is correct today. Both need the catalog, which is why materializeSubqueries
// now takes one.
TEST_F(TypeThroughDivision, MaterializedScalarDividedByAnIntegerCOLUMNRefuses) {
    expectSubqueryRefusedBothWays(
        "SELECT lap_id FROM laps WHERE round / "
        "(SELECT MAX(CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END) FROM laps l2) > 0",
        catalog_);
}

// FAILS WITHOUT THE FIX: no throw, five rows.
// Pre-fix on 689ea9a: 5 rows. SQLite: zero. Volcano cannot adjudicate this one
// at all (it refuses derived tables by capability), which is exactly why the
// wrong answer survived four audit passes.
//
// The cut INSIDE a derived body: materializeSubqueries recurses into the body
// before the outer statement, so the request has to be computed per statement
// rather than once for the query.
TEST_F(TypeThroughDivision, MaterializedScalarInsideADerivedBodyRefuses) {
    expectSubqueryRefusedBothWays(
        "SELECT k FROM (SELECT lap_id AS k FROM laps WHERE 7 / "
        "(SELECT MAX(CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END) FROM laps l2) > 3) t",
        catalog_);
}


// ===========================================================================
// 5. The fence around the cut — answers that are right today and must not move
// ===========================================================================

// GUARD. The three controls the audit named, each of which a shape-only refusal
// at the cut would have broken. The refusal is armed by the plan and fired by
// the VALUE, so an armed body that never produces an INT still answers.
TEST_F(TypeThroughDivision, GuardTheArmedBodyStillAnswersWhenNoIntArrives) {
    // MIN picks the REAL branch, so the Value crossing the cut is 0.5.
    expectSubqueryRowsBothWays(
        "SELECT lap_id FROM laps WHERE 7 / "
        "(SELECT MIN(CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END) FROM laps l2) > 3",
        catalog_, {"1|", "2|", "3|", "4|", "5|"});
    // `THEN 2.0` — no INT branch at all, nothing to lose.
    expectSubqueryRowsBothWays(
        "SELECT lap_id FROM laps WHERE 7 / "
        "(SELECT MAX(CASE WHEN l2.lap_id = 2 THEN 2.0 ELSE 0.5 END) FROM laps l2) > 3",
        catalog_, {"1|", "2|", "3|", "4|", "5|"});
    // An INT-DECLARED body column: nothing narrows, so `7 / 5` is 1 in both
    // engines. `= 1` rather than `> 0` on purpose — a body that came back as
    // 5.0 would make this 1.4 and the suite would report zero rows.
    expectSubqueryRowsBothWays(
        "SELECT lap_id FROM laps WHERE 7 / (SELECT COUNT(*) FROM laps l2) = 1",
        catalog_, {"1|", "2|", "3|", "4|", "5|"});
}

// GUARD. THE ANSWER THE CONSERVATIVE VERSION OF THIS FIX WOULD HAVE COST.
// `speed` is REAL, so `speed / <anything>` is REAL division in Volcano too and
// the body's stored type cannot change the answer. Arming on "the subquery is
// under a `/`" — without typing the other operand — refuses this, and it is
// right in all four modes today.
TEST_F(TypeThroughDivision, GuardAREALOperandBesideTheDivisionIsNotArmed) {
    expectSubqueryRowsBothWays(
        "SELECT lap_id FROM laps WHERE speed / "
        "(SELECT MAX(CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END) FROM laps l2) > 150",
        catalog_, {"1|", "2|", "3|", "4|", "5|"});
    // the literal form of the same fact
    expectSubqueryRowsBothWays(
        "SELECT lap_id FROM laps WHERE 7.0 / "
        "(SELECT MAX(CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END) FROM laps l2) > 3",
        catalog_, {"1|", "2|", "3|", "4|", "5|"});
}

// GUARD. The subquery's value is only COMPARED, which coerces, so nothing is
// observable and the body must not be armed at all — the cut's analogue of
// GuardAdditionComparisonAndOrderByAreNotObservers.
TEST_F(TypeThroughDivision, GuardAMaterializedScalarThatIsNotDividedIsUntouched) {
    expectSubqueryRowsBothWays(
        "SELECT lap_id FROM laps WHERE speed > "
        "(SELECT MAX(CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END) FROM laps l2)",
        catalog_, {"1|", "2|", "3|", "4|", "5|"});
    expectSubqueryRowsBothWays(
        "SELECT lap_id FROM laps WHERE speed + "
        "(SELECT MAX(CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END) FROM laps l2) > 300",
        catalog_, {"1|", "2|", "3|", "4|", "5|"});
}


// ===========================================================================
// 6. The over-firing fix — `/` needs BOTH operands, not one
//
// Every test here FAILS WITHOUT THE FIX by REFUSING a query that is correct in
// every mode that can run it. Fix round 3 armed both operands of `/` with no
// test on their types; `INTEGER/INTEGER truncates while INTEGER/REAL does not`
// is the entire justification and it needs the divisor as well as the dividend.
// Seam pass 4 reported this as E-14; the derived-table form had NO working mode,
// because Volcano refuses derived tables by capability.
// ===========================================================================

// FAILS WITHOUT THE FIX: refused. Post-fix: 3.5, which is what both Volcano
// modes answer. `7 / 2.0` and `7.0 / 2.0` are both 3.5 — the stored type cannot
// reach this answer.
TEST_F(TypeThroughDivision, DividingAMixedCaseByAREALLiteralAnswers) {
    expectRowsBothWays(
        "SELECT MAX(CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END) / 2.0 AS y "
        "FROM laps WHERE lap_id < 4",
        catalog_, {"3.5|"});
}

// FAILS WITHOUT THE FIX: refused, in the only mode that can run it — Volcano
// refuses a derived table by capability, so before this the query had no
// working mode anywhere. Post-fix: the answer it gave before fix round 3.
TEST_F(TypeThroughDivision, TheDerivedFormOfThatQueryHasAWorkingModeAgain) {
    expectRowsBothWays("SELECT x / 2.0 FROM " + std::string(MIXED_BODY),
                       catalog_, {"0.25|", "3.5|", "0.25|"});
}

// FAILS WITHOUT THE FIX: refused. The taint reaches a `/`, but by the time it
// gets there the value is REAL in BOTH engines — `x + 0.5` is 7.5 whether x was
// the INT 7 or the double 7.0 — so there is no truncation left to lose. Dropping
// the origins at an arithmetic node that can no longer be INTEGER is the same
// `both operands` rule, applied one level down.
TEST_F(TypeThroughDivision, TaintThroughArithmeticThatCanNoLongerBeIntegerAnswers) {
    expectRowsBothWays("SELECT (x + 0.5) / 2 FROM " + std::string(MIXED_BODY),
                       catalog_, {"0.5|", "3.75|", "0.5|"});
    expectRowsBothWays("SELECT (x * 2.0) / 4 FROM " + std::string(MIXED_BODY),
                       catalog_, {"0.25|", "3.5|", "0.25|"});
}

// GUARD. One character apart from the first test in this section, and it must
// STILL refuse: `/ 2` is INTEGER division in Volcano and in SQLite, so the
// flattened column really does change the answer (3 against 3.5). Getting this
// pair wrong in either direction is the whole difficulty.
TEST_F(TypeThroughDivision, GuardTheINTEGERDivisorTwinStillRefuses) {
    expectRefusedBothWays(
        "SELECT MAX(CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END) / 2 AS y "
        "FROM laps WHERE lap_id < 4",
        catalog_);
    expectRefusedBothWays("SELECT x / 2 FROM " + std::string(MIXED_BODY), catalog_);
}


// ===========================================================================
// 7. The magnitude rule's other half — the %.15g bound on a value never printed
//
// narrowToDoubleColumn enforces BOTH "is it the same number" (2^53) and "does it
// still print the same" (1e15), and 1e15 binds first. The second question has no
// answer for a column whose text is never read, and the plan can tell: an origin
// that does not appear in the origin sets of the ROOT's output columns cannot
// reach %.15g. See tests/test_int_double_materialization.cc's
// ValueBoundIsExactlyTwoToThe53 for the boundary itself.
// ===========================================================================

// FAILS WITHOUT THE FIX: refused. Volcano answers 1.
// 2e15 is below 2^53, so the double IS the integer; only the rendering differs,
// and the comparison consumes the value without rendering it.
TEST_F(TypeThroughDivision, AMagnitudeThatIsNeverPrintedIsNotRefused) {
    expectRowsBothWays(
        "SELECT MAX(CASE WHEN lap_id = 2 THEN 2000000000000000 ELSE 0.5 END) > 1 AS big "
        "FROM laps WHERE lap_id < 4",
        catalog_, {"1|"});
    // the same value reached through a derived column and consumed by a filter
    expectRowsBothWays(
        "SELECT COUNT(*) AS n FROM (SELECT CASE WHEN lap_id = 2 THEN 2000000000000000 "
        "ELSE 0.5 END AS x FROM laps WHERE lap_id < 4) t WHERE t.x > 1",
        catalog_, {"1|"});
}

// GUARD. The same magnitude in the OUTPUT still refuses, because %.15g really
// does render it `2e+15` while Volcano prints all sixteen digits. This is the
// half E10_VOLCANO_ONLY's render witness pins, and the relaxation must not
// reach it.
TEST_F(TypeThroughDivision, GuardTheSameMagnitudeIsStillRefusedWhenItIsPRINTED) {
    expectRefusedBothWays(
        "SELECT MAX(CASE WHEN lap_id = 2 THEN 2000000000000000 ELSE 0.5 END) AS m "
        "FROM laps WHERE lap_id < 4",
        catalog_);
    expectRefusedBothWays(
        "SELECT x FROM (SELECT CASE WHEN lap_id = 2 THEN 1000000000000001 ELSE 0.5 END "
        "AS x FROM laps WHERE lap_id < 4) t ORDER BY x DESC",
        catalog_);
}

// GUARD. Above 2^53 the VALUE is gone, and no plan shape can make that
// unobservable — 9007199254740993 and 9007199254740992 are one double. The
// relaxation is to the text bound only.
TEST_F(TypeThroughDivision, GuardAboveTwoToThe53TheUnprintedColumnStillRefuses) {
    expectRefusedBothWays(
        "SELECT MAX(CASE WHEN lap_id = 2 THEN 9007199254740993 ELSE 0.5 END) > 1 AS big "
        "FROM laps WHERE lap_id < 4",
        catalog_);
}
