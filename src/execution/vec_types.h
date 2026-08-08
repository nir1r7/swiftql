#pragma once

#include "common/type_id.h"
#include "common/value.h"
#include <string>
#include <stdexcept>
#include <variant>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>


// vectorized execution batch size
// distinct from zone map CHUNK_SIZE (8192)
static constexpr int BATCH_SIZE = 1024;

// What the plan knows about one materialized DOUBLE column, for the three
// refusals below. Exactly one state per column; the vectorized plan builder
// decides it from the LOGICAL plan (collectIntOrigins) and the two materializing
// nodes stamp it onto the ColumnVector.
//
// TWO INDEPENDENT QUESTIONS live in this one enum, and they are about two
// different values:
//   - the first three states judge an INT Value arriving at a DOUBLE column —
//     is its VALUE, its TEXT, or its TYPE still recoverable;
//   - the last two judge a DOUBLE that Volcano computed as an exact integer,
//     which is what is left after the INT has already been consumed by an
//     arithmetic node one level down.
// A column can genuinely be in both situations (a CASE with one INT branch and
// one tainted-arithmetic branch), and the VOLCANO_INT states are the strict
// superset — they apply the same magnitude bound to the INT as RENDERED and
// UNRENDERED do, and then ask the second question too. That is why build()
// UPGRADES into them rather than choosing between them.
//
// RENDERED is the conservative default and the only state a column that the
// builder said nothing about can be in, so a mask that is missing, short, or
// never set behaves exactly as the engine did before any refusal existed.
enum class IntNarrowing : uint8_t {
    // The value may reach the query's output text, so BOTH halves of
    // "indistinguishable from the INT it was" have to hold — including the
    // %.15g rendering, which binds first (1e15).
    RENDERED,
    // The plan proves this column's own text is never read: its INT-ness dies
    // in a comparison, a key encoding or an aggregate that emits a fresh
    // DOUBLE, and no expression above carries it to an output column. Only the
    // VALUE has to survive, so the bound is 2^53 rather than 1e15.
    UNRENDERED,
    // An INT stored here would change an answer at any magnitude, because the
    // stored TYPE reaches a `/` whose other operand is also an INT. Refused
    // outright; see refuseObservableIntNarrowing.
    OBSERVABLE,
    // The value arriving here is ALREADY A DOUBLE and Volcano computes it as an
    // INT: the expression is `+ - *` (or `/`) over a column that was itself
    // narrowed, so both operands are INTEGER in Volcano and REAL here. The two
    // states above cannot see this — both are `if (v.type() != INT) return` by
    // construction, and by this point the INT-ness is one node further down.
    // Same two bounds, applied to the DOUBLE: VOLCANO_INT is the printed column
    // (1e15, the %.15g cliff), VOLCANO_INT_UNRENDERED the one the plan proves is
    // never printed (2^53, the value alone). See refuseDivergentVolcanoInt.
    VOLCANO_INT,
    VOLCANO_INT_UNRENDERED,
};

// one column of decoded, materialized data for batch
// no encoding, scan nodes decode before data enters struct
struct ColumnVector {
    std::variant<std::vector<int64_t>, std::vector<double>, std::vector<std::string>> data;
    TypeId type;

    // Row-level validity.
    // INVARIANT: all_valid == true  <=>  validity is empty, and every row is
    // non-NULL. That is the overwhelmingly common case (ColumnarTable cannot
    // express NULL, so scan output is always all-valid), which is why the
    // operators that never manufacture a NULL need no validity code at all.
    // Once a NULL is written, validity.size() == data size, 1 = valid, 0 = NULL.
    // The typed vector stays dense: a NULL row holds a placeholder that callers
    // must not read without consulting isNull().
    bool all_valid = true;
    std::vector<uint8_t> validity;

    // Plan-shape arming for the INT -> DOUBLE narrowing; see
    // refuseObservableIntNarrowing, narrowToDoubleColumn and
    // refuseDivergentVolcanoInt below.
    // IntNarrowing::RENDERED — the default, and the value on every scan chunk,
    // join output and ExpressionExecutor result — is the conservative state:
    // the type is not observable and the value may be printed. The two nodes
    // that materialize a `Value` into a column type declared BEFORE the value
    // exists (VecProjectNode, VecHashAggregateNode) set it per column, from a
    // mask the vectorized plan builder computes over the LOGICAL plan.
    //
    // It rides on the ColumnVector, not on the node, because appendColumnValue
    // is the single funnel every such write goes through; the cost is one
    // predictably not-taken branch on the DOUBLE arm.
    IntNarrowing int_narrowing = IntNarrowing::RENDERED;

    int size() const {
        return std::visit([](const auto&v){
            return static_cast<int>(v.size());
        }, data);
    }

    bool isNull(int row) const {
        return !all_valid && validity[row] == 0;
    }
};

// indices of rows within a DataChunk that have passed a filter
// produced by VecFilterNode
struct SelectionVector {
    std::vector<int> indices;
    int size = 0;  // mirrors indices.size(); updated by VecFilterNode after each predicate loop
};

// batch of rows
// column i corresponds to schema column i in order
// num_rows may be less than BATCH_SIZE for the final chunk
//
// filter_applied: set true by VecFilterNode after evaluating a predicate.
// When false (e.g. chunk came directly from VecScanNode), sel.indices is
// meaningless and all num_rows rows are valid.
// When true, sel.indices is authoritative — an empty sel.indices means
// zero rows passed the filter, NOT "all rows valid".
struct DataChunk {
    std::vector<ColumnVector> columns;
    int num_rows = 0;
    SelectionVector sel;
    bool filter_applied = false;
};


// ===== ColumnVector access helpers =====
//
// Every operator that reads a chunk cell or writes one must go through these
// three functions. Reading a typed vector directly skips the validity mask and
// silently turns a SQL NULL into the placeholder value underneath it.

// Empty column of the given type, ready for appendColumnValue.
inline ColumnVector makeColumnVector(TypeId type) {
    ColumnVector cv;
    cv.type = type;
    switch (type) {
        case TypeId::INT:    cv.data = std::vector<int64_t>();     break;
        case TypeId::DOUBLE: cv.data = std::vector<double>();      break;
        case TypeId::STRING: cv.data = std::vector<std::string>(); break;
    }
    return cv;
}

// One cell into an existing Value, NULL-aware. The in-place form exists for the
// aggregate's inner loop, where returning by value cost a measurable ~4.7ms per
// million rows on SELECT SUM(speed).
inline void readColumnValue(const ColumnVector& cv, int row, Value& out) {
    if (cv.isNull(row)) { out = Value::null(); return; }
    std::visit([&](const auto& vec) { out = Value(vec[row]); }, cv.data);
}

// One cell as a Value, NULL-aware. Used by every Row-reconstruction site.
inline Value valueAt(const ColumnVector& cv, int row) {
    Value out;
    readColumnValue(cv, row, out);
    return out;
}

// The largest magnitude an INT Value may carry into a DOUBLE column and still
// be indistinguishable from the INT it was. Both halves of "indistinguishable"
// bite, and the SMALLER bound is the one that decides:
//
//   - VALUE. Above 2^53 (9007199254740992) consecutive int64_t collapse onto one
//     double: 2^53 and 2^53+1 both become 9007199254740992.0.
//   - TEXT. `Value::toString()` renders a DOUBLE with `%.15g`, which switches to
//     exponent form at 1e15 — so 1000000000000001 prints as "1e+15" while
//     `std::to_string(int64_t)` prints all sixteen digits. The double is exact
//     there; the rendering is not.
//
// 1e15 < 2^53, so the text bound subsumes the value bound and one comparison
// covers both. Verified by construction, not by argument, in
// tests/test_int_double_materialization.cc (`ThresholdIsExactlyTheBoundary`),
// which walks outward from the constant and asserts it is the first magnitude
// that fails either half — so a change to `%.15g` in Value::toString() breaks a
// test rather than silently widening this window.
//
// !! "BOTH HALVES BITE" IS A STATEMENT ABOUT A COLUMN THAT IS PRINTED, and it
// was applied to every column until seam pass 4 (E-14) ran a query that prints
// none of it: `SELECT MAX(CASE WHEN c THEN 2000000000000000 ELSE 0.5 END) > 1`
// is `1` in Volcano and was REFUSED here, for a rendering that never happens.
// A column whose text the plan proves is never read is judged by the VALUE
// bound alone (`MAX_EXACT_INT_IN_DOUBLE_COLUMN`), which is the only half that
// can change such an answer. Which half applies is the ColumnVector's
// `int_narrowing`, not a property of the value — see IntNarrowing above.
static constexpr int64_t MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN = 1000000000000000LL;

// The VALUE half on its own: 2^53, the last magnitude at which every int64_t is
// still its own double. |i| <= this and (double)i == i exactly; at 2^53 + 1 two
// consecutive integers collapse onto one double and the value itself is gone.
// Used only for an UNRENDERED column, where the %.15g cliff above cannot be
// observed. `ValueBoundIsExactlyTwoToThe53` pins that this constant is the last
// integer that round-trips and the next one is not.
static constexpr int64_t MAX_EXACT_INT_IN_DOUBLE_COLUMN = 9007199254740992LL;

// The INT -> DOUBLE narrowing, refused when it would be observable.
//
// This is NOT the promotion `evaluate()` does via toNumeric(). That one is a SQL
// semantic: `9007199254740993 * 1.0` is REAL arithmetic and every engine —
// Volcano, vectorized, SQLite — loses the same bits, so it agrees. THIS one is a
// storage artifact of the columnar batch: a ColumnVector holds one type, so a
// value whose runtime type is INT under a schema that says DOUBLE has to be
// converted to be stored at all. Volcano has no equivalent step — ProjectNode
// emits the Value the evaluator produced, untouched by the schema — so wherever
// the conversion is visible, the two engines answer differently and SQLite sides
// with Volcano.
//
// The conversion is reachable whenever an expression's INFERRED type is DOUBLE
// while its runtime Value is INT; `CASE` with one numeric branch of each kind is
// the general route (inferExprType unifies to DOUBLE, evaluate() returns the
// taken branch verbatim), and MIN/MAX over such a CASE reaches it a second way,
// through the aggregate's own materialization.
//
// Refusing per VALUE rather than per EXPRESSION is deliberate. A plan-time
// refusal of "CASE with mixed numeric branches" would also reject
// `CASE WHEN c THEN 1 ELSE 0.5 END`, which is correct today in every mode. The
// runtime test rejects exactly the queries that are wrong today and no others.
//
// The magnitude test runs on the DOUBLE, not on the int64_t, and that is a
// measurement rather than a preference. `Value::type()` lives in value.cc and
// does not inline, so asking it FIRST put a second out-of-line call on the arm
// every ordinary DOUBLE cell takes — 3.6ms -> 7.3ms per million on this
// function in isolation, the same order as the 4.7ms readColumnValue's comment
// above already treats as worth avoiding. This order keeps the single
// toNumeric() call the line always made and adds two compares, which at the
// appendColumnValue level measured within noise.
//
// It is an EXACT proxy, not an approximation. The argument is that 1e15 is
// itself a representable double and rounding to nearest is monotonic, so
// |(double)i| >= 1e15 exactly when |i| >= 1e15 — but the argument is not the
// evidence: `ThresholdIsExactlyTheBoundary` brute-forces the agreement over a
// 10k window either side of the bound, 200k pseudo-random int64_t and the
// extrema. NaN and infinity fail both compares, fall through to the type check,
// and pass — they are DOUBLEs, and this rule is only about INTs.
// `unrendered` relaxes the TEXT half away; the fast path below 1e15 is reached
// without consulting it, so the measurement above still describes every
// ordinary DOUBLE cell.
inline double narrowToDoubleColumn(const Value& v, bool unrendered) {
    // STRING raises bad_variant_access from toNumeric(), as it did before.
    const double d = v.toNumeric();
    const double bound = static_cast<double>(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN);
    if (d < bound && d > -bound) return d;
    if (v.type() != TypeId::INT) return d;   // a genuine DOUBLE that big is fine
    const int64_t i = v.asInt();
    // Exact on the int64_t, not on the double, because THIS bound is about the
    // integer surviving and the rounding it is testing for is the one that
    // would have already happened to `d`.
    if (unrendered && i >= -MAX_EXACT_INT_IN_DOUBLE_COLUMN
                   && i <= MAX_EXACT_INT_IN_DOUBLE_COLUMN) {
        return d;
    }
    throw std::runtime_error(
        "vectorized execution cannot materialize the integer " +
        std::to_string(i) + " into a DOUBLE result column without "
        "changing it. A chunk column holds one type, so an expression that "
        "mixes INTEGER and REAL results (typically a CASE) is stored as REAL, "
        + (unrendered
               ? std::string("which holds it exactly only below 2^53 "
                             "(9007199254740992)")
               : std::string("which is exact and prints identically only "
                             "below 1e15"))
        + ". Re-run with --execution volcano, or give the branches the same "
          "type.");
}

// The SECOND thing INT -> DOUBLE narrowing destroys, and it is not magnitude.
//
// narrowToDoubleColumn above asks "is this value still the same NUMBER, and
// does it still PRINT the same?" and at 7 the answer to both is yes. What it
// cannot ask is whether the value is still the same TYPE — and type is
// load-bearing for exactly one operator, `/`, because evaluate() truncates
// INT/INT and does not truncate INT/DOUBLE. So
//
//     SELECT x / 2 FROM (SELECT CASE WHEN c THEN 7 ELSE 0.5 END AS x ...) t
//
// is 3 in Volcano and in SQLite (x is the INT 7) and 3.5 here (x came back out
// of a DOUBLE ColumnVector as 7.0). No magnitude is involved: it bites at 7.
//
// Why the test is not simply "refuse every INT -> DOUBLE narrowing". That
// would reject `SELECT CASE WHEN c THEN 1 ELSE 0.5 END`, which is correct
// today in all four modes and agrees with SQLite — the DOUBLE 1.0 prints as
// "1" through %.15g, so a column that is only PROJECTED OUT is fine. Refusing
// it would move a passing answer, which is not on offer.
//
// The distinction is therefore a PLAN-SHAPE fact, not a value fact: is this
// materialized column read by another expression, or is it the query's output?
// appendColumnValue sees one Value and cannot know. vectorized_plan_builder.cc
// can, and it sets `cv.int_narrowing` to OBSERVABLE on exactly the columns whose
// INT-ness can reach an operand of a `/` whose OTHER operand can also be an INT
// (collectIntOrigins). When the plan is CUT IN TWO by materializeSubqueries the
// division and the column live in different builds, and the request crosses the
// cut instead of the taint — see VectorizedPlanBuilder::build's
// `result_int_type_observable`.
//
// Both halves are needed and both are narrow:
//   - plan shape alone would refuse `SELECT x/2 FROM (SELECT CASE WHEN c THEN
//     7 ELSE 0.5 END AS x FROM t WHERE never)`, where no row ever takes the INT
//     branch and today's answer is right;
//   - the runtime type test alone is the blanket refusal ruled out above.
// Together they refuse a query only when a real INT value really does reach a
// real division. The residue is stated rather than hidden: when that division
// happens to be exact (`x` is 6, not 7, over `/2`) INT and REAL division agree
// and the query was right before this refusal. Closing that last gap means
// asking the question at the DIVISION rather than at the materialization, and
// the division runs in expression_executor.cc / evaluator.cc, which this rule
// deliberately does not reach into.
//
// !! A SENTENCE THAT USED TO STAND HERE IS RETRACTED, because arming acted on
// it and refused correct queries. It read that the taint reaches "an operand of
// a `/`", full stop. `INTEGER/INTEGER truncates while INTEGER/REAL does not` is
// the whole justification, and it needs BOTH operands: in `x / 2.0` the divisor
// is already REAL, so Volcano divides in REAL too and the stored type cannot
// change the answer. Fix round 3 armed both operands unconditionally and
// refused `SELECT MAX(CASE WHEN c THEN 7 ELSE 0.5 END) / 2.0`, which Volcano
// answers 3.5 — and its derived-table form, which then had no working mode at
// all (seam pass 4, E-14). The test is now "an INT can reach an operand of `/`
// WHOSE OTHER OPERAND CAN ALSO BE AN INT", and it lives in one place,
// taintWalk's `/` arm in vectorized_plan_builder.cc.
inline void refuseObservableIntNarrowing(const Value& v) {
    if (v.type() != TypeId::INT) return;
    throw std::runtime_error(
        "vectorized execution cannot materialize the integer " +
        std::to_string(v.asInt()) + " into a DOUBLE result column that another "
        "expression divides. A chunk column holds one type, so an expression "
        "that mixes INTEGER and REAL results (typically a CASE) is stored as "
        "REAL — and INTEGER/INTEGER truncates while INTEGER/REAL does not, so "
        "the stored type changes the answer. Give the branches the same type, "
        "or re-run with --execution volcano where that path supports the query "
        "(it does not run derived tables).");
}

// THE THIRD THING INT -> DOUBLE NARROWING DESTROYS, and it is the one the two
// refusals above cannot see, because by the time it is observable the value is
// no longer an INT.
//
// narrowToDoubleColumn asks its question of the value being STORED, and the
// divergence it is guarding appears at the value being USED. `MAX` over a
// mixed-type CASE keeps the argument's own Value, so Volcano holds the INT
// 123456789 and this engine holds the double 123456789.0 — both below every
// bound, both printing identically, no refusal on either side. Then one
// ordinary multiplication:
//
//   SELECT MAX(CASE WHEN lap_id=1 THEN 123456789 ELSE 0.5 END) * 987654321
//     Volcano, and SQLite   121932631112635269
//     vectorized            1.21932631112635e+17     <- a SILENT WRONG ANSWER
//
// Seam audit pass 5, E-19, ranked BLOCKER: no error, no derived table, no
// --no-optimize, no 15-digit literal, on the shipped catalog. The minimal form
// is `+ 1`. The comment above (`Why '/' alone`) priced `+ - *` as safe on the
// ground that "`x+1` is 8 either way" — a statement about the OPERAND, while the
// %.15g cliff applies to the RESULT, and a single `*` moves a value from 1.2e8
// to 1.2e17.
//
// So the test is asked HERE, of the result, and it is VALUE-driven exactly as
// its two neighbours are — which is what keeps it from refusing the queries that
// are right today. The plan marks a column VOLCANO_INT when it proves both
// operands of the arithmetic are INTEGER in Volcano (taintWalk's `both`, with a
// narrowed origin underneath); at that point Volcano's result is an exact
// int64_t and ours is the double it rounds to, so:
//
//   |d| < the bound   the double IS that integer and prints as that integer, so
//                     the two engines agree and nothing is refused. This is what
//                     keeps `MAX(CASE WHEN c THEN 1 ELSE 0.5 END) + 1` = 2.
//   |d| >= the bound  they disagree — in the TEXT from 1e15, in the VALUE from
//                     2^53, and past INT64_MAX Volcano's checkedMul throws where
//                     this engine answers. All three are refused, by one test.
//
// The one over-refusal is stated rather than hidden, and it is why the integral
// test is here: when the CASE takes its REAL branch, Volcano's result is REAL
// too and the engines agree at any magnitude. A non-integral double proves that
// happened and is let through; a REAL branch that happens to hold an INTEGRAL
// value at or above the bound is refused although it agrees. That needs a
// per-row "was INT" bit to decide, which is a runtime type the column does not
// carry — the same reason this rule exists at all.
inline void refuseDivergentVolcanoInt(const Value& v, bool unrendered) {
    // An INT Value here is narrowToDoubleColumn's subject, not this one's: it is
    // applied to the same value straight after, with the same bound.
    if (v.type() == TypeId::INT) return;
    const double d = v.toNumeric();
    // The TEXT bound first and unconditionally, so the fast path is one pair of
    // compares on every ordinary cell — the same shape, and the same constant,
    // as narrowToDoubleColumn's, including its exclusivity: 1e15 ITSELF renders
    // as "1e+15" and so is already outside.
    const double text_bound = static_cast<double>(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN);
    if (d < text_bound && d > -text_bound) return;
    // NaN and infinity fail both compares above and are not integral, so they
    // fall out here — they are genuine DOUBLEs and this rule is only about the
    // integers Volcano would have produced.
    if (!std::isfinite(d) || d != std::floor(d)) return;
    if (unrendered) {
        // The VALUE bound, INCLUSIVE, exactly as narrowToDoubleColumn's is:
        // 2^53 is the last magnitude at which every integer is still its own
        // double, so at |d| == 2^53 the two engines still hold the same number
        // and only its text — which the plan proves is never read — differs.
        const double exact = static_cast<double>(MAX_EXACT_INT_IN_DOUBLE_COLUMN);
        if (d <= exact && d >= -exact) return;
    }
    char text[64];
    std::snprintf(text, sizeof(text), "%.15g", d);
    throw std::runtime_error(
        "vectorized execution cannot compute this expression the way the Volcano "
        "engine does: its operands are INTEGER there and REAL here, because a "
        "column below it mixes INTEGER and REAL results (typically a CASE) and a "
        "chunk column holds one type. The two agree while the result stays below "
        + std::string(unrendered ? "2^53 (9007199254740992)" : "1e15")
        + "; this one reached " + text
        + ". Re-run with --execution volcano, or give the branches the same type.");
}

// Append one cell, NULL-aware. `cv.type` decides the storage type. A value
// reaching a DOUBLE column passes THREE refusals, and which of them can see a
// given divergence is decided by WHERE the INT still is:
//   - refuseObservableIntNarrowing — the value IS an INT and its TYPE is
//     observable to a `/` whose other operand is also INT, at any magnitude;
//   - narrowToDoubleColumn — the value IS an INT and the value or its rendering
//     changes, above 1e15, or above 2^53 when the plan proves the text is never
//     read;
//   - refuseDivergentVolcanoInt — the value is ALREADY A DOUBLE and Volcano
//     computed it as an exact integer, so neither of the two above can see it
//     (both are `if (v.type() != INT) return`). Seam pass 5's E-19.
// All three THROW rather than answer differently from Volcano and SQLite. Read
// all three comments before touching this. This comment called the conversion "lossless" from 85be432
// (Week 24, the commit that added validity) until seam pass 3 found it. It is
// not, above 1e15. Every other type disagreement is a planner/schema bug and
// surfaces as bad_variant_access from the typed accessor, as it did before
// validity existed. (Checked, not assumed: DOUBLE or STRING into an INT column
// and any number into a STRING column all reach a std::get of the wrong
// alternative. INT -> DOUBLE was the only silent one.)
inline void appendColumnValue(ColumnVector& cv, const Value& v) {
    if (v.isNull()) {
        // first NULL in this column: back-fill the all-valid prefix so
        // validity stays index-aligned with the dense typed vector
        if (cv.all_valid) {
            cv.validity.assign(static_cast<size_t>(cv.size()), 1);
            cv.all_valid = false;
        }
        cv.validity.push_back(0);
        // placeholder keeps the typed vector dense; never read without isNull()
        switch (cv.type) {
            case TypeId::INT:
                std::get<std::vector<int64_t>>(cv.data).push_back(0); break;
            case TypeId::DOUBLE:
                std::get<std::vector<double>>(cv.data).push_back(0.0); break;
            case TypeId::STRING:
                std::get<std::vector<std::string>>(cv.data).push_back(std::string()); break;
        }
        return;
    }

    if (!cv.all_valid) cv.validity.push_back(1);
    switch (cv.type) {
        case TypeId::INT:
            std::get<std::vector<int64_t>>(cv.data).push_back(v.asInt()); break;
        case TypeId::DOUBLE:
            // The type rule first: it fires at any magnitude, and it is the one
            // the plan armed. OBSERVABLE holds on all but a handful of columns
            // in a plan, so the out-of-line Value::type() call inside stays off
            // the path every ordinary DOUBLE cell takes.
            if (cv.int_narrowing == IntNarrowing::OBSERVABLE) {
                refuseObservableIntNarrowing(v);
            } else if (cv.int_narrowing == IntNarrowing::VOLCANO_INT
                    || cv.int_narrowing == IntNarrowing::VOLCANO_INT_UNRENDERED) {
                // The value here is already a DOUBLE and Volcano's is an exact
                // integer; neither call below can see that, so this one asks.
                refuseDivergentVolcanoInt(
                    v, cv.int_narrowing == IntNarrowing::VOLCANO_INT_UNRENDERED);
            }
            std::get<std::vector<double>>(cv.data).push_back(
                narrowToDoubleColumn(
                    v, cv.int_narrowing == IntNarrowing::UNRENDERED
                    || cv.int_narrowing == IntNarrowing::VOLCANO_INT_UNRENDERED));
            break;
        case TypeId::STRING:
            std::get<std::vector<std::string>>(cv.data).push_back(v.asString()); break;
    }
}
