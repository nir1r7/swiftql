#pragma once
#include "planner/vec_plan_node.h"
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
    private:
    std::unique_ptr<Expr> predicate_;      // declared before child_ so child_ is destroyed first
    std::unique_ptr<VecPlanNode> child_;
};