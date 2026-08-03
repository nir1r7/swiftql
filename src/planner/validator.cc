#include "validator.h"
#include "join_condition.h"
#include "parser/expr_utils.h"

void Validator::validate(const SelectStatement& stmt, const Catalog& catalog){
    // FROM table must exist
    if (!catalog.hasTable(stmt.from_table)) {
        throw std::runtime_error(
            "Table not found: '" + stmt.from_table + "'");
    }
    const Schema& schema = catalog.getTable(stmt.from_table).schema;

    // SELECT list columns must exist (skip for SELECT *)
    if (!stmt.select_star) {
        for (const auto& expr : stmt.select_list) {
            validateExpr(expr.get(), schema, "SELECT");
        }
    }

    // aggregate functions must be applied to compatible column types
    for (const auto& expr : stmt.select_list) {
        if (auto* agg = dynamic_cast<const AggregateExpr*>(expr.get())) {
            if (agg->is_star) continue;
            if (agg->function_name == "SUM" || agg->function_name == "AVG") {
                if (agg->argument) {
                    if (auto* col = dynamic_cast<const ColumnRef*>(agg->argument.get())) {
                        if (col->table_name.empty() && schema.hasColumn(col->column_name)) {
                            TypeId t = schema.column(schema.indexOf(col->column_name)).type;
                            if (t == TypeId::STRING){
                                throw std::runtime_error(agg->function_name + "() requires a numeric column, but '" + col->column_name + "' is of type STRING");
                            }
                        }
                    }
                }
            }
        }
    }

    // JOIN table must exist (if present)
    if (stmt.join.has_value()) {
        if (!catalog.hasTable(stmt.join->join_table)) {
            throw std::runtime_error(
                "Join table not found: '" + stmt.join->join_table + "'");
        }
        if (stmt.join->condition) {
            // shape first (single cross-relation equality), then column existence
            classifyJoinCondition(stmt.join->condition.get());
            const Schema& right_schema = catalog.getTable(stmt.join->join_table).schema;
            validateJoinCondition(stmt.join->condition.get(), schema, stmt.from_table, right_schema, stmt.join->join_table);
        }
    }

    // WHERE columns must exist; aggregates are not allowed in WHERE
    if (stmt.where) {
        validateExpr(stmt.where.get(), schema, "WHERE", /*allow_aggregates=*/false);
    }

    // GROUP BY columns must exist. Binder-resolved entries (slot stamped) were
    // already verified; the rest check against the FROM table or, when a join
    // is present, the joined table.
    for (const auto& g : stmt.group_by) {
        if (g.relation_slot >= 0 && !g.table_name.empty()) continue; // binder verified
        bool found;
        if (!g.table_name.empty()) {
            // qualified but unbound (validator-only callers that skip the Binder)
            found = (g.table_name == stmt.from_table && schema.hasColumn(g.column_name))
                 || (stmt.join.has_value() && g.table_name == stmt.join->join_table
                     && catalog.getTable(stmt.join->join_table).schema.hasColumn(g.column_name));
        } else {
            found = schema.hasColumn(g.column_name);
            if (!found && stmt.join.has_value()) {
                found = catalog.getTable(stmt.join->join_table).schema.hasColumn(g.column_name);
            }
        }
        if (!found) {
            throw std::runtime_error(
                "GROUP BY column not found: '" + g.column_name + "'");
        }
    }

    // HAVING requires GROUP BY
    if (stmt.having && stmt.group_by.empty()) {
        throw std::runtime_error("HAVING requires GROUP BY");
    }

    // HAVING columns (and aggregate arguments) must exist
    if (stmt.having) {
        validateExpr(stmt.having.get(), schema, "HAVING", /*allow_aggregates=*/true);
    }

    // non aggregated SELECT columns must appear in GROUP BY when aggregates are present
    bool has_aggregates = false;
    for (const auto& expr : stmt.select_list) {
        if (dynamic_cast<const AggregateExpr*>(expr.get())) {
            has_aggregates = true;
            break;
        }
    }
    if (has_aggregates && !stmt.select_star) {
        for (const auto& expr : stmt.select_list) {
            if (auto* col = dynamic_cast<const ColumnRef*>(expr.get())) {
                bool in_group_by = false;
                for (const auto& g : stmt.group_by){
                    // name match plus slot compatibility: SELECT a.grp with
                    // GROUP BY b.grp is a different column, not a match.
                    // Unbound slots (-1) stay name-only for direct-validate callers.
                    if (g.column_name == col->column_name &&
                        (col->relation_slot < 0 || g.relation_slot < 0 ||
                         col->relation_slot == g.relation_slot)) {
                        in_group_by = true;
                        break;
                    }
                }
                if (!in_group_by){
                    throw std::runtime_error("SELECT column '" + col->column_name + "' must appear in GROUP BY or be used in an aggregate function");
                }
            }
        }
    }

    // ORDER BY: validate ColumnRef nodes against the base table schema.
    // Aggregate expressions (e.g. COUNT(*)) resolve against the post-aggregate
    // output schema at execution time and are not checked here.
    for (const auto& item : stmt.order_by) {
        if (auto* col = dynamic_cast<const ColumnRef*>(item.expr.get())) {
            if (col->table_name.empty() && !schema.hasColumn(col->column_name)) {
                throw std::runtime_error("ORDER BY column not found: '" + col->column_name + "'");
            }
        }
    }

    // an ORDER BY aggregate needs an aggregation context to be computed in
    if (stmt.group_by.empty() && !has_aggregates) {
        std::vector<const AggregateExpr*> order_aggs;
        for (const auto& item : stmt.order_by) {
            collectAggregates(item.expr.get(), order_aggs);
        }
        if (!order_aggs.empty()) {
            throw std::runtime_error(
                "ORDER BY aggregate requires GROUP BY or an aggregated SELECT list");
        }
    }
}


// recursively validate an expression and its sub expressions
void Validator::validateExpr(const Expr* expr, const Schema& schema, const std::string& context, bool allow_aggregates) {
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // skip validation for qualified refs (table.column)
        // full resolution handled when join schema is merged
        if (col->table_name.empty() && !schema.hasColumn(col->column_name)) {
            throw std::runtime_error(context + ": column not found: '" + col->column_name + "'");
        }
    }
    else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        validateExpr(bin->left.get(), schema, context, allow_aggregates);
        validateExpr(bin->right.get(), schema, context, allow_aggregates);
    }
    else if (auto* isnull = dynamic_cast<const IsNullExpr*>(expr)) {
        validateExpr(isnull->operand.get(), schema, context, allow_aggregates);
    }
    else if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        if (!allow_aggregates) {
            throw std::runtime_error(
                context + ": aggregate functions are not allowed in WHERE clause; use HAVING instead");
        }
        if (!agg->is_star && agg->argument) {
            validateExpr(agg->argument.get(), schema, context, allow_aggregates);
        }
    }
    // literal nodes need no validation
}


void Validator::validateJoinCondition(const Expr* expr, const Schema& left_schema, const std::string& left_table, const Schema& right_schema, const std::string& right_table){
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        if (col->table_name == left_table) {
            if (!left_schema.hasColumn(col->column_name)){
                throw std::runtime_error("JOIN ON: column '" + col->column_name + "' not found in table '" + left_table + "'");
            }
        } else if (col->table_name == right_table) {
            if (!right_schema.hasColumn(col->column_name)){
                throw std::runtime_error("JOIN ON: column '" + col->column_name + "' not found in table '" + right_table + "'");
            }
        } else if (col->table_name.empty()) {
            if (!left_schema.hasColumn(col->column_name) && !right_schema.hasColumn(col->column_name)){
                throw std::runtime_error("JOIN ON: column '" + col->column_name + "' not found in either joined table");
            }
        }
        // qualified ref with unknown table prefix: skip (alias, deferred resolution)
    } else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        validateJoinCondition(bin->left.get(),  left_schema, left_table, right_schema, right_table);
        validateJoinCondition(bin->right.get(), left_schema, left_table, right_schema, right_table);
    }
}