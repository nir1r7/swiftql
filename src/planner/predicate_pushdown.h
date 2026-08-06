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
// so the estimator annotates the rewritten shape. Inner-join only: for an inner
// equi-join, pushing a single-relation predicate onto its side preserves the
// result (σ_p(R⋈S) ≡ σ_p(R)⋈S). Week 29's outer join must revisit this — a
// predicate on the null-supplying side cannot be pushed through an outer join.
class PredicatePushdown {
    public:
        static std::unique_ptr<LogicalPlanNode> apply(std::unique_ptr<LogicalPlanNode> root, const Catalog& catalog);
};

// The set of relation slots a predicate's columns reference. DISPATCH SITE 8 —
// an unhandled Expr subtype yields an empty slot set, which costs pushdown
// silently here and, since Week 27, makes a forward reference invisible in
// classifyJoinCondition (the second caller). Declared rather than file-local so
// join_condition.cc shares this one walker instead of growing an eleventh
// silent site; keep it in lockstep with restampSlots.
void collectSlots(const Expr* expr, std::unordered_set<int>& out);
