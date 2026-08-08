#pragma once

#include "parser/ast.h"
#include "common/value.h"
#include "common/schema.h"

// Resolve a ColumnRef to a column index in `schema`. When the ref carries a
// relation slot (>= 0, assigned by the Binder) and the schema has a column
// matching both (slot, name), that wins — this is what distinguishes join
// sides that share a column name. Otherwise falls back to first-match by bare
// name, preserving behavior for single-relation and post-aggregate schemas.
// Returns -1 if unresolved.
int resolveColumnIndex(const ColumnRef& col, const Schema& schema);

// recursively evaluates an Expr tree against a singe row
Value evaluate(const Expr* expr, const Row& row, const Schema& schema);

// Evaluate an expression AS A FILTER PREDICATE. Returns 1 TRUE, 0 FALSE,
// -1 UNKNOWN (NULL). Every Volcano node that keeps a row iff its predicate is
// TRUE must call this rather than evaluate() — see the comment on the
// definition (evaluator.cc) for why the difference is a semantic one and not a
// speed-up.
int evaluatePredicate(const Expr* pred, const Row& row, const Schema& schema);

// Scalar semantics shared with the vectorized kernels in
// expression_executor.cc. Exposed so the two implementations call the SAME
// code rather than two copies that must be kept in agreement by review.
bool likeMatch(const std::string& text, const std::string& pattern);
std::string substringOf(const std::string& s, int64_t start, bool has_length, int64_t length);