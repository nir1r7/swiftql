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
    const int num_cols = schema.size();
    const int num_keys = static_cast<int>(order_by_.size());

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

    // PRECOMPUTED DECLARED KEYS, and why the buffered row is WIDER than the
    // schema.
    //
    // `sort_comparator::rowLess` calls `evaluate()` on every declared key of
    // BOTH sides of EVERY comparison, so a sort of n rows over k keys costs
    // O(k·n·log n) walks of an expression tree whose value is a function of the
    // row alone. That is not a small constant: `evaluate` dispatches through a
    // chain of `dynamic_cast`s, and an ORDER BY over an AGGREGATE — the shape
    // every `GROUP BY ... ORDER BY SUM(...)` produces — additionally rebuilds
    // `aggregateOutputName` and runs a linear `Schema::indexOf` on it, per
    // comparison. Measured on TPC-H sf0.01 before this change: q10 sorts 399
    // rows in 3614µs (45.6% of execution) and q3 sorts 138 rows in 1007µs
    // (25.8%) — row counts far too small for the sort itself to cost that.
    //
    // So each key is evaluated ONCE per buffered row, at materialization, and
    // parked in a slot appended AFTER the schema's columns: a buffer row is
    // `num_cols` real columns followed by `num_keys` key values. One vector, one
    // allocation, and the heap moves the keys with their row for free.
    //
    // Nothing below the declared keys changes. `tie_order`'s entries are all
    // schema indices (< num_cols), so the trailing slots are invisible to the
    // tie-break, and the tie-break is still the SHARED one: when every declared
    // key ties, this comparator hands both rows to `rowLess` with NO declared
    // keys, which falls straight through to `compareCanonical`. There is exactly
    // one implementation of the canonical order, and it is Volcano's.
    const std::vector<OrderByItem> no_declared_keys;
    auto less = [&](const Row& a, const Row& b) {
        for (int k = 0; k < num_keys; ++k) {
            // compareForSort and the `desc` rule, identical to rowLess's
            // declared-key loop — only the operands are already computed.
            const int c = compareForSort(a[num_cols + k], b[num_cols + k]);
            if (c != 0) return order_by_[k].desc ? c > 0 : c < 0;
        }
        return sort_comparator::rowLess(no_declared_keys, schema, tie_order, a, b);
    };

    // One buffer row: the schema's columns, then the declared keys evaluated
    // against those columns. `out` is reserved to its full width first, so the
    // `evaluate` calls cannot be reading a row that push_back is reallocating.
    auto materialize = [&](Row& out, const DataChunk& chunk, int r) {
        out.clear();   // keeps capacity when the row is the reused candidate
        out.reserve(static_cast<size_t>(num_cols + num_keys));
        for (int c = 0; c < num_cols; ++c) out.push_back(valueAt(chunk.columns[c], r));
        for (const auto& item : order_by_) {
            out.push_back(evaluate(item.expr.get(), out, schema));
        }
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
    // Child time EXCLUDED, per chunk. This node is a pipeline breaker, so the
    // old timer -- which wrapped this entire call from nextChunk() -- charged
    // every operator beneath it to the sort. On any ORDER BY query that made the
    // sort read ~100% of execution and hid the real hot node, which is exactly
    // what the README's "child time excluded" contract exists to prevent.
    while (true) {
        DataChunk* chunk = child_->nextChunk();
        if (!chunk) break;
        auto t0 = std::chrono::high_resolution_clock::now();

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

        // The heap is FULL and there are no declared keys, so a candidate can be
        // rejected by reading columns straight out of the chunk -- almost every
        // one loses on the first compared column, and building a Row for it is
        // pure waste. Same comparison, one implementation: compareCanonical is
        // what rowLess's tie-break loop is written in terms of.
        const bool lazy = row_cap_ > 0 && order_by_.empty();

        rows_seen_ += static_cast<int>(indices_ptr->size());
        for (int r : *indices_ptr) {
            const bool heap_full = row_cap_ > 0
                && static_cast<int>(flat_buffer_.size()) >= row_cap_;
            if (lazy && heap_full) {
                // `lazy` implies no declared keys, so a buffer row is exactly
                // the schema's columns and `worst` carries no key slots.
                const Row& worst = flat_buffer_.front();
                auto read = [&](int i) { return valueAt(chunk->columns[i], r); };
                if (sort_comparator::compareCanonical(tie_order, num_cols, read, worst) >= 0)
                    continue;
            }

            // Unbounded: build straight into its final home. Going through the
            // reusable candidate would copy every Value — strings included — a
            // second time, for a row that is being kept either way.
            if (row_cap_ <= 0) {
                flat_buffer_.emplace_back();
                materialize(flat_buffer_.back(), *chunk, r);
                continue;
            }

            materialize(candidate, *chunk, r);
            if (!heap_full) {
                flat_buffer_.push_back(candidate);
                std::push_heap(flat_buffer_.begin(), flat_buffer_.end(), less);
            } else if (lazy || less(candidate, flat_buffer_.front())) {
                std::pop_heap(flat_buffer_.begin(), flat_buffer_.end(), less);
                flat_buffer_.back() = candidate;
                std::push_heap(flat_buffer_.begin(), flat_buffer_.end(), less);
            }
        }
        stats.elapsed_us += std::chrono::duration<double, std::micro>(
            std::chrono::high_resolution_clock::now() - t0).count();
    }

    // The sort itself is this node's own work and is timed as such.
    auto t_sort = std::chrono::high_resolution_clock::now();
    if (row_cap_ > 0) {
        std::sort_heap(flat_buffer_.begin(), flat_buffer_.end(), less);
    } else {
        std::stable_sort(flat_buffer_.begin(), flat_buffer_.end(), less);
    }
    stats.elapsed_us += std::chrono::duration<double, std::micro>(
        std::chrono::high_resolution_clock::now() - t_sort).count();
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
        // consumeAndSort accumulates stats.elapsed_us itself, per chunk plus the
        // sort, and excludes the child call. Timing it from here would
        // re-include the whole subtree.
        consumeAndSort();
        // rows_in is the INPUT count, which the bounded top-N no longer keeps
        stats.rows_in  = rows_seen_;
        stats.rows_out = static_cast<int>(flat_buffer_.size());
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
