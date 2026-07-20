#include "table_stats.h"
#include <unordered_set>

TableStats TableStats::compute(const std::vector<Row>& rows, const Schema& schema) {
    TableStats stats;
    stats.row_count = static_cast<int64_t>(rows.size());

    for (int c = 0; c < schema.size(); ++c) {
        const ColumnDef& col = schema.column(c);
        ColumnStats cs;

        // typed distinct sets: Value has no std::hash, so dispatch on the
        // column's declared type (uniform within a column by loader contract)
        std::unordered_set<int64_t> distinct_ints;
        std::unordered_set<double> distinct_doubles;
        std::unordered_set<std::string> distinct_strings;

        double width_sum = 0.0;
        int64_t non_null = 0;

        for (const auto& row : rows) {
            const Value& v = row[c];
            if (v.isNull()) {
                ++cs.null_count;
                continue;
            }

            // seed min/max from the first non-null value: Value comparisons
            // return false against null, so a null accumulator never updates
            if (non_null == 0) {
                cs.min_val = v;
                cs.max_val = v;
            } else {
                if (v < cs.min_val) cs.min_val = v;
                if (v > cs.max_val) cs.max_val = v;
            }
            ++non_null;

            switch (col.type) {
                case TypeId::INT:
                    distinct_ints.insert(v.asInt());
                    width_sum += sizeof(int64_t);
                    break;
                case TypeId::DOUBLE:
                    distinct_doubles.insert(v.asDouble());
                    width_sum += sizeof(double);
                    break;
                case TypeId::STRING:
                    distinct_strings.insert(v.asString());
                    width_sum += static_cast<double>(v.asString().size());
                    break;
            }
        }

        cs.distinct_count = static_cast<int64_t>(
            distinct_ints.size() + distinct_doubles.size() + distinct_strings.size());
        cs.avg_width = non_null > 0 ? width_sum / non_null : 0.0;
        // min_val/max_val stay Value::null() when non_null == 0 (default ctor)

        stats.columns.emplace(col.name, std::move(cs));
    }

    return stats;
}
