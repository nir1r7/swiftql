#pragma once

#include "parser/ast.h"
#include "catalog/catalog.h"
#include <string>
#include <utility>
#include <vector>

class Validator {
    public:
        static void validate(const SelectStatement& stmt, const Catalog& catalog);
    private:
        static void validateExpr(const Expr* expr, const Schema& schema, const std::string& context, bool allow_aggregates = true);
        // `relations` is (ref table name, schema) for every relation in the
        // query, in range-table order — the N-relation replacement for the
        // (left, right) pair Phase 4 passed.
        static void validateJoinCondition(const Expr* expr,
            const std::vector<std::pair<std::string, const Schema*>>& relations);
};