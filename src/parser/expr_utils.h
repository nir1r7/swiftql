#pragma once
#include "ast.h"
#include <cstdint>
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
    // Week 30 — DISPATCH SITE 10. Abbreviated on purpose: this string reaches
    // --explain's predicate text, and no output column is ever named after a
    // subquery because Validator refuses one in a select list. Rendering the
    // whole nested query here would make a filter line unreadable for no gain.
    if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        const char* notq = sq->negated ? "NOT " : "";
        switch (sq->kind) {
            case SubqueryExpr::Kind::SCALAR: return "(SELECT ...)";
            case SubqueryExpr::Kind::EXISTS: return std::string(notq) + "EXISTS (SELECT ...)";
            case SubqueryExpr::Kind::IN:
                return exprToString(sq->operand.get()) + " " + notq + "IN (SELECT ...)";
        }
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
        if (col->id.isResolved()) {
            // Week 30 round 2: the LEVEL is part of the identity. A slot is a
            // position in the range table of the scope query_level blocks out,
            // so `0#driver_id` meant two different columns and two refs
            // differing only in level hashed alike. checkGroupedRefs matches
            // expression group keys through this function BEFORE its
            // query_level guard, so `EXISTS (SELECT driver_id + 1 FROM drivers d
            // GROUP BY l.driver_id + 1)` accepted an ungrouped LOCAL column
            // against a CORRELATED group key — and substituteInto rewrites on
            // the same key, so it would also have replaced the local subtree
            // with a ref to the correlated group column.
            //
            // Prefixed only above level 0, so every pre-existing key — and
            // therefore every aggregate-spec dedupe and group-key match in a
            // query with no subquery — is byte-identical. This text is for
            // MATCHING, never display, so widening it costs nothing visible.
            std::string key = std::to_string(col->id.slotInOwnScope("exprKey"))
                            + "#" + col->column_name;
            // The ':' is load-bearing, not decoration. Concatenating two decimal
            // numbers is not prefix-free: level 1 / slot 23 and level 12 / slot 3
            // both render "^123#team" — the identical collision this prefix
            // exists to remove, one order of magnitude further out. Both halves
            // are legal SwiftQL (a 24-relation block plans; nesting is unbounded).
            return !col->id.isLocal()
                ? "^" + std::to_string(col->id.level()) + ":" + key
                : key;
        }
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
    // Week 30 — DISPATCH SITE 1. Falling through to "?" here would give two
    // different subqueries the SAME identity, and substituteInto() rewrites any
    // subtree whose exprKey matches a GROUP BY key — so a subquery in HAVING
    // could be replaced by a ColumnRef to a group column.
    //
    // The statement's address is the identity. It is stable across cloneExpr
    // precisely BECAUSE the statement is shared rather than deep-copied
    // (ast.h), so a clone keys the same as its original, and two distinct
    // subqueries can never collide. exprKey is for MATCHING, never for display,
    // which is what makes an address acceptable here and nowhere else.
    if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        std::string s = "SUBQUERY";
        s += sq->negated ? "!" : "";
        s += std::to_string(static_cast<int>(sq->kind));
        if (sq->operand) s += "(" + exprKey(sq->operand.get()) + ")";
        s += "@" + std::to_string(reinterpret_cast<uintptr_t>(sq->subquery.get()));
        return s;
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
    // Week 30 — DISPATCH SITE 7, and the sharpest of the silent ones. Descend
    // into the IN operand, which is written in THIS query, and NEVER into the
    // subquery's body: `HAVING SUM(x) > (SELECT AVG(y) FROM z)` would otherwise
    // collect the inner AVG(y) as an outer AggregateSpec, computing it over the
    // outer relation and emitting a column for it.
    if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        collectAggregates(sq->operand.get(), out);   // nullptr-safe
        return;
    }
    // IntervalLiteral: a constant, nothing to collect
}

// Flatten an AND-chain into its atomic conjuncts, MOVING ownership out of the
// tree. OR / comparison / IS NULL are indivisible — each becomes one leaf.
// Mirrors the recursion shape of collectCols() in logical_plan.cc and of the
// borrowing flattenAnd() in join_condition.cc.
//
// Week 32 hoisted this out of predicate_pushdown.cc's anonymous namespace so the
// set-membership lowering (subquery_lowering.h) shares it rather than growing a
// third flattener. Which subtrees count as one conjunct is a semantic decision —
// it is what makes `x IN (SELECT ...) OR y > 5` a single indivisible conjunct,
// and therefore not extractable as a semi-join — so the two passes must agree by
// construction, not by coincidence.
inline void splitConjuncts(std::unique_ptr<Expr> pred, std::vector<std::unique_ptr<Expr>>& out) {
    auto* bin = dynamic_cast<BinaryExpr*>(pred.get());
    if (bin && bin->op == "AND") {
        // move both operands out before the AND node dies at end of scope
        splitConjuncts(std::move(bin->left), out);
        splitConjuncts(std::move(bin->right), out);
        return;
    }
    out.push_back(std::move(pred));
}

// Rebuild a left-deep AND-chain from conjuncts, or nullptr if empty. The
// inverse of splitConjuncts()/flattenAnd(). Shared by predicate pushdown (which
// re-conjoins what it did not push) and by both planners (which fold residual ON
// conjuncts into the WHERE conjunction, Week 27). Takes ownership; the parts are
// MOVED, never cloned, so raw Expr* captured before the call — the scan pruning
// hint in Planner::plan, for one — stay valid.
inline std::unique_ptr<Expr> conjoinAll(std::vector<std::unique_ptr<Expr>> parts) {
    if (parts.empty()) return nullptr;
    std::unique_ptr<Expr> acc = std::move(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
        auto conj = std::make_unique<BinaryExpr>();
        conj->op = "AND";
        conj->left = std::move(acc);
        conj->right = std::move(parts[i]);
        acc = std::move(conj);
    }
    return acc;
}

// Deep copy of an expression tree, preserving binder stamps and alias.
// DISPATCH SITE: every new Expr subtype must be added here.
inline std::unique_ptr<Expr> cloneExpr(const Expr* expr) {
    if (!expr) return nullptr;
    std::unique_ptr<Expr> out;
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        out = std::make_unique<ColumnRef>(*col);   // memberwise: keeps ColumnId
    } else if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        auto l = std::make_unique<Literal>(lit->value);
        // Week 31: null_type is the type of a NULL constant, which only a
        // materialized scalar subquery produces. It is part of the node's
        // meaning, not a cache — dropping it here would retype a cloned NULL as
        // INT and make inferExprType disagree with itself across a clone.
        l->null_type = lit->null_type;
        out = std::move(l);
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
    } else if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        // Week 30. The nested statement is SHARED, not deep-copied:
        // SelectStatement is move-only, so a deep copy would need a
        // clone-a-statement walker whose omissions (a dropped HAVING) are
        // silent. Sharing is safe because binding is idempotent — see
        // Binder::resolveColumnRef — and it is what makes exprKey's identity
        // stable across a clone.
        auto n = std::make_unique<SubqueryExpr>();
        n->kind = sq->kind;
        n->negated = sq->negated;
        n->correlated = sq->correlated;
        n->operand = cloneExpr(sq->operand.get());   // nullptr-safe
        n->subquery = sq->subquery;
        out = std::move(n);
    } else {
        throw std::runtime_error("cloneExpr(): unknown Expr subtype");
    }
    out->alias = expr->alias;
    return out;
}
