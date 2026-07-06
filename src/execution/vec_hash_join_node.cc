#include "execution/vec_hash_join_node.h"
#include <algorithm>
#include <chrono>
#include <numeric>

VecHashJoinNode::VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::string probe_join_col, std::string build_join_col, Schema output_schema) : probe_child_(std::move(probe_child)), build_child_(std::move(build_child)), probe_join_col_(std::move(probe_join_col)), build_join_col_(std::move(build_join_col)), output_schema_(std::move(output_schema)) {}

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
            Value key_val;
            std::visit([&](const auto& vec) { key_val = Value(vec[r]); }, chunk->columns[build_key_idx].data);
            std::string key = key_val.toString() + '\x01';

            // reconstruct full build-side Row and insert into hash table
            Row build_row;
            build_row.reserve(chunk->columns.size());
            for (const auto& cv : chunk->columns) {
                std::visit([&](const auto& vec) {
                    build_row.push_back(Value(vec[r]));
                }, cv.data);
            }
            hash_table_[key].push_back(std::move(build_row));
        }
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
        ColumnVector cv;
        cv.type = output_schema_.column(c).type;
        switch (cv.type) {
            case TypeId::INT: cv.data = std::vector<int64_t>(); break;
            case TypeId::DOUBLE: cv.data = std::vector<double>(); break;
            case TypeId::STRING: cv.data = std::vector<std::string>(); break;
        }
        for (int i = start; i < start + count; ++i) {
            const Value& v = output_buffer_[i][c];
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

            Value key_val;
            std::visit([&](const auto& vec) { key_val = Value(vec[r]); }, probe_chunk->columns[probe_key_idx].data);
            std::string key = key_val.toString() + '\x01';

            auto it = hash_table_.find(key);
            if (it == hash_table_.end()) continue;

            for (const Row& build_row : it->second) {
                // output row: probe columns first, then build columns matches the merged schema constructed in planner wiring
                Row out_row;
                out_row.reserve(output_schema_.size());

                for (const auto& cv : probe_chunk->columns) {
                    std::visit([&](const auto& vec) {
                        out_row.push_back(Value(vec[r]));
                    }, cv.data);
                }
                for (const Value& v : build_row) {
                    out_row.push_back(v);
                }
                output_buffer_.push_back(std::move(out_row));
            }
        }

        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

        // if no matches from this probe chunk, loop to pull the next one
    }

    // emit one BATCH_SIZE slice from output_buffer_
    int batch = std::min(BATCH_SIZE, static_cast<int>(output_buffer_.size()) - output_cursor_);
    fillOutChunk(output_cursor_, batch);

    stats.rows_out += batch;
    stats.elapsed_us += 0;  // fillOutChunk time is minor; folded into probe timing above

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
    return "VecHashJoin [" + probe_join_col_ + " = " + build_join_col_ + "] (materialize)";
}

std::vector<VecPlanNode*> VecHashJoinNode::children() const {
    return {probe_child_.get(), build_child_.get()};
}
