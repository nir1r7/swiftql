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
// a LEFT join and such conjuncts stay above the whole join tree. Since Week 37
// the PRESERVED side is conditional too — not because the identity fails there,
// but because it is about the join's OUTPUT and a LEFT join's ON residual is
// evaluated on its CANDIDATE PAIRS. See distribute().
//
// !! RESULT PRESERVATION IS NOT THE WHOLE OBLIGATION, and Week 37 is where that
// stopped being an oversight. PER-ROW EVALUATION CAN THROW (substringOf,
// checkedArith overflow, and a comparison across the STRING boundary), so
// MOVING a conjunct decides whether the query ERRORS. The row sets are equal;
// the ERROR BEHAVIOUR is not, and `optimized != --no-optimize` was reproducible
// in BOTH directions on the shipped catalog.
//
// THE OBLIGATION IS NOT PER MOVE — IT IS PER EXPRESSION WHOSE ROW SET A MOVE
// CHANGES, and that is a strictly larger set than the conjuncts this pass
// touches. Two revisions of this paragraph enumerated MOVES, each was short by
// one entry, and both times the missing entry was the divergence:
//
//   * pass 4's P4-B1 — the paragraph listed three moves and the unlisted fourth
//     (descending below a derived body's projection) was the unscreened one;
//   * pass 5's P5-B1 — the paragraph listed four moves, all four were screened,
//     and the divergence was in an expression NO move touches. A LEFT join's ON
//     residual is evaluated once per CANDIDATE PAIR inside the probe loop, and
//     move 2 (pushing a conjunct into the preserved side) shrinks that pair set.
//     It is not a conjunct of any list `firstMayRaise` reads, so an enumeration
//     of moves could not have found it.
//
// So the count is no longer the claim. Every move is screened, and the two moves
// that rest on a SET EQUIVALENCE about a node's OUTPUT carry a SECOND screen for
// the expressions evaluated INSIDE that node: the PROJECT descent screens
// `project.exprs`, and the push below a join screens `LogicalJoin::on_residual`.
// The .cc states both, beside the identity each one is the missing half of.
//
// The screen is `exprMayRaise` / `firstMayRaise` in parser/expr_totality.h,
// shared with ChunkPruner and with the LIMIT rule in logical_plan.cc, and the
// .cc states the precondition and what property it restores. It is a no-op on a
// query with no raising conjunct, which is every TPC-H query and every harness
// entry.
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
