#include "csv_loader.h"
#include <fstream>
#include <stdexcept>

std::vector<Row> CSVLoader::load(const std::string& filepath, const Schema& schema){
    std::ifstream file(filepath);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filepath);
    }

    std::vector<Row> rows {};
    std::string line;

    // skip header line
    std::getline(file, line);

    while(std::getline(file, line)){
        if (line.empty()) continue;

        auto fields = splitLine(line);

        if (fields.size() != static_cast<size_t>(schema.size())) {
            throw std::runtime_error("Column count mismatch in CSV row");
        }

        Row row;
        for (int i = 0; i < schema.size(); i++) {
            row.push_back(parseField(fields[i], schema.column(i).type));
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
        case TypeId::INT:
            return Value(static_cast<int64_t>(std::stoll(field)));
        case TypeId::DOUBLE:
            return Value(std::stod(field));
        case TypeId::STRING:
            return Value(field);
        default:
            throw std::runtime_error("Unknown TypeId in parseField");
    }
}