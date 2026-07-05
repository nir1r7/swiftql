#include "execution/vec_distinct_node.h"
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

    while (DataChunk* chunk = child_->nextChunk()) {
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
            // serialize row, same separator as Volcano DistinctNode ('\x01')
            std::string key;
            for (const auto& cv : chunk->columns) {
                std::visit([&](const auto& vec) {
                    key += Value(vec[r]).toString();
                    key += '\x01';
                }, cv.data);
            }

            if (seen.insert(key).second) {
                Row row;
                row.reserve(chunk->columns.size());
                for (const auto& cv : chunk->columns) {
                    std::visit([&](const auto& vec) {
                        row.push_back(Value(vec[r]));
                    }, cv.data);
                }
                dedup_buffer_.push_back(std::move(row));
            }
        }
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
        ColumnVector cv;
        cv.type = schema.column(c).type;
        switch (cv.type) {
            case TypeId::INT: cv.data = std::vector<int64_t>(); break;
            case TypeId::DOUBLE: cv.data = std::vector<double>(); break;
            case TypeId::STRING: cv.data = std::vector<std::string>(); break;
        }
        for (int i = start; i < start + count; ++i) {
            const Value& v = dedup_buffer_[i][c];
            switch (cv.type) {
                case TypeId::INT:
                    std::get<std::vector<int64_t>>(cv.data).push_back(v.asInt()); break;
                case TypeId::DOUBLE:
                    std::get<std::vector<double>>(cv.data).push_back(v.asDouble()); break;
                case TypeId::STRING:
                    std::get<std::vector<std::string>>(cv.data).push_back(v.asString()); break;
            }
        }
        out_chunk_.columns.push_back(std::move(cv));
    }
}

DataChunk* VecDistinctNode::nextChunk() {
    if (!materialized_) {
        auto t0 = std::chrono::high_resolution_clock::now();
        stats.rows_in  = consumeAndDedup();
        stats.rows_out = static_cast<int>(dedup_buffer_.size());
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
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
    return "VecDistinct";
}

std::vector<VecPlanNode*> VecDistinctNode::children() const {
    return {child_.get()};
}
