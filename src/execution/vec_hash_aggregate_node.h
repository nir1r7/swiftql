#pragma once
#include "planner/vec_plan_node.h"
#include "planner/logical_plan.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class VecHashAggregateNode : public VecPlanNode {
    public:
        VecHashAggregateNode(std::unique_ptr<VecPlanNode> child, std::vector<std::string> group_by_cols, std::vector<AggregateSpec> specs, Schema output_schema);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

    private:
        std::unique_ptr<VecPlanNode> child_;
        std::vector<std::string> group_by_cols_;
        std::vector<AggregateSpec> specs_;
        Schema output_schema_;

        struct Accumulator {
            int64_t count = 0;
            std::vector<Value> group_vals;
            struct SpecAccum {
                int64_t non_null_count = 0;
                double sum = 0.0;
                Value min_val;
                Value max_val;
            };
            std::vector<SpecAccum> per_spec; // one entry per specs_ element
        };

        std::unordered_map<std::string, Accumulator> groups_;
        std::vector<std::string> group_order_; // insertion order
        std::vector<Row> result_rows_;
        int cursor_ = 0;
        bool materialized_ = false;
        DataChunk out_chunk_;

        void consumeAll();
        void materializeResults();
        void fillChunk(int start, int count);
};
