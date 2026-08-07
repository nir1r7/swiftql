#pragma once

#include "planner/logical_plan.h"
#include "parser/ast.h"
#include "catalog/catalog.h"
#include <memory>
#include <vector>

// Week 33 — DECORRELATION of `[NOT] EXISTS (SELECT ...)`.
//
// A correlated subquery is a DEPENDENT JOIN: for each outer row, run the body
// with that row's values substituted. Decorrelating it means promoting the
// correlation predicate out of the body and INTO the join condition, which turns
// the dependent join into an ordinary one.
//
//   WHERE EXISTS (SELECT * FROM laps l WHERE l.driver_id = d.driver_id
//                                        AND l.speed > 340)
//     ->  outer  SEMI JOIN  (sigma_{speed > 340} laps)  ON driver_id = driver_id
//
// NO NEW OPERATOR. Week 32 built JoinSemantics::{SEMI, ANTI}, VecHashJoinNode's
// set-probe and the schema invariant; this file only produces the join keys.
// It is a SIBLING of subquery_lowering.h rather than an addition to it, because
// that header's stated preconditions include "no correlated subquery anywhere",
// which stops being true this week and must be restated rather than silently
// invalidated.
//
// WHEN THE REWRITE IS VALID, stated as conditions rather than left implicit. A
// semi-join emits each left row AT MOST ONCE when a match exists, so it is
// equivalent to EXISTS only when:
//   1. the subquery appears as a WHOLE TOP-LEVEL WHERE CONJUNCT. `EXISTS(...) OR
//      x > 5` has no disjunctive semi-join to lower to (Week 32's rule for IN,
//      inherited verbatim);
//   2. every correlated conjunct in the body's WHERE is an EQUALITY between two
//      plain ColumnRefs, one level 0 (the body) and one level 1 (the immediately
//      enclosing block). JoinKey holds column NAMES, so an inequality or a
//      computed side has no key to become — and there is no cross-product
//      operator to run the residual on;
//   3. the body has NO GROUP BY / HAVING / aggregate / LIMIT / DISTINCT. Those
//      make the body's row set depend on which outer row selected it, so
//      evaluating the body ONCE and probing it is not the same query. This is
//      the condition that is easiest to forget and impossible to see in the
//      results of a small dataset;
//   4. at least one key was produced. A body correlated by nothing is
//      uncorrelated and is materialized before planning; a body correlated ONLY
//      by non-equalities has no join to build.
// A shape failing any of these is REFUSED by name at the site, not silently
// mis-rewritten. See docs/week-33-plan.md Task 5.
//
// NOT EXISTS AND NULL. Unlike NOT IN, an anti-join IS exactly NOT EXISTS: EXISTS
// is a pure existence test that is never UNKNOWN, so a NULL join key simply
// fails to match and the outer row survives — which is what SQL says. Week 32's
// unmatchable-key machinery exists for NOT IN's three-valued rule and must NOT
// be applied here.
struct ExistsLoweringResult {
    std::unique_ptr<LogicalPlanNode> plan;
    int lowered = 0;
};

ExistsLoweringResult lowerExistsSubqueries(std::unique_ptr<LogicalPlanNode> spine,
                                           std::vector<std::unique_ptr<Expr>>& conjuncts,
                                           const Catalog& catalog);

// Throws the stated refusal if any CORRELATED SubqueryExpr survives anywhere in
// `expr`. The same tripwire shape refuseUnloweredIn uses, and for the same
// reason: without it the node reaches inferExprType (dispatch site 12) and dies
// with a Week-31 message naming the materialization walker — a diagnostic
// pointing at the wrong pass.
void refuseUnloweredCorrelated(const Expr* expr, const char* clause);
