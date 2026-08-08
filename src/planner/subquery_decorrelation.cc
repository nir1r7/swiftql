#include "planner/subquery_decorrelation.h"
#include "planner/subquery_materialization.h"   // forEachSubquery{,Const}
#include "planner/predicate_pushdown.h"       // collectSlots (dispatch site 8)
#include "parser/expr_utils.h"
#include "parser/expr_totality.h"   // firstMayRaise — refuseUnguardedRaiser
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

// SEAM AUDIT pass 2 — B-3. "Does this expression reach outside THE BODY?"
//
// collectSlots (dispatch site 8) is the maintained walker for the neighbouring
// question, and this asks IT rather than growing a nineteenth private walker
// (Week 30 refused to add one for the ORDER BY position rule). But it is not
// the same question, and the difference is exactly one of its branches.
// collectSlots has THREE producers of its "cannot name it here" sentinel -1:
//
//   1. a ColumnRef resolved to an ENCLOSING block   -> yes, reaches outside;
//   2. an UNRESOLVED ColumnRef                      -> cannot be classified at
//                                                      all, so refuse loudly —
//                                                      the safe direction, and
//                                                      the message below names
//                                                      it (round 1, L-8);
//   3. a nested CORRELATED SubqueryExpr             -> NOT AN ANSWER TO THIS
//                                                      QUESTION.
//
// The enumeration in this comment used to say TWO, and (3) is the one it
// missed. (3) is right for PUSHDOWN, which is what collectSlots is for: a
// conjunct holding a correlated subquery owns no single relation slot, so
// withholding it is conservative and safe. As a CORRELATION test it is simply
// the wrong reading. `sq->correlated` means "some ref inside sq's own body
// resolved to an enclosing scope" — and for a one-level reference the scope it
// names is THIS BODY, the one being split. Body-local: exactly the
// classification it must get, and the only route by which a nested correlated
// subquery can reach the body's OWN lowering pass.
//
// What the misreading cost: a conjunct that merely CONTAINED a nested
// correlated subquery was routed to the refusing branch, failed the
// `BinaryExpr && op == "="` test, and was reported as
//
//     correlated subquery: only an equality between two columns can become a
//     join key (a correlated inequality has no equi-join to lower to; ...)
//
// for queries with no inequality anywhere in them. Same wrong-cause class as
// round 1's L-8, in this same function, which had already been corrected for it
// once. The identical nesting under an UNCORRELATED IN ran and was right — the
// only difference was whether the ENCLOSING subquery happened to be correlated.
//
// HOW THE THIRD PRODUCER IS SUPPRESSED, and why this is not a second walker:
// forEachSubquery (dispatch site 19) is the maintained walk to every nested
// SubqueryExpr; the flag is cleared across the single collectSlots call and put
// back immediately, so what runs is site 8 itself answering a narrower
// question. Every node cleared is recorded and restored, by a DESTRUCTOR rather
// than by a trailing loop: `correlated` is live data — the body's own lowering
// passes route on it, and forEachSubquery descends into nested BODIES, which are
// held by a shared_ptr and may be reachable from another expression — so a throw
// that skipped the restore would leave a shared tree silently mis-flagged.
// collectSlots cannot in fact throw today (its one throwing call, localSlot, is
// guarded by isLocal()), which is exactly why the restore must not depend on
// that staying true.
//
// !! THIS IS HALF OF A COUPLED PAIR. See the depth refusal below.
class SuppressNestedCorrelation {
public:
    explicit SuppressNestedCorrelation(std::unique_ptr<Expr>& e) {
        forEachSubquery(e, [this](std::unique_ptr<Expr>& slot) {
            auto* sq = static_cast<SubqueryExpr*>(slot.get());
            if (!sq->correlated) return;
            sq->correlated = false;
            cleared_.push_back(sq);
        });
    }
    ~SuppressNestedCorrelation() { for (auto* sq : cleared_) sq->correlated = true; }
    SuppressNestedCorrelation(const SuppressNestedCorrelation&) = delete;
    SuppressNestedCorrelation& operator=(const SuppressNestedCorrelation&) = delete;
private:
    std::vector<SubqueryExpr*> cleared_;
};

bool reachesOutsideThisBody(std::unique_ptr<Expr>& e) {
    if (!e) return false;
    SuppressNestedCorrelation guard(e);
    std::unordered_set<int> slots;
    collectSlots(e.get(), slots);
    return slots.find(-1) != slots.end();
}

// Splits the body's WHERE into join keys (the correlated equalities) and the
// conjuncts that stay inside the body. Refuses any correlated conjunct that is
// not a key, rather than leaving it in the body where its level-1 ref would be
// meaningless — that is the silent wrong answer this whole week is about.
//
// SEAM AUDIT PASS 5, subquery B-3 — `unguarded` is the fifth parameter and it
// exists because LIFTING A CONJUNCT OUT OF A LIST IS A REWRITE OF THE CASCADE.
// `expr_totality.h` says a conjunct is evaluated on the rows for which every
// conjunct WRITTEN BEFORE IT evaluated TRUE. A correlated equality that becomes
// a join key stops being a conjunct of the body at all: it is enforced by the
// PROBE, above the body, so every body-local conjunct written AFTER it is now
// evaluated on the body's WHOLE relation. That is more rows than the written
// order gives it, which is the "introduce a raise" direction — the mirror of
// P5-1's "mask a raise", and unfixable in place because the lifted equality
// cannot stay in the body (its level-1 ref means nothing there).
//
// So the conjuncts that LOST a guard are handed back, in written order and as
// clones, for `refuseUnguardedRaiser` to screen once the body's schema exists.
// Clones rather than pointers: `local` is conjoined into `body.where` and then
// MOVED into the plan, where a nested lowering may consume individual conjuncts.
//
// WEEK 36 — `residuals` IS THE SIXTH PARAMETER AND IT IS A POINTER, not a
// reference, because the two callers differ in whether a residual EXISTS as a
// destination:
//
//   lowerExistsSubqueries  passes a vector. A correlated conjunct that is not an
//                          equality becomes an ON RESIDUAL on the semi/anti
//                          join, evaluated inside the probe against a
//                          probe(+)build pair. TPC-H q21 is exactly this shape:
//                          `l2.l_suppkey != l1.l_suppkey` beside the good key
//                          `l2.l_orderkey = l1.l_orderkey`.
//   lowerCorrelatedScalars passes nullptr and KEEPS the old refusal. Its join is
//                          a STANDARD LEFT join over a GROUPED derived table:
//                          the correlation keys ARE the GROUP BY, so a residual
//                          would have to be applied to a row that is already an
//                          aggregate over the very rows it would have selected.
//                          A different rewrite, not a flag on this one.
//
// !! THE REFUSAL NARROWS; IT DOES NOT DISAPPEAR. A body correlated ONLY by
// inequalities produces no key at all, and the `keys.empty()` check at each call
// site still refuses it by name — there is no hash key to build, and the
// fallback is a cross product this engine has no operator for. What changed is
// only that an inequality BESIDE a surviving key is no longer read as "no
// equi-join to lower to".
//
// !! A ROUTED RESIDUAL BREAKS THE CASCADE EXACTLY AS A LIFTED KEY DOES, and sets
// the same flag. The residual stops being a conjunct of the body: it is enforced
// by the PROBE, above the body, so every body-local conjunct written AFTER it is
// evaluated on the body's whole relation. Missing that would leave `unguarded`
// under-filled for precisely the queries this parameter admits.
void splitCorrelation(std::vector<std::unique_ptr<Expr>>& body_conjuncts,
                      std::vector<JoinKey>& keys,
                      std::vector<std::unique_ptr<Expr>>& body_key_refs,
                      std::vector<std::unique_ptr<Expr>>& local,
                      std::vector<std::unique_ptr<Expr>>& unguarded,
                      std::vector<std::unique_ptr<Expr>>* residuals = nullptr) {
    bool lifted_a_key = false;
    for (auto& c : body_conjuncts) {
        // Does this conjunct reach outside the body? See reachesOutsideThisBody
        // above for why that is not the same as "collectSlots yields -1".
        if (!reachesOutsideThisBody(c)) {
            // Body-local, and written AFTER a key this loop already lifted: its
            // guard is gone. Recorded for refuseUnguardedRaiser.
            //
            // NOT CLONED IF IT HOLDS A SUBQUERY, and this is not a shortcut —
            // cloneExpr SHARES a SubqueryExpr's shared_ptr (the ownership shape
            // planBody and lowerExistsSubqueries both turn on), so cloning such
            // a conjunct raises `use_count` and the body's own lowering pass
            // then refuses it as "a subquery body shared by two expressions".
            // Measured: 12 ordinary nested-EXISTS oracle queries errored and 4
            // refusal pins reported the wrong cause. Excluding them also avoids
            // an over-refusal, since exprMayRaise answers TRUE for every
            // SubqueryExpr — screening one would refuse every body holding a
            // nested subquery after its correlated equality. The residue is a
            // recorded gap, not a claim: a nested subquery whose OWN body can
            // raise, written after a lifted key, is still unscreened here.
            if (lifted_a_key) {
                bool holds_subquery = false;
                forEachSubqueryConst(c.get(), [&](const SubqueryExpr&) { holds_subquery = true; });
                if (!holds_subquery) unguarded.push_back(cloneExpr(c.get()));
            }
            local.push_back(std::move(c));
            continue;
        }

        auto* bin = dynamic_cast<BinaryExpr*>(c.get());
        if (!bin || bin->op != "=") {
            // Week 36 — THIS IS WHERE THE REFUSAL BECAME A ROUTE. It used to
            // read "a correlated inequality has no equi-join to lower to", which
            // named a dead end that was never one: TPC-H q21 writes
            // `l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey != l1.l_suppkey` —
            // a perfectly good key WITH an inequality beside it. The inequality
            // now rides as an ON RESIDUAL on the semi/anti join, evaluated inside
            // the probe against a probe(+)build pair (the caller appends its
            // body-side columns to the body projection and restamps its refs).
            //
            // A residual holding a SubqueryExpr is still refused, and not for
            // tidiness: evaluate() throws unconditionally on one, so a nested
            // body inside a per-pair predicate is an error at run time rather
            // than a plan-time decision. It cannot arrive from an UNCORRELATED
            // subquery (materializeSubqueries folded those to Literals before
            // planning) and a CORRELATED one is classified body-local by
            // reachesOutsideThisBody, so this guards a route that is closed
            // twice over — which is exactly the kind that opens quietly.
            if (!residuals)
                refuse("only an equality between two columns can become a join "
                       "key (a correlated inequality can ride as an ON residual "
                       "on a semi/anti join, but a correlated SCALAR subquery is "
                       "lowered to a grouped derived table, whose rows are "
                       "already aggregated over the rows a residual would have "
                       "selected)");
            bool holds_subquery = false;
            forEachSubqueryConst(c.get(), [&](const SubqueryExpr&) { holds_subquery = true; });
            if (holds_subquery)
                refuse("a correlated conjunct holding a subquery cannot become an "
                       "ON residual (it is evaluated per candidate pair, and "
                       "evaluate() has no case for a subquery node)");
            residuals->push_back(std::move(c));
            // See the header note: a routed residual takes a guard away from
            // every body-local conjunct written after it, exactly as a lifted
            // key does.
            lifted_a_key = true;
            continue;
        }

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
        // !! THE OTHER HALF OF B-3's COUPLED PAIR, and it changed state with
        // that fix. This guard was UNREACHABLE (seam audit pass 2, B-5.3): every
        // route to a level-2 reference was closed by something earlier, and the
        // nearest one was B-3's own misclassification — a nested correlated
        // subquery was refused above before its body could ever be split.
        //
        // Fixing B-3 OPENS this route deliberately, and this guard is what makes
        // it safe. The route is now:
        //
        //   EXISTS (SELECT 1 FROM laps l WHERE l.driver_id = d.driver_id
        //             AND EXISTS (SELECT 1 FROM laps l2
        //                         WHERE l2.driver_id = d.driver_id))
        //
        // The inner conjunct is classified body-local, the middle body is handed
        // to LogicalPlanBuilder::build, and the body's OWN lowering pass splits
        // the inner body — where `d.driver_id` still carries LEVEL 2. Levels are
        // NOT decremented when the middle body is decorrelated (the rewrite
        // moves a join, not a scope), so a stale level would otherwise reach
        // leftKeyIndices and be read as a slot of the wrong range table. This
        // test is the whole containment, and it now fires: the query above is
        // refused by NAME, with the level as the stated cause.
        //
        // It is `!= 1` and not `> 1` on purpose: level 0 is local and cannot
        // arrive here (the l_outer test above already parted the sides), so a
        // level of anything but 1 is a case this rewrite does not model, and
        // failing closed is the right direction for both.
        //
        // Pinned by message in the rejection suite, because the diffed oracle
        // cannot hold a query that errors — a guard that fires but is asserted
        // nowhere is how the previous three came to be dead without anyone
        // noticing.
        if (outer_side->id.level() != 1)
            refuse("a reference to a query block more than one level out cannot "
                   "be decorrelated here");

        // from_slot names the OUTER range table, which is the domain
        // leftKeyIndices() resolves against. One step outward makes the level-1
        // reference level 0 THERE, and localSlot() narrows it at a named point.
        keys.push_back(JoinKey{outer_side->column_name,
                               body_side->column_name,
                               outer_side->id.outward().localSlot("splitCorrelation")});
        lifted_a_key = true;

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

// The schema the body's WHERE conjuncts were written against: the output of the
// body's FROM/JOIN spine, which is the first SCAN / JOIN / DERIVED on the way
// down from the body's root. LogicalPlanBuilder puts the WHERE filter directly
// above that node, so this is the schema `evaluate()` will resolve them in —
// taken from the plan rather than re-derived, so there is no second derivation
// to drift (the same reason blockOutputSchema exists rather than a private copy
// in the Binder).
const Schema* bodySpineSchema(const LogicalPlanNode* n) {
    while (n) {
        if (n->type == LogicalNodeType::SCAN
            || n->type == LogicalNodeType::JOIN
            || n->type == LogicalNodeType::DERIVED) return &n->output_schema;
        if (n->children.empty()) return nullptr;
        n = n->children[0].get();
    }
    return nullptr;
}

// SEAM AUDIT PASS 5, subquery B-3. The other half of splitCorrelation's
// `unguarded` — see the comment there for what was lost and why.
//
// The discriminator is SwiftQL against itself, on the shipped catalog. `drivers`
// holds seven rows with age < 30, for which `d.age - 29 <= 0`:
//
//   ... AND EXISTS (SELECT 1 FROM drivers d
//                   WHERE d.driver_id = l.driver_id                 -- guard
//                     AND SUBSTRING(d.name, d.age - 29, 1) = 'D')   -- raiser
//     -> Error: SUBSTRING: start position must be >= 1
//   the same body with `d.driver_id = 1` in the guard's place       -> answers 0
//
// Same relation, same raiser, same position; the only difference is whether the
// guard is the conjunct decorrelation takes away. The body-local form honours
// the cascade, the correlated form does not.
//
// REFUSE rather than repair, and the choice is forced. P5-1's semi-join case can
// be repaired by MOVING the earlier conjuncts down to where they belong, because
// they are still conjuncts of a filter in the same block. Here the guard is a
// correlated equality: inside the body its level-1 reference names nothing, so
// there is no position in the body that reproduces the row set it gave. Keeping
// it would need the residue to ride as an ON residual on the semi/anti join —
// real operator work this engine's set-probe build side cannot do today, named
// in the correlated-inequality refusal a few lines up and in docs/week-36-plan.md
// Task 3. Refusing by name beats answering where the definition says raise.
//
// !! WEEK 36 CHANGED WHAT FILLS `unguarded`, NOT WHAT THIS DOES WITH IT. An ON
// residual is now a second way a conjunct leaves the body's cascade, and
// splitCorrelation sets the same flag for it, so a body-local raiser written
// after `l2.l_suppkey != l1.l_suppkey` is screened here exactly as one written
// after a lifted key is. The paragraph above still describes the OTHER half —
// the residue that a residual now carries and that this refusal no longer has to
// decline for lack of an operator.
//
// conjunctMayRaise via firstMayRaise, not exprMayRaise: a body conjunct is
// truth-tested by the filter that holds it, so a non-INT static type raises on
// its own.
void refuseUnguardedRaiser(const std::vector<std::unique_ptr<Expr>>& unguarded,
                           const LogicalPlanNode& body_plan) {
    if (unguarded.empty()) return;
    const Schema* schema = bodySpineSchema(&body_plan);
    if (!schema) return;   // no spine to evaluate anything per row against
    if (firstMayRaise(unguarded, *schema) < unguarded.size())
        refuse("a conjunct that can raise is written after the correlated "
               "equality this rewrite lifts into a join key, so decorrelating "
               "would evaluate it on rows the written order excluded");
}

// WEEK 36 — BIND A ROUTED RESIDUAL TO THE SCHEMA IT WILL BE EVALUATED IN, and
// the single most dangerous function in this change.
//
// q21's residual is `l3.l_suppkey != l1.l_suppkey`: ONE COLUMN NAME, TWO
// RELATIONS, one on each side of the join. Left to resolve by name in the
// concatenated probe(+)build schema, BOTH sides find the probe's `l_suppkey`
// (indexOf takes the first match and the probe half is first), the residual
// becomes `x != x`, and the query answers with no error and an identical
// --explain. That is round 1's H-1 shape verbatim, and it is why every ref is
// restamped here rather than left to the fallback.
//
// The two sides are separated by SLOT — see kResidualBuildSlot (logical_plan.h):
//
//   BODY SIDE (level 0)   its column is APPENDED to the body's projection, after
//                         the keys, under the generated name `$rN` — where N is
//                         its POSITION in that projection, which is what makes
//                         the names unique without a second counter and makes
//                         --explain say which build column the residual reads.
//                         APPENDED, never inserted: the right-hand key indices
//                         are POSITIONAL 0..k-1 (rightKeyIndices), so anything
//                         placed before them re-points every key at the wrong
//                         column. (q21's residual therefore prints `$r1`: one
//                         key at 0, the residual's `l_suppkey` at 1.)
//   OUTER SIDE (level 1)  `id.outward()`, which is the SAME arithmetic
//                         splitCorrelation applies to JoinKey::from_slot: one
//                         step out makes a level-1 reference level 0 in the
//                         enclosing block, whose schema is the probe half.
//
// `$` is not lexable in an identifier, so no body column and no probe column can
// ever be named `$rN` — the name is a second, independent barrier behind the
// slot, and it is what makes a mis-binding VISIBLE: --explain renders the
// residual `(l1.l_suppkey != $r0)`, so the two same-named columns read
// differently on the surface used to debug them.
//
// forEachLocalColumnRef (predicate_pushdown.h) is the maintained writing walk —
// collectSlots's mutable twin — and asking it is what keeps this from becoming a
// twentieth private dispatch site. It deliberately does NOT descend into a
// SubqueryExpr's BODY (another scope's range table), which is right here too;
// splitCorrelation has already refused a residual that holds one.
//
// An UNRESOLVED ref reports isLocal() == true, so it would be silently taken for
// a body column and projected under a name the body cannot produce. It is
// refused with the SAME message splitCorrelation gives it, because it is the
// same fault seen one branch later (round 1, L-8).
// `body_projection` is the body's select list, ALREADY holding the key refs;
// this appends to it, so `size()` is the position the appended column will take.
void bindResidualRefs(std::vector<std::unique_ptr<Expr>>& residuals,
                      std::vector<std::unique_ptr<Expr>>& body_projection) {
    for (auto& r : residuals) {
        forEachLocalColumnRef(r.get(), [&](ColumnRef& cr) {
            if (!cr.id.isResolved())
                refuse("a column reference in this body could not be resolved to "
                       "any relation, so the conjunct cannot be classified as "
                       "local or correlated");
            if (cr.id.isLocal()) {
                const std::string name =
                    "$r" + std::to_string(body_projection.size());
                auto projected = cloneExpr(&cr);
                projected->alias = name;   // buildProjectSchema publishes it
                body_projection.push_back(std::move(projected));
                cr.table_name.clear();
                cr.column_name = name;
                cr.id = ColumnId::local(kResidualBuildSlot);
                return;
            }
            // The same containment splitCorrelation applies to a key, for the
            // same reason: levels are NOT decremented when a middle body is
            // decorrelated, so a level-2 reference would be read as a slot of
            // the wrong range table.
            if (cr.id.level() != 1)
                refuse("a reference to a query block more than one level out "
                       "cannot be decorrelated here");
            cr.id = cr.id.outward();
        });
    }
}

// WEEK 36 — the residual's own half of the cascade rule, and the MIRROR of
// refuseUnguardedRaiser rather than a copy of it.
//
// That function screens the conjuncts that LOST a guard. This one screens the
// conjunct that GAINED one. A residual is lifted out of the body's AND cascade
// and evaluated in the probe, so every body-local conjunct written AFTER it now
// runs BEFORE it — in the body's own filter, on the build side — and the
// residual is evaluated on FEWER rows than the written order gives it. For an
// expression that can raise, that MASKS a raise: the query answers where the
// written cascade says it must error. Same rule, opposite sign; expr_totality.h
// binds the rewrite, not the direction.
//
// CONSERVATIVE ON PURPOSE: `unguarded` holds the body-local conjuncts written
// after ANYTHING this rewrite lifted, which is a superset of "written after a
// residual". Refusing on the superset can decline a query whose raising residual
// is in fact written last, and that costs nothing measurable — residuals are new
// this week, so nothing that ran before can be declined by this — while the
// precise version would need splitCorrelation to hand back a per-residual index
// nothing else wants.
//
// The schema is the FULL residual schema, not the body's: a residual names both
// sides by construction, and screening it against the body's spine schema alone
// would answer "unresolvable, so may raise" for every one of them.
void refuseMaskedResidualRaiser(const std::vector<std::unique_ptr<Expr>>& residuals,
                                const std::vector<std::unique_ptr<Expr>>& unguarded,
                                const Schema& residual_schema) {
    if (residuals.empty() || unguarded.empty()) return;
    for (const auto& r : residuals) {
        if (conjunctMayRaise(r.get(), residual_schema))
            refuse("a correlated conjunct that can raise is lifted onto the "
                   "semi/anti join as an ON residual while a body-local conjunct "
                   "written after it stays in the body, so the residual would be "
                   "evaluated on fewer rows than the written order gives it");
    }
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
// reachesOutsideThisBody is the SAME predicate splitCorrelation classifies with,
// and it must be — this check runs on what splitCorrelation left behind, so a
// question asked one way there and another way here is a refusal for a conjunct
// the classifier just called legal. An UNRESOLVED ref maps to -1 there and here
// alike, and is named in the message rather than mis-diagnosed as correlation
// (round 1, L-8).
//
// !! IT MATTERS THAT THIS USES THE NARROWED PREDICATE AND NOT collectSlots
// DIRECTLY (seam audit pass 2, B-3). The WHERE check below runs AFTER
// splitCorrelation has put the body-local conjuncts back, and a nested
// correlated subquery is now one of them. Left on plain collectSlots, this
// function would refuse exactly the queries B-3's fix just admitted, one line
// later and with a different wrong cause — the fix would have moved the refusal
// rather than removed it. Both call sites, one predicate.
void refuseSurvivingCorrelatedRefs(SelectStatement& body) {
    auto check = [](std::unique_ptr<Expr>& e, const char* where) {
        if (!e) return;
        if (reachesOutsideThisBody(e))
            refuse(std::string("a reference this body cannot name locally survives "
                               "in its ") + where + " (a correlated reference is "
                   "lowered only from a top-level equality in the body's WHERE; "
                   "an unresolved one would report the same)");
    };
    for (auto& j : body.joins)       check(j.condition, "JOIN ... ON clause");
    for (auto& e : body.select_list) check(e, "SELECT list");
    for (auto& o : body.order_by)    check(o.expr, "ORDER BY");
    // splitCorrelation guarantees this one is empty of correlated refs. Checked
    // anyway, because a guarantee that is never tested is the shape this round
    // has now found twice.
    check(body.where, "WHERE");
}

// Week 36 — IS THIS SUBTREE A CONSTANT? The other half of
// constantWrapperAggregateSlot below, and written beside it on purpose: the two
// are one whitelist read in two directions, and a node admitted by one that the
// other cannot descend into is the drift this pair exists to make impossible.
//
// Literals and arithmetic over literals only. Everything else is refused, and
// each exclusion earns its place:
//   ColumnRef      a body-local ref outside the aggregate is an UNGROUPED
//                  reference -- a different query; an OUTER one is a correlated
//                  ref in the SELECT list, which refuseSurvivingCorrelatedRefs
//                  already declines by name. Do not widen that silently.
//   AggregateExpr  a second aggregate needs a second output column and a second
//                  zero-row rule; the COUNT CASE below is written for one.
//   SubqueryExpr   a nested body inside the wrapper is a scope question this
//                  rewrite does not answer.
//   everything else (CASE, SUBSTRING, LIKE, IN, IS NULL, INTERVAL)
//                  no TPC-H query needs one, each adds a type or NULL question,
//                  and an IntervalLiteral must not survive planning at all.
bool constantOnly(const Expr* e) {
    if (!e) return true;
    if (dynamic_cast<const Literal*>(e)) return true;
    if (auto* bin = dynamic_cast<const BinaryExpr*>(e))
        return constantOnly(bin->left.get()) && constantOnly(bin->right.get());
    if (auto* un = dynamic_cast<const UnaryExpr*>(e))
        return constantOnly(un->operand.get());
    return false;
}

// Week 36 — THE CONSTANT WRAPPER around the body's aggregate.
//
// TPC-H Q17 writes `(SELECT 0.2 * AVG(l_quantity) ...)`: the select-list item is
// a BinaryExpr WRAPPING the aggregate, not the aggregate. Week 34 required
// found[0] == body.select_list[0] and refused the spec's own text, while the
// semantically identical constant-OUTSIDE form `0.2 * (SELECT AVG(...) ...)`
// decorrelated and matched SQLite. This function is the whole difference.
//
// THE WRAPPER IS LIFTED OUT OF THE BODY, not pushed through it. The body still
// selects the bare aggregate; the caller re-attaches the wrapper around the
// SUBSTITUTED reference in the outer expression. That is sound exactly when
// every leaf other than the aggregate is a constant -- then f(agg) evaluated per
// group inside and f(agg_column) evaluated per outer row outside are the same
// function of the same argument.
//
// !! WHY NOT KEEP THE WRAPPER IN THE BODY. Naming the derived column
// exprToString(wrapper) also runs -- `SELECT team, 0.2 * AVG(speed) ... GROUP BY
// team` is already legal here. It breaks the COUNT rule: the zero-row CASE would
// substitute 0 for the WHOLE wrapper, so a body of `1 + COUNT(*)` over an empty
// correlation group answers 0 where SQL says 1. Lifting puts the CASE at the
// aggregate's own position, where it is correct by construction and needs no
// second copy of the tree.
//
// Returns the OWNING SLOT of the aggregate, so the caller can move the aggregate
// out and assign the substitution back in. When the wrapper IS the aggregate
// (Week 34's shape) it returns the item's own slot, which makes that shape a
// special case of this one rather than a second production.
//
// NOT A DISPATCH SITE. It walks only BinaryExpr and UnaryExpr and refuses every
// other node type, so a new expression node owes it nothing -- see
// development.md's "Extending the expression language" checklist -- NINETEEN
// sites, ten of them silent -- which this deliberately does not join. If it is
// ever widened to more node types, it DOES join that list. (Named rather than
// numbered on purpose: an earlier form of this comment said "17-site" and was
// already two behind the table it points at.)
std::unique_ptr<Expr>* constantWrapperAggregateSlot(std::unique_ptr<Expr>& item) {
    if (dynamic_cast<AggregateExpr*>(item.get())) return &item;

    if (auto* bin = dynamic_cast<BinaryExpr*>(item.get())) {
        std::unique_ptr<Expr>* l = constantOnly(bin->left.get())
                                 ? nullptr : constantWrapperAggregateSlot(bin->left);
        std::unique_ptr<Expr>* r = constantOnly(bin->right.get())
                                 ? nullptr : constantWrapperAggregateSlot(bin->right);
        if (l && r)
            refuse("a correlated scalar subquery's select list may hold ONE "
                   "aggregate (two would need two output columns and two "
                   "zero-row rules)");
        if (l || r) return l ? l : r;
        // Neither side carries the aggregate and both are constant, so there is
        // no aggregate anywhere -- the non-aggregate body Week 34 refused, and
        // the message below is still its message.
    } else if (auto* un = dynamic_cast<UnaryExpr*>(item.get())) {
        if (!constantOnly(un->operand.get()))
            return constantWrapperAggregateSlot(un->operand);
    }

    // THE LOAD-BEARING REFUSAL, narrowed but not removed. Without an aggregate,
    // GROUP BY does not guarantee one row per key, and Week 31's runtime `scalar
    // subquery returned more than one row` check has nowhere to live after the
    // rewrite -- a query SQL calls an error would return an arbitrary row
    // instead. Refusing keeps that divergence honest. Week 36 adds the second
    // half: a wrapper that is not constant cannot be lifted out of the body, so
    // the aggregate it holds is not one this rewrite can name.
    refuse("a correlated scalar subquery is decorrelated only when its select "
           "list is a single aggregate, optionally wrapped in constant "
           "arithmetic (TPC-H Q17's `0.2 * AVG(...)`); a non-aggregate body has "
           "no one-row-per-key guarantee, so 'returned more than one row' could "
           "not be checked, and a non-constant wrapper cannot be lifted out of "
           "the body");
}

// Week 34 — the SCALAR guard. Deliberately NOT requireDecorrelatableBody with a
// flag: that function's stated condition is "no GROUP BY / HAVING / aggregate /
// LIMIT / DISTINCT", and this rewrite REQUIRES an aggregate and ADDS a GROUP BY.
// Widening it would leave one header stating a rule it no longer enforces for
// half its callers, which is the shape that produced three silent wrong answers
// in Week 33.
//
// Week 36: the wrapper rule is enforced HERE, by calling the same locator the
// lowering calls -- one function invoked twice, never two walkers that must
// agree. It deliberately does NOT hand the located slot back: the caller moves
// the select-list item out of the vector, which empties the slot a pointer taken
// here would name. (That is not hypothetical -- it was the first form of this
// change, and it broke every UNWRAPPED body while the wrapped one worked.) The
// caller re-locates on its own local instead.
//
// The clause checks stay AHEAD of the wrapper check so a body with both a LIMIT
// and a bad wrapper still reports the LIMIT, which is what the rejection suites
// pin.
void requireDecorrelatableScalarBody(SelectStatement& body) {
    if (body.limit)    refuse("a scalar body with LIMIT cannot be decorrelated");
    if (body.distinct) refuse("a scalar body with DISTINCT cannot be decorrelated");
    if (body.having)   refuse("a scalar body with HAVING cannot be decorrelated");
    if (!body.group_by.empty())
        refuse("a scalar body with its own GROUP BY cannot be decorrelated "
               "(the rewrite supplies the grouping)");
    if (body.select_star || body.select_list.size() != 1)
        refuse("a correlated scalar subquery must select exactly one expression");

    (void)constantWrapperAggregateSlot(body.select_list[0]);
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
            std::vector<std::unique_ptr<Expr>> unguarded;
            splitCorrelation(body_conjuncts, keys, body_key_refs, local, unguarded);
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
            // Week 36: take the WHOLE select-list item -- which for TPC-H Q17 is
            // `0.2 * AVG(l_quantity)`, not the aggregate. agg_slot points at a
            // unique_ptr INSIDE it, or at `wrapper` itself when the wrapper IS
            // the aggregate (Week 34's shape); the substitution below assigns
            // back into that slot, so one code path serves both.
            auto wrapper = std::move(body.select_list[0]);
            // LOCATE AFTER THE MOVE, not before. The guard above already proved
            // the wrapper is legal; re-running the same locator on this local is
            // what makes the pointer structurally valid rather than valid by a
            // precondition nobody rechecks. Locating in the guard and holding the
            // pointer across the move is wrong for the unwrapped body, where the
            // slot IS body.select_list[0] and the move empties it.
            std::unique_ptr<Expr>* agg_slot = constantWrapperAggregateSlot(wrapper);
            auto agg_expr = std::move(*agg_slot);
            // SEAM AUDIT pass 2 — B-2. DROP THE SELECT ALIAS before the
            // aggregate is pushed back into the body's rewritten select list.
            //
            // `agg_name` below is derived from the aggregate NODE; the column
            // the derived relation actually publishes is named by
            // buildProjectSchema, which lets a SELECT alias override the
            // canonical name (`if (!expr->alias.empty()) cols.back().name =
            // ...`). Two derivations of one name, and they disagreed exactly
            // when the alias sat on the node that gets moved — i.e. when the
            // body is UNWRAPPED, so the select-list item IS the aggregate:
            //
            //   (SELECT AVG(l2.speed) AS a FROM laps l2 WHERE l2.team = l.team)
            //     relation published [$k0, "a"], the substituted outer ref
            //     looked for "AVG(l2.speed)" -> Error: column not found:
            //     'AVG(l2.speed)', naming a column the user never wrote, for
            //     legal SQL that SQLite answers.
            //
            // The WRAPPED form was unaffected and that asymmetry is the tell:
            // there the alias rides on the BinaryExpr, which is lifted OUT of
            // the body, so the bare aggregate moved into the select list never
            // carried one. It also broke a claim the README and this file's
            // oracle both rest on — that the wrapped and constant-outside forms
            // produce the SAME plan. With an alias they did not: one planned and
            // one did not. Clearing it here restores that, and the two forms are
            // now byte-identical under --explain WITH an alias as well as
            // without.
            //
            // Dropping it loses nothing. A scalar subquery has no name in SQL —
            // its body's output column is never addressable from outside — so
            // the alias was already unobservable; it was only ever a second,
            // divergent source for an internal name. This is the same cure the
            // correlation keys got in 8a23b9d ($kN, generated at one point and
            // consumed at one point), applied to the one remaining column of
            // this relation that still resolved by a name derived twice.
            //
            // Cleared on the AGGREGATE only, deliberately. A wrapper's alias
            // rides into the outer WHERE conjunct on the lifted expression,
            // where it is inert: `alias` is read only by buildProjectSchema, for
            // select-list items, and a subquery may only appear in WHERE or
            // HAVING (the Validator's position rule). Clearing it too would be
            // touching a thing that does nothing.
            agg_expr->alias.clear();
            const auto* agg = static_cast<const AggregateExpr*>(agg_expr.get());
            const std::string agg_name = aggregateOutputName(agg);
            // !! COUNT IS THE EXCEPTION TO THE ZERO-ROW RULE, and getting this
            // wrong was a silent wrong answer (Week 34 audit round 1, F1).
            // SUM / AVG / MIN / MAX over an empty set are NULL, so the LEFT join's
            // null-extension IS their value. COUNT over an empty set is **0**, and
            // the rewrite produces NO GROUP ROW AT ALL for a correlation key with
            // no matching body rows — so the join null-extends it and the outer
            // predicate reads NULL where SQL says 0. Measured on shipped data:
            // `WHERE d.age > (SELECT COUNT(*) FROM laps l WHERE
            // l.driver_id = d.driver_id AND l.speed > 999)` returned 0 rows
            // against SQLite's 20.
            //
            // COUNT(DISTINCT x) has the identical rule — it is still a COUNT —
            // which is why this tests the FUNCTION and not the `distinct` flag.
            const bool count_body = agg->function_name == "COUNT";
            body.select_list.clear();
            for (auto& ref : body_key_refs) {
                auto* cr = static_cast<ColumnRef*>(ref.get());
                body.group_by.push_back(
                    GroupByColumn{cr->table_name, cr->column_name, cr->id, nullptr});
                body.select_list.push_back(cloneExpr(ref.get()));
            }
            body.select_list.push_back(std::move(agg_expr));

            auto body_plan = LogicalPlanBuilder::build(std::move(body), catalog);

            // SEAM AUDIT PASS 5, subquery B-3. Screened HERE and not inside
            // splitCorrelation because this is the first point at which the
            // body's own schema exists, and the screen needs operand TYPES.
            refuseUnguardedRaiser(unguarded, *body_plan);

            // THE SAME NODE FROM (subquery) produces, and the same normalization:
            // every column stamped slot 0, the outer slot applied by the merged
            // schema below. Not a special case -- if it were, the four
            // descend-to-SCAN walkers, the range-table size and every
            // development.md row would need a second argument and the two would
            // drift.
            const std::string alias = "$scalar" + std::to_string(out.lowered);
            TableRef synthetic = TableRef::named("", alias);

            // SEAM AUDIT (subquery chain, pass 1) — F1 and F2, which are ONE
            // defect seen from two sides, and must be fixed together.
            //
            // F1: two correlated equalities on the SAME body column
            // (`l.k = d.a AND l.k = d.b`) push two group keys named `k`, and
            // derivedRelationSchema — written for a user's `FROM (…) AS d` —
            // refuses a duplicate output name with "give one of them an alias".
            // The user wrote no derived table and cannot alias a column of
            // `$scalar0`, so a legal query that SQLite answers is refused with
            // advice nobody can act on.
            //
            // F2: this is the only one of the four subquery lowerings whose
            // BUILD-SIDE key is resolved by BARE NAME — the join it builds is
            // STANDARD, so rightKeyIndices takes the `indexOf(k.join_col)`,
            // first-match-wins path rather than the positional one the semi/anti
            // lowerings get. That was safe only because F1's refusal made a
            // duplicate build-side name unreachable. Relaxing F1's check would
            // therefore have opened a silent wrong answer through F2.
            //
            // Both close by RENAMING the group-key columns to generated,
            // per-index names instead of relaxing anything. `$k0…$k{n-1}` are
            // pairwise distinct by construction, so F1's collision cannot arise;
            // and `$` is not lexable in an identifier, so no body column can ever
            // share one of these names — the bare-name lookup is now unique by
            // construction rather than by another pass's refusal. Duplicate keys
            // then behave as SQL says: `$k0` and `$k1` both carry `l.k`, and the
            // join tests `d.a = l.k AND d.b = l.k`, which is the written meaning.
            //
            // The AGGREGATE column keeps its own name: the outer predicate reads
            // it by that name (the ColumnRef built below), and it is the one
            // column of this relation the enclosing query refers to.
            // The rename is applied to the RELATION, not to the body: it is the
            // same thing `FROM (…) AS d (a, b)` does, and derivedRelationSchema
            // is given the already-renamed schema so its duplicate check runs on
            // the names the relation will actually publish.
            std::vector<ColumnDef> renamed = body_plan->output_schema.columns();
            // buildAggregateSchema emits group keys first, in key order, then the
            // aggregates — so the body is exactly [keys…, aggregate].
            if (renamed.size() != keys.size() + 1) {
                throw std::runtime_error(
                    "internal: correlated scalar body produced "
                    + std::to_string(renamed.size()) + " columns for "
                    + std::to_string(keys.size()) + " correlation keys");
            }
            for (size_t i = 0; i < keys.size(); ++i) {
                renamed[i].name = "$k" + std::to_string(i);
                keys[i].join_col = renamed[i].name;
            }

            Schema normalized =
                derivedRelationSchema(Schema(std::move(renamed)), synthetic);
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
                // SEAM AUDIT pass 2 — B-1. HIDDEN, because `$scalarN` is not a
                // relation the user wrote and `SELECT *` means "the columns of
                // the relations in my FROM clause". Without this the star
                // expansion in LogicalPlanBuilder::build (which runs LAST, over
                // this merged schema) emitted `$k0` and the aggregate as result
                // columns: `SELECT * FROM drivers d WHERE d.age > (SELECT
                // COUNT(*) ...)` returned SEVEN columns where SQLite returns
                // five. A WRONG ANSWER, not an error — the extra columns resolve
                // cleanly. Inside a derived body the same defect surfaced as
                // `internal: derived table 'x' was bound against a 5-column
                // schema but planned to 7 columns`, because blockOutputSchema
                // models no subquery lowering and so never saw them.
                //
                // `hidden` is exactly the right mechanism and not a borrowed
                // one: its stated meaning is "computed and flows through the
                // plan, but SELECT * synthesis skips it" (common/schema.h), and
                // it is read in exactly three places — build()'s star expansion,
                // Planner::plan's, and blockOutputSchema's step 3. Nothing about
                // resolution consults it, so the outer predicate's
                // slot-qualified read of the aggregate column is untouched;
                // indexOf matches on name (and slot) alone.
                //
                // This is what re-narrows the star to the user's relations at
                // EVERY nesting depth: a body containing a correlated scalar
                // hides its own synthetic columns in its own build(), so the
                // derived relation the enclosing block sees is already narrow,
                // and blockOutputSchema (which drops hidden columns for a star
                // body too) agrees with it by construction.
                //
                // The other three lowerings need no equivalent: SEMI / ANTI /
                // ANTI_NOT_IN joins take children[0]'s schema unchanged
                // (subquery_lowering.cc, and the anti-join site below), so they
                // never widen the star's domain in the first place.
                col.hidden = true;
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

            // Week 36 — the substitution goes back into the AGGREGATE'S OWN SLOT
            // inside the wrapper, and the wrapper then becomes the conjunct's
            // expression. That ordering is what makes the COUNT rule correct for
            // a wrapped body for free: `1 + COUNT(*)` over an empty correlation
            // group becomes `1 + CASE WHEN ref IS NULL THEN 0 ELSE ref END` = 1,
            // where substituting 0 for the whole wrapper would have answered 0.
            // When the wrapper IS the aggregate, agg_slot == &wrapper and the
            // assignment refills `wrapper` -- one path, no branch.
            if (!count_body) {
                *agg_slot = std::move(ref);
            } else {
                // COALESCE(ref, 0), spelled in the dialect this engine has:
                //   CASE WHEN ref IS NULL THEN 0 ELSE ref END
                // No new node, no new operator and no new grammar — searched CASE
                // and IS NULL both shipped in Week 25, and both branches type as
                // INT (aggregateResultType(COUNT) is INT, and the literal is), so
                // inferExprType's CASE unification accepts it without promotion.
                //
                // WHAT IT COSTS, stated rather than discovered later: CASE has no
                // vectorized kernel, deliberately (Week 25 — evaluate() short-
                // circuits and an eager chunk kernel would raise on discarded
                // branches). compileNode declines it and the enclosing predicate
                // falls back to the scalar evaluate(). That is correct-but-slow,
                // it is paid ONLY by a correlated-scalar COUNT body, and it is the
                // same fallback CASE has had since it shipped. The alternative —
                // refusing a COUNT body outright, which the audit offered as the
                // minimum fix — would have left TPC-H a correlated shape short for
                // a cost the dialect can already express.
                auto is_null = std::make_unique<IsNullExpr>();
                is_null->operand = cloneExpr(ref.get());
                is_null->is_not_null = false;

                auto zero = std::make_unique<Literal>(Value(static_cast<int64_t>(0)));

                auto case_expr = std::make_unique<CaseExpr>();
                CaseExpr::WhenClause when;
                when.condition = std::move(is_null);
                when.result = std::move(zero);
                case_expr->when_clauses.push_back(std::move(when));
                case_expr->else_expr = std::move(ref);
                *agg_slot = std::move(case_expr);
            }
            slot = std::move(wrapper);
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
        std::vector<std::unique_ptr<Expr>> unguarded;
        // WEEK 36 — the sixth argument is what makes q21 plannable. A correlated
        // conjunct that is not an equality is ROUTED here instead of refused; it
        // becomes this join's ON residual below.
        std::vector<std::unique_ptr<Expr>> correlated_residuals;
        splitCorrelation(body_conjuncts, keys, body_key_refs, local, unguarded,
                         &correlated_residuals);
        // UNCHANGED, and it is the half of the old refusal that must survive: a
        // body correlated ONLY by inequalities has no hash key, and the fallback
        // would be a cross product this engine has no operator for. The routing
        // above narrowed the refusal to exactly this case.
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
        //
        // WEEK 36 — THE RESIDUAL'S BODY COLUMNS ARE APPENDED TO THIS LIST, AFTER
        // THE KEYS, and the word "after" is load-bearing. Everything above rests
        // on the right-hand key indices being POSITIONAL 0..k-1
        // (rightKeyIndices), so a column placed anywhere but the tail re-points
        // every key at its neighbour — wrong rows, no error. bindResidualRefs
        // gives each one the generated name `$rN` and rewrites the residual's
        // ref to match; see its comment for why a name is not enough on its own.
        body.select_star = false;
        body.select_list = std::move(body_key_refs);
        bindResidualRefs(correlated_residuals, body.select_list);
        auto body_plan = LogicalPlanBuilder::build(std::move(body), catalog);

        // SEAM AUDIT PASS 5, subquery B-3. Screened HERE and not inside
        // splitCorrelation because this is the first point at which the body's
        // own schema exists, and the screen needs operand TYPES.
        refuseUnguardedRaiser(unguarded, *body_plan);

        // WEEK 36 — the mirror screen, and it needs BOTH schemas, so it is the
        // first thing after the body plan and before the join exists. Same
        // derivation the operator and the two static screens will use, from the
        // one function, so none of them can be answering about a different
        // expression than the one that runs.
        const Schema residual_schema =
            joinResidualSchema(spine->output_schema, body_plan->output_schema);
        refuseMaskedResidualRaiser(correlated_residuals, unguarded, residual_schema);

        // SEAM AUDIT PASS 5, P5-1 — the same screen lowerInSubqueries takes, for
        // the same reason: this join goes BELOW the WHERE filter and removes
        // rows, so a conjunct written before this one would be evaluated on its
        // survivors. See logical_plan.h.
        spine = guardLoweredConjunctPrefix(std::move(spine), kept);

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
        // WEEK 36 — and it is ANTI, never ANTI_NOT_IN, for a SECOND reason now:
        // ANTI_NOT_IN may carry no residual at all (VecHashJoinNode's
        // constructor says so), because its NULL short-circuit is a claim about
        // the key column that a residual makes untrue.
        //
        // CONJOINED, not assigned: `spine` is a chain, and two EXISTS conjuncts
        // over one body would each own a residual. conjoinAll of an empty vector
        // is nullptr, which is exactly the pre-Week-36 value for every semi/anti
        // join and keeps every existing plan string byte-identical.
        join->on_residual = conjoinAll(std::move(correlated_residuals));

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
        // !! P4-M1 — the decline this copy forces, and why the fix is not one
        // line, are recorded once at the twin paragraph in subquery_lowering.cc.

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
