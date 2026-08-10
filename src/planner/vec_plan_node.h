#pragma once

#include "common/schema.h"
#include "execution/bloom_filter.h"
#include "execution/vec_types.h"
#include <memory>
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

        // WEEK 38 — SIDEWAYS INFORMATION PASSING. A hash join offers the probe
        // pipeline a Bloom filter over its build keys; a node that can use it
        // drops rows that CANNOT match before they reach the join. The default
        // is a no-op, so an unsupported probe shape silently DECLINES rather
        // than breaking — declining costs a filter, applying one wrongly costs
        // an answer.
        //
        // KEYS ARRIVE AS COLUMN INDICES, not names, for the reason
        // VecHashJoinNode's own keys did in Week 27: a schema can hold the same
        // name at several relation slots, and a VecDerivedNode RENAMES its
        // child's columns positionally — so a name resolved again further down
        // can land on a different column than the join meant. An index is exact
        // wherever the chain is positionally aligned, which is precisely the set
        // of nodes that forward this call: VecFilterNode and VecDerivedNode both
        // pass their child's chunk through untouched, so column i is the same
        // ColumnVector, of the same type, at every level. Everything else keeps
        // the no-op — including VecLimitNode, deliberately: dropping rows BELOW
        // a LIMIT changes WHICH rows the limit passes, so a row that would have
        // been cut by the limit takes the place of one that was, and the join
        // gains output rows. That is a wrong answer, not a lost optimization.
        //
        // A NULL `filter` is the ARMING call, made at plan-build time so that
        // plain --explain (which never executes) can report that the pushdown is
        // in place. The real filter follows at open() time, after the build side
        // has been consumed. A node that stores the arming call must not act on
        // it — there is nothing to test against until the filter arrives.
        virtual void pushBloomFilter(const std::vector<int>& /*key_indices*/,
                                     std::shared_ptr<const BloomFilter> /*filter*/) {}
};