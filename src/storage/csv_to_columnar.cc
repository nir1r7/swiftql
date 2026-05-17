
#include "csv_to_columnar.h"

ColumnarTable CSVToColumnar::convert(const std::vector<Row>& rows, const Schema& schema){
    ColumnarTable table(schema, static_cast<int>(rows.size()));

    /*
    - two pass structure (initialize, then disperse) is intentional
    - could combine into single pass, but would need to check and initialize each column slot on first visit
    */

    // initialize one typed array per column
    for (int c = 0; c < schema.size(); ++c){
        const std::string& name = schema.column(c).name;
        switch (schema.column(c).type){
            case TypeId::INT: {
                auto& v = table.columns[name].emplace<std::vector<int64_t>>();
                v.reserve(rows.size());
                break;
            }
            case TypeId::DOUBLE: {
                auto& v = table.columns[name].emplace<std::vector<double>>();
                v.reserve(rows.size());
                break;
            }
            case TypeId::STRING: {
                auto& v = table.columns[name].emplace<std::vector<std::string>>();
                v.reserve(rows.size());
                break;
            }
        }
    }

    // dispere each row's values into the appropriate column arrays
    for (const auto& row : rows){
        for (int c = 0; c < schema.size(); ++c){
            const std::string& name = schema.column(c).name;
            switch (schema.column(c).type){
                case TypeId::INT:
                    std::get<std::vector<int64_t>>(table.columns[name]).push_back(row[c].asInt());
                    break;
                case TypeId::DOUBLE:
                    std::get<std::vector<double>>(table.columns[name]).push_back(row[c].asDouble());
                    break;
                case TypeId::STRING:
                    std::get<std::vector<std::string>>(table.columns[name]).push_back(row[c].asString());
                    break;
            }
        }
    }
    return table;
}