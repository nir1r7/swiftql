#pragma once

#include "parser/ast.h"
#include "catalog/catalog.h"
#include <vector>

// resolves table aliases and qualified/unqualified column references to a stable relation identity (the catalog table name)
// occurs before validation and planning run
// table existence errors are intentionally left to Validator (see bind()) so
// existing error messages for a missing FROM/JOIN table are unchanged
class Binder {
    public:
        static void bind(SelectStatement& stmt, const Catalog& catalog);

    private:
        struct RangeEntry {
            std::string ref_name; // alias if present, else the table name
            std::string table_name; // canonical catalog table name
            const Schema* schema;
        };

        static void bindExpr(Expr* expr, const std::vector<RangeEntry>& range_table);
        static void resolveColumnRef(ColumnRef* col, const std::vector<RangeEntry>& range_table);
};
