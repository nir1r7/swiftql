#include "constant_folding.h"
#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include <stdexcept>

namespace {

// A constant subtree reads neither the row nor the schema, so an empty one is
// all evaluate() needs. Schema has no default constructor.
const Schema& kEmptySchema() {
    static const Schema empty{std::vector<ColumnDef>{}};
    return empty;
}

bool isArithOp(const std::string& op) {
    return op == "+" || op == "-" || op == "*" || op == "/";
}

// Replace `expr` with a Literal when it is a constant arithmetic subtree.
// Returns true when `expr` is (now) a Literal, so the parent can decide whether
// it too can fold.
bool foldNode(std::unique_ptr<Expr>& expr) {
    if (!expr) return false;

    if (dynamic_cast<Literal*>(expr.get())) return true;

    if (auto* bin = dynamic_cast<BinaryExpr*>(expr.get())) {
        bool l = foldNode(bin->left);
        bool r = foldNode(bin->right);
        if (!isArithOp(bin->op) || !l || !r) return false;
        Value v;
        try {
            // both children are Literals, so neither the Row nor the Schema is
            // touched; this is the same evaluator the per-row path uses, so the
            // folded value is by construction the value it would have produced
            v = evaluate(expr.get(), Row{}, kEmptySchema());
        } catch (const std::runtime_error&) {
            // ill-typed ('a' + 1) or overflowing (INT64_MAX + 1) constant: leave
            // the tree alone so the existing error surfaces from its usual place
            // with its usual message
            return false;
        }
        if (v.isNull()) return false;   // no NULL literal in the grammar
        std::string alias = expr->alias;
        auto lit = std::make_unique<Literal>(v);
        lit->alias = alias;             // an aliased constant keeps its name
        expr = std::move(lit);
        return true;
    }

    if (auto* un = dynamic_cast<UnaryExpr*>(expr.get())) {
        if (!foldNode(un->operand)) return false;
        Value v;
        try {
            v = evaluate(expr.get(), Row{}, kEmptySchema());
        } catch (const std::runtime_error&) {
            return false;
        }
        if (v.isNull()) return false;
        std::string alias = expr->alias;
        auto lit = std::make_unique<Literal>(v);
        lit->alias = alias;
        expr = std::move(lit);
        return true;
    }

    if (auto* isn = dynamic_cast<IsNullExpr*>(expr.get())) {
        // fold inside the operand, but never the IS NULL itself: its result
        // depends on the operand's nullability, not just its value
        foldNode(isn->operand);
        return false;
    }

    if (auto* agg = dynamic_cast<AggregateExpr*>(expr.get())) {
        if (!agg->is_star) foldNode(agg->argument);
        return false;   // an aggregate is never a compile-time constant
    }

    return false;   // ColumnRef, or a subtype with no folding rule
}

} // namespace


void foldConstantsInExpr(std::unique_ptr<Expr>& expr) {
    foldNode(expr);
}

void foldConstants(SelectStatement& stmt) {
    for (auto& e : stmt.select_list) foldNode(e);
    foldNode(stmt.where);
    foldNode(stmt.having);
    for (auto& item : stmt.order_by) foldNode(item.expr);
    for (auto& g : stmt.group_by) {
        if (!g.expr) continue;
        // GroupByColumn::expr is a shared_ptr so the struct stays copyable, and
        // foldNode owns what it rewrites. Fold a clone and swap it in — a clone
        // per group key at plan time costs nothing, and it avoids handing the
        // same Expr to two owning smart pointers.
        std::unique_ptr<Expr> folded = cloneExpr(g.expr.get());
        foldNode(folded);
        g.expr = std::shared_ptr<Expr>(std::move(folded));
    }
    if (stmt.join.has_value()) foldNode(stmt.join->condition);
}
