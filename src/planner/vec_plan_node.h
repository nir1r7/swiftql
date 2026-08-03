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
        // returns a pointer to a chunk buffer, reused on the next call — any
        // reference into the returned chunk's data (e.g. const auto& v =
        // chunk->columns[i].data) is invalidated on the next call. The pointer
        // may alias a DESCENDANT's buffer: VecFilter stamps sel/filter_applied
        // onto its child's chunk and VecLimit truncates it in place, both
        // passing the pointer through without copying (late materialization).
        // Consequences for implementers: a direct parent may mutate sel,
        // filter_applied, and (limit only) truncate data; nothing survives the
        // next nextChunk() call; and every chunk producer must fully reset all
        // chunk fields (columns, num_rows, sel, filter_applied) per emission.
        virtual DataChunk* nextChunk() = 0;
        virtual void close() = 0;

        virtual const Schema& outputSchema() const = 0;
        virtual std::string explain() const = 0;
        virtual std::vector<VecPlanNode*> children() const = 0;
};