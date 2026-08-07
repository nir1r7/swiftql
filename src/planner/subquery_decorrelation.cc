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
                      std::vector<std::unique_ptr<Expr>>& body_key_refs,
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

        // The BODY side's full identity, kept rather than reduced to its name.
        // JoinKey has no field for it (join_condition.h's struct predates this
        // second producer), and a name alone is not an identity -- that is the
        // root of H-1/H-2: the right key was resolved by BARE NAME against a
        // schema the name was never resolved in. Carrying the ref itself lets
        // the body be projected to exactly its key columns below, after which
        // the right key indices are positional and no name lookup happens.
        body_key_refs.push_back(cloneExpr(body_side));
    }
    body_conjuncts.clear();
}

// Condition 2, enforced over the WHOLE body rather than only its WHERE.
//
// splitCorrelation reads `body.where` and nothing else, so a correlated ref
// anywhere ELSE in the body survives into a plan. The one that matters is the
// body's `JOIN ... ON`: classifyJoinCondition routes a non-local ref to
// out.residuals (Week 30, working as designed), LogicalPlanBuilder::build folds
// inner-join residuals into the body's stmt.where AFTER splitCorrelation has run
// and cleared it, and from there the ref reaches inferExprType and
// resolveColumnIndex — both of which branch on isLocal() and fall back to a BARE
// NAME lookup against the body's own merged schema. `d2.team = d.team` became
// `d2.team = laps.team`: wrong rows, no error, an identical --explain. That is
// the exact collapse ColumnId exists to prevent, surviving because the fallback
// resolves instead of throwing.
//
// A refusal by name beats a plausible wrong answer, so the shape is declined
// here rather than half-supported. Extracting ON-clause correlations as join
// keys is a real feature (it needs the residual/key split to happen before the
// fold, not after) and it is not this week's.
//
// collectSlots is the same maintained walker splitCorrelation uses, and -1 is
// the same sentinel; an UNRESOLVED ref also maps to -1 and is named in the
// message rather than mis-diagnosed as correlation (round 1, L-8).
void refuseSurvivingCorrelatedRefs(const SelectStatement& body) {
    auto check = [](const Expr* e, const char* where) {
        if (!e) return;
        std::unordered_set<int> slots;
        collectSlots(e, slots);
        if (slots.find(-1) != slots.end())
            refuse(std::string("a reference this body cannot name locally survives "
                               "in its ") + where + " (a correlated reference is "
                   "lowered only from a top-level equality in the body's WHERE; "
                   "an unresolved one would report the same)");
    };
    for (const auto& j : body.joins)       check(j.condition.get(), "JOIN ... ON clause");
    for (const auto& e : body.select_list) check(e.get(), "SELECT list");
    for (const auto& o : body.order_by)    check(o.expr.get(), "ORDER BY");
    // splitCorrelation guarantees this one is empty of correlated refs. Checked
    // anyway, because a guarantee that is never tested is the shape this round
    // has now found twice.
    check(body.where.get(), "WHERE");
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
        std::vector<std::unique_ptr<Expr>> body_key_refs;
        std::vector<std::unique_ptr<Expr>> local;
        splitCorrelation(body_conjuncts, keys, body_key_refs, local);
        if (keys.empty())
            refuse("no equality links the subquery to the enclosing query, so "
                   "there is no join key to decorrelate on");

        // The body is planned AFTER the correlated conjuncts are removed. Plan
        // it before, and a level-1 ref reaches validateExpr, collectSlots,
        // buildScanSchema and the pruning hint inside a block where it means
        // nothing.
        body.where = conjoinAll(std::move(local));
        refuseSurvivingCorrelatedRefs(body);

        // PROJECT THE BODY TO ITS KEY COLUMNS, in key order. A semi/anti join
        // emits no body column at all, so the body's own SELECT list is dead
        // weight -- and resolving the join key against it BY NAME was three
        // separate defects (round 1 H-1, H-2, M-3):
        //
        //   H-1  a body that is a JOIN has a MERGED output schema, where
        //        invariant 3 makes duplicate names legal; indexOf(name) took the
        //        first match, so the probe ran against the wrong relation's
        //        column. Wrong rows, no error, an identical --explain.
        //   H-2  buildProjectSchema names columns by SELECT ALIAS, so
        //        `SELECT l.speed AS driver_id` rebound the key `driver_id` to
        //        `speed`. Verified: 0 rows where SQLite returns 20.
        //   M-3  the correlated conjunct is removed from body.where BEFORE the
        //        body is planned, so buildScanSchema never sees the key column
        //        and narrows it away. `EXISTS (SELECT 1 FROM ...)` -- the most
        //        idiomatic EXISTS body in SQL -- died with "join key not found".
        //
        // Replacing the list fixes all three at once and makes the right key
        // indices POSITIONAL (0..k-1), the shape Week 32's IN lowering already
        // had by taking body column 0. Nothing is silently dropped: the
        // discarded expressions could not have been read by anything, because
        // the operator never emits a body row. The clauses for which a select
        // list WOULD change the row set -- DISTINCT, GROUP BY, HAVING, an
        // aggregate, LIMIT -- are every one of them refused by
        // requireDecorrelatableBody, which is what makes this rewrite sound
        // rather than merely convenient.
        body.select_star = false;
        body.select_list = std::move(body_key_refs);
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
