#pragma once

#include "common/type_id.h"
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

    int size() const {
        return std::visit([](const auto&v){
            return static_cast<int>(v.size());
        }, data);
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
