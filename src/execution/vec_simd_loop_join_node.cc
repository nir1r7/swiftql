#include "execution/vec_simd_loop_join_node.h"
#include <algorithm>
#include <chrono>
#include <numeric>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

VecSimdLoopJoinNode::VecSimdLoopJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::string probe_join_col, std::string build_join_col, Schema output_schema, bool swapped, bool use_simd) : probe_child_(std::move(probe_child)), build_child_(std::move(build_child)), probe_join_col_(std::move(probe_join_col)), build_join_col_(std::move(build_join_col)), output_schema_(std::move(output_schema)), swapped_(swapped), use_simd_(use_simd) {}

void VecSimdLoopJoinNode::open() {
    probe_child_->open();
    build_child_->open();

    build_keys_.clear();
    build_rows_.clear();
    output_buffer_.clear();
    output_cursor_ = 0;

    // build phase: consume all build side chunks into the flat key buffer +
    // parallel payload rows
    const Schema& build_schema = build_child_->outputSchema();
    int build_key_idx = build_schema.indexOf(build_join_col_); // by name, never positional

    while (DataChunk* chunk = build_child_->nextChunk()) {
        // build work is self-time; the child pull above is excluded, matching
        // the per-chunk timing pattern of the other pipeline breakers
        auto t0 = std::chrono::high_resolution_clock::now();
        // sel.indices is authoritative when filter_applied (vec_types.h)
        const std::vector<int>* indices_ptr = nullptr;
        std::vector<int> all_indices;
        if (chunk->filter_applied) {
            indices_ptr = &chunk->sel.indices;
        } else {
            all_indices.resize(chunk->num_rows);
            std::iota(all_indices.begin(), all_indices.end(), 0);
            indices_ptr = &all_indices;
        }

        // the builder guarantees an INT key column; a non-INT key here is a
        // planning bug, so let std::get's bad_variant_access bubble
        const ColumnVector& key_col = chunk->columns[build_key_idx];
        const auto& keys = std::get<std::vector<int64_t>>(key_col.data);
        for (int r : *indices_ptr) {
            // SQL: NULL never equals anything, so a NULL key can never match.
            // Keeping it out of build_keys_ also keeps the flat SIMD key buffer
            // free of the placeholder underneath a NULL.
            if (key_col.isNull(r)) continue;
            build_keys_.push_back(keys[r]);

            Row build_row;
            build_row.reserve(chunk->columns.size());
            for (const auto& cv : chunk->columns) {
                build_row.push_back(valueAt(cv, r));
            }
            build_rows_.push_back(std::move(build_row));
        }
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    build_child_->close();
}

void VecSimdLoopJoinNode::probeKeyScalar(int64_t key, std::vector<int>& matches) const {
    for (int i = 0; i < static_cast<int>(build_keys_.size()); ++i) {
        if (build_keys_[i] == key) matches.push_back(i);
    }
}

void VecSimdLoopJoinNode::probeKeySimd(int64_t key, std::vector<int>& matches) const {
#if defined(__ARM_NEON)
    const int64_t* keys = build_keys_.data();
    int n = static_cast<int>(build_keys_.size());
    int64x2_t probe = vdupq_n_s64(key);   // broadcast probe key to both lanes
    int i = 0;
    for (; i + 2 <= n; i += 2) {
        uint64x2_t eq = vceqq_s64(vld1q_s64(keys + i), probe);
        if (vgetq_lane_u64(eq, 0)) matches.push_back(i);
        if (vgetq_lane_u64(eq, 1)) matches.push_back(i + 1);
    }
    // scalar tail — never pad build_keys_ with a sentinel: any sentinel value
    // is a legal key and would fabricate matches
    for (; i < n; ++i) {
        if (keys[i] == key) matches.push_back(i);
    }
#elif defined(__AVX2__)
    // compile-checked via x86 cross-compilation only — not yet run on x86
    // hardware (developed on ARM); the SimdMatchesScalar tests gate it the
    // day it executes for real
    const int64_t* keys = build_keys_.data();
    int n = static_cast<int>(build_keys_.size());
    __m256i probe = _mm256_set1_epi64x(key);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256i eq = _mm256_cmpeq_epi64(
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(keys + i)), probe);
        int mask = _mm256_movemask_pd(_mm256_castsi256_pd(eq));
        while (mask) {
            matches.push_back(i + __builtin_ctz(mask));
            mask &= mask - 1;
        }
    }
    for (; i < n; ++i) {
        if (keys[i] == key) matches.push_back(i);
    }
#else
    probeKeyScalar(key, matches);
#endif
}

void VecSimdLoopJoinNode::fillOutChunk(int start, int count) {
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

DataChunk* VecSimdLoopJoinNode::nextChunk() {
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

        const ColumnVector& probe_key_col = probe_chunk->columns[probe_key_idx];
        const auto& probe_keys = std::get<std::vector<int64_t>>(probe_key_col.data);

        std::vector<int> matches;   // build indices matching one probe key
        for (int r : *indices_ptr) {
            stats.rows_in++;

            if (probe_key_col.isNull(r)) continue;   // NULL matches nothing (see open())

            // compare pass: the hot loop touches only the flat key buffer —
            // payload columns are read below, only for rows that matched
            matches.clear();
            if (use_simd_) probeKeySimd(probe_keys[r], matches);
            else probeKeyScalar(probe_keys[r], matches);

            for (int b : matches) {
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
                    for (const Value& v : build_rows_[b]) out_row.push_back(v);
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
    auto t_fill = std::chrono::high_resolution_clock::now();
    fillOutChunk(output_cursor_, batch);
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t_fill).count();

    stats.rows_out += batch;
    output_cursor_ += batch;
    return &out_chunk_;
}

void VecSimdLoopJoinNode::close() {
    probe_child_->close();
    build_keys_.clear();
    build_rows_.clear();
}

const Schema& VecSimdLoopJoinNode::outputSchema() const {
    return output_schema_;
}

std::string VecSimdLoopJoinNode::explain() const {
    std::string s = "VecSimdLoopJoin [" + probe_join_col_ + " = " + build_join_col_ + "] (materialize)";
    if (!cost_decision_.empty()) s += " " + cost_decision_;
    return s;
}

std::vector<VecPlanNode*> VecSimdLoopJoinNode::children() const {
    return {probe_child_.get(), build_child_.get()};
}
