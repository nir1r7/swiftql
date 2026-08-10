#pragma once
#include "planner/vec_plan_node.h"
#include "execution/columnar_eval.h"
#include "parser/ast.h"
#include <memory>

class VecFilterNode : public VecPlanNode{
    public:
        VecFilterNode(std::unique_ptr<VecPlanNode> child, std::unique_ptr<Expr> predicate);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

        // Week 38 — forwarded, but ONLY when this node's predicate cannot RAISE.
        // The indices survive the trip (this node stamps a SelectionVector onto
        // its CHILD's chunk and passes the pointer through, so column i is the
        // same ColumnVector on both sides); what does not automatically survive
        // is WHICH ROWS the predicate above is evaluated on. See the definition.
        void pushBloomFilter(const std::vector<int>& key_indices,
                             std::shared_ptr<const BloomFilter> filter) override;
    private:
    std::unique_ptr<Expr> predicate_;      // declared before child_ so child_ is destroyed first
    std::unique_ptr<VecPlanNode> child_;
    // Compiled fallback subexpressions, keyed by Expr* into predicate_. Declared
    // after predicate_ so it is destroyed first — its keys point into that tree.
    PredicateExecutorCache exec_cache_;
};