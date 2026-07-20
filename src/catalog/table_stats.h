
#pragma once

#include "common/schema.h"
#include "common/value.h"
#include <string>
#include <unordered_map>
#include <vector>

// per-column summary statistics for the Phase 4 optimizer
struct ColumnStats {
    Value min_val;
    Value max_val;
    int64_t distinct_count = 0;
    int64_t null_count = 0;
    double avg_width = 0.0;     // bytes per non-null value; 8 for INT/DOUBLE
};

// table-level statistics, computed once from loaded rows
struct TableStats {
    int64_t row_count = 0;
    std::unordered_map<std::string, ColumnStats> columns;

    // compute stats in one pass per column over the loaded rows
    // static factory, matching CSVLoader::load / DictionaryEncoder::encode style
    static TableStats compute(const std::vector<Row>& rows, const Schema& schema);
};