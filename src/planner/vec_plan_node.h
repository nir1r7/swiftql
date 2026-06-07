#pragma once

#include "common/schema.h"
#include "execution/vec_types.h"
#include <vector>
#include <string>

struct VecNodeStats {
    int rows_in = 0;
    int rows_out = 0;
    double elapsed_us = 0.0;
};

class VecPlanNode {
    public:
        VecNodeStats stats;

        virtual ~VecPlanNode() = default;
        // returns a pointer to operator's internal chunk
        // reused on next call
        virtual DataChunk* nextChunk() = 0;
        virtual void close() = 0;

        virtual const Schema& outputSchema() const = 0;
        virtual std::string explain() const = 0;
        virtual std::vector<VecPlanNode*> children() const = 0;
};