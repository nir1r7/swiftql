#pragma once

#include "catalog/table_metadata.h"
#include "common/schema.h"
#include "common/value.h"
#include <vector>
#include <string>

using Row = std::vector<Value>;

class CSVLoader {
    public:
        // Load a delimited file and return all rows typed according to schema.
        //
        // Week 35: `fmt` carries the delimiter, whether line 1 is a header, and
        // whether each line ends with a trailing delimiter (TPC-H .tbl does).
        // It is DEFAULTED to CSV so every pre-existing call site — all of them
        // in tests/, all genuinely CSV — is unchanged. main.cc passes
        // TableMetadata::format.
        static std::vector<Row> load(const std::string& filepath,
                                     const Schema& schema,
                                     const FileFormat& fmt = FileFormat::csv());

    private:
        // split one line into raw string fields on `delimiter`
        static std::vector<std::string> splitLine(const std::string& line, char delimiter = ',');

        // convert raw string field to a typed Value
        static Value parseField(const std::string& field, TypeId type);
};
