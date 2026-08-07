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
// folded tree. Folding cannot change results — it computes the same value the
// evaluator would have computed per row.
void foldConstants(SelectStatement& stmt);

// Single-expression entry point, exposed for tests.
void foldConstantsInExpr(std::unique_ptr<Expr>& expr);
