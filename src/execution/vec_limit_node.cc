#include "execution/vec_limit_node.h"
#include <chrono>

VecLimitNode::VecLimitNode(std::unique_ptr<VecPlanNode> child, int limit) : child_(std::move(child)), limit_(limit) {}

void VecLimitNode::open() {
    child_->open();
    rows_emitted_ = 0;
}

DataChunk* VecLimitNode::nextChunk() {
    if (rows_emitted_ >= limit_) return nullptr;

    DataChunk* chunk = child_->nextChunk();
    if (!chunk) return nullptr;

    auto t0 = std::chrono::high_resolution_clock::now();

    // when filter_applied, passing row count is sel.indices.size(), not num_rows
    int available = chunk->filter_applied
        ? static_cast<int>(chunk->sel.indices.size())
        : chunk->num_rows;
    int remaining = limit_ - rows_emitted_;

    // copy child's chunk into our owned buffer, child reuses its buffer on next call
    out_chunk_ = *chunk;

    if (available <= remaining) {
        rows_emitted_ += available;
        stats.rows_in += available;
        stats.rows_out += available;
    } else {
        // final partial chunk: keep only the first `remaining` passing rows
        if (out_chunk_.filter_applied) {
            // columns hold all physical rows; sel.indices are positions into them.
            // only truncate sel.indices — resizing columns would invalidate the indices.
            out_chunk_.sel.indices.resize(remaining);
            out_chunk_.sel.size = remaining;
        } else {
            out_chunk_.num_rows = remaining;
            for (auto& cv : out_chunk_.columns) {
                std::visit([&](auto& vec) { vec.resize(remaining); }, cv.data);
            }
        }
        rows_emitted_ = limit_;
        stats.rows_in += available;
        stats.rows_out += remaining;
    }

    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    return &out_chunk_;
}

void VecLimitNode::close() {
    child_->close();
}

const Schema& VecLimitNode::outputSchema() const {
    return child_->outputSchema();
}

std::string VecLimitNode::explain() const {
    return "VecLimit [" + std::to_string(limit_) + "]";
}

std::vector<VecPlanNode*> VecLimitNode::children() const {
    return {child_.get()};
}
