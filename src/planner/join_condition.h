#pragma once

#include "parser/ast.h"
#include <string>
#include <vector>

// One equi-join key. from_col resolves against the already-joined (left) input,
// join_col against the relation this JOIN introduces. from_slot is the left
// column's binder slot — kept because the left input's merged schema can hold
// the same column name at several slots (laps.team and drivers.team), so a
// bare-name lookup there is a coin flip. -1 = unresolved (unbound callers).
//
// !! from_slot is the relation slot AS PRESENTED BY THE LEFT CHILD'S OWN SCHEMA:
// 0 when the left child is a single relation, the binder slot when it is a join
// subtree. Three consumers resolve from_col against exactly that schema —
// leftKeyIndices() (physical, THROWS on a miss), LogicalJoin::explain() and
// joinCardinality()'s left StatsContext — and a standalone scan stamps every
// column slot 0, because one relation has nothing to disambiguate. Written-order
// trees hide the distinction: their bottom join's left child IS relation 0. Join
// enumeration (Week 28) can put any relation at the bottom of the spine, so
// JoinEnumeration::rebuild sets from_slot = 0 on the FIRST join's keys
// deliberately. Slot 0 is unambiguous there: exactly one relation is present.
struct JoinKey {
    std::string from_col;
    std::string join_col;
    int from_slot = -1;
};

// Result of decomposing an ON clause: the equi-join keys, plus every conjunct
// that is not one.
//
// Week 27 stopped rejecting the non-key conjuncts. For an INNER join, ON and
// WHERE are interchangeable — R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S) — so a residual needs
// no new node and no new position; it is handed to the same predicate-assignment
// machinery that places WHERE conjuncts (soleSlot()/distribute() in
// predicate_pushdown.cc), which routes it to the relation slot that owns it.
//
// !! Week 29 (outer join) must revisit this. For a LEFT OUTER join an ON
// predicate filters the match test (unmatched left rows survive with NULLs)
// while a WHERE predicate filters the result, so merging the two changes the
// answer. Same trap predicate_pushdown.h documents for pushdown itself.
//
// `residuals` are BORROWED pointers into the statement's ON trees — this
// function owns nothing. Callers that need to keep one past the statement's
// lifetime clone it (cloneExpr, dispatch site 11).
struct JoinCondition {
    std::vector<JoinKey> keys;
    std::vector<const Expr*> residuals;
};

// Validate and decompose an ON condition into equi-join keys plus residuals.
// `right_slot` is the binder range-table slot of the relation this JOIN adds
// (stmt.joins[i] -> i + 1).
//
// Per conjunct of the flattened AND-chain, in this order:
//   1. any ColumnRef naming a slot > right_slot is a forward reference and
//      throws — those columns are absent from this join's output schema, so the
//      conjunct cannot even be a residual;
//   2. `=` between two ColumnRefs, one at right_slot and the other below it, is
//      an equi-join key (multi-key equi-joins, Week 26, required for TPC-H Q9);
//   3. everything else — non-equality operators, OR, literal or computed
//      operands, same-relation equalities, Week 25 nodes — is a residual.
// Identical keys are collapsed: `ON a.x = b.x AND a.x = b.x` is a legal
// predicate but not a legal key list (see the .cc for what it costs).
//
// Throws when no key survives: SwiftQL has no cross-product join operator, so a
// JOIN whose ON yields zero keys is a cartesian product with a filter on top.
//
// Slot routing requires a bound statement. When a ref carries no slot
// (validator-only callers that skip the Binder), keys fall back to positional
// routing (left = already-joined side) and the slot checks are skipped — the
// real pipeline always binds first.
JoinCondition classifyJoinCondition(const Expr* condition, int right_slot);
