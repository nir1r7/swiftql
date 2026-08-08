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
        // A CORRELATED IN is NOT this pass's business, and the ordering here is
        // load-bearing: refuseUnloweredCorrelated runs on what is LEFT in
        // `conjuncts` (logical_plan.cc), so a node this loop consumes is a node
        // the tripwire never sees. Consuming one lowered it to a semi-join whose
        // ONLY key was the IN operand -- the body's correlated equality was
        // simply planned inside the body, where the outer ref fell back to
        // bare-name resolution against the BODY's own schema and
        // `l.team = d.team` became the tautology `laps.team = laps.team`. The
        // predicate was silently discarded and the semi-join degenerated to
        // "does the body have any row at all": 20 rows where SQLite says 6, and
        // 0 where it says 14.
        //
        // This is the third instance of one shape -- code trusting the Validator
        // refusal Week 33 deleted -- so the routing decision is made HERE, off
        // the Binder's own flag, rather than inherited from a precondition
        // stated in another file. See the header, whose precondition bullet was
        // corrected in the same commit.
        //
        // A correlated IN IS lowerable in principle (a second join key, from the
        // body's correlated equality, exactly as lowerExistsSubqueries builds
        // one) but that is a feature and not this week's; it is refused by name
        // downstream instead of being half-supported.
        if (!sq || sq->kind != SubqueryExpr::Kind::IN || sq->correlated) {
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

        // SEAM AUDIT PASS 5, P5-1. The semi/anti join goes BELOW the WHERE
        // filter, and it removes rows — so every conjunct written before this
        // one would be evaluated on its survivors instead of on the spine's
        // rows. `guardLoweredConjunctPrefix` puts them where the written order
        // says they are evaluated, and does nothing at all unless one of them
        // can raise. See logical_plan.h for the rule and the measurement.
        spine = guardLoweredConjunctPrefix(std::move(spine), kept);

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
        // ANTI_NOT_IN, not ANTI: `NOT IN` is three-valued and an anti-join is
        // not. See JoinSemantics in logical_plan.h — the whole reason the
        // enumerator is separate is that decorrelated NOT EXISTS must NOT get
        // this rule, and a comment saying so did not stop it.
        join->semantics = sq->negated ? JoinSemantics::ANTI_NOT_IN : JoinSemantics::SEMI;

        // THE CONTAINMENT: this join's output schema is its LEFT child's, never a
        // merged one -- the invariant that keeps the body's slot numbering out of
        // the outer plan.
        //
        // A loop comparing join->output_schema against
        // join->children[0]->output_schema STOOD HERE and was DELETED, because it
        // could not fail: left_schema is copied from spine->output_schema on the
        // line above and passed as the join's output_schema, children[0] IS that
        // spine, and the unique_ptr move does not touch its schema. It compared a
        // copy of one object with the object. It was introduced as the single
        // check "that replaces an audit round", which is the worst thing a dead
        // assertion can be: it reads as a guarantee and stops anyone looking.
        //
        // Where the property is ACTUALLY enforced, on objects that are genuinely
        // different and can genuinely diverge:
        //   - VectorizedPlanBuilder compares each LOWERED input's schema size
        //     against the logical child's before building the operator;
        //   - VecHashJoinNode's constructor throws unless output_schema_ has the
        //     same size as the LOWERED probe child's schema -- a schema derived
        //     through the vectorized lowering, not a copy of this one;
        //   - rightKeyIndices(positional) throws unless the build input's schema
        //     is exactly the key tuple.
        // Those three run on every semi/anti join the CLI builds.
        //
        // !! WHY THE CHECK CANNOT SIMPLY BE PUT BACK, and what it would take
        // (seam audit pass 4 P4-M1, carried and re-sized in pass 5). The
        // paragraph above is right that the deleted loop compared a copy with
        // its own original. It is the SECOND half that matters now: the copy
        // WOULD genuinely diverge from the object if JoinEnumeration ever
        // reordered the spine below this node, because `rebuild` preserves the
        // merged schema's (relation_slot, name) pair SET but PERMUTES its
        // sequence. That is exactly why the enumerator declines the whole tree
        // at a semi/anti join today — and the decline is measured, not
        // hypothetical:
        //
        //   FROM laps l JOIN drivers d ON … JOIN drivers d2 ON d.team = d2.team
        //     order=drivers@1,drivers@2,laps@0 cost=43104 (written=60637) method=dp
        //   the same spine + WHERE l.driver_id IN (SELECT …)
        //     LogicalSemiJoin … join-ordering=skipped (semi/anti join)
        //       LogicalJoin [team@1 = team]   <- fully inner, 3 relations, NOT enumerated
        //
        //   1.58x on the spine's join time and 1.23x on total execution by
        //   --explain-analyze; 1.41x by the model. Answers identical (32193).
        //
        // THE FIX IS NOT "RE-DERIVE THIS NODE'S SCHEMA AFTER ENUMERATING
        // children[0]", and pass 5 established the extra reason by construction
        // rather than by argument. This node is not always the top of its plan:
        // lowerCorrelatedScalars attaches a LogicalLeftJoin ABOVE it, and that
        // join's merged schema is a POSITIONAL CONCATENATION of this node's
        // columns with the derived relation's (logical_plan.cc's merged_cols
        // loop). Producible today:
        //
        //   … WHERE l.driver_id IN (SELECT …)
        //     AND l.speed > (SELECT AVG(l2.speed) FROM laps l2
        //                    WHERE l2.driver_id = l.driver_id)
        //
        //     LogicalLeftJoin [driver_id@0 = $k0]      <- merged, positional
        //       LogicalSemiJoin [driver_id@0 = driver_id]
        //         LogicalJoin [team@1 = team]          <- what would be permuted
        //
        // and LogicalFilter / Sort / Distinct / Limit each copy their child's
        // schema at construction too. So permuting the spine invalidates every
        // ancestor's stored schema up to the first PROJECT / AGGREGATE /
        // DERIVED, and a correct fix is a plan-wide bottom-up schema
        // re-derivation that reuses `rebuild`'s merge rather than opening a
        // second copy of it — plus inverting test_join_enumeration.cc's
        // assertion that ONLY the declining node carries an order_decision.
        // That is a week's task, not a fix-round's, and it lands in
        // join_enumeration.cc rather than here. Recorded, not attempted; the
        // decline stays, because declining is always legal and the loss is plan
        // quality only.

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
