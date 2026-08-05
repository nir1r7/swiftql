#pragma once
#include "ast.h"
#include <stdexcept>
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

// Canonical identity string for an expression — for MATCHING, never for display.
//
// A ColumnRef renders by its binder-assigned relation slot instead of the
// as-typed qualifier, so `season` and `laps.season` (both slot 0) produce the
// same key, while a self-join's `l1.speed` and `l2.speed` (slots 0 and 1) stay
// distinct. That is exactly the identity the plain-column GROUP BY path already
// uses — (relation_slot, column_name) — so expression group keys now match the
// same way. Unbound refs (slot -1) fall back to the as-typed qualifier.
//
// Display names stay on exprToString: users see `(season - 1)`, not `(0#season - 1)`.
inline std::string exprKey(const Expr* expr) {
    if (!expr) return "?";
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        if (col->relation_slot >= 0)
            return std::to_string(col->relation_slot) + "#" + col->column_name;
        return col->table_name.empty()
            ? col->column_name
            : col->table_name + "." + col->column_name;
    }
    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        // Tag the type. Value::toString() renders the DOUBLE 1.0 as "1" (%.15g),
        // so an untagged key made `GROUP BY season - 1` match
        // `SELECT season - 1.0` and emit the INT column where SQL says REAL.
        if (lit->value.isNull()) return "NULL";
        switch (lit->value.type()) {
            case TypeId::INT:    return "i" + lit->value.toString();
            case TypeId::DOUBLE: return "d" + lit->value.toString();
            case TypeId::STRING: return "s'" + lit->value.toString() + "'";
        }
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        return "(" + exprKey(bin->left.get()) + " " + bin->op + " "
                   + exprKey(bin->right.get()) + ")";
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        return "(" + un->op + exprKey(un->operand.get()) + ")";
    }
    if (auto* n = dynamic_cast<const IsNullExpr*>(expr)) {
        return exprKey(n->operand.get()) + (n->is_not_null ? " IS NOT NULL" : " IS NULL");
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        std::string arg = agg->is_star ? "*" : exprKey(agg->argument.get());
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

// Deep copy of an expression tree, preserving binder stamps and alias.
// DISPATCH SITE: every new Expr subtype must be added here.
inline std::unique_ptr<Expr> cloneExpr(const Expr* expr) {
    if (!expr) return nullptr;
    std::unique_ptr<Expr> out;
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        out = std::make_unique<ColumnRef>(*col);   // memberwise: keeps relation_slot
    } else if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        out = std::make_unique<Literal>(lit->value);
    } else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        auto b = std::make_unique<BinaryExpr>();
        b->op = bin->op;
        b->left = cloneExpr(bin->left.get());
        b->right = cloneExpr(bin->right.get());
        out = std::move(b);
    } else if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        auto u = std::make_unique<UnaryExpr>();
        u->op = un->op;
        u->operand = cloneExpr(un->operand.get());
        out = std::move(u);
    } else if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) {
        auto n = std::make_unique<IsNullExpr>();
        n->operand = cloneExpr(isn->operand.get());
        n->is_not_null = isn->is_not_null;
        out = std::move(n);
    } else if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        auto a = std::make_unique<AggregateExpr>();
        a->function_name = agg->function_name;
        a->argument = cloneExpr(agg->argument.get());
        a->is_star = agg->is_star;
        out = std::move(a);
    } else {
        throw std::runtime_error("cloneExpr(): unknown Expr subtype");
    }
    out->alias = expr->alias;
    return out;
}
