#include "catalog.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using json = nlohmann::json;

Catalog::Catalog(const std::string& catalog_path){
    std::ifstream file(catalog_path);
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open catalog: " + catalog_path);
    }

    json data = json::parse(file);

    for (auto& table_json : data["tables"]){
        std::vector<ColumnDef> cols;

        for (auto& col : table_json["columns"]){
            ColumnDef coldef {col["name"].get<std::string>(), parseTypeId(col["type"].get<std::string>())};
            cols.push_back(coldef);
        }

        TableMetadata meta {table_json["name"].get<std::string>(), table_json["file"].get<std::string>(), Schema(cols)};

        tables_[meta.name] = std::move(meta);
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