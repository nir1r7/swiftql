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
    if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        std::string s = exprToString(in->operand.get()) + (in->negated ? " NOT IN (" : " IN (");
        for (size_t i = 0; i < in->values.size(); ++i) {
            if (i) s += ", ";
            s += in->values[i].toString();
        }
        return s + ")";
    }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        return exprToString(lk->operand.get())
            + (lk->negated ? " NOT LIKE '" : " LIKE '") + lk->pattern + "'";
    }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        std::string s = "CASE";
        for (const auto& w : c->when_clauses) {
            s += " WHEN " + exprToString(w.condition.get())
               + " THEN " + exprToString(w.result.get());
        }
        if (c->else_expr) s += " ELSE " + exprToString(c->else_expr.get());
        return s + " END";
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        std::string s = "SUBSTRING(" + exprToString(sub->operand.get())
                      + ", " + exprToString(sub->start.get());
        if (sub->length) s += ", " + exprToString(sub->length.get());
        return s + ")";
    }
    if (auto* iv = dynamic_cast<const IntervalLiteral*>(expr)) {
        const char* unit = iv->unit == IntervalLiteral::Unit::DAY   ? "DAY"
                         : iv->unit == IntervalLiteral::Unit::MONTH ? "MONTH" : "YEAR";
        return "INTERVAL '" + std::to_string(iv->count) + "' " + unit;
    }
    return "?";
}

// Type-tagged identity for a constant. Value::toString() renders the DOUBLE 1.0
// as "1" (%.15g), so an untagged key made `GROUP BY season - 1` match
// `SELECT season - 1.0` and emit the INT column where SQL says REAL. Shared by
// the Literal and InExpr cases of exprKey so the two cannot drift.
inline std::string literalKey(const Value& v) {
    if (v.isNull()) return "NULL";
    switch (v.type()) {
        case TypeId::INT:    return "i" + v.toString();
        case TypeId::DOUBLE: return "d" + v.toString();
        case TypeId::STRING: return "s'" + v.toString() + "'";
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
        return literalKey(lit->value);
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
    if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        // `negated` and the value TYPES are part of the identity: x IN (1) and
        // x NOT IN (1) are different expressions, and so are IN (1) and IN (1.0)
        std::string s = exprKey(in->operand.get()) + (in->negated ? " NOT IN (" : " IN (");
        for (size_t i = 0; i < in->values.size(); ++i) {
            if (i) s += ",";
            s += literalKey(in->values[i]);
        }
        return s + ")";
    }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        return exprKey(lk->operand.get())
            + (lk->negated ? " NOT LIKE " : " LIKE ") + literalKey(Value(lk->pattern));
    }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        std::string s = "CASE";
        for (const auto& w : c->when_clauses) {
            s += " WHEN " + exprKey(w.condition.get()) + " THEN " + exprKey(w.result.get());
        }
        if (c->else_expr) s += " ELSE " + exprKey(c->else_expr.get());
        return s + " END";
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        std::string s = "SUBSTRING(" + exprKey(sub->operand.get())
                      + "," + exprKey(sub->start.get());
        if (sub->length) s += "," + exprKey(sub->length.get());
        return s + ")";
    }
    if (auto* iv = dynamic_cast<const IntervalLiteral*>(expr)) {
        return "INTERVAL " + std::to_string(iv->count) + " "
             + std::to_string(static_cast<int>(iv->unit));
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
        return;
    }
    if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        collectAggregates(in->operand.get(), out);   // values are literals
        return;
    }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        collectAggregates(lk->operand.get(), out);
        return;
    }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        // every branch: CASE WHEN c THEN SUM(x) ELSE 0 END is legal, and a
        // missed aggregate here is never computed as a spec
        for (const auto& w : c->when_clauses) {
            collectAggregates(w.condition.get(), out);
            collectAggregates(w.result.get(), out);
        }
        collectAggregates(c->else_expr.get(), out);
        return;
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        collectAggregates(sub->operand.get(), out);
        collectAggregates(sub->start.get(), out);
        collectAggregates(sub->length.get(), out);   // nullptr-safe
        return;
    }
    // IntervalLiteral: a constant, nothing to collect
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
    } else if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        auto n = std::make_unique<InExpr>();
        n->operand = cloneExpr(in->operand.get());
        n->values = in->values;
        n->negated = in->negated;
        out = std::move(n);
    } else if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        auto n = std::make_unique<LikeExpr>();
        n->operand = cloneExpr(lk->operand.get());
        n->pattern = lk->pattern;
        n->negated = lk->negated;
        out = std::move(n);
    } else if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        auto n = std::make_unique<CaseExpr>();
        for (const auto& w : c->when_clauses) {
            CaseExpr::WhenClause clause;
            clause.condition = cloneExpr(w.condition.get());
            clause.result = cloneExpr(w.result.get());
            n->when_clauses.push_back(std::move(clause));
        }
        n->else_expr = cloneExpr(c->else_expr.get());
        out = std::move(n);
    } else if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        auto n = std::make_unique<SubstringExpr>();
        n->operand = cloneExpr(sub->operand.get());
        n->start = cloneExpr(sub->start.get());
        n->length = cloneExpr(sub->length.get());   // nullptr-safe
        out = std::move(n);
    } else if (auto* iv = dynamic_cast<const IntervalLiteral*>(expr)) {
        out = std::make_unique<IntervalLiteral>(*iv);   // memberwise: count + unit
    } else {
        throw std::runtime_error("cloneExpr(): unknown Expr subtype");
    }
    out->alias = expr->alias;
    return out;
}
