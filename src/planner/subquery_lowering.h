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
// PRECONDITIONS. One is established by Validator::validate, which both planner
// entry points run FIRST — the trust-don't-recheck stance materializeSubqueries
// takes, and for the same reason: re-checking here would put the message in a
// layer that does not own it.
//   - an IN body has exactly one output column.
//
// !! THE SECOND PRECONDITION THIS HEADER USED TO STATE IS GONE, and it is worth
// recording why rather than quietly deleting the line. It read "no correlated
// subquery anywhere (refused in Week 33's name), so every ColumnRef in a body is
// query_level 0 against the BODY's range table". Week 33 DELETED that Validator
// refusal. The sentence outlived it in three files, and in two of them the code
// went on behaving as though it held: materializeSubqueries ran correlated
// bodies for a value (7c46bdf), and lowerInSubqueries lowered a correlated IN to
// a one-key semi-join, discarding the correlation (round 2 R2-C1). Both returned
// wrong rows with no error.
//
// So this pass no longer INHERITS the property; it ENFORCES its own share of it.
// lowerInSubqueries skips any node whose Binder-set `correlated` flag is true,
// which leaves it in `conjuncts` for refuseUnloweredCorrelated to refuse by name
// (logical_plan.cc). Inside a body reached from here, every ColumnRef is
// therefore query_level 0 against that body's range table — the same guarantee
// as before, now held by a check in this file instead of by a refusal in
// another one.
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
//
// A FOURTH REFUSAL APPLIES TO THE KEY THIS PASS BUILDS AND IS NOT RAISED HERE:
// a STRING operand against a numeric body column, or the reverse. It is raised
// by Validator::validateJoinKeyTypes over the finished plan, where all four
// JoinKey producers converge, and NOT at this site on purpose — Week 29 wrote
// that rule into Validator::validate's `stmt.joins` loop, this pass shipped
// after it and inherited nothing, and the key silently half-matched for four
// weeks (seam audit pass 3, B3-2: 1 row where SQLite returns 3, and the
// complement for NOT IN). Restating it here would be the fourth copy of one
// rule, which is how it came to be missing three times. Listed because a reader
// of this header must know the refusal exists; do not add it to this file.
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
