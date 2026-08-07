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
        // Week 30. The body of validate(), which used to be everything except
        // the final "not yet executable" refusal — a refusal Week 33 deleted, so
        // the two now differ only in that validate() is the entry point. The
        // split is KEPT rather than collapsed: a NESTED query is validated
        // through validateQuery, and the distinction between "runs once for the
        // whole statement" and "runs per query block" is the shape any future
        // statement-level rule needs. There is no statement-level rule today.
        static void validateQuery(const SelectStatement& stmt, const Catalog& catalog);
        // `catalog` is needed since Week 30: a SubqueryExpr's body is a whole
        // query and is validated against its own FROM schema.
        // `allow_subqueries` defaults to FALSE — a new call site must opt in,
        // the same fail-closed discipline distribute() uses with `== INNER`.
        static void validateExpr(const Expr* expr, const Schema& schema, const std::string& context,
                                 const Catalog& catalog,
                                 bool allow_aggregates = true,
                                 bool allow_subqueries = false);
        // `relations` is (ref table name, schema) for every relation in the
        // query, in range-table order — the N-relation replacement for the
        // (left, right) pair Phase 4 passed.
        static void validateJoinCondition(const Expr* expr,
            const std::vector<std::pair<std::string, const Schema*>>& relations);
};