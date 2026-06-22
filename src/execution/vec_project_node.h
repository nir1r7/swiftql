#pragma once

#include "planner/vec_plan_node.h"
#include "parser/ast.h"
#include "common/schema.h"
#include <memory>
#include <vector>

class VecProjectNode : public VecPlanNode {
    public:
        VecProjectNode(std::unique_ptr<VecPlanNode> child, std::vector<std::unique_ptr<Expr>> expressions, Schema output_schema);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;
    
    private:
        std::unique_ptr<VecPlanNode> child_;
        std::vector<std::unique_ptr<Expr>> expressions_;
        Schema output_schema_;
        DataChunk out_chunk_; // rebuilt every call
};