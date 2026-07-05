#pragma once
#include "planner/vec_plan_node.h"
#include "parser/ast.h"
#include "common/schema.h"
#include <memory>
#include <vector>

class VecSortNode : public VecPlanNode {
    public:
        VecSortNode(std::unique_ptr<VecPlanNode> child, std::vector<std::unique_ptr<Expr>> sort_exprs);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

    private:
        std::unique_ptr<VecPlanNode> child_;
        std::vector<std::unique_ptr<Expr>> sort_exprs_;
        std::vector<Row> flat_buffer_;
        int cursor_ = 0;
        bool materialized_ = false;
        DataChunk out_chunk_;

        void consumeAndSort();
        void fillChunk(int start, int count);
};
