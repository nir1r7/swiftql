#include "parser/ast.h"
#include "common/value.h"
#include "common/schema.h"


Value evaluate(const Expr* expr, const Row& row, const Schema& schema);