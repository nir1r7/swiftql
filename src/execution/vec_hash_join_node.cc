#include "execution/vec_hash_join_node.h"
#include <algorithm>
#include <chrono>
#include <numeric>

VecHashJoinNode::VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::vector<int> probe_key_indices, std::vector<int> build_key_indices, Schema output_schema, bool swapped) : probe_child_(std::move(probe_child)), build_child_(std::move(build_child)), probe_key_idx_(std::move(probe_key_indices)), build_key_idx_(std::move(build_key_indices)), output_schema_(std::move(output_schema)), swapped_(swapped) {}

namespace {

// Serialize one row's k-key tuple into `out`, reusing its capacity.
//
// The '\x01' sentinel goes after EVERY field, not between them: it is what stops
// ("ab","c") and ("a","bc") from producing identical bytes and colliding in one
// bucket. The single-key form appended exactly the same sentinel, so a one-key
// tuple hashes byte-identically to the pre-Week-27 encoding.
//
// Returns false when any key column is NULL. SQL's NULL equals nothing, so with
// k keys the rule composes: one NULL member makes the whole tuple unmatchable,
// on either side. Dropping such rows keeps them out of the hash table instead of
// bucketing them under toString()'s "NULL", which would make NULL = NULL match.
bool serializeKey(const DataChunk& chunk, const std::vector<int>& key_idx, int r, std::string& out) {
    out.clear();
    for (int c : key_idx) {
        // valueAt, never the typed vector directly: a raw read bypasses the
        // validity mask and turns a NULL into the placeholder underneath it
        Value v = valueAt(chunk.columns[c], r);
        if (v.isNull()) return false;
        out += v.toString();
        out += '\x01';
    }
    return true;
}

} // namespace

void VecHashJoinNode::open() {
    probe_child_->open();
    build_child_->open();

    hash_table_.clear();
    output_buffer_.clear();
    output_cursor_ = 0;

    // build phase: consume all build side chunks into hash table
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
            // serialize the key tuple with the same '\x01' sentinel Volcano's
            // HashJoinNode uses; a NULL member makes the row unmatchable
            if (!serializeKey(*chunk, build_key_idx_, r, key_buf_)) continue;

            // reconstruct full build-side Row and insert into hash table
            Row build_row;
            build_row.reserve(chunk->columns.size());
            for (const auto& cv : chunk->columns) {
                build_row.push_back(valueAt(cv, r));
            }
            hash_table_[key_buf_].push_back(std::move(build_row));
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

            if (!serializeKey(*probe_chunk, probe_key_idx_, r, key_buf_)) continue;   // NULL matches nothing (see open())

            auto it = hash_table_.find(key_buf_);
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
    // Names come from the children's schemas, so --explain still prints columns
    // rather than the integers this node holds. A one-key join renders exactly
    // the pre-Week-27 string.
    std::string s = "VecHashJoin [";
    for (size_t i = 0; i < probe_key_idx_.size(); ++i) {
        if (i) s += " AND ";
        s += probe_child_->outputSchema().column(probe_key_idx_[i]).name + " = "
           + build_child_->outputSchema().column(build_key_idx_[i]).name;
    }
    s += "] (materialize)";
    if (!cost_decision_.empty()) s += " " + cost_decision_;
    return s;
}

std::vector<VecPlanNode*> VecHashJoinNode::children() const {
    return {probe_child_.get(), build_child_.get()};
}
