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
// from a scan that is entitled to it. An EMPTY slot set withholds, deliberately:
// collectSlots is dispatch site 8, where a missed Expr subtype yields an empty
// set, and every other caller treats empty as the conservative answer. Reading it
// as permissive here would let a future node type turn this guard off silently.
// A genuinely constant predicate (`WHERE 1 = 1`) carries no ColumnRef, so
// collectSimplePredicates can make nothing prunable of it either way.
const Expr* pruningHintForPreservedSide(const Expr* hint, JoinType join_type,
                                        const std::unordered_set<int>& preserved_slots);

// The set of relation slots a predicate's columns reference. DISPATCH SITE 8 —
// an unhandled Expr subtype yields an empty slot set, which costs pushdown
// silently here and, since Week 27, makes a forward reference invisible in
// classifyJoinCondition (the second caller). Declared rather than file-local so
// join_condition.cc shares this one walker instead of growing an eleventh
// silent site; keep it in lockstep with restampSlots.
void collectSlots(const Expr* expr, std::unordered_set<int>& out);
