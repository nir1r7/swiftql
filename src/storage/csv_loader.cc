#include "csv_loader.h"
#include <fstream>
#include <stdexcept>

std::vector<Row> CSVLoader::load(const std::string& filepath, const Schema& schema,
                                 const FileFormat& fmt){
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filepath);
    }

    std::vector<Row> rows {};
    std::string line;
    size_t line_no = 0;

    // Week 35: a header is a PROPERTY, not an assumption. This used to be an
    // unconditional getline, which on a headerless TPC-H .tbl silently discards
    // the first real row of the table — `SELECT COUNT(*) FROM region` returning
    // 4 instead of 5, with no diagnostic anywhere.
    if (fmt.has_header) {
        std::getline(file, line);
        ++line_no;
    }

    while(std::getline(file, line)){
        ++line_no;
        if (line.empty()) continue;
        // Tolerate CRLF input: a stray '\r' would otherwise become part of the
        // last field, so an INT last column would throw and a STRING one would
        // silently carry an invisible byte into every comparison and hash key.
        if (line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        auto fields = splitLine(line, fmt.delimiter);

        // TPC-H .tbl TERMINATES each field rather than separating them, so a
        // correct split leaves one trailing empty field. Gated on BOTH the
        // format flag and the field actually being empty: an unconditional
        // pop_back would eat a genuine last column whose value is empty.
        if (fmt.trailing_delimiter && !fields.empty() && fields.back().empty()) {
            fields.pop_back();
        }

        if (fields.size() != static_cast<size_t>(schema.size())) {
            // Name the file and the line: at 60k rows "Column count mismatch"
            // on its own is unactionable.
            throw std::runtime_error(
                "Column count mismatch in " + filepath + " line " +
                std::to_string(line_no) + ": expected " +
                std::to_string(schema.size()) + ", got " +
                std::to_string(fields.size()));
        }

        Row row;
        for (int i = 0; i < schema.size(); i++) {
            try {
                row.push_back(parseField(fields[i], schema.column(i).type));
            } catch (const std::exception& e) {
                // The field's own message says WHAT it could not be; this adds
                // WHERE. A mistyped catalog column is the likeliest cause and
                // the column name is what makes that obvious.
                throw std::runtime_error(
                    std::string(e.what()) + " (" + filepath + " line " +
                    std::to_string(line_no) + ", column '" +
                    schema.column(i).name + "')");
            }
        }
        rows.push_back(std::move(row));
    }

    return rows;
}

std::vector<std::string> CSVLoader::splitLine(const std::string& line, char delimiter){
    std::vector<std::string> vec {};

    std::string str = "";
    for (int i = 0; i < line.size(); i++){
        char c = line.at(i);
        if (c == delimiter){
            vec.push_back(str);
            str = "";
        } else {
            str += c;
        }
    }
    vec.push_back(str);

    return vec;
}

Value CSVLoader::parseField(const std::string& field, TypeId type){
    switch (type) {
        case TypeId::INT: {
            size_t pos = 0;
            int64_t v = std::stoll(field, &pos);
            // Week 35 — FULL CONSUMPTION is the whole point of this check.
            // Without it a mistyped catalog column converts SILENTLY: stod on
            // "1996-01-02" stops at the first '-' and returns 1996.0, so a DATE
            // column typed DOUBLE answers every predicate on a year-shaped
            // number with no error at any layer. This week hand-types ~61 TPC-H
            // columns; that is the failure mode to make loud.
            if (pos != field.size()) {
                throw std::runtime_error("not an INT: '" + field + "'");
            }
            return Value(v);
        }
        case TypeId::DOUBLE: {
            size_t pos = 0;
            double v = std::stod(field, &pos);
            if (pos != field.size()) {
                throw std::runtime_error("not a DOUBLE: '" + field + "'");
            }
            return Value(v);
        }
        case TypeId::STRING:
            return Value(field);
        default:
            throw std::runtime_error("Unknown TypeId in parseField");
    }
}
