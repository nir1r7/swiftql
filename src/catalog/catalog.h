#pragma once

#include "table_metadata.h"
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

    private:
        std::unordered_map<std::string, TableMetadata> tables_;
    
        // Helper to parse TypeId from string ("INT", "DOUBLE", "STRING")
        TypeId parseTypeId(const std::string& type_str) const;
};