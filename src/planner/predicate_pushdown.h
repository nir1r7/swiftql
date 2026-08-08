#pragma once

#include "catalog/catalog.h"
#include "planner/logical_plan.h"
#include <memory>
#include <unordered_set>

// Week 21 — predicate pushdown optimizer pass.
// Rewrites the logical plan so single-relation WHERE predicates are evaluated on
// their own scan (below the join) instead of above it, and orders the conjuncts
// left on each scan most-selective-first so the executor's selection-vector
// cascade (columnar_eval AND) does the least work.
//
// Runs after LogicalPlanBuilder::build and before CardinalityEstimator::estimate,
// so the estimator annotates the rewritten shape. For an inner equi-join,
// pushing a single-relation predicate onto its side preserves the result
// (σ_p(R⋈S) ≡ σ_p(R)⋈S) on BOTH sides. For a LEFT join it holds on the preserved
// side only, so since Week 29 distribute() declines to push into children[1] of
// a LEFT join and such conjuncts stay above the whole join tree.
//
// !! RESULT PRESERVATION IS NOT THE WHOLE OBLIGATION, and Week 37 is where that
// stopped being an oversight. PER-ROW EVALUATION CAN THROW (substringOf, and
// checkedArith overflow), so MOVING a conjunct — reordering it inside one filter
// OR pushing it below a join OR pushing it into a derived body — decides whether
// the query ERRORS. The row sets above are equal; the ERROR BEHAVIOUR is not,
// and `optimized != --no-optimize` was reproducible in BOTH directions on the
// shipped catalog. Every move this pass makes is now screened: see the totality
// screen and `firstMayRaise` in the .cc, which state the precondition and what
// property it restores. The screen is a no-op on a query with no raising
// conjunct, which is every TPC-H query and every harness entry.
//
// Since Week 37 the pass also ENTERS a derived relation's body (seam audit pass 3,
// B3-3) and DESCENDS past the first node it can rewrite (pass 2, B-2) — it used
// to return from the FILTER-over-JOIN branch, so a body was optimized only when
// the enclosing block happened to have no join. A conjunct REFUSED entry to a
// body is reported on LogicalDerived::pushdown_decision; nothing is stamped when
// there was no refusal.
class PredicatePushdown {
    public:
        static std::unique_ptr<LogicalPlanNode> apply(std::unique_ptr<LogicalPlanNode> root, const Catalog& catalog);
};

// The zone-map pruning hint a join may hand down to its PRESERVED (left) input,
// or nullptr when it must be withheld. `preserved_slots` is the set of relation
// slots that input carries.
//
// Week 29. There are exactly two places that route a WHERE predicate to a scan as
// a pruning hint — VectorizedPlanBuilder's JOIN case and Planner::plan's FROM
// scan — and they must not hold two copies of this rule: an outer join stops the
// residual fold, so `stmt.where` / the residual filter above the join is where
// null-supplying-side conjuncts now live, and a hint carrying one is routed
// straight at the preserved relation's scan. It cannot act, because
// ChunkPruner ignores a `relation_slot >= 1` ref (chunk_pruner.h) — but that
// leaves the whole safety argument in another file, and the failure mode is
// silent row loss on the side an outer join exists to preserve.
//
// An INNER join is returned unchanged: its residuals were folded into the WHERE
// conjunction, so every conjunct is a legal filter on the join output, and
// withholding would cost the mixed-slot `--no-optimize` hint the Phase 4
// benchmark measures.
//
// The test is `slots ⊆ preserved_slots` and NOT `slots ⊆ {0}`: in `(A ⋈ B) ⟕ C`
// relation B is preserved too, and testing slot 0 alone withheld a hint over B
// from a scan that is entitled to it.
//
// An EMPTY slot set withholds, deliberately, and the reason is specific to THIS
// caller rather than shared with the other two (corrected in Week 30; the claim
// here used to be "every other caller treats empty as the conservative answer",
// which is false and is exactly the claim that stopped anyone checking them):
//
//   soleSlot                -> empty gives -1, so the conjunct is not pushed.
//                              CONSERVATIVE: right answers, lost pushdown.
//   classifyJoinCondition   -> empty means the forward-reference loop has
//                              nothing to iterate, so the conjunct is ACCEPTED.
//                              PERMISSIVE — join_condition.cc says so in as
//                              many words ("a missed subtype makes a forward
//                              reference invisible rather than loud").
//   here                    -> empty would read as "mentions nothing
//                              unpreserved" and turn the guard OFF.
//
// So this caller must fail closed on its own account, not on a property of the
// others. collectSlots is dispatch site 8, where a missed Expr subtype yields
// an empty set. A genuinely constant predicate (`WHERE 1 = 1`) carries no
// ColumnRef, so collectSimplePredicates can make nothing prunable of it either
// way.
const Expr* pruningHintForPreservedSide(const Expr* hint, JoinType join_type,
                                        const std::unordered_set<int>& preserved_slots);

// The set of relation slots a predicate's columns reference, IN THE QUERY BLOCK
// the predicate is written in. DISPATCH SITE 8 — an unhandled Expr subtype
// yields an empty slot set, which costs pushdown silently at soleSlot and,
// since Week 27, makes a forward reference invisible in classifyJoinCondition.
// Declared rather than file-local so join_condition.cc shares this one walker
// instead of growing an eleventh silent site; keep it in lockstep with
// restampSlots.
//
// THREE callers since Week 29, not two: soleSlot, classifyJoinCondition and
// pruningHintForPreservedSide above. They disagree about an empty set — see the
// list at that function, and development.md's site-8 row.
//
// Week 30: -1 means "references something this block cannot name" — an
// unresolved ref, or a correlated reference inside a subquery, whose slot is a
// position in another block's range table.
void collectSlots(const Expr* expr, std::unordered_set<int>& out);
