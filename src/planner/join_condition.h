#pragma once

#include "parser/ast.h"
#include <string>
#include <vector>

// One equi-join key. from_col resolves against the already-joined (left) input,
// join_col against the relation this JOIN introduces. from_slot is the left
// column's binder slot — kept because the left input's merged schema can hold
// the same column name at several slots (laps.team and drivers.team), so a
// bare-name lookup there is a coin flip. -1 = unresolved (unbound callers).
struct JoinKey {
    std::string from_col;
    std::string join_col;
    int from_slot = -1;
};

// Validate and decompose an ON condition into one or more equi-join keys.
// `right_slot` is the binder range-table slot of the relation this JOIN adds
// (stmt.joins[i] -> i + 1).
//
// Week 26 accepts a single equality or an AND-chain of them (multi-key
// equi-joins, required for TPC-H Q9). OR, non-equality operators, computed
// operands and same-relation conjuncts still throw a specific error instead of
// silently degrading (compound conditions used to produce out-of-bounds key
// indices, non-equality operators executed as `=`, and same-relation conditions
// were rerouted across sides). Routing non-equality ON conjuncts as post-join
// residual filters is Week 27.
//
// Slot routing requires a bound statement. When a ref carries no slot
// (validator-only callers that skip the Binder), keys fall back to positional
// routing (left = already-joined side) and the cross-relation check is skipped
// — the real pipeline always binds first.
std::vector<JoinKey> classifyJoinCondition(const Expr* condition, int right_slot);
