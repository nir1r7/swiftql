#pragma once

#include "parser/ast.h"
#include "common/value.h"
#include "common/schema.h"

// recursively evaluates an Expr tree against a singe row
Value evaluate(const Expr* expr, const Row& row, const Schema& schema);