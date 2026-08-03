#pragma once

#include "common/value.h"
#include "common/schema.h"
#include "dictionary_encoder.h"
#include "rle_column.h"
#include <stdexcept>
#include <variant>
#include <vector>
#include <string>
#include <unordered_map>

using ColumnArray = std::variant<std::vector<int64_t>, std::vector<double>, std::vector<std::string>, DictionaryEncoder, RLEColumn>;

static constexpr int CHUNK_SIZE = 8192;

struct ColumnChunk {
    int start_row;
    int row_count;
    Value min_val;
    Value max_val;
};

struct ColumnarTable {
    explicit ColumnarTable() : schema(std::vector<ColumnDef>{}), num_rows(0) {}
    ColumnarTable(Schema s, int n) : schema(std::move(s)), num_rows(n) {}

    Schema schema;
    int num_rows;
    // ordering is with schema
    std::unordered_map<std::string, ColumnArray> columns;
    // same key as columns map above
    std::unordered_map<std::string, std::vector<ColumnChunk>> zone_maps;

    Value getValue(const std::string& col_name, int row_idx) const {
        const auto& arr = columns.at(col_name);
        if (std::holds_alternative<std::vector<int64_t>>(arr)){
            return Value(std::get<std::vector<int64_t>>(arr).at(row_idx));
        }
        if (std::holds_alternative<std::vector<double>>(arr)){
            return Value(std::get<std::vector<double>>(arr).at(row_idx));
        }
        if (std::holds_alternative<std::vector<std::string>>(arr)){
            return Value(std::get<std::vector<std::string>>(arr).at(row_idx));
        }
        if (std::holds_alternative<DictionaryEncoder>(arr)){
            return Value(std::get<DictionaryEncoder>(arr).decode(row_idx));
        }
        if (std::holds_alternative<RLEColumn>(arr)){
            return Value(std::get<RLEColumn>(arr).get(static_cast<int32_t>(row_idx)));
        }
        throw std::runtime_error("unknown ColumnArray variant in getValue");
    }
};

inline size_t columnByteSize(const ColumnArray& arr) {
    if (std::holds_alternative<std::vector<int64_t>>(arr)){
        return std::get<std::vector<int64_t>>(arr).size() * sizeof(int64_t);
    }
    if (std::holds_alternative<std::vector<double>>(arr)){
        return std::get<std::vector<double>>(arr).size() * sizeof(double);
    }
    if (std::holds_alternative<std::vector<std::string>>(arr)) {
        size_t sz = 0;
        for (const auto& s : std::get<std::vector<std::string>>(arr))
            sz += sizeof(std::string) + s.size();
        return sz;
    }
    if (std::holds_alternative<DictionaryEncoder>(arr)){
        return std::get<DictionaryEncoder>(arr).byteSize();
    }
    if (std::holds_alternative<RLEColumn>(arr)){
        return std::get<RLEColumn>(arr).byteSize();
    }
    return 0;
}

inline size_t columnarTableByteSize(const ColumnarTable& table) {
    size_t total = 0;
    for (const auto& [name, arr] : table.columns)
        total += columnByteSize(arr);
    return total;
}