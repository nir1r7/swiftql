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
inline double narrowToDoubleColumn(const Value& v) {
    // STRING lands on asDouble() and raises bad_variant_access, as before.
    if (v.type() != TypeId::INT) return v.asDouble();
    const int64_t i = v.asInt();
    if (i >= MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN ||
        i <= -MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN) {
        throw std::runtime_error(
            "vectorized execution cannot materialize the integer " +
            std::to_string(i) + " into a DOUBLE result column without changing "
            "it. A chunk column holds one type, so an expression that mixes "
            "INTEGER and REAL results (typically a CASE) is stored as REAL, "
            "which is exact and prints identically only below 1e15. Re-run with "
            "--execution volcano, or give the branches the same type.");
    }
    return static_cast<double>(i);
}

// Append one cell, NULL-aware. `cv.type` decides the storage type. An INT Value
// narrows into a DOUBLE column through narrowToDoubleColumn above, which THROWS
// rather than change the value or its rendering — read that comment before
// touching this; the widening was called "lossless" here for three weeks and it
// is not, above 1e15. Every other type disagreement is a planner/schema bug and
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
            std::get<std::vector<double>>(cv.data).push_back(narrowToDoubleColumn(v)); break;
        case TypeId::STRING:
            std::get<std::vector<std::string>>(cv.data).push_back(v.asString()); break;
    }
}
