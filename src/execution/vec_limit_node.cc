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

    // late materialization: truncate the child's chunk in place and pass the
    // pointer through — safe because once the limit is hit this node never
    // pulls again, and every producer rebuilds its buffer per emission
    if (available <= remaining) {
        rows_emitted_ += available;
        stats.rows_in += available;
        stats.rows_out += available;
    } else {
        // final partial chunk: keep only the first `remaining` passing rows
        if (chunk->filter_applied) {
            // columns hold all physical rows; sel.indices are positions into them.
            // only truncate sel.indices — resizing columns would invalidate the indices.
            chunk->sel.indices.resize(remaining);
            chunk->sel.size = remaining;
        } else {
            chunk->num_rows = remaining;
            for (auto& cv : chunk->columns) {
                std::visit([&](auto& vec) { vec.resize(remaining); }, cv.data);
                // validity must shrink with the data or isNull() reads past the
                // truncation point (or out of bounds on the next parent read)
                if (!cv.all_valid) cv.validity.resize(remaining);
            }
        }
        rows_emitted_ = limit_;
        stats.rows_in += available;
        stats.rows_out += remaining;
    }

    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    return chunk;
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
