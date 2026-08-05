#include "execution/vec_hash_join_node.h"
#include <algorithm>
#include <chrono>
#include <numeric>

VecHashJoinNode::VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::string probe_join_col, std::string build_join_col, Schema output_schema, bool swapped) : probe_child_(std::move(probe_child)), build_child_(std::move(build_child)), probe_join_col_(std::move(probe_join_col)), build_join_col_(std::move(build_join_col)), output_schema_(std::move(output_schema)), swapped_(swapped) {}

void VecHashJoinNode::open() {
    probe_child_->open();
    build_child_->open();

    hash_table_.clear();
    output_buffer_.clear();
    output_cursor_ = 0;

    // build phase: consume all build side chunks into hash table
    const Schema& build_schema = build_child_->outputSchema();
    int build_key_idx = build_schema.indexOf(build_join_col_); // by name, never positional

    while (DataChunk* chunk = build_child_->nextChunk()) {
        // build work is self-time; the child pull above is excluded, matching
        // the per-chunk timing pattern of the other pipeline breakers
        auto t0 = std::chrono::high_resolution_clock::now();
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
            // extract join key value, serialize with same '\x01' sentinel as Volcano HashJoinNode
            Value key_val = valueAt(chunk->columns[build_key_idx], r);
            // SQL: NULL never equals anything, so a NULL key can never match a
            // probe row. Dropping it here keeps it out of the hash table
            // instead of letting toString() bucket it under "NULL".
            if (key_val.isNull()) continue;
            std::string key = key_val.toString() + '\x01';

            // reconstruct full build-side Row and insert into hash table
            Row build_row;
            build_row.reserve(chunk->columns.size());
            for (const auto& cv : chunk->columns) {
                build_row.push_back(valueAt(cv, r));
            }
            hash_table_[key].push_back(std::move(build_row));
        }
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    build_child_->close();
}

void VecHashJoinNode::fillOutChunk(int start, int count) {
    out_chunk_.columns.clear();
    out_chunk_.num_rows = count;
    out_chunk_.filter_applied = false;
    out_chunk_.sel.indices.clear();
    out_chunk_.sel.size = 0;

    for (int c = 0; c < output_schema_.size(); ++c) {
        ColumnVector cv = makeColumnVector(output_schema_.column(c).type);
        for (int i = start; i < start + count; ++i) {
            appendColumnValue(cv, output_buffer_[i][c]);
        }
        out_chunk_.columns.push_back(std::move(cv));
    }
}

DataChunk* VecHashJoinNode::nextChunk() {
    while (output_cursor_ >= static_cast<int>(output_buffer_.size())) {
        // current output buffer exhausted, pull next probe chunk
        output_buffer_.clear();
        output_cursor_ = 0;

        DataChunk* probe_chunk = probe_child_->nextChunk();
        if (!probe_chunk) return nullptr; // probe side exhausted

        auto t0 = std::chrono::high_resolution_clock::now();

        const Schema& probe_schema = probe_child_->outputSchema();
        int probe_key_idx = probe_schema.indexOf(probe_join_col_); // by name, never positional

        // determine valid probe row indices
        const std::vector<int>* indices_ptr = nullptr;
        std::vector<int> all_indices;
        if (probe_chunk->filter_applied) {
            indices_ptr = &probe_chunk->sel.indices;
        } else {
            all_indices.resize(probe_chunk->num_rows);
            std::iota(all_indices.begin(), all_indices.end(), 0);
            indices_ptr = &all_indices;
        }

        for (int r : *indices_ptr) {
            stats.rows_in++;

            Value key_val = valueAt(probe_chunk->columns[probe_key_idx], r);
            if (key_val.isNull()) continue;   // NULL matches nothing (see open())
            std::string key = key_val.toString() + '\x01';

            auto it = hash_table_.find(key);
            if (it == hash_table_.end()) continue;

            for (const Row& build_row : it->second) {
                // output row order always matches output_schema_'s fixed
                // [FROM, JOIN] logical order — reorder here when the FROM
                // table ended up on the build side (swapped_).
                Row out_row;
                out_row.reserve(output_schema_.size());

                auto append_probe = [&]() {
                    for (const auto& cv : probe_chunk->columns) {
                        out_row.push_back(valueAt(cv, r));
                    }
                };
                auto append_build = [&]() {
                    for (const Value& v : build_row) out_row.push_back(v);
                };

                if (swapped_) {
                    append_build();
                    append_probe();
                } else {
                    append_probe();
                    append_build();
                }
                output_buffer_.push_back(std::move(out_row));
            }
        }

        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

        // if no matches from this probe chunk, loop to pull the next one
    }

    // emit one BATCH_SIZE slice from output_buffer_; materialization is real
    // work and counts toward this node's time
    int batch = std::min(BATCH_SIZE, static_cast<int>(output_buffer_.size()) - output_cursor_);
    auto t0 = std::chrono::high_resolution_clock::now();
    fillOutChunk(output_cursor_, batch);
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    stats.rows_out += batch;
    output_cursor_ += batch;
    return &out_chunk_;
}

void VecHashJoinNode::close() {
    probe_child_->close();
    hash_table_.clear();
}

const Schema& VecHashJoinNode::outputSchema() const {
    return output_schema_;
}

std::string VecHashJoinNode::explain() const {
    std::string s = "VecHashJoin [" + probe_join_col_ + " = " + build_join_col_ + "] (materialize)";
    if (!cost_decision_.empty()) s += " " + cost_decision_;
    return s;
}

std::vector<VecPlanNode*> VecHashJoinNode::children() const {
    return {probe_child_.get(), build_child_.get()};
}
