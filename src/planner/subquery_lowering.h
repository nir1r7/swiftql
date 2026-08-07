#pragma once

#include "planner/logical_plan.h"
#include "parser/ast.h"
#include "catalog/catalog.h"
#include <memory>
#include <vector>

// Week 32 — set-membership lowering. A DIFFERENT PRODUCTION from Week 31's
// materialization (subquery_materialization.h), not a replacement for it:
// materialization is right when the subquery's contribution is a CONSTANT (a
// scalar, an uncorrelated EXISTS — one value for the whole query), and a
// semi-join is right when it is a MEMBERSHIP TEST evaluated per outer row
// (IN / NOT IN). See docs/week-32-plan.md §2 for the routing table, and note
// what is deliberately NOT re-routed: an uncorrelated EXISTS stays materialized,
// because its value does not depend on the outer row at all and a semi-join
// would turn a foldable constant into a pipeline breaker.
//
// WHY IT CANNOT LIVE WHERE MATERIALIZATION LIVES. materializeSubqueries runs on
// the AST, in main.cc, before planning, and its output is an Expr. A semi-join's
// output is a PLAN NODE, so the rewrite must happen where plan nodes are built:
// inside LogicalPlanBuilder, after the FROM/JOIN spine exists and before the
// WHERE LogicalFilter is constructed.
//
// PRECONDITIONS, all established by Validator::validate, which both planner
// entry points run FIRST — the same trust-don't-recheck stance
// materializeSubqueries takes, and for the same reason: re-checking here would
// put the message in a layer that does not own it.
//   - no correlated subquery anywhere (refused in Week 33's name), so every
//     ColumnRef in a body is query_level 0 against the BODY's range table;
//   - an IN body has exactly one output column.
//
// WHAT IS REFUSED, and why refusing beats falling back. Since
// materializeSubqueries no longer consumes a Kind::IN node at all, every one of
// them must be lowered here or refused here — a silent fallback would re-open
// the two-paths problem (two productions that must agree on NULL semantics is
// exactly the drift Weeks 26/28/30 each had to undo). The refusals, each a
// stated row in the README dialect table and an entry in the rejection suite of
// python_tools/compare_against_sqlite.py:
//   - an IN subquery that is not a whole top-level WHERE conjunct (e.g. under
//     an OR): a semi-join is a whole-conjunct construct and there is no
//     disjunctive semi-join here;
//   - an IN subquery in HAVING: the join would have to sit above the aggregate,
//     and no TPC-H query needs it (Q11's HAVING subquery is scalar);
//   - a computed operand (`x + 1 IN (SELECT ...)`): JoinKey holds column NAMES,
//     not expressions — this engine has no computed-key join.
struct InLoweringResult {
    std::unique_ptr<LogicalPlanNode> plan;   // the spine, wrapped in one join per extraction
    int lowered = 0;                          // how many conjuncts were extracted
};

// Extracts every top-level WHERE conjunct that IS a SubqueryExpr{IN} and wraps
// `spine` in one LogicalJoin{SEMI|ANTI} per extraction. `conjuncts` is edited in
// place; what is left is re-conjoined into the LogicalFilter by the caller.
//
// Two IN conjuncts over one shared body statement produce TWO semi-joins, and
// that is correct — they are two separate membership tests against the same
// relation. There is nothing to cache; Week 31's statement-keyed result cache is
// deliberately NOT ported.
InLoweringResult lowerInSubqueries(std::unique_ptr<LogicalPlanNode> spine,
                                   std::vector<std::unique_ptr<Expr>>& conjuncts,
                                   const Catalog& catalog);

// Throws the stated refusal if any SubqueryExpr{IN} survives anywhere in `expr`.
// Called on what is left of the WHERE conjunction after lowering, and on HAVING.
//
// This is the tripwire that keeps a missed nesting position LOUD. Without it the
// node would reach inferExprType (dispatch site 12), which throws a Week-31
// message naming the materialization walker — a diagnostic pointing at the wrong
// pass. See docs/week-32-plan.md §3.
void refuseUnloweredIn(const Expr* expr, const char* clause);
