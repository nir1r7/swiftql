#include "execution/vec_distinct_node.h"
#include "execution/key_encoding.h"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <unordered_set>

VecDistinctNode::VecDistinctNode(std::unique_ptr<VecPlanNode> child) : child_(std::move(child)) {}

void VecDistinctNode::open() {
    child_->open();
    materialized_ = false;
    cursor_ = 0;
    dedup_buffer_.clear();
}

int VecDistinctNode::consumeAndDedup() {
    int rows_consumed = 0;
    std::unordered_set<std::string> seen;

    // Child time EXCLUDED, per chunk. The timer used to sit in nextChunk()
    // around this whole call, so a blocking node reported its entire subtree as
    // its own self-time -- see the same fix in vec_sort_node.cc.
    while (true) {
        DataChunk* chunk = child_->nextChunk();
        if (!chunk) break;
        auto t0 = std::chrono::high_resolution_clock::now();

        // determine valid row indices, handles both filtered and unfiltered chunks
        const std::vector<int>* indices_ptr = nullptr;
        std::vector<int> all_indices;
        if (chunk->filter_applied) {
            indices_ptr = &chunk->sel.indices;
        }
        else {
            all_indices.resize(chunk->num_rows);
            std::iota(all_indices.begin(), all_indices.end(), 0);
            indices_ptr = &all_indices;
        }

        rows_consumed += static_cast<int>(indices_ptr->size());
        for (int r : *indices_ptr) {
            // Build the dedup key from NULL-aware reads — valueAt, never the
            // typed vector, or a NULL reads back as the placeholder under it —
            // through the shared encoding (key_encoding.h).
            std::string key;
            for (const auto& cv : chunk->columns) {
                appendGroupKeyField(key, valueAt(cv, r));
            }

            if (seen.insert(key).second) {
                Row row;
                row.reserve(chunk->columns.size());
                for (const auto& cv : chunk->columns) {
                    row.push_back(valueAt(cv, r));
                }
                dedup_buffer_.push_back(std::move(row));
            }
        }
        stats.elapsed_us += std::chrono::duration<double, std::micro>(
            std::chrono::high_resolution_clock::now() - t0).count();
    }
    return rows_consumed;
}

void VecDistinctNode::fillChunk(int start, int count) {
    const Schema& schema = child_->outputSchema();
    out_chunk_.columns.clear();
    out_chunk_.num_rows = count;
    out_chunk_.filter_applied = false;
    out_chunk_.sel.indices.clear();
    out_chunk_.sel.size = 0;

    for (int c = 0; c < schema.size(); ++c) {
        ColumnVector cv = makeColumnVector(schema.column(c).type);
        for (int i = start; i < start + count; ++i) {
            appendColumnValue(cv, dedup_buffer_[i][c]);
        }
        out_chunk_.columns.push_back(std::move(cv));
    }
}

DataChunk* VecDistinctNode::nextChunk() {
    if (!materialized_) {
        // consumeAndDedup accumulates stats.elapsed_us itself, per chunk and
        // excluding the child call. Timing it from here would re-include the
        // whole subtree, which is the defect this replaced.
        stats.rows_in  = consumeAndDedup();
        stats.rows_out = static_cast<int>(dedup_buffer_.size());
        materialized_ = true;
    }

    if (cursor_ >= static_cast<int>(dedup_buffer_.size())) return nullptr;

    int batch = std::min(BATCH_SIZE, static_cast<int>(dedup_buffer_.size()) - cursor_);
    fillChunk(cursor_, batch);
    cursor_ += batch;
    return &out_chunk_;
}

void VecDistinctNode::close() {
    child_->close();
}

const Schema& VecDistinctNode::outputSchema() const {
    return child_->outputSchema();
}

std::string VecDistinctNode::explain() const {
    return "VecDistinct (materialize)";
}

std::vector<VecPlanNode*> VecDistinctNode::children() const {
    return {child_.get()};
}
