#include "vec_derived_node.h"
#include <chrono>
#include <stdexcept>

VecDerivedNode::VecDerivedNode(std::unique_ptr<VecPlanNode> child, std::string alias,
                               Schema schema)
    : child_(std::move(child)), alias_(std::move(alias)),
      output_schema_(std::move(schema)) {
    // The one thing that can be wrong here: a rename list of a different length
    // than the body produced. derivedRelationSchema already refuses that at plan
    // time, so this is the shape of a planner bug — checked anyway, because the
    // failure it prevents is every downstream indexOf resolving against a width
    // the chunks do not have.
    if (output_schema_.size() != child_->outputSchema().size()) {
        throw std::runtime_error(
            "internal: derived relation '" + alias_ + "' reports "
            + std::to_string(output_schema_.size()) + " columns over a child of "
            + std::to_string(child_->outputSchema().size()));
    }
}

void VecDerivedNode::open() { child_->open(); }

DataChunk* VecDerivedNode::nextChunk() {
    // Child time EXCLUDED — t0 starts after the child returns. README's
    // --explain-analyze contract is per-node *exclusive* self-time, and this
    // node used to start its timer above the call, charging the whole body's
    // execution to a node that does no work at all.
    DataChunk* chunk = child_->nextChunk();
    auto t0 = std::chrono::high_resolution_clock::now();
    // Rows in == rows out, always: this node selects nothing and computes
    // nothing. What it must NOT do is count the chunk's WIDTH when the body
    // applied a filter — `num_rows` is the buffer size and `sel.indices.size()`
    // is the surviving count, and a derived body with a WHERE reaches here with
    // filter_applied set. Counting num_rows over-reported both figures on
    // --explain-analyze for exactly the shape derived tables are used for. This
    // is the convention VecLimitNode and VecFilterNode already share; found by
    // reading them rather than by a failing test, because it is a reporting
    // defect and no assertion covers per-node row counts.
    if (chunk) {
        const int rows = chunk->filter_applied
            ? static_cast<int>(chunk->sel.indices.size())
            : chunk->num_rows;
        stats.rows_in += rows;
        stats.rows_out += rows;
    }
    stats.elapsed_us += std::chrono::duration<double, std::micro>(
        std::chrono::high_resolution_clock::now() - t0).count();
    return chunk;
}

void VecDerivedNode::close() { child_->close(); }

const Schema& VecDerivedNode::outputSchema() const { return output_schema_; }

std::string VecDerivedNode::explain() const {
    return "VecDerived [" + alias_ + ", "
         + std::to_string(output_schema_.size()) + " columns]";
}

std::vector<VecPlanNode*> VecDerivedNode::children() const { return {child_.get()}; }
