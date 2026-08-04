#pragma once
#include "ast.h"
#include <string>

inline std::string exprToString(const Expr* expr) {
    if (!expr) return "?";
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        return col->table_name.empty()
            ? col->column_name
            : col->table_name + "." + col->column_name;
    }
    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        return lit->value.toString();
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        // parens are load-bearing: aggregateOutputName() is the byte-for-byte
        // contract between schema construction and evaluate()'s lookup, and
        // extractAggregates dedupes specs by this name — without parens,
        // SUM(a - (b - c)) and SUM((a - b) - c) would collide into one column
        return "(" + exprToString(bin->left.get())
            + " " + bin->op + " "
            + exprToString(bin->right.get()) + ")";
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        return "(" + un->op + exprToString(un->operand.get()) + ")";
    }
    if (auto* n = dynamic_cast<const IsNullExpr*>(expr)) {
        return exprToString(n->operand.get())
            + (n->is_not_null ? " IS NOT NULL" : " IS NULL");
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        std::string arg = agg->is_star ? "*" : exprToString(agg->argument.get());
        return agg->function_name + "(" + arg + ")";
    }
    return "?";
}

// Output-column name for a computed aggregate.
// Contract: schema construction (buildAggregateSchema / buildProjectSchema)
// and evaluate()'s AggregateExpr lookup must agree byte-for-byte — both call
// this on the bound AST. Qualified arguments keep their as-typed qualifier,
// which keeps self-join aggregates like AVG(l1.id) / AVG(l2.id) distinct in
// the aggregate output schema.
inline std::string aggregateOutputName(const AggregateExpr* agg) {
    return exprToString(agg);
}

// Collect every AggregateExpr in an expression tree (aggregates cannot nest,
// so an AggregateExpr terminates the walk).
inline void collectAggregates(const Expr* expr, std::vector<const AggregateExpr*>& out) {
    if (!expr) return;
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        out.push_back(agg);
        return;
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        collectAggregates(bin->left.get(), out);
        collectAggregates(bin->right.get(), out);
        return;
    }
    if (auto* isnull = dynamic_cast<const IsNullExpr*>(expr)) {
        collectAggregates(isnull->operand.get(), out);
        return;
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        collectAggregates(un->operand.get(), out);
    }
}
