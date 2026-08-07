#include "planner/subquery_decorrelation.h"
#include "planner/subquery_materialization.h"   // forEachSubqueryConst
#include "planner/predicate_pushdown.h"       // collectSlots (dispatch site 8)
#include "parser/expr_utils.h"
#include <unordered_set>
#include <stdexcept>
#include <utility>

namespace {

[[noreturn]] void refuse(const std::string& why) {
    throw std::runtime_error("correlated subquery: " + why);
}

// Condition 3 of the header. A body whose row SET depends on which outer row
// selected it cannot be evaluated once and probed: GROUP BY/HAVING recompute per
// correlation value, LIMIT picks a different prefix, and an aggregate collapses
// a different set. DISTINCT is harmless for a semi-join (membership is
// idempotent) but is refused too, because it would have to be dropped rather
// than preserved and a silently-dropped clause is the shape of a wrong answer.
void requireDecorrelatableBody(const SelectStatement& body) {
    if (!body.group_by.empty()) refuse("a body with GROUP BY cannot be decorrelated");
    if (body.having)            refuse("a body with HAVING cannot be decorrelated");
    if (body.limit)             refuse("a body with LIMIT cannot be decorrelated");
    if (body.distinct)          refuse("a body with DISTINCT cannot be decorrelated");
    for (const auto& item : body.select_list) {
        std::vector<const AggregateExpr*> found;
        collectAggregates(item.get(), found);
        if (!found.empty())
            refuse("a body with an aggregate cannot be decorrelated");
    }
}

// Splits the body's WHERE into join keys (the correlated equalities) and the
// conjuncts that stay inside the body. Refuses any correlated conjunct that is
// not a key, rather than leaving it in the body where its level-1 ref would be
// meaningless — that is the silent wrong answer this whole week is about.
void splitCorrelation(std::vector<std::unique_ptr<Expr>>& body_conjuncts,
                      std::vector<JoinKey>& keys,
                      std::vector<std::unique_ptr<Expr>>& local) {
    for (auto& c : body_conjuncts) {
        // Does this conjunct reach outside the body? collectSlots (dispatch
        // site 8) is the maintained walker for exactly this question and maps a
        // correlated ref to its "cannot name it here" sentinel, -1. A private
        // walker here would be a nineteenth silent dispatch site, which is what
        // Week 30 refused to add for the ORDER BY position rule.
        //
        // -1 also means UNRESOLVED, so a conjunct carrying an unbound ref lands
        // in the refusing branch below. That is the safe direction: it is loud,
        // not silently mis-classified as body-local.
        std::unordered_set<int> slots;
        collectSlots(c.get(), slots);
        if (slots.find(-1) == slots.end()) { local.push_back(std::move(c)); continue; }

        auto* bin = dynamic_cast<BinaryExpr*>(c.get());
        if (!bin || bin->op != "=")
            refuse("only an equality between two columns can become a join key "
                   "(a correlated inequality has no equi-join to lower to)");

        auto* l = dynamic_cast<ColumnRef*>(bin->left.get());
        auto* r = dynamic_cast<ColumnRef*>(bin->right.get());
        if (!l || !r)
            refuse("both sides of a correlated equality must be plain column "
                   "references (JoinKey holds column names, not expressions)");

        const bool l_outer = !l->id.isLocal();
        const bool r_outer = !r->id.isLocal();
        if (l_outer == r_outer)
            refuse("a correlated equality must compare one column of the "
                   "subquery with one of the enclosing query");

        const ColumnRef* body_side  = l_outer ? r : l;
        const ColumnRef* outer_side = l_outer ? l : r;
        if (outer_side->id.level() != 1)
            refuse("a reference to a query block more than one level out cannot "
                   "be decorrelated here");

        // from_slot names the OUTER range table, which is the domain
        // leftKeyIndices() resolves against. One step outward makes the level-1
        // reference level 0 THERE, and localSlot() narrows it at a named point.
        keys.push_back(JoinKey{outer_side->column_name,
                               body_side->column_name,
                               outer_side->id.outward().localSlot("splitCorrelation")});
    }
    body_conjuncts.clear();
}

} // namespace

ExistsLoweringResult lowerExistsSubqueries(std::unique_ptr<LogicalPlanNode> spine,
                                           std::vector<std::unique_ptr<Expr>>& conjuncts,
                                           const Catalog& catalog) {
    ExistsLoweringResult out;
    std::vector<std::unique_ptr<Expr>> kept;

    for (auto& conjunct : conjuncts) {
        auto* sq = dynamic_cast<SubqueryExpr*>(conjunct.get());
        // An UNCORRELATED EXISTS stays materialized: its value does not depend
        // on the outer row at all, and a semi-join would turn a foldable
        // constant into a pipeline breaker (subquery_lowering.h says the same
        // for IN). It never reaches here — materializeSubqueries replaced it
        // with a Literal before planning — so this is a routing statement, not a
        // guard.
        if (!sq || sq->kind != SubqueryExpr::Kind::EXISTS || !sq->correlated) {
            kept.push_back(std::move(conjunct));
            continue;
        }

        // Same ownership shape planBody() uses (subquery_lowering.cc):
        // SelectStatement is move-only, and cloneExpr SHARES the shared_ptr, so
        // two SubqueryExpr nodes can name one statement — only one of them can
        // be lowered from it, and the second would plan an emptied statement.
        if (sq->subquery.use_count() > 1)
            refuse("a subquery body shared by two expressions is not supported "
                   "by decorrelation");

        SelectStatement body = std::move(*sq->subquery);
        requireDecorrelatableBody(body);

        std::vector<std::unique_ptr<Expr>> body_conjuncts;
        splitConjuncts(std::move(body.where), body_conjuncts);

        std::vector<JoinKey> keys;
        std::vector<std::unique_ptr<Expr>> local;
        splitCorrelation(body_conjuncts, keys, local);
        if (keys.empty())
            refuse("no equality links the subquery to the enclosing query, so "
                   "there is no join key to decorrelate on");

        // The body is planned AFTER the correlated conjuncts are removed. Plan
        // it before, and a level-1 ref reaches validateExpr, collectSlots,
        // buildScanSchema and the pruning hint inside a block where it means
        // nothing.
        body.where = conjoinAll(std::move(local));
        auto body_plan = LogicalPlanBuilder::build(std::move(body), catalog);

        // !! output_schema is the LEFT child's, NOT a merged schema — the
        // invariant that keeps the body's slot numbering out of the outer plan
        // (Week 32). join_slot is -1: children[1] is not a relation of this
        // block's range table.
        Schema left_schema = spine->output_schema;
        auto join = std::make_unique<LogicalJoin>(
            std::move(spine), std::move(body_plan), std::move(keys),
            /*join_slot=*/-1, left_schema);
        // ANTI, never ANTI_NOT_IN. This is where the header's central claim is
        // ENFORCED rather than asserted: NOT EXISTS is two-valued, so Week 32's
        // unmatchable-key machinery must not reach it. Until Week 33 round 2
        // both lowerings named the same enumerator and the operator had no way
        // to tell them apart, so one NULL in the body's key column emptied the
        // whole result and a NULL correlated key dropped a row SQL keeps.
        join->semantics = sq->negated ? JoinSemantics::ANTI : JoinSemantics::SEMI;

        // Assert the containment rather than leaving it as an observation, the
        // same single check subquery_lowering.cc makes in place of an audit round.
        const auto& jc = join->output_schema.columns();
        const auto& lc = join->children[0]->output_schema.columns();
        bool same = jc.size() == lc.size();
        for (size_t i = 0; same && i < jc.size(); ++i) {
            same = jc[i].name == lc[i].name && jc[i].type == lc[i].type
                && jc[i].relation_slot == lc[i].relation_slot;
        }
        if (!same)
            throw std::runtime_error(
                "internal: semi/anti join output schema must be its left child's");

        spine = std::move(join);
        ++out.lowered;
    }

    conjuncts = std::move(kept);
    out.plan = std::move(spine);
    return out;
}

void refuseUnloweredCorrelated(const Expr* expr, const char* clause) {
    if (!expr) return;
    bool found = false;
    forEachSubqueryConst(expr, [&](const SubqueryExpr& sq) {
        if (sq.correlated) found = true;
    });
    if (!found) return;
    throw std::runtime_error(
        std::string("correlated subquery: supported only as a whole top-level "
                    "WHERE conjunct (found one in ") + clause + ")");
}
