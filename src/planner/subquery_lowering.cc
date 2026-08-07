#include "planner/subquery_lowering.h"
#include "planner/subquery_materialization.h"   // forEachSubqueryConst
#include "parser/expr_utils.h"
#include <stdexcept>
#include <utility>

namespace {

// The body is a self-contained query block, so it is planned by the same
// builder. Its refs are level 0 against ITS OWN range table, which is exactly
// why nothing here needs a query level (docs/week-32-plan.md §0).
//
// The statement is MOVED out of the shared_ptr, the same ownership shape
// runOnce() uses (subquery_materialization.cc): SelectStatement holds
// unique_ptr<Expr> members and is not copyable. cloneExpr shares the shared_ptr,
// so two SubqueryExpr nodes CAN name one statement — but only one of them can be
// lowered from it, and the second would then plan an emptied statement. That is
// why the shared_ptr's use_count is checked rather than assumed.
std::unique_ptr<LogicalPlanNode> planBody(SubqueryExpr* sq, const Catalog& catalog) {
    if (sq->subquery.use_count() > 1) {
        throw std::runtime_error(
            "IN subquery: a subquery body shared by two expressions is not "
            "supported by set-membership lowering");
    }
    return LogicalPlanBuilder::build(std::move(*sq->subquery), catalog);
}

} // namespace

InLoweringResult lowerInSubqueries(std::unique_ptr<LogicalPlanNode> spine,
                                   std::vector<std::unique_ptr<Expr>>& conjuncts,
                                   const Catalog& catalog) {
    InLoweringResult out;
    std::vector<std::unique_ptr<Expr>> kept;

    for (auto& conjunct : conjuncts) {
        auto* sq = dynamic_cast<SubqueryExpr*>(conjunct.get());
        if (!sq || sq->kind != SubqueryExpr::Kind::IN) {
            kept.push_back(std::move(conjunct));
            continue;
        }

        // JoinKey holds column NAMES, not expressions: there is no computed-key
        // join in this engine. Refuse rather than fall back to materialization,
        // which would re-open the two-paths problem (see the header).
        const auto* operand = dynamic_cast<const ColumnRef*>(sq->operand.get());
        if (!operand) {
            throw std::runtime_error(
                "IN subquery: the left operand must be a column reference "
                "(computed operands are not supported)");
        }

        auto body_plan = planBody(sq, catalog);

        // One equi-join key. from_col/from_slot come from the OPERAND, which
        // belongs to the ENCLOSING query and is already bound at level 0 there —
        // so from_slot is a slot in the OUTER range table, exactly the domain
        // leftKeyIndices() resolves against. join_col is the body's single
        // output column (Validator guarantees the arity), resolved against
        // body_plan's OWN schema. The two slot domains never meet.
        JoinKey key{operand->column_name,
                    body_plan->output_schema.columns()[0].name,
                    operand->id.localSlot("lowerInSubqueries")};

        // !! output_schema is the LEFT child's, NOT a merged schema. This is the
        // invariant that keeps the body's slot numbering out of the outer plan
        // (docs/week-32-plan.md §0). join_slot is -1: children[1] is not a
        // relation of this block's range table, so there is no slot to name it
        // by, and every reader of join_slot declines on semantics != STANDARD.
        Schema left_schema = spine->output_schema;
        auto join = std::make_unique<LogicalJoin>(
            std::move(spine), std::move(body_plan), std::vector<JoinKey>{key},
            /*join_slot=*/-1, left_schema);
        join->semantics = sq->negated ? JoinSemantics::ANTI : JoinSemantics::SEMI;

        // Assert the containment rather than leaving it as an observation: this
        // single check is what replaces an audit round.
        const auto& jc = join->output_schema.columns();
        const auto& lc = join->children[0]->output_schema.columns();
        bool same = jc.size() == lc.size();
        for (size_t i = 0; same && i < jc.size(); ++i) {
            same = jc[i].name == lc[i].name && jc[i].type == lc[i].type
                && jc[i].relation_slot == lc[i].relation_slot;
        }
        if (!same) {
            throw std::runtime_error(
                "internal: semi/anti join output schema must be its left child's");
        }

        spine = std::move(join);
        ++out.lowered;
    }

    conjuncts = std::move(kept);
    out.plan = std::move(spine);
    return out;
}

void refuseUnloweredIn(const Expr* expr, const char* clause) {
    if (!expr) return;
    bool found = false;
    forEachSubqueryConst(expr, [&](const SubqueryExpr& sq) {
        if (sq.kind == SubqueryExpr::Kind::IN) found = true;
    });
    if (!found) return;
    throw std::runtime_error(
        std::string("IN subquery: supported only as a whole top-level WHERE "
                    "conjunct (found one in ") + clause + ")");
}
