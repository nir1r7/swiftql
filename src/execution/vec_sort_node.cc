#include "execution/vec_sort_node.h"
#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include <algorithm>
#include <chrono>
#include <numeric>

VecSortNode::VecSortNode(std::unique_ptr<VecPlanNode> child, std::vector<std::unique_ptr<Expr>> sort_exprs) : child_(std::move(child)), sort_exprs_(std::move(sort_exprs)) {}

void VecSortNode::open() {
    child_->open();
    materialized_ = false;
    cursor_ = 0;
    flat_buffer_.clear();
}

void VecSortNode::consumeAndSort() {
    const Schema& schema = child_->outputSchema();

    while (DataChunk* chunk = child_->nextChunk()) {
        // determine valid row indices, same pattern as VecProjectNode
        const std::vector<int>* indices_ptr = nullptr;
        std::vector<int> all_indices;
        if (chunk->filter_applied) {
            indices_ptr = &chunk->sel.indices;
        } else {
            all_indices.resize(chunk->num_rows);
            std::iota(all_indices.begin(), all_indices.end(), 0);
            indices_ptr = &all_indices;
        }

        for (int r : *indices_ptr) {
            Row row;
            row.reserve(chunk->columns.size());
            for (const auto& cv : chunk->columns) {
                std::visit([&](const auto& vec) {
                    row.push_back(Value(vec[r]));
                }, cv.data);
            }
            flat_buffer_.push_back(std::move(row));
        }
    }

    // sort ascending on each sort expression, matches Volcano SortNode exactly
    std::sort(flat_buffer_.begin(), flat_buffer_.end(), [&](const Row& a, const Row& b) {
            for (const auto& expr : sort_exprs_) {
                Value va = evaluate(expr.get(), a, schema);
                Value vb = evaluate(expr.get(), b, schema);
                if (va < vb) return true;
                if (vb < va) return false;
            }
            return false;
        });
}

void VecSortNode::fillChunk(int start, int count) {
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
            case TypeId::DOUBLE: cv.data = std::vector<double>();  break;
            case TypeId::STRING: cv.data = std::vector<std::string>(); break;
        }
        for (int i = start; i < start + count; ++i) {
            const Value& v = flat_buffer_[i][c];
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

DataChunk* VecSortNode::nextChunk() {
    if (!materialized_) {
        auto t0 = std::chrono::high_resolution_clock::now();
        consumeAndSort();
        stats.rows_in  = static_cast<int>(flat_buffer_.size());
        stats.rows_out = static_cast<int>(flat_buffer_.size());
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
        materialized_ = true;
    }

    if (cursor_ >= static_cast<int>(flat_buffer_.size())) return nullptr;

    int batch = std::min(BATCH_SIZE, static_cast<int>(flat_buffer_.size()) - cursor_);
    fillChunk(cursor_, batch);
    cursor_ += batch;
    return &out_chunk_;
}

void VecSortNode::close() {
    child_->close();
}

const Schema& VecSortNode::outputSchema() const {
    return child_->outputSchema();
}

std::string VecSortNode::explain() const {
    std::string s = "VecSort [";
    for (size_t i = 0; i < sort_exprs_.size(); ++i) {
        if (i) s += ", ";
        s += exprToString(sort_exprs_[i].get());
    }
    return s + "]";
}

std::vector<VecPlanNode*> VecSortNode::children() const {
    return {child_.get()};
}
