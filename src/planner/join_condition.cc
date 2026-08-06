#include "join_condition.h"
#include "predicate_pushdown.h"   // collectSlots — dispatch site 8, shared

#include <stdexcept>
#include <unordered_set>

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

JoinCondition classifyJoinCondition(const Expr* condition, int right_slot) {
    std::vector<const Expr*> conjuncts;
    flattenAnd(condition, conjuncts);

    JoinCondition out;
    for (const Expr* c : conjuncts) {
        // (1) Forward reference: a ref naming a relation joined LATER. Those
        // columns are absent from this join's output schema, so the conjunct is
        // neither a key nor a legal residual. Checking every ref in the conjunct
        // (not just an equality's two operands) is what keeps this true now that
        // a residual can be any expression shape — `ON a.x = b.x AND c.val > 1`
        // has no equality to inspect.
        //
        // collectSlots is dispatch site 8, shared with predicate pushdown: a
        // private copy here would be an eleventh silent site, where a missed
        // subtype makes a forward reference invisible rather than loud.
        std::unordered_set<int> slots;
        collectSlots(c, slots);
        for (int s : slots) {
            if (s <= right_slot) continue;
            // keys[k].from_col is defined to resolve against children[0] — the
            // left tree — so accepting this silently rewires the conjunct to
            // whatever column of that name the left tree happens to have, and
            // explain() renders a plan indistinguishable from the correct one.
            throw std::runtime_error(
                "JOIN ON: a condition may only reference the table being joined "
                "and tables already joined; slot " + std::to_string(s)
                + " is joined later");
        }

        // (2) An equi-join key: '=' between two ColumnRefs from different
        // relations, one of them the relation being joined. Operand order is
        // normalized here — whichever side carries `right_slot` becomes
        // join_col, the other from_col.
        auto* bin = dynamic_cast<const BinaryExpr*>(c);
        const ColumnRef* lc = bin ? dynamic_cast<const ColumnRef*>(bin->left.get())  : nullptr;
        const ColumnRef* rc = bin ? dynamic_cast<const ColumnRef*>(bin->right.get()) : nullptr;
        if (bin && bin->op == "=" && lc && rc) {
            if (lc->relation_slot < 0 || rc->relation_slot < 0) {
                // unbound: positional routing, as documented in the header
                out.keys.push_back({lc->column_name, rc->column_name, lc->relation_slot});
                continue;
            }
            if (rc->relation_slot == right_slot && lc->relation_slot != right_slot) {
                out.keys.push_back({lc->column_name, rc->column_name, lc->relation_slot});
                continue;
            }
            if (lc->relation_slot == right_slot && rc->relation_slot != right_slot) {
                out.keys.push_back({rc->column_name, lc->column_name, rc->relation_slot});
                continue;
            }
            // Falls through when both operands are the same relation (a local
            // filter, e.g. ON a.id = a.grp) or when neither is the relation being
            // joined (a condition between two already-joined relations). Both are
            // ordinary predicates over this join's output — residuals, not keys.
        }

        // (3) everything else — non-equality operators, OR, literal or computed
        // operands, same-relation equalities, Week 25 nodes — is a residual
        // predicate. Legal because this is an inner join; see the header.
        out.residuals.push_back(c);
    }

    // Identical keys are a legal predicate (k AND k ≡ k) but not a legal key
    // list: the probe tuple gains a redundant field, and CardinalityEstimator
    // divides by the same NDV twice, underestimating the join by a factor of
    // NDV. Deduped here, at the single source, rather than at each of the three
    // consumers. O(n²) over a list that is never longer than a handful, and it
    // preserves written order, which explain() prints.
    std::vector<JoinKey> deduped;
    for (const JoinKey& k : out.keys) {
        bool seen = false;
        for (const JoinKey& d : deduped) {
            if (d.from_col == k.from_col && d.join_col == k.join_col
                && d.from_slot == k.from_slot) { seen = true; break; }
        }
        if (!seen) deduped.push_back(k);
    }
    out.keys = std::move(deduped);

    // SwiftQL has no cross-product join operator, so a JOIN whose ON yields no
    // key is a cartesian product with a filter on top — refuse rather than
    // materialize one. `ON a.x = b.x OR a.y = b.y` is the shape that lands here:
    // an OR is one indivisible conjunct, so it contributes no key.
    if (out.keys.empty()) {
        throw std::runtime_error(
            "JOIN ON: at least one equality between the joined table and an "
            "already-joined table is required");
    }
    return out;
}
