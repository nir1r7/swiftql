#pragma once

#include "parser/ast.h"

// Constant folding over arithmetic subtrees, run as the last step of binding.
//
// WHY IT MATTERS BEYOND SAVING A MULTIPLY: three separate fast paths in this
// engine pattern-match on the literal shape `ColumnRef op Literal`, and a
// constant subexpression on the right defeats all of them at once —
//
//   chunk_pruner.h        zone-map chunk skipping
//   columnar_eval.cc      the tight typed comparison loop in scanColumn
//   cardinality_estimator selectivity for equality and range predicates
//
// Measured (1M rows, Release): `WHERE season = 2024` skips chunks and filters in
// 337us; `WHERE season = 2020 + 4` skipped no chunks and took 201ms. Folding
// restores all three at once.
//
// SCOPE: arithmetic (`+ - * /`) and unary minus only, which is what the reported
// regression needs. Comparisons and AND/OR are deliberately left alone — folding
// them buys nothing here and would change predicate shapes the pushdown and join
// classification passes inspect.
//
// A fold that evaluates to NULL (`1 / 0`) is skipped. There is no NULL literal
// in the grammar, so this pass has never needed to make one — and it still does
// not, which is the point of keeping the rule.
//
// Week 31 note: a null Literal is no longer impossible in a bound tree. A
// materialized scalar subquery that returned zero rows (or one NULL row)
// substitutes one, and it carries its type on Literal::null_type so
// inferExprType can still answer. Nothing here changes: folding declines any
// fold that evaluates to NULL, so a null constant is never propagated by this
// pass, only carried past it. materializeSubqueries re-runs foldConstants after
// substitution, which is what restores the `ColumnRef op Literal` shape for
// `WHERE speed > (SELECT AVG(speed) FROM laps) * 2`.
//
// Runs before Validator::validate, so validation and every later pass see the
// folded tree.
//
// !! THIS PASS IS NOT OPTIONAL, AND THE LINE THAT USED TO END THIS HEADER —
// "Folding cannot change results — it computes the same value the evaluator
// would have computed per row" — WAS REFUTED BY EXECUTION (seam audit pass 2's
// B-5; binder.cc carries the corrected claim and the full census of what it
// affects). It is repeated here rather than only there because this header is
// what a reader consults before deciding whether the pass may be skipped, and a
// retracted claim surviving in a header is how this codebase has produced silent
// wrong answers twice.
//
// TWO facts a future week needs:
//
//   * WHAT IS TRUE ABOUT VALUES, and it is narrower than the old line: for any
//     expression foldNode agrees to fold, the folded node evaluates to the same
//     Value as the original would have, on every row. It earns that — same
//     evaluate(), declines on any throw, on a NULL result, on IS NULL, on
//     aggregates, and never descends into a subquery body. What does NOT follow
//     is that a query's OUTCOME is unchanged: a consumer that tests the SHAPE of
//     the tree sees a Literal the user did not write, and binder.cc lists all
//     five such consumers with a verdict each.
//
//   * IT IS LOAD-BEARING FOR CORRECTNESS (seam audit pass 3, L-2). This pass is
//     the ONLY thing that removes an `IntervalLiteral`; inferExprType and
//     evaluate both THROW on one that survives. So `date '1994-01-01' + interval
//     '1' year` — every TPC-H date-range predicate — depends on it having run.
//     Gating it behind `--no-optimize`, or making it cost-based, does not make
//     those queries slower: it makes them ERROR, and only on the differential
//     leg, which is the shape that reads as an engine bug rather than as a
//     dependency someone switched off.
void foldConstants(SelectStatement& stmt);

// Single-expression entry point, exposed for tests.
void foldConstantsInExpr(std::unique_ptr<Expr>& expr);
