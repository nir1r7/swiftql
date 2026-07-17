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