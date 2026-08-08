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
            // Week 37, seam-storage pass 2 finding S-8. A repeated column name
            // is the ONE input found across two audit passes that makes the two
            // storage formats disagree, and it disagrees SILENTLY:
            //
            //   catalog: t(k INT, k INT)   data: k,k / 1,100 / 2,200 / 3,300
            //   SELECT k FROM t            row 1,2,3   columnar 1,100,2
            //   SELECT COUNT(*), SUM(k)    row 3,6     columnar 3,103
            //
            // Row storage is positional (csv_loader.cc) and picks the first `k`.
            // ColumnarTable::columns is keyed by NAME (columnar_table.h), so
            // CSVToColumnar's pass 1 emplaces one vector for both schema entries
            // and pass 2 pushes BOTH values of every row into it — the vector
            // interleaves while num_rows stays 3, and getValue("k", r) reads
            // element r, i.e. row 1 gets row 0's second column.
            //
            // Refused here rather than repaired downstream, and refused for the
            // reason the engine ALREADY refuses the identical shape one layer up
            // for a derived relation (logical_plan.cc, "column '<c>' is produced
            // twice"): a schema with two same-named columns has no answer to
            // "which one is `k`". The catalog was the one place that shape could
            // still get in. Exact-match, because Schema::indexOf is exact-match
            // and ColumnarTable's map is keyed on the same bytes.
            for (const ColumnDef& seen : cols) {
                if (seen.name != colDef.name) continue;
                throw std::runtime_error(
                    "catalog: table '" + table_json["name"].get<std::string>() +
                    "': column '" + colDef.name + "' is declared twice; "
                    "give one of them a distinct name");
            }
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