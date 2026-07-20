#pragma once

#include "table_metadata.h"
#include "table_stats.h"
#include <string>
#include <unordered_map>
#include <optional>

class Catalog {
    public:
        // load all table metadata from a catalog.json file
        explicit Catalog(const std::string& catalog_path);

        bool hasTable(const std::string& name) const;

        // get metadata for a table
        const TableMetadata& getTable(const std::string& name) const;

        void setStats(const std::string& table_name, TableStats stats);
        bool hasStats(const std::string& table_name) const;
        const TableStats& getStats(const std::string& table_name) const; // throws if absent

    private:
        std::unordered_map<std::string, TableMetadata> tables_;
        std::unordered_map<std::string, TableStats> stats_;
    
        // helper to parse TypeId from string ("INT", "DOUBLE", "STRING")
        TypeId parseTypeId(const std::string& type_str) const;
};