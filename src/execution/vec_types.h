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

// batch of rows
// column i corrsponds to schema column i in order
// numbers of rows may be less than batch size for final chunk
struct DataChunk {
    std::vector<ColumnVector> columns;
    int num_rows = 0;
};

// indices of rows within a DataCHunk that have passed a dilter
// produced by VecFilterNode
struct SelectionVector {
    std::vector<int> indices;
    int size = 0;
};