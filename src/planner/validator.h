#pragma once

#include "parser/ast.h"
#include "catalog/catalog.h"

class Validator {
    public:
        static void validate(const SelectStatement& stmt, const Catalog& catalog);
    private:
        static void validateExpr(const Expr* expr, const Schema& schema, const std::string& context);
        static void validateJoinCondition(const Expr* expr, const Schema& left_schema, const std::string& left_table, const Schema& right_schema, const std::string& right_table);
};