#include "join_condition.h"

#include <stdexcept>

namespace {

// Flatten an AND-chain without taking ownership — the ON tree still belongs to
// the statement. Same recursion shape as splitConjuncts() in
// predicate_pushdown.cc, which flattens the WHERE clause.
void flattenAnd(const Expr* pred, std::vector<const Expr*>& out) {
    auto* bin = dynamic_cast<const BinaryExpr*>(pred);
    if (bin && bin->op == "AND") {
        flattenAnd(bin->left.get(), out);
        flattenAnd(bin->right.get(), out);
        return;
    }
    out.push_back(pred);
}

} // namespace

std::vector<JoinKey> classifyJoinCondition(const Expr* condition, int right_slot) {
    std::vector<const Expr*> conjuncts;
    flattenAnd(condition, conjuncts);

    std::vector<JoinKey> keys;
    for (const Expr* c : conjuncts) {
        auto* bin = dynamic_cast<const BinaryExpr*>(c);
        if (!bin || bin->op == "OR") {
            throw std::runtime_error(
                "JOIN ON: condition must be an equality, or an AND-chain of equalities, "
                "between one column from each joined table");
        }
        if (bin->op != "=") {
            // Week 27 routes these as residual post-join filters; until then a
            // clean refusal beats executing '<' as '='.
            throw std::runtime_error(
                "JOIN ON: non-equality join conditions are not supported (got '" + bin->op + "')");
        }
        auto* lc = dynamic_cast<const ColumnRef*>(bin->left.get());
        auto* rc = dynamic_cast<const ColumnRef*>(bin->right.get());
        if (!lc || !rc) {
            throw std::runtime_error(
                "JOIN ON: both sides of the join equality must be column references");
        }

        if (lc->relation_slot < 0 || rc->relation_slot < 0) {
            // unbound: positional routing, as documented in the header
            keys.push_back({lc->column_name, rc->column_name, lc->relation_slot});
            continue;
        }
        if (lc->relation_slot == rc->relation_slot) {
            throw std::runtime_error(
                "JOIN ON: condition must compare a column from each joined table; "
                "both sides reference '" + lc->table_name + "'");
        }
        // The rule has two halves, and checking only the first accepts the same
        // forward reference the second rejects, purely on operand order: one
        // side must be the relation being joined, AND the other must already be
        // in the left tree.
        const ColumnRef* joined_ref;   // the side in relation `right_slot`
        const ColumnRef* left_ref;     // the side that must already be joined
        if (rc->relation_slot == right_slot) {
            joined_ref = rc;
            left_ref = lc;
        } else if (lc->relation_slot == right_slot) {
            joined_ref = lc;
            left_ref = rc;
        } else {
            // Neither side is the relation being joined in: with two relations
            // this was unreachable, with three it is a forward reference
            // (ON a.x = c.x before c is joined) or a stale join.
            throw std::runtime_error(
                "JOIN ON: each condition must reference the table being joined; '"
                + lc->table_name + "." + lc->column_name + " = "
                + rc->table_name + "." + rc->column_name + "' does not");
        }
        if (left_ref->relation_slot > right_slot) {
            // The other operand names a relation joined LATER. keys[k].from_col
            // is defined to resolve against children[0] — the left tree — so
            // accepting this silently rewires the key to whatever column of that
            // name the left tree happens to have, and explain() renders a plan
            // indistinguishable from the correct one.
            throw std::runtime_error(
                "JOIN ON: '" + left_ref->table_name + "." + left_ref->column_name
                + "' references a table that is joined later; a condition may only "
                  "reference the table being joined and tables already joined");
        }
        keys.push_back({left_ref->column_name, joined_ref->column_name, left_ref->relation_slot});
    }
    return keys;
}
