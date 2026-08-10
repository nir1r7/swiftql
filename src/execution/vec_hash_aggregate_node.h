#pragma once
#include "planner/vec_plan_node.h"
#include "planner/logical_plan.h"
#include <cstdint>
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

        // How each output column must judge a value narrowing into a DOUBLE
        // ColumnVector. MIN/MAX over a mixed-type CASE is the route that reaches
        // it, because they keep the argument's own Value AND type: the INT
        // arrives here under a DOUBLE declaration (OBSERVABLE / RENDERED /
        // UNRENDERED), and an aggregate over INT ARITHMETIC over such a column
        // arrives as the DOUBLE of an exact integer instead (VOLCANO_INT).
        // vectorized_plan_builder.cc computes the mask, vec_types.h's three
        // refusals enforce it. Empty (the default) = every column RENDERED.
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

        // Accumulators live in a vector in FIRST-APPEARANCE ORDER and the map
        // holds only an index into it. That is what preserves group output order
        // now that there is no separate group_order_ list: the vector IS the
        // order, so emitting is a walk over it and no key string is stored twice.
        // (The old shape kept one std::string copy of every key in group_order_
        // on top of the map's own, and re-hashed each of them to emit.)
        std::vector<Accumulator> groups_;
        std::unordered_map<std::string, uint32_t> group_index_;

        // Reused across every input row so the per-row key costs no allocation:
        // clear() keeps the capacity. Same discipline as vec_hash_join_node's
        // key_buf_. key_vals_ only carries the evaluate() fallback path (see
        // consumeAll); the columnar path never materializes a Value per row.
        std::string key_buf_;
        std::vector<Value> key_vals_;

        std::vector<Row> result_rows_;
        int cursor_ = 0;
        bool materialized_ = false;
        DataChunk out_chunk_;

        void consumeAll();
        void materializeResults();
        void fillChunk(int start, int count);
};
