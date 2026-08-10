#include <gtest/gtest.h>

#include "catalog/catalog.h"
#include "common/schema.h"
#include "common/value.h"
#include "execution/vec_types.h"
#include "parser/parser.h"
#include "planner/binder.h"
#include "planner/cardinality_estimator.h"
#include "planner/join_enumeration.h"
#include "planner/logical_plan.h"
#include "planner/predicate_pushdown.h"
#include "planner/subquery_materialization.h"   // collectQueryTables
#include "planner/vectorized_plan_builder.h"
#include "storage/csv_loader.h"
#include "storage/csv_to_columnar.h"

#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

// THE INT -> DOUBLE MATERIALIZATION SEAM, THIRD HALF: THE RESULT, NOT THE CELL.
//
// tests/test_int_double_materialization.cc pins the MAGNITUDE rule (an integer
// too large to survive the conversion, refused at 1e15 or at 2^53).
// tests/test_int_double_type_through_division.cc pins the TYPE rule (an integer
// whose INT-ness reaches a `/` with an INT partner, refused at any magnitude).
// Both ask their question of the value being STORED. This file is about the one
// place where asking there cannot work, because the divergence appears at the
// value being USED.
//
// THE DEFECT (seam audit pass 5, E-19, ranked BLOCKER — a SILENT WRONG ANSWER
// on the shipped catalog, no error, no derived table, no --no-optimize, no
// 15-digit literal):
//
//   SELECT MAX(CASE WHEN lap_id = 1 THEN 123456789 ELSE 0.5 END) * 987654321
//     row/volcano, columnar/volcano, and SQLite   121932631112635269
//     columnar/vectorized (both legs)             1.21932631112635e+17
//
// MIN/MAX are order statistics: they hand back an element of the input domain,
// so Volcano's MAX is the INT 123456789 and this engine's cell is the double
// 123456789.0. Both are far below every bound either existing refusal knows
// about, both print identically, and neither refusal fires — correctly, because
// at that point the two engines still agree. Then one ordinary multiplication
// moves the value from 1.2e8 to 1.2e17, past the %.15g cliff AND past 2^53, and
// the two engines print different numbers. Both refusals are
// `if (v.type() != INT) return` by construction and by then the value IS a
// genuine double, so neither can see it.
//
// The comment that priced this route as safe (vectorized_plan_builder.cc, "Why
// `/` alone and not consumed by any expression") argued: "`+ - *` on INT/INT
// give an INT whose %.15g rendering is identical to the DOUBLE (`x+1` is 8
// either way)". That is a statement about the OPERAND while the cliff applies to
// the RESULT.
//
// THE RULE, and why it does not move a passing answer. taintWalk already knows
// when Volcano computes an arithmetic node in INTEGER — its `both` test, which
// exists for the `/` rule — and the origin sets already know when this engine's
// operand came out of a narrowed DOUBLE column. Together those are exactly "the
// result here is the double that Volcano's exact int64_t rounds to", and the
// column holding it is marked IntNarrowing::VOLCANO_INT (or its UNRENDERED twin
// when the plan proves the text is never printed). The test then runs on the
// DOUBLE, at the same two bounds:
//
//   below the bound   the double IS that integer and prints as that integer, so
//                     the engines agree and NOTHING is refused;
//   at or above it    they disagree — in the TEXT from 1e15, in the VALUE from
//                     2^53, and past INT64_MAX Volcano's checkedMul THROWS where
//                     this engine answers — and it is refused.
//
// So `MAX(CASE WHEN c THEN 1 ELSE 0.5 END) + 1` is still 2 in all six modes, and
// `SELECT x * 2` over a mixed-CASE column is still answered. That is the whole
// difference between this and the blanket arming the audit offered as the other
// option, and every Guard below is a query that would have died under it.
//
// DISCRIMINATION. Every test named ...Refuses... was run against this worktree's
// parent commit (8fba8a1) before the change: all of them produced the divergent
// answer quoted in their comment instead of throwing. Every Guard passes on both
// sides. The file uses nothing but the public planner API so it builds unchanged
// against that tree.
//
// EVERY EXPECTATION WAS TAKEN FROM VOLCANO AND FROM SQLite over the same five
// rows, not from this engine.

namespace {

const char* CATALOG = "../tests/data/test_catalog.json";

std::unordered_map<std::string, std::shared_ptr<const ColumnarTable>> loadColumnar(
        const SelectStatement& stmt, const Catalog& cat) {
    std::unordered_map<std::string, std::shared_ptr<const ColumnarTable>> tables;
    std::vector<std::string> names;
    collectQueryTables(stmt, names);
    for (const auto& name : names) {
        if (tables.count(name)) continue;
        const auto& m = cat.getTable(name);
        tables.emplace(name, std::make_shared<const ColumnarTable>(CSVToColumnar::convert(
            CSVLoader::load(m.filepath, m.schema), m.schema)));
    }
    return tables;
}

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

// RENDERED TEXT, not Value. The whole subject is a number that is the same to
// within a Value comparison's coercion and different on the screen, so a Value
// assertion would pass on the wrong answer.
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

// Both optimizer settings, always: the arming runs inside
// VectorizedPlanBuilder::build, OUTSIDE main.cc's --no-optimize gate, so the two
// legs must agree — and a rule that fired on only one of them would be invisible
// to the harness's optimizer-invariant mode by construction.
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

class ArithmeticResult : public ::testing::Test {
  protected:
    Catalog cat{CATALOG};
};

}  // namespace

// ── 1. The three magnitudes, each at its own boundary ───────────────────────

TEST_F(ArithmeticResult, MultiplicationPastTwoToThe53Refuses) {
    // Volcano and SQLite: 121932631112635269. This engine, before the fix:
    // 1.21932631112635e+17 — a different NUMBER, not merely different text,
    // because the product is past 2^53.
    expectRefusedBothWays(
        "SELECT MAX(CASE WHEN lap_id = 1 THEN 123456789 ELSE 0.5 END) * 987654321 AS y "
        "FROM laps", cat);
}

TEST_F(ArithmeticResult, TheMinimalFormAddingOneRefuses) {
    // The smallest possible instance, and the reason the witness is honest: the
    // stored value 999999999999999 is exactly one BELOW
    // MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN, so it stores without complaint, and
    // `+ 1` crosses the bound the store was checked against.
    // Volcano and SQLite: 1000000000000000. Before the fix: 1e+15.
    expectRefusedBothWays(
        "SELECT MAX(CASE WHEN lap_id = 1 THEN 999999999999999 ELSE 0.5 END) + 1 AS y "
        "FROM laps", cat);
}

TEST_F(ArithmeticResult, SubtractionAcrossTheRenderingCliffRefuses) {
    // Volcano and SQLite: -8000000000000001. Before the fix: -8e+15. The bound
    // is on |d|, so the negative side is not a separate rule — this test is what
    // says so.
    expectRefusedBothWays(
        "SELECT MAX(CASE WHEN lap_id = 1 THEN 999999999999999 ELSE 0.5 END) "
        "- 9000000000000000 AS y FROM laps", cat);
}

TEST_F(ArithmeticResult, PastInt64MaxWhereVolcanoThrowsRefuses) {
    // The ERROR/ANSWER face. SwiftQL's INT arithmetic is overflow-checked and
    // its DOUBLE arithmetic is not, so the same one-cell type difference decided
    // whether the query threw: all four Volcano modes said "integer overflow in
    // '*'" and this engine answered 6.45636042579834e+19. It fires at a stored
    // magnitude of SEVEN — the stored value is irrelevant, only the multiplier
    // is, which is the sharpest statement of why no bound on the stored value
    // could ever have covered this.
    expectRefusedBothWays(
        "SELECT MAX(CASE WHEN lap_id = 1 THEN 7 ELSE 0.5 END) "
        "* 9223372036854775807 AS y FROM laps", cat);
}

TEST_F(ArithmeticResult, MinReachesItThroughTheSameDoor) {
    // MIN and MAX are the same rule (order statistics keep the argument's own
    // Value AND type); pinned separately because collectIntOrigins tests the
    // function NAME.
    expectRefusedBothWays(
        "SELECT MIN(CASE WHEN lap_id = 1 THEN 123456789 ELSE 999999999.5 END) "
        "* 987654321 AS y FROM laps", cat);
}

// ── 2. The same defect without an aggregate: a derived body's own column ────

TEST_F(ArithmeticResult, ADerivedBodysMixedColumnRefusesAboveTheBound) {
    // No MIN/MAX at all — the general route is a mixed CASE, and the aggregate
    // is only the second way to reach it. Volcano refuses derived tables by
    // capability, so before the fix this shape had NO mode that could contradict
    // it: SQLite said 1000000000000000 and this engine said 1e+15, alone.
    expectRefusedBothWays(
        "SELECT t.x + 1 AS y FROM (SELECT CASE WHEN lap_id = 1 THEN 999999999999999 "
        "ELSE 0.5 END AS x FROM laps WHERE lap_id < 2) t", cat);
}

TEST_F(ArithmeticResult, AnAggregateOverTaintedArithmeticRefuses) {
    // The mark has to be made at the AGGREGATE too, not only at the projection:
    // MAX returns an element of its input domain, so `MAX(t.x * 987654321)` is
    // an exact int64_t in Volcano and the rounded double here.
    expectRefusedBothWays(
        "SELECT MAX(t.x * 987654321) AS y FROM (SELECT CASE WHEN lap_id = 1 "
        "THEN 123456789 ELSE 0.5 END AS x FROM laps) t", cat);
}

// ── 3. The guards. Every one of these answers today and must keep answering ─

TEST_F(ArithmeticResult, GuardAddingOneToASmallMixedCaseStillAnswers) {
    // THE test that separates this rule from the blanket arming the audit
    // offered as the alternative. Volcano and SQLite: 2. Under "arm `+ - *` the
    // way `/` is armed" this query would be REFUSED, which is a passing answer
    // moved; under a value test on the result it is 2.0, prints "2", and agrees.
    expectRowsBothWays("SELECT MAX(CASE WHEN lap_id = 1 THEN 1 ELSE 0.5 END) + 1 AS y "
                       "FROM laps", cat, {"2|"});
}

TEST_F(ArithmeticResult, GuardMultiplyingASmallMixedCaseStillAnswers) {
    // `SELECT x*2`, named in vectorized_plan_builder.cc as the query the wider
    // rule would cost. Volcano and SQLite: 14.
    expectRowsBothWays("SELECT MAX(CASE WHEN lap_id = 1 THEN 7 ELSE 0.5 END) * 2 AS y "
                       "FROM laps", cat, {"14|"});
}

TEST_F(ArithmeticResult, GuardAREALPartnerIsNotArmedAtAnyMagnitude) {
    // `both` is false, so Volcano computes this in REAL too and the two engines
    // cannot be told apart. 2e+15 in all six modes, above the RENDERED bound —
    // which is what makes this a test of the RULE and not of the magnitude.
    expectRowsBothWays(
        "SELECT MAX(CASE WHEN lap_id = 1 THEN 2000000000000000 ELSE 0.5 END) + 0.5 AS y "
        "FROM laps", cat, {"2e+15|"});
}

TEST_F(ArithmeticResult, GuardTheDivisionRuleIsUntouched) {
    // Round 4's E-14 repair. `/ 2.0` has a REAL partner, is not armed by the
    // type rule, and must not be armed by this one either. Volcano: 3.5.
    expectRowsBothWays("SELECT MAX(CASE WHEN lap_id = 1 THEN 7 ELSE 0.5 END) / 2.0 AS y "
                       "FROM laps", cat, {"3.5|"});
}

TEST_F(ArithmeticResult, GuardAComparisonAboveTheAggregateStillAnswers) {
    // A comparison's result is a boolean INT in BOTH engines, so int_arith dies
    // there exactly as the origins do. Volcano: 1. This is also round 4's E-14
    // second repair, re-pinned from the new rule's side.
    expectRowsBothWays(
        "SELECT MAX(CASE WHEN lap_id = 1 THEN 2000000000000000 ELSE 0.5 END) > 1 AS y "
        "FROM laps", cat, {"1|"});
}

TEST_F(ArithmeticResult, GuardOrdinaryArithmeticOnRealAndIntColumnsIsUnaffected) {
    // No narrowed column anywhere underneath, so no mark and no per-value path.
    // These are the queries every plan is made of.
    expectRowsBothWays("SELECT speed * 2 AS y FROM laps WHERE lap_id = 1", cat,
                       {"624.9|"});
    expectRowsBothWays("SELECT lap_id * 987654321 AS y FROM laps WHERE lap_id = 1", cat,
                       {"987654321|"});
}

TEST_F(ArithmeticResult, GuardTheUnprintedBandIsStillRelaxed) {
    // Round 4's UNRENDERED relaxation, extended to this rule rather than
    // overridden by it: a result in (1e15, 2^53] whose text the plan proves is
    // never read is judged on the VALUE alone, where (double)i == i exactly and
    // the engines agree. Using the RENDERED bound for every marked column would
    // have refused this, which is a passing answer moved.
    expectRowsBothWays(
        "SELECT COUNT(*) AS n FROM (SELECT b.y AS y FROM (SELECT CASE WHEN l.lap_id = 2 "
        "THEN 999999999999999 ELSE 0.5 END * 2 AS y FROM laps l) b) t WHERE t.y > 0",
        cat, {"5|"});
}

// ── 4. The runtime rule on its own ─────────────────────────────────────────
//
// refuseDivergentVolcanoInt is where the whole thing bottoms out, and it is
// three compares. Pinned directly so the boundary cannot drift with the plan
// machinery above it.

TEST(DivergentVolcanoIntRule, BelowTheRenderedBoundNothingIsRefused) {
    EXPECT_NO_THROW(refuseDivergentVolcanoInt(Value(999999999999999.0), false));
    EXPECT_NO_THROW(refuseDivergentVolcanoInt(Value(-999999999999999.0), false));
}

TEST(DivergentVolcanoIntRule, AtTheRenderedBoundItRefuses) {
    EXPECT_THROW(refuseDivergentVolcanoInt(Value(1000000000000000.0), false),
                 std::runtime_error);
    EXPECT_THROW(refuseDivergentVolcanoInt(Value(-1000000000000000.0), false),
                 std::runtime_error);
}

TEST(DivergentVolcanoIntRule, TheUnrenderedBoundIsTwoToThe53) {
    EXPECT_NO_THROW(refuseDivergentVolcanoInt(Value(2000000000000000.0), true));
    EXPECT_NO_THROW(refuseDivergentVolcanoInt(Value(9007199254740992.0), true));
    EXPECT_THROW(refuseDivergentVolcanoInt(Value(9007199254740994.0), true),
                 std::runtime_error);
}

TEST(DivergentVolcanoIntRule, ANonIntegralValueProvesVolcanoWasRealToo) {
    // The one over-refusal this rule could have had, and the compare that
    // removes most of it: when the CASE takes its REAL branch, Volcano's result
    // is REAL as well and the engines agree at any magnitude. A fractional part
    // is proof that happened.
    EXPECT_NO_THROW(refuseDivergentVolcanoInt(Value(2000000000000000.5), false));
}

TEST(DivergentVolcanoIntRule, AnIntValueIsLeftToTheMagnitudeRule) {
    // narrowToDoubleColumn is applied to the same Value immediately after, with
    // the same bound and a message about the STORE. Answering here too would
    // report the wrong cause.
    EXPECT_NO_THROW(refuseDivergentVolcanoInt(
        Value(static_cast<int64_t>(2000000000000000LL)), false));
}

TEST(DivergentVolcanoIntRule, NonFiniteValuesAreGenuineDoubles) {
    const double inf = std::numeric_limits<double>::infinity();
    EXPECT_NO_THROW(refuseDivergentVolcanoInt(Value(inf), false));
    EXPECT_NO_THROW(refuseDivergentVolcanoInt(Value(-inf), false));
    EXPECT_NO_THROW(refuseDivergentVolcanoInt(
        Value(std::numeric_limits<double>::quiet_NaN()), false));
}
