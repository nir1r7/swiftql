#pragma once

#include "common/type_id.h"
#include "common/value.h"
#include <string>
#include <stdexcept>
#include <variant>
#include <vector>
#include <cstdint>


// vectorized execution batch size
// distinct from zone map CHUNK_SIZE (8192)
static constexpr int BATCH_SIZE = 1024;

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
    // refuseObservableIntNarrowing below. false — the default, and the value on
    // every scan chunk, join output and ExpressionExecutor result — means the
    // narrowing cannot change an answer here and only the magnitude rule
    // applies. The two nodes that materialize a `Value` into a column type
    // declared BEFORE the value exists (VecProjectNode, VecHashAggregateNode)
    // set it per column, from a mask the vectorized plan builder computes over
    // the LOGICAL plan.
    //
    // It rides on the ColumnVector, not on the node, because appendColumnValue
    // is the single funnel every such write goes through; the cost is one
    // predictably not-taken branch on the DOUBLE arm.
    bool int_observable = false;

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
static constexpr int64_t MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN = 1000000000000000LL;

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
inline double narrowToDoubleColumn(const Value& v) {
    // STRING raises bad_variant_access from toNumeric(), as it did before.
    const double d = v.toNumeric();
    const double bound = static_cast<double>(MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN);
    if (d < bound && d > -bound) return d;
    if (v.type() != TypeId::INT) return d;   // a genuine DOUBLE that big is fine
    throw std::runtime_error(
        "vectorized execution cannot materialize the integer " +
        std::to_string(v.asInt()) + " into a DOUBLE result column without "
        "changing it. A chunk column holds one type, so an expression that "
        "mixes INTEGER and REAL results (typically a CASE) is stored as REAL, "
        "which is exact and prints identically only below 1e15. Re-run with "
        "--execution volcano, or give the branches the same type.");
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
// can, and it arms `cv.int_observable` on exactly the columns whose INT-ness
// can reach an operand of a `/` somewhere above them (armIntObservableColumns).
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

// Append one cell, NULL-aware. `cv.type` decides the storage type. An INT Value
// narrows into a DOUBLE column through TWO refusals — refuseObservableIntNarrowing
// (the value's TYPE is observable to a `/` above, at any magnitude) and
// narrowToDoubleColumn (the value or its rendering changes, above 1e15) — both
// of which THROW rather than answer differently from Volcano and SQLite. Read
// both comments before touching this. This comment called the conversion "lossless" from 85be432
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
            // the plan armed. `int_observable` is false on all but a handful of
            // columns in a plan, so the out-of-line Value::type() call inside
            // stays off the path every ordinary DOUBLE cell takes.
            if (cv.int_observable) refuseObservableIntNarrowing(v);
            std::get<std::vector<double>>(cv.data).push_back(narrowToDoubleColumn(v)); break;
        case TypeId::STRING:
            std::get<std::vector<std::string>>(cv.data).push_back(v.asString()); break;
    }
}
