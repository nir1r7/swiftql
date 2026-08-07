#include "catalog.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <filesystem>

using json = nlohmann::json;

Catalog::Catalog(const std::string& catalog_path){
    std::ifstream file(catalog_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open catalog: " + catalog_path);
    }

    // resolve data file paths relative to the catalog file's directory.
    // use absolute() first so the result is cwd-independent.
    std::filesystem::path catalog_dir =
        std::filesystem::absolute(catalog_path).parent_path();

    json data = json::parse(file);

    for (auto& table_json : data["tables"]){
        std::vector<ColumnDef> cols;

        for (auto& col : table_json["columns"]){
            ColumnDef colDef {col["name"].get<std::string>(), parseTypeId(col["type"].get<std::string>())};
            cols.push_back(colDef);
        }

        std::string rel_path = table_json["file"].get<std::string>();
        std::string abs_path = (catalog_dir / rel_path).string();

        // Week 35 — the optional "format" object. ABSENT means CSV, which is
        // what keeps the F1 catalog.json byte-compatible; a TPC-H catalog says
        // {"delimiter": "|", "header": false, "trailing_delimiter": true}.
        FileFormat fmt = FileFormat::csv();
        if (table_json.contains("format")) {
            const auto& f = table_json["format"];
            if (f.contains("delimiter")) {
                std::string d = f["delimiter"].get<std::string>();
                // splitLine takes a char, so a multi-character delimiter has no
                // representation. Refusing it beats silently using its first
                // byte and mis-splitting every row in the file.
                if (d.size() != 1) {
                    throw std::runtime_error(
                        "catalog: table '" + table_json["name"].get<std::string>() +
                        "' has a multi-character delimiter '" + d +
                        "'; only a single character is supported");
                }
                fmt.delimiter = d[0];
            }
            if (f.contains("header")) fmt.has_header = f["header"].get<bool>();
            if (f.contains("trailing_delimiter")) {
                fmt.trailing_delimiter = f["trailing_delimiter"].get<bool>();
            }
        }

        TableMetadata meta {table_json["name"].get<std::string>(), abs_path,
                            Schema(cols), fmt};

        tables_.emplace(meta.name, std::move(meta));
    }
}

bool Catalog::hasTable(const std::string& name) const {
    return tables_.find(name) != tables_.end();
}

const TableMetadata& Catalog::getTable(const std::string& name) const {
    if (!hasTable(name)){
        throw std::runtime_error("Table name does not exist");
    }

    return tables_.at(name);
}

void Catalog::setStats(const std::string& table_name, TableStats stats) {
    if (!hasTable(table_name)) {
        throw std::runtime_error("Cannot set stats for unknown table: " + table_name);
    }
    stats_[table_name] = std::move(stats);
}

bool Catalog::hasStats(const std::string& table_name) const {
    return stats_.find(table_name) != stats_.end();
}

const TableStats& Catalog::getStats(const std::string& table_name) const {
    auto it = stats_.find(table_name);
    if (it == stats_.end()) {
        throw std::runtime_error("No statistics for table: " + table_name);
    }
    return it->second;
}

TypeId Catalog::parseTypeId(const std::string& type_str) const {
    if (type_str == "INT")    return TypeId::INT;
    if (type_str == "DOUBLE") return TypeId::DOUBLE;
    if (type_str == "STRING") return TypeId::STRING;
    throw std::runtime_error("Unknown type in catalog: " + type_str);
}