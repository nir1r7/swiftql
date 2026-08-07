#include "planner/subquery_decorrelation.h"
#include "planner/subquery_materialization.h"   // forEachSubquery{,Const}
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
            // Both branches of l_outer == r_outer land here, and they are not the
            // same fault. BOTH-OUTER is a genuine correlation this cannot key on.
            // BOTH-LOCAL means the conjunct reached here only because collectSlots
            // returned -1 for an UNRESOLVED ref -- an unresolved id reports
            // isLocal() == true -- so there is no correlation in it at all. The
            // message used to name only the first, diagnosing a query with no
            // correlated reference as a correlation error (round 1, L-8).
            refuse(l_outer
                   ? "a correlated equality must compare one column of the "
                     "subquery with one of the enclosing query (both sides name "
                     "an enclosing query)"
                   : "a column reference in this body could not be resolved to "
                     "any relation, so the conjunct cannot be classified as "
                     "local or correlated");

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
// IS THE REFUSAL NARROWER THAN IT NEEDS TO BE? Yes, and by a known amount, so
// record the boundary rather than leave the next reader to rediscover it. For an
// INNER join ON and WHERE are interchangeable -- R (join)_(p and q) S is
// sigma_q(R (join)_p S), which is the identity join_condition.h already relies on
// to route residuals -- so a correlated conjunct in an INNER join's ON could be
// treated exactly as one in the body's WHERE and become a key. For a LEFT OUTER
// join they are NOT interchangeable (a residual there decides null-extension,
// not row survival), so that half must stay refused whatever else changes. The
// refusal is uniform today because splitting it means moving the extraction
// ahead of the residual fold, which is the feature above.
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

// Week 34 — the SCALAR guard. Deliberately NOT requireDecorrelatableBody with a
// flag: that function's stated condition is "no GROUP BY / HAVING / aggregate /
// LIMIT / DISTINCT", and this rewrite REQUIRES an aggregate and ADDS a GROUP BY.
// Widening it would leave one header stating a rule it no longer enforces for
// half its callers, which is the shape that produced three silent wrong answers
// in Week 33.
void requireDecorrelatableScalarBody(const SelectStatement& body) {
    if (body.limit)    refuse("a scalar body with LIMIT cannot be decorrelated");
    if (body.distinct) refuse("a scalar body with DISTINCT cannot be decorrelated");
    if (body.having)   refuse("a scalar body with HAVING cannot be decorrelated");
    if (!body.group_by.empty())
        refuse("a scalar body with its own GROUP BY cannot be decorrelated "
               "(the rewrite supplies the grouping)");
    if (body.select_star || body.select_list.size() != 1)
        refuse("a correlated scalar subquery must select exactly one expression");

    // THE LOAD-BEARING ONE, and the reason it is not merely a scope reduction.
    // Without an aggregate, GROUP BY does not guarantee one row per key, and
    // Week 31's runtime `scalar subquery returned more than one row` check has
    // nowhere to live after the rewrite -- a query SQL calls an error would
    // return an arbitrary row instead. Refusing keeps that divergence honest.
    std::vector<const AggregateExpr*> found;
    collectAggregates(body.select_list[0].get(), found);
    if (found.size() != 1 || found[0] != body.select_list[0].get())
        refuse("a correlated scalar subquery is decorrelated only when its select "
               "list is a single aggregate (a non-aggregate body has no "
               "one-row-per-key guarantee, so 'returned more than one row' could "
               "not be checked)");
}

} // namespace

ScalarLoweringResult lowerCorrelatedScalars(std::unique_ptr<LogicalPlanNode> spine,
                                            std::vector<std::unique_ptr<Expr>>& conjuncts,
                                            int range_table_size,
                                            const Catalog& catalog) {
    ScalarLoweringResult out;

    for (auto& conjunct : conjuncts) {
        // UNLIKE EXISTS AND IN, the node is not the conjunct: Q17 writes
        // `l.speed > 0.2 * (SELECT ...)`. forEachSubquery (dispatch site 19) is
        // the maintained walker that reaches every SubqueryExpr through its
        // owning slot, which is what lets the node be REPLACED in place.
        forEachSubquery(conjunct, [&](std::unique_ptr<Expr>& slot) {
            auto* sq = static_cast<SubqueryExpr*>(slot.get());
            if (sq->kind != SubqueryExpr::Kind::SCALAR || !sq->correlated) return;

            // Same ownership shape planBody() and lowerExistsSubqueries use:
            // cloneExpr SHARES the shared_ptr, so two SubqueryExpr nodes can name
            // one statement and only one of them can be lowered from it -- the
            // second would plan an emptied statement. `(SELECT ...) BETWEEN a AND
            // b` is legal syntax and produces exactly that shape.
            if (sq->subquery.use_count() > 1)
                refuse("a subquery body shared by two expressions is not supported "
                       "by decorrelation");

            SelectStatement body = std::move(*sq->subquery);
            requireDecorrelatableScalarBody(body);

            std::vector<std::unique_ptr<Expr>> body_conjuncts;
            splitConjuncts(std::move(body.where), body_conjuncts);

            // REUSED VERBATIM from Week 33: it already produces
            // JoinKey{outer_col, body_col, outer_slot} plus the body-side refs,
            // and it already refuses a correlated inequality, a computed side and
            // a reference more than one level out.
            std::vector<JoinKey> keys;
            std::vector<std::unique_ptr<Expr>> body_key_refs;
            std::vector<std::unique_ptr<Expr>> local;
            splitCorrelation(body_conjuncts, keys, body_key_refs, local);
            if (keys.empty())
                refuse("no equality links the scalar subquery to the enclosing "
                       "query, so there is no group key to decorrelate on");

            body.where = conjoinAll(std::move(local));
            refuseSurvivingCorrelatedRefs(body);

            // GROUP BY the correlation keys, SELECT them and then the aggregate.
            // The group keys must come FIRST and in key order: buildAggregateSchema
            // emits group columns then aggregates, which makes the right-side key
            // indices positional 0..k-1 -- the same shape Week 33's body projection
            // produced and Week 32's IN lowering had by taking body column 0.
            auto agg_expr = std::move(body.select_list[0]);
            const std::string agg_name = aggregateOutputName(
                static_cast<const AggregateExpr*>(agg_expr.get()));
            body.select_list.clear();
            for (auto& ref : body_key_refs) {
                auto* cr = static_cast<ColumnRef*>(ref.get());
                body.group_by.push_back(
                    GroupByColumn{cr->table_name, cr->column_name, cr->id, nullptr});
                body.select_list.push_back(cloneExpr(ref.get()));
            }
            body.select_list.push_back(std::move(agg_expr));

            auto body_plan = LogicalPlanBuilder::build(std::move(body), catalog);

            // THE SAME NODE FROM (subquery) produces, and the same normalization:
            // every column stamped slot 0, the outer slot applied by the merged
            // schema below. Not a special case -- if it were, the four
            // descend-to-SCAN walkers, the range-table size and every
            // development.md row would need a second argument and the two would
            // drift.
            const std::string alias = "$scalar" + std::to_string(out.lowered);
            TableRef synthetic = TableRef::named("", alias);
            Schema normalized =
                derivedRelationSchema(body_plan->output_schema, synthetic);
            auto derived = std::make_unique<LogicalDerived>(
                std::move(body_plan), alias, normalized);

            // MERGED schema: the aggregate's column IS in scope above this join,
            // because the outer predicate reads it. That is the containment
            // Week 32 established and this week replaced with the slot-0
            // normalization plus a real range-table slot.
            const int derived_slot = range_table_size + out.lowered;
            std::vector<ColumnDef> merged = spine->output_schema.columns();
            for (ColumnDef col : normalized.columns()) {
                col.relation_slot = derived_slot;
                merged.push_back(col);
            }

            auto join = std::make_unique<LogicalJoin>(
                std::move(spine), std::move(derived), std::move(keys),
                derived_slot, Schema(merged));
            // LEFT, not INNER, and this is the whole zero-row rule: an outer row
            // whose key matches no group must survive NULL-EXTENDED, which is
            // what SQL says a scalar subquery over zero rows evaluates to. An
            // inner join would silently DELETE that row. Week 29's three
            // consequences follow and are correct here: pushdown declines the
            // null-supplying side, the build side is forced, and enumeration
            // declines the tree and REPORTS it.
            join->join_type = JoinType::LEFT;
            spine = std::move(join);

            // Replace the node in place with a reference to the aggregate's
            // output column, stamped with the DERIVED RELATION'S SLOT. A bare-name
            // ColumnRef would resolve -- against whichever relation holds that
            // name first, which is the silent wrong-relation class this week's
            // normalization exists to prevent.
            auto ref = std::make_unique<ColumnRef>();
            ref->column_name = agg_name;
            ref->id = ColumnId::local(derived_slot);
            slot = std::move(ref);
            ++out.lowered;
        });
    }

    out.plan = std::move(spine);
    return out;
}

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

        spine = std::move(join);
        ++out.lowered;
    }

    conjuncts = std::move(kept);
    out.plan = std::move(spine);
    return out;
}

void refuseUnloweredCorrelated(const Expr* expr, const char* clause) {
    if (!expr) return;
    // The KIND is kept, not just the fact, because the two reasons a correlated
    // node survives to here are different and a message that cannot tell them
    // apart names the wrong cause. A correlated EXISTS reaches this only from a
    // position lowering does not read (under an OR, in HAVING); a correlated IN
    // reaches it from a perfectly ordinary top-level conjunct, because
    // lowerInSubqueries deliberately declines it.
    const SubqueryExpr* found = nullptr;
    forEachSubqueryConst(expr, [&](const SubqueryExpr& sq) {
        if (sq.correlated && !found) found = &sq;
    });
    if (!found) return;
    if (found->kind == SubqueryExpr::Kind::IN) {
        // No clause suffix: POSITION is not the reason. A correlated IN is
        // declined wherever it appears, including as the whole WHERE, so naming
        // a position here would be the same wrong-cause diagnostic the call
        // order above exists to avoid.
        (void)clause;
        throw std::runtime_error(
            "correlated subquery: a correlated IN / NOT IN is not lowered — "
            "decorrelation covers EXISTS / NOT EXISTS only, and an IN needs a "
            "SECOND join key built from the body's correlated equality");
    }
    throw std::runtime_error(
        std::string("correlated subquery: supported only as a whole top-level "
                    "WHERE conjunct (found one in ") + clause + ")");
}
