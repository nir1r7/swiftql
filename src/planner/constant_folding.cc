#include "constant_folding.h"
#include "common/date_util.h"
#include "execution/checked_arith.h"
#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include <stdexcept>
#include <utility>

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

// `date '1998-12-01' - interval '90' day` -> Literal("1998-09-02").
//
// This is the one fold that is not plain arithmetic: the operands are a STRING
// date Literal and an IntervalLiteral, a pair inferExprType rejects outright.
// Folding it is the whole reason IntervalLiteral exists — it turns the
// predicate back into `ColumnRef op Literal`, which is the shape ChunkPruner,
// scanColumn's tight loop and selectivity() all pattern-match on. Every TPC-H
// interval expression is constant, so nothing is lost. Returns true when `expr`
// was replaced by a Literal.
bool foldDateInterval(std::unique_ptr<Expr>& expr, BinaryExpr* bin) {
    if (bin->op != "+" && bin->op != "-") return false;

    // SQL allows `interval + date` as well; normalise so one shape is handled.
    // Subtraction is not commutative, so only '+' may be swapped.
    Expr* date_side = bin->left.get();
    Expr* iv_side   = bin->right.get();
    if (bin->op == "+" && dynamic_cast<IntervalLiteral*>(date_side)) {
        std::swap(date_side, iv_side);
    }

    auto* date_lit = dynamic_cast<Literal*>(date_side);
    auto* iv       = dynamic_cast<IntervalLiteral*>(iv_side);
    if (!date_lit || !iv) return false;
    if (date_lit->value.isNull() || date_lit->value.type() != TypeId::STRING) return false;
    if (!isIsoDate(date_lit->value.asString())) return false;

    // checkedNegate, not `-count`: negating INT64_MIN is undefined behaviour,
    // and `interval '-9223372036854775808' day` parses fine (stoll accepts it),
    // so this is reachable from ordinary SQL. Same helper the arithmetic
    // evaluator uses — date folding had been bypassing checked_arith.h
    // entirely. The add* functions bound their own count before touching it,
    // so this is the only unguarded operation left on the path.
    const int64_t n = (bin->op == "-") ? checkedNegate(iv->count) : iv->count;
    std::string folded;
    switch (iv->unit) {
        case IntervalLiteral::Unit::DAY:   folded = addDays(date_lit->value.asString(), n);   break;
        case IntervalLiteral::Unit::MONTH: folded = addMonths(date_lit->value.asString(), n); break;
        case IntervalLiteral::Unit::YEAR:  folded = addYears(date_lit->value.asString(), n);  break;
    }

    std::string alias = expr->alias;
    auto lit = std::make_unique<Literal>(Value(folded));
    lit->alias = alias;
    expr = std::move(lit);
    return true;
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
        // Runs AFTER the children fold (so a nested date expression is already a
        // Literal) and BEFORE the arithmetic rule (whose `l && r` guard is false
        // here: foldNode returns false for an IntervalLiteral, which is not a
        // Literal). Chains like `date + interval + interval` fold left to right.
        if (foldDateInterval(expr, bin)) return true;
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

    // Week 25 nodes: fold inside them (SUBSTRING(x, 1 + 0, 2)) but never fold
    // the node itself. A constant LIKE/IN/CASE/SUBSTRING is not worth a rule,
    // and declining is always safe — see the "decline and fall back" pattern.
    if (auto* in = dynamic_cast<InExpr*>(expr.get())) {
        foldNode(in->operand);
        return false;   // values are already constants
    }
    if (auto* lk = dynamic_cast<LikeExpr*>(expr.get())) {
        foldNode(lk->operand);
        return false;
    }
    if (auto* c = dynamic_cast<CaseExpr*>(expr.get())) {
        for (auto& w : c->when_clauses) {
            foldNode(w.condition);
            foldNode(w.result);
        }
        foldNode(c->else_expr);
        return false;
    }
    if (auto* sub = dynamic_cast<SubstringExpr*>(expr.get())) {
        foldNode(sub->operand);
        foldNode(sub->start);
        foldNode(sub->length);   // nullptr-safe
        return false;
    }

    return false;   // ColumnRef, IntervalLiteral, or a subtype with no folding rule
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
    for (auto& j : stmt.joins) foldNode(j.condition);
}
