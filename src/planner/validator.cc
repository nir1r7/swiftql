#include "validator.h"

void Validator::validate(const SelectStatement& stmt, const Catalog& catalog){
    // FROM table must exist
    if (!catalog.hasTable(stmt.from_table)) {
        throw std::runtime_error(
            "Table not found: '" + stmt.from_table + "'");
    }
    const Schema& schema = catalog.getTable(stmt.from_table).schema;

    // JOIN table must exist (if present)
    if (stmt.join.has_value()) {
        if (!catalog.hasTable(stmt.join->join_table)) {
            throw std::runtime_error(
                "Join table not found: '" + stmt.join->join_table + "'");
        }
    }

    // WHERE columns must exist
    if (stmt.where) {
        validateExpr(stmt.where.get(), schema, "WHERE");
    }

    // GROUP BY columns must exist
    for (const auto& col : stmt.group_by) {
        if (!schema.hasColumn(col)) {
            throw std::runtime_error(
                "GROUP BY column not found: '" + col + "'");
        }
    }

    // HAVING requires GROUP BY
    if (stmt.having && stmt.group_by.empty()) {
        throw std::runtime_error("HAVING requires GROUP BY");
    }

    // ORDER BY columns must exist
    for (const auto& col : stmt.order_by) {
        if (!schema.hasColumn(col)) {
            throw std::runtime_error(
                "ORDER BY column not found: '" + col + "'");
        }
    }
}


// recursively validate an expression and its sub expressions
void Validator::validateExpr(const Expr* expr, const Schema& schema, const std::string& context) {
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // skip validation for qualified refs (table.column)
        // full resolution handled when join schema is merged
        if (col->table_name.empty() && !schema.hasColumn(col->column_name)) {
            throw std::runtime_error(context + ": column not found: '" + col->column_name + "'");
        }
    }
    else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        validateExpr(bin->left.get(), schema, context);
        validateExpr(bin->right.get(), schema, context);
    }
    else if (auto* isnull = dynamic_cast<const IsNullExpr*>(expr)) {
        validateExpr(isnull->operand.get(), schema, context);
    }
    else if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        if (!agg->is_star && agg->argument) {
            validateExpr(agg->argument.get(), schema, context);
        }
    }
    // literal nodes need no validation
}