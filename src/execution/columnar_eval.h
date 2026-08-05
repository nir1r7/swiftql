#pragma once

#include "execution/expression_executor.h"
#include "execution/vec_types.h"
#include "common/schema.h"
#include "parser/ast.h"
#include <memory>
#include <unordered_map>

// Per-node cache of compiled subexpressions, owned by VecFilterNode so the
// compile happens once per query rather than once per chunk.
//
// evalPredicate keeps its own fast paths (the AND cascade, the OR union, and the
// tight `col op literal` comparison loops) because they beat a compiled
// expression — they touch one column and allocate nothing. This cache serves the
// FALLBACK: subtrees like `speed * 2 > 600` that used to reconstruct a full Row
// per row and walk the AST with dynamic_cast. Measured on 1M rows, Release, that
// fallback cost 231ms against 0.75ms for the equivalent literal-shaped predicate.
class PredicateExecutorCache {
public:
    // Compiled executor for `expr`, or nullptr when it cannot be compiled.
    // Both outcomes are cached, so a declined subtree is only attempted once.
    ExpressionExecutor* get(const Expr* expr, const Schema& schema) {
        auto it = compiled_.find(expr);
        if (it != compiled_.end()) return it->second.get();
        auto exec = ExpressionExecutor::compile(expr, schema);
        ExpressionExecutor* raw = exec.get();
        compiled_.emplace(expr, std::move(exec));
        return raw;
    }

private:
    // keyed by Expr* identity: the predicate tree is owned by the filter node and
    // outlives the cache, and its nodes never move
    std::unordered_map<const Expr*, std::unique_ptr<ExpressionExecutor>> compiled_;
};

// evaluates pred over every row in chunk, returning a SelectionVector of passing row indices
//
// fast paths (tight loop, no per row allocation):
// col op literal -- where op is =, !=, <, >, <=, >=
// AND/OR -- recursive composition via SelectionVector intersect/union
//
// fallback:
// IS NULL/IS NOT NULL, arithmetic subexpressions, col op col,
// literal op col, type mismatches, any unrecognised shape.
// With a cache, the fallback compiles the subtree once and evaluates it a chunk
// at a time; without one (or for a shape compile() declines) it reconstructs a
// Row per row and calls the scalar evaluate().
//
// input_sel: when non-null, only rows in input_sel->indices are evaluated.
// Pass &raw->sel when the incoming chunk has filter_applied=true so rejected
// rows are never touched (late-materialization contract).
SelectionVector evalPredicate(const Expr* pred, const DataChunk& chunk, const Schema& schema,
                              const SelectionVector* input_sel = nullptr,
                              PredicateExecutorCache* cache = nullptr);
