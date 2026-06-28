#pragma once

#include "execution/vec_types.h"
#include "common/schema.h"
#include "parser/ast.h"

// evaluates pred over every row in chunk, returning a SelectionVector of passing row indices
//
// fast paths (tight loop, no per row allocation):
// col op literal -- where op is =, !=, <, >, <=, >=
// AND/OR -- recursive composition via SelectionVector intersect/union
//
// fallback (row reconstruction + scalar evaluate()):
// IS NULL/IS NOT NULL, arithmetic subexpressions, col op col,
// literal op col, type mismatches, any unrecognised shape
SelectionVector evalPredicate(const Expr* pred, const DataChunk& chunk, const Schema& schema);
