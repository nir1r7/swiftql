#pragma once
#include "planner/vec_plan_node.h"
#include "parser/ast.h"
#include "common/schema.h"
#include <memory>
#include <vector>

class VecSortNode : public VecPlanNode {
    public:
        // row_cap > 0 turns this into a bounded TOP-N: only the row_cap
        // smallest rows are kept. Set by the planner when the parent is a LIMIT
        // -- see consumeAndSort, and the deterministic cut in
        // planner/logical_plan.cc that made it necessary. 0 = sort everything.
        VecSortNode(std::unique_ptr<VecPlanNode> child, std::vector<OrderByItem> order_by,
                    int row_cap = 0);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

    private:
        std::unique_ptr<VecPlanNode> child_;
        std::vector<OrderByItem> order_by_;
        int row_cap_ = 0;
        std::vector<Row> flat_buffer_;
        int rows_seen_ = 0;
        int cursor_ = 0;
        bool materialized_ = false;
        DataChunk out_chunk_;

        void consumeAndSort();
        void fillChunk(int start, int count);
};
