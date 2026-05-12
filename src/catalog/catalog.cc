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

        TableMetadata meta {table_json["name"].get<std::string>(), abs_path, Schema(cols)};

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

TypeId Catalog::parseTypeId(const std::string& type_str) const {
    if (type_str == "INT")    return TypeId::INT;
    if (type_str == "DOUBLE") return TypeId::DOUBLE;
    if (type_str == "STRING") return TypeId::STRING;
    throw std::runtime_error("Unknown type in catalog: " + type_str);
}