#pragma once

#include "parser/ast.h"
#include <string>

// Join keys extracted from a JOIN ... ON condition, routed to their relation
// by binder-assigned slot (0 = FROM, 1 = JOIN).
struct JoinConditionKeys {
    std::string from_col;
    std::string join_col;
};

// Validate and decompose an ON condition. Phase 4 supports exactly one
// cross-relation equality (ColumnRef = ColumnRef); anything else throws a
// specific error instead of silently degrading (compound conditions used to
// produce out-of-bounds key indices, non-equality operators executed as `=`,
// and same-relation conditions were rerouted across sides). Multi-key
// equi-joins and residual non-equality conjuncts are planned with the
// multi-way join work (README, Weeks 26-27).
//
// Slot routing requires a bound statement. When a ref carries no slot
// (validator-only callers that skip the Binder), keys fall back to positional
// routing (left = FROM) and the same-relation check is skipped — the real
// pipeline always binds first.
JoinConditionKeys classifyJoinCondition(const Expr* condition);
