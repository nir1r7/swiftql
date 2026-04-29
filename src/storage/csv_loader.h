#pragma once

#include "common/schema.h"
#include "common/value.h"
#include <vector>
#include <string>

using Row = std::vector<Value>;

class CSVLoader {
    public:
        // load CSV file and return all rows typed according to schema
        static std::vector<Row> load(const std::string& filepath, const Schema& schema);

    private:
        // split a CSV line into raw string fields
        static std::vector<std::string> splitLine(const std::string& line, char delimiter = ',');

        // convert raw string field to a typed Value
        static Value parseField(const std::string& field, TypeId type);
};