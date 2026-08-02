#pragma once

#include "common/schema.h"
#include "execution/vec_types.h"
#include <vector>
#include <string>

struct VecNodeStats {
    int64_t rows_in = 0;
    int64_t rows_out = 0;
    double elapsed_us = 0.0;
};

class VecPlanNode {
    public:
        VecNodeStats stats;
        // estimated output rows carried across lowering from the logical node
        // (Week 20 CardinalityEstimator); -1 = estimator never ran (--no-optimize)
        double estimated_rows = -1.0;

        virtual ~VecPlanNode() = default;

        virtual void open() = 0;
        // returns a pointer to operator's internal chunk
        // reused on next call — any reference into the returned chunk's data
        // (e.g. const auto& v = chunk->columns[i].data) is invalidated on the next call
        virtual DataChunk* nextChunk() = 0;
        virtual void close() = 0;

        virtual const Schema& outputSchema() const = 0;
        virtual std::string explain() const = 0;
        virtual std::vector<VecPlanNode*> children() const = 0;
};