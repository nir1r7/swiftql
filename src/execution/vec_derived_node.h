#pragma once
#include "planner/vec_plan_node.h"
#include <memory>
#include <string>
#include <vector>

// Week 34 — the physical side of a derived table, and it computes NOTHING.
//
// A derived relation is a naming and slot-normalization artifact, not a
// computation: its rows are exactly its body's rows. So this node forwards every
// chunk through unchanged and differs from its child in one respect only — the
// SCHEMA it reports. That schema carries the `AS d (a, b)` column renames and
// the slot-0 normalization (derivedRelationSchema), and it has to reach the
// physical layer because resolveColumnIndex (evaluator.cc) and every
// indexOf(name, slot) above the graft look the new names up.
//
// Why a node rather than swapping the child's schema in place: a VecScanNode's
// schema is also its scan-pruning input and is shared with the ColumnarTable it
// moved in, and a VecProjectNode's is the contract its executor was compiled
// against. Renaming either in place would edit an object another operator is
// still reading. One forwarding node costs one virtual call per chunk and no
// copies — the derived table's whole physical cost.
class VecDerivedNode : public VecPlanNode {
    public:
        VecDerivedNode(std::unique_ptr<VecPlanNode> child, std::string alias, Schema schema);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

        // Week 38 — forwarded verbatim, and INDICES are what makes that safe.
        // This node forwards its child's chunk untouched and differs only in the
        // NAMES its schema reports, so column i is the same ColumnVector on both
        // sides while the name at i may not be the same name. The constructor
        // already refuses a width mismatch, so the index is in range below too.
        void pushBloomFilter(const std::vector<int>& key_indices,
                             std::shared_ptr<const BloomFilter> filter) override {
            child_->pushBloomFilter(key_indices, std::move(filter));
        }

    private:
        std::unique_ptr<VecPlanNode> child_;
        std::string alias_;
        Schema output_schema_;
};
