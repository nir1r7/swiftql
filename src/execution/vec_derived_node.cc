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
    auto t0 = std::chrono::high_resolution_clock::now();
    DataChunk* chunk = child_->nextChunk();
    // Rows in == rows out, always: this node selects nothing and computes
    // nothing. Reporting both keeps --explain-analyze's per-node accounting
    // honest rather than showing a node with no rows.
    if (chunk) {
        stats.rows_in += chunk->num_rows;
        stats.rows_out += chunk->num_rows;
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
