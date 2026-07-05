#pragma once
#include "planner/vec_plan_node.h"
#include <memory>
#include <vector>
#include <unordered_set>
#include <string>

class VecDistinctNode : public VecPlanNode {
    public:
        explicit VecDistinctNode(std::unique_ptr<VecPlanNode> child);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

    private:
        std::unique_ptr<VecPlanNode> child_;
        std::vector<Row> dedup_buffer_;
        int cursor_ = 0;
        bool materialized_ = false;
        DataChunk out_chunk_;

        int consumeAndDedup(); // returns total rows consumed (before dedup)
        void fillChunk(int start, int count);
};
