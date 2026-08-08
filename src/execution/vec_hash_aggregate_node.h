#pragma once
#include "planner/vec_plan_node.h"
#include "planner/logical_plan.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class VecHashAggregateNode : public VecPlanNode {
    public:
        VecHashAggregateNode(std::unique_ptr<VecPlanNode> child, std::vector<GroupByColumn> group_by_cols, std::vector<AggregateSpec> specs, Schema output_schema);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

        // Output columns whose INT -> DOUBLE narrowing is observable to a
        // division above this node; MIN/MAX over a mixed-type CASE is the route
        // that reaches it (MIN/MAX keep the argument's own Value and type).
        // vectorized_plan_builder.cc computes the mask, vec_types.h's
        // refuseObservableIntNarrowing enforces it. Empty (the default) = none.
        void setIntNarrowingColumns(std::vector<IntNarrowing> mask) {
            int_narrowing_ = std::move(mask);
        }

    private:
        std::unique_ptr<VecPlanNode> child_;
        std::vector<GroupByColumn> group_by_cols_;
        std::vector<AggregateSpec> specs_;
        Schema output_schema_;
        std::vector<IntNarrowing> int_narrowing_;   // parallel to output_schema_ when non-empty

        struct Accumulator {
            int64_t count = 0;
            std::vector<Value> group_vals;
            struct SpecAccum {
                int64_t non_null_count = 0;
                double sum = 0.0;
                Value min_val;
                Value max_val;
                // Week 34 — COUNT(DISTINCT x). Populated only for a distinct
                // spec. Keyed through appendGroupKeyField (key_encoding.h) and
                // never Value::toString(): %.15g is lossy and collapsed 3245
                // distinct doubles into 2526 texts in Week 27, in all four
                // modes. The Volcano HashAggregateNode holds the identical
                // field and runs the identical rule — this feature is one of the
                // few Week 34 ships on ALL FOUR modes, so the two must agree.
                std::unordered_set<std::string> distinct_keys;
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
