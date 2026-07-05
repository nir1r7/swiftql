#pragma once
#include "planner/vec_plan_node.h"
#include <memory>

class VecLimitNode : public VecPlanNode {
public:
    VecLimitNode(std::unique_ptr<VecPlanNode> child, int limit);

    void open() override;
    DataChunk* nextChunk() override;
    void close() override;
    const Schema& outputSchema() const override;
    std::string explain() const override;
    std::vector<VecPlanNode*> children() const override;

private:
    std::unique_ptr<VecPlanNode> child_;
    int limit_;
    int rows_emitted_ = 0;
    DataChunk out_chunk_;
};
