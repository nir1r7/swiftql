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
//   3. the body has NO GROUP BY / HAVING / aggregate / LIMIT / DISTINCT. THIS
//      CONDITION IS THIS FUNCTION'S, NOT THE FILE'S — Week 34's
//      lowerCorrelatedScalars below REQUIRES an aggregate and ADDS a GROUP BY,
//      and has its own guard saying so. Do not widen requireDecorrelatableBody
//      with a flag to serve both: a header stating a rule it no longer enforces
//      for half its callers is the shape that produced three silent wrong
//      answers in Week 33. Those
//      make the body's row set depend on which outer row selected it, so
//      evaluating the body ONCE and probing it is not the same query. This is
//      the condition that is easiest to forget and impossible to see in the
//      results of a small dataset;
//   4. at least one key was produced. A body correlated by nothing is
//      uncorrelated and is materialized before planning; a body correlated ONLY
//      by non-equalities has no join to build.
//   6. NO BODY-LOCAL CONJUNCT THAT CAN RAISE IS WRITTEN AFTER A KEY (seam audit
//      pass 5, subquery B-3). Lifting a correlated equality into a join key
//      takes it out of the body's AND cascade: it is enforced by the PROBE,
//      above the body, so every body-local conjunct written after it is
//      evaluated on the body's whole relation instead of on the rows the guard
//      admitted. `expr_totality.h` forbids exactly that for an expression that
//      can raise, and this is the "introduce a raise" direction — the mirror of
//      the semi-join's "mask a raise" (subquery_lowering.h). It is REFUSED and
//      not repaired: the lifted equality's level-1 reference means nothing
//      inside the body, so no position in the body reproduces the row set it
//      gave. Keeping it would need the residue to ride as an ON residual on the
//      semi/anti join — the operator work condition 2's message already names.
//      Enforced by refuseUnguardedRaiser, at BOTH call sites, after the body's
//      plan exists (the screen needs operand types). Its one recorded gap: a
//      conjunct that itself HOLDS a subquery is not screened, because cloneExpr
//      shares a SubqueryExpr's shared_ptr and screening one would refuse every
//      nested-subquery body.
// A shape failing any of these is REFUSED by name at the site, not silently
// mis-rewritten. See docs/week-33-plan.md Task 5.
//
// AND THE SAME PASS OWES subquery_lowering.h's P5-1 SCREEN, for the join it
// builds rather than for the body: lowerExistsSubqueries deletes its conjunct
// and interposes a row-reducing semi/anti join BELOW the WHERE filter, so it
// calls guardLoweredConjunctPrefix exactly as lowerInSubqueries does. Two
// different halves of one rule, in one function.
//
//   5. every key pairs two STRING columns or two numeric ones. THIS ONE IS NOT
//      ENFORCED AT THIS SITE and must not be added here: it is
//      Validator::validateJoinKeyTypes, over the finished plan, where all four
//      JoinKey producers converge. Stated in this list anyway because it is a
//      condition on the rewrite's validity like the other four, and because the
//      four weeks it went unstated are the finding — Week 29 wrote the rule into
//      Validator::validate's `stmt.joins` loop, splitCorrelation shipped after
//      it, and `d.driver_id = l.team` planned into a semi-join whose text
//      comparison half-matched (seam audit pass 3, B3-2). The
//      correlated-scalar rewrite below inherits the same condition through
//      splitCorrelation, and the same single enforcement.
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

// Week 34 — CORRELATED SCALAR subqueries (the Q17 shape), decorrelated onto the
// derived-table machinery this week built.
//
//   WHERE l.speed > 0.2 * (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)
//     ->  outer  LEFT JOIN  (SELECT team, AVG(speed) FROM laps l2 GROUP BY team)
//                ON team = team,  the SubqueryExpr replaced by a ColumnRef
//
// A DECORRELATED SCALAR SUBQUERY IS A DERIVED TABLE WITH AN IMPLICIT JOIN. That
// sentence is why this is Week 34's and not Week 33's: the join is STANDARD, so
// its output_schema is MERGED and the aggregate's column is in scope above it,
// which is exactly the containment Week 32 established and Week 33 preserved.
// The right child here is built as a LogicalDerived — literally the same node
// FROM (subquery) produces — so the four descend-to-SCAN walkers, the range-table
// size and every development.md row need ONE argument rather than two.
//
// THREE THINGS MAKE THIS HARDER THAN THE EXISTS CASE, and each is handled rather
// than assumed:
//
//  1. THE JOIN IS LEFT, NOT INNER — AND THAT IS ONLY HALF THE ZERO-ROW RULE.
//     A key with no matching body rows produces NO GROUP ROW AT ALL, so the join
//     null-extends it. For SUM / AVG / MIN / MAX that IS the right value: those
//     are NULL over an empty set (Week 31 shipped the typed-null Literal for
//     exactly that), and an INNER join would instead DROP the outer row.
//     **COUNT IS THE EXCEPTION AND THE LEFT JOIN ALONE GETS IT WRONG**: SQL says
//     COUNT over zero rows is 0, so the outer predicate read NULL where it must
//     read 0. That shipped as a silent wrong answer and was caught by the Week 34
//     audit (F1) — `d.age > (SELECT COUNT(*) ... AND l.speed > 999)` returned 0
//     rows against SQLite's 20. The substituted value therefore depends on the
//     BODY'S AGGREGATE, not on the join: see the CASE wrapper at the substitution
//     site. Week 29's rules then follow from LEFT and are all correct here:
//     pushdown declines the null-supplying side, the build side is forced, and
//     join enumeration declines the whole tree with
//     `join-ordering=skipped (outer join)` — a real, reported plan-quality cost.
//
//  2. THE "MORE THAN ONE ROW" RULE DISAPPEARS. Week 31's deliberate divergence
//     (`scalar subquery returned more than one row`) is a RUNTIME CARDINALITY
//     check, and after the rewrite there is no per-outer-row result to count: the
//     GROUP BY makes exactly one row per key BY CONSTRUCTION. That is sound for
//     an AGGREGATE body and is the point. For a non-aggregate body the check
//     would vanish silently and a query SQL calls an error would return an
//     arbitrary row — so a non-aggregate body is REFUSED by name.
//
//  3. THE NODE IS NOT A WHOLE CONJUNCT. Q17 writes `l.speed > 0.2 * (SELECT ...)`,
//     so unlike EXISTS and IN the node sits arbitrarily deep inside a conjunct and
//     must be REPLACED IN PLACE while the join is grafted onto the spine.
//     forEachSubquery (dispatch site 19) is the maintained walker for that; a
//     private one here would be a twentieth silent dispatch site.
//
// WEEK 36 — THE CONSTANT WRAPPER, and what it changed about point 2 above.
// Week 34 enforced "the body's select-list item IS the aggregate"
// (`found[0] == body.select_list[0]`), which refused TPC-H Q17'S OWN TEXT:
// the spec writes `(SELECT 0.2 * AVG(l_quantity) ...)`, a `*` node WRAPPING the
// aggregate. The semantically identical constant-OUTSIDE form
// `0.2 * (SELECT AVG(...) ...)` decorrelated and matched SQLite, so the engine
// could answer the query but not read it.
//
// The rule is now "an aggregate, optionally wrapped in CONSTANT arithmetic", and
// THE WRAPPER IS LIFTED OUT OF THE BODY rather than pushed through it: the body
// still selects the bare aggregate, and the wrapper is re-attached around the
// SUBSTITUTED reference outside. Sound exactly when every leaf other than the
// aggregate is a constant — then f(agg) per group and f(agg_column) per outer row
// are the same function of the same argument. After the rewrite the spec's text
// and the constant-outside text produce the SAME PLAN, which is how the change is
// verified rather than merely tested.
//
// !! WHY NOT PUSH THE WRAPPER THROUGH THE BODY, since `SELECT k, 0.2 * AVG(x)
// ... GROUP BY k` is already legal here: it breaks point 1's COUNT rule. The
// zero-row CASE would substitute 0 for the WHOLE wrapper, so a body of
// `1 + COUNT(*)` over an empty correlation group answers 0 where SQL says 1.
// Lifting puts the CASE at the AGGREGATE'S position inside the wrapper, where it
// is correct by construction. Point 2's refusal therefore NARROWED and did not
// disappear: a non-aggregate body, a second aggregate and a non-constant wrapper
// are each still refused by name, and each is pinned in
// WEEK34_CORRELATED_SCALAR_REFUSED.
//
// !! requireDecorrelatableBody (the EXISTS guard) IS NOT REUSED, and must not be
// widened with a flag. Its condition 3 states "the body has NO GROUP BY /
// HAVING / aggregate / LIMIT / DISTINCT" — and this lowering REQUIRES an
// aggregate and ADDS a GROUP BY. One function whose header states a rule it no
// longer enforces for half its callers is the exact shape that produced three
// silent wrong answers in Week 33 and seven stale preconditions after it. Two
// guards, two headers, each true of its own caller.
struct ScalarLoweringResult {
    std::unique_ptr<LogicalPlanNode> plan;
    int lowered = 0;
};

ScalarLoweringResult lowerCorrelatedScalars(std::unique_ptr<LogicalPlanNode> spine,
                                            std::vector<std::unique_ptr<Expr>>& conjuncts,
                                            int range_table_size,
                                            const Catalog& catalog);

// Throws the stated refusal if any CORRELATED SubqueryExpr survives anywhere in
// `expr`. The same tripwire shape refuseUnloweredIn uses, and for the same
// reason: without it the node reaches inferExprType (dispatch site 12) and dies
// with a Week-31 message naming the materialization walker — a diagnostic
// pointing at the wrong pass.
void refuseUnloweredCorrelated(const Expr* expr, const char* clause);
