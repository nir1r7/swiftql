#pragma once

#include "common/value.h"
#include "common/schema.h"
#include <variant>
#include <vector>
#include <string>
#include <unordered_map>

using ColumnArray = std::variant<std::vector<int64_t>, std::vector<double>, std::vector<std::string>>;

struct ColumnarTable {
    explicit ColumnarTable() : schema(std::vector<ColumnDef>{}), num_rows(0) {}
    ColumnarTable(Schema s, int n) : schema(std::move(s)), num_rows(n) {}

    Schema schema;
    int num_rows;
    // ordering is with schema
    std::unordered_map<std::string, ColumnArray> columns;

    Value getValue(const std::string& col_name, int row_idx) const {
        const auto& arr = columns.at(col_name);
        if (std::holds_alternative<std::vector<int64_t>>(arr)){
            return Value(std::get<std::vector<int64_t>>(arr).at(row_idx));
        }
        if (std::holds_alternative<std::vector<double>>(arr)){
            return Value(std::get<std::vector<double>>(arr).at(row_idx));
        }
        return Value(std::get<std::vector<std::string>>(arr).at(row_idx));
    }
};