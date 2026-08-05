#pragma once

#include "common/type_id.h"
#include "common/value.h"
#include <string>
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

// Append one cell, NULL-aware. `cv.type` decides the storage type. An INT Value
// widens into a DOUBLE column (lossless, and the same promotion evaluate() does
// via toNumeric() — reachable when a schema declares DOUBLE for a type-
// preserving MIN/MAX over an INT column). Every other type disagreement is a
// planner/schema bug and surfaces as bad_variant_access from the typed
// accessor, as it did before validity existed.
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
            std::get<std::vector<double>>(cv.data).push_back(v.toNumeric()); break;
        case TypeId::STRING:
            std::get<std::vector<std::string>>(cv.data).push_back(v.asString()); break;
    }
}
