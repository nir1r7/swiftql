#include "execution/vec_sort_node.h"
#include "execution/evaluator.h"
#include "execution/sort_comparator.h"
#include "parser/expr_utils.h"
#include <algorithm>
#include <chrono>
#include <numeric>

VecSortNode::VecSortNode(std::unique_ptr<VecPlanNode> child, std::vector<OrderByItem> order_by) : child_(std::move(child)), order_by_(std::move(order_by)) {}

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
                row.push_back(valueAt(cv, r));
            }
            flat_buffer_.push_back(std::move(row));
        }
    }

    // ONE comparator, shared with Volcano's SortNode — including the
    // deterministic tie-break below the declared keys, which is what makes the
    // row that survives a LIMIT cut independent of the plan shape (and so of the
    // build side this engine chooses from cardinality estimates while Volcano
    // chooses it from raw row counts). See execution/sort_comparator.h.
    //
    // The canonical column order is computed ONCE here, not inside the
    // comparator — and it is what makes the tie-break independent of the column
    // ORDER of the merged join schema, which JoinEnumeration permutes.
    const std::vector<int> tie_order = sort_comparator::tieBreakOrder(schema);
    std::stable_sort(flat_buffer_.begin(), flat_buffer_.end(), [&](const Row& a, const Row& b) {
            return sort_comparator::rowLess(order_by_, schema, tie_order, a, b);
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
        ColumnVector cv = makeColumnVector(schema.column(c).type);
        for (int i = start; i < start + count; ++i) {
            appendColumnValue(cv, flat_buffer_[i][c]);
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
    if (order_by_.empty()) return "VecSort [canonical row order] (materialize)";
    std::string s = "VecSort [";
    for (size_t i = 0; i < order_by_.size(); ++i) {
        if (i) s += ", ";
        s += exprToString(order_by_[i].expr.get());
        if (order_by_[i].desc) s += " DESC";
    }
    return s + "] (materialize)";
}

std::vector<VecPlanNode*> VecSortNode::children() const {
    return {child_.get()};
}
