#include "execution/vec_sort_node.h"
#include "execution/evaluator.h"
#include "execution/sort_comparator.h"
#include "parser/expr_utils.h"
#include <algorithm>
#include <chrono>
#include <numeric>

VecSortNode::VecSortNode(std::unique_ptr<VecPlanNode> child, std::vector<OrderByItem> order_by, int row_cap) : child_(std::move(child)), order_by_(std::move(order_by)), row_cap_(row_cap) {}

void VecSortNode::open() {
    child_->open();
    materialized_ = false;
    cursor_ = 0;
    rows_seen_ = 0;
    flat_buffer_.clear();
}

void VecSortNode::consumeAndSort() {
    const Schema& schema = child_->outputSchema();

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
    auto less = [&](const Row& a, const Row& b) {
        return sort_comparator::rowLess(order_by_, schema, tie_order, a, b);
    };

    // BOUNDED TOP-N, when the parent is a LIMIT (row_cap_ > 0).
    //
    // A determined order is only needed for the rows that survive the cut, and
    // this node is the whole reason a cut can afford one. Unbounded, it
    // materializes the ENTIRE input and sorts it: the deterministic cut
    // (planner/logical_plan.cc) put a sort under a `LIMIT` that previously
    // streamed, and on the --no-optimize leg the input is the un-pushed join
    // product — 5,000,000 rows for a `driver_id` self-join over `laps`, measured
    // at 1.8s streaming and 74s sorted. `flat_buffer_` is therefore a MAX-heap of
    // the row_cap_ SMALLEST rows: O(row_cap_) memory instead of O(input), one
    // comparison against the heap's largest for the rows that lose, and no Row
    // allocation for them either (the candidate buffer is reused).
    //
    // The input must still be READ in full — the canonically smallest n rows
    // cannot be known before the last row is seen, and that is inherent to
    // determinism at a cut, not to this implementation.
    //
    // Ties are safe under the heap even though it is not a stable sort: the
    // tie-break compares EVERY column of the schema, so two rows that remain
    // tied are equal in every column and whichever the heap keeps renders the
    // same. That is the same property `sort_comparator.h` states for the cut.
    Row candidate;
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

        rows_seen_ += static_cast<int>(indices_ptr->size());
        for (int r : *indices_ptr) {
            candidate.clear();   // keeps capacity: no per-row reallocation
            candidate.reserve(chunk->columns.size());
            for (const auto& cv : chunk->columns) {
                candidate.push_back(valueAt(cv, r));
            }
            if (row_cap_ <= 0) {
                flat_buffer_.push_back(candidate);
            } else if (static_cast<int>(flat_buffer_.size()) < row_cap_) {
                flat_buffer_.push_back(candidate);
                std::push_heap(flat_buffer_.begin(), flat_buffer_.end(), less);
            } else if (less(candidate, flat_buffer_.front())) {
                std::pop_heap(flat_buffer_.begin(), flat_buffer_.end(), less);
                flat_buffer_.back() = candidate;
                std::push_heap(flat_buffer_.begin(), flat_buffer_.end(), less);
            }
        }
    }

    if (row_cap_ > 0) {
        std::sort_heap(flat_buffer_.begin(), flat_buffer_.end(), less);
    } else {
        std::stable_sort(flat_buffer_.begin(), flat_buffer_.end(), less);
    }
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
        // rows_in is the INPUT count, which the bounded top-N no longer keeps
        stats.rows_in  = rows_seen_;
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
