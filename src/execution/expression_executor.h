#pragma once

#include "common/schema.h"
#include "common/type_id.h"
#include "execution/vec_types.h"
#include "parser/ast.h"
#include <memory>
#include <vector>

// Chunk-at-a-time expression evaluation for the vectorized path.
//
// WHY THIS EXISTS
// Week 24 routed general expressions (aggregate arguments, expression group
// keys) through the scalar Volcano evaluate(), a tree-walking interpreter with
// a dynamic_cast chain per node per row, called from inside the vectorized
// aggregate's inner loop. Measured on 1M rows, Release: SUM(speed) took 13.9ms
// while SUM(speed*(1-sector_1)) took 286ms — a 20.6x penalty paid entirely in
// per-row dispatch. This class hoists that dispatch out of the row loop:
// compile() resolves it once, execute() runs one typed loop per node per chunk,
// so the cost is amortised 1024:1.
//
// CONTRACT
//  - evaluate() in evaluator.cc remains the semantic reference. Every kernel
//    here mirrors it exactly, including SQLite division (INT/INT truncates,
//    x/0 is NULL), NULL propagation through arithmetic/comparison/AND/OR, and
//    the INT-as-boolean convention. The differential tests in
//    test_vectorized.cc are what hold the two in agreement.
//  - type() equals inferExprType(expr, input_schema). Callers rely on this to
//    pre-allocate output columns before the row loop.
//  - execute() returns a DENSE column: result index i corresponds to sel[i],
//    not to chunk row sel[i].
//  - compile() returns nullptr for any shape it has no kernel for. Callers keep
//    the per-row evaluate() path in that case, so an unsupported expression is
//    slow rather than wrong.
class ExpressionExecutor {
public:
    // Returns nullptr when `expr` cannot be compiled (unknown node type,
    // unresolvable column, ill-typed subtree, comparison across STRING and a
    // numeric type). Never throws.
    static std::unique_ptr<ExpressionExecutor> compile(const Expr* expr,
                                                       const Schema& input_schema);

    ~ExpressionExecutor();

    // Evaluate over the `sel` rows of `chunk`, in order. The returned reference
    // points at internal scratch that the next execute() call overwrites.
    const ColumnVector& execute(const DataChunk& chunk, const std::vector<int>& sel);

    TypeId type() const;

    // Compiled node. Public only so the kernels in expression_executor.cc can
    // name it; the definition stays in that file and no other translation unit
    // can do anything with an incomplete type.
    struct Node;

private:
    explicit ExpressionExecutor(std::unique_ptr<Node> root);

    std::unique_ptr<Node> root_;
};
