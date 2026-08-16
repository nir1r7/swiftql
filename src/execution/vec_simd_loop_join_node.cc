#include "execution/vec_simd_loop_join_node.h"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <type_traits>
#include <variant>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#elif defined(__AVX2__)
#include <immintrin.h>
#endif

VecSimdLoopJoinNode::VecSimdLoopJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, int probe_key_index, int build_key_index, Schema output_schema, bool swapped, bool use_simd) : probe_child_(std::move(probe_child)), build_child_(std::move(build_child)), probe_key_idx_(probe_key_index), build_key_idx_(build_key_index), output_schema_(std::move(output_schema)), swapped_(swapped), use_simd_(use_simd) {}

namespace {

// A reused output column's validity bookkeeping, cleared for a fresh chunk. The
// DATA is sized by whichever gather is about to fill it, because the two want
// opposite things: the typed copy RESIZES (a STRING column's elements survive, so
// its `operator=` reuses the character buffer this node already allocated) while
// an appendColumnValue gather CLEARS and pushes back. Sizing here would mean one
// of the two throwing the other's work away.
inline void resetValidity(ColumnVector& cv) {
    cv.all_valid = true;
    cv.validity.clear();
}

} // namespace

void VecSimdLoopJoinNode::open() {
    probe_child_->open();
    build_child_->open();

    build_keys_.clear();
    build_rows_.clear();
    probe_chunk_ = nullptr;
    out_probe_rows_.clear();
    out_build_rows_.clear();
    output_cursor_ = 0;

    // Where each side's block sits inside output_schema_'s fixed [FROM, JOIN]
    // order — read off the schemas the operator was given, never derived from a
    // chunk's column count. THE ONE PLACE swapped_ IS TRANSLATED, so neither the
    // probe loop nor fillOutChunk has to know about it; this is what replaced the
    // per-output-row `if (swapped_) { build; probe } else { probe; build }` that
    // assembled each Row, and it reorders exactly the same way.
    probe_out_base_ = swapped_ ? build_child_->outputSchema().size() : 0;
    build_out_base_ = swapped_ ? 0 : probe_child_->outputSchema().size();

    // build phase: consume all build side chunks into the flat key buffer +
    // parallel payload rows

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
        const ColumnVector& key_col = chunk->columns[build_key_idx_];
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

// One output column gathered from the probe chunk at the row ids
// out_probe_rows_[start .. start+count).
//
// THE FAST PATH IS A TYPED COPY, not a `Value` round trip, and the difference is
// two string allocations per cell: valueAt() constructs a Value (copying the
// characters into it) and appendColumnValue then copies them out again. Straight
// `dst[i] = src[row]` on the underlying vectors copies once, and resizing rather
// than clearing keeps the destination string's existing buffer for it to reuse.
//
// Taken only when the output column's storage type IS the source's and the source
// has no NULLs — exactly the case where appendColumnValue reduces to that same
// assignment. Anything else falls back to appendColumnValue ITSELF rather than to
// a second copy of its rules, so every refusal in vec_types.h still applies to
// every cell that could trip one. (The same gather VecHashJoinNode has carried
// since Week 37, whose comment holds the full argument for why the DOUBLE arm is
// safe here.)
void VecSimdLoopJoinNode::gatherProbeColumn(ColumnVector& dst, const ColumnVector& src,
                                           int start, int count) {
    const int32_t* rows = out_probe_rows_.data() + start;
    resetValidity(dst);
    if (dst.type == src.type && src.all_valid) {
        std::visit([&](auto& d) {
            using Vec = std::decay_t<decltype(d)>;
            d.resize(static_cast<size_t>(count));
            const Vec& s = std::get<Vec>(src.data);
            for (int i = 0; i < count; ++i) d[i] = s[rows[i]];
        }, dst.data);
        return;
    }
    std::visit([](auto& vec) { vec.clear(); }, dst.data);
    for (int i = 0; i < count; ++i) appendColumnValue(dst, valueAt(src, rows[i]));
}

// One output column gathered from build_rows_, at the row ids
// out_build_rows_[start .. start+count).
//
// No typed fast path, and that is a property of the STORE rather than an
// omission: build_rows_ holds `Value`s, so `build_rows_[b][bc]` is already the
// Value appendColumnValue wants and there is nothing to reinterpret. What matters
// is that it is read ONCE per output cell, here, instead of being copied into a
// per-output-row Row first — and that build_rows_ is at most ~50 rows, so the
// whole store stays in L1 however many times these passes walk it.
void VecSimdLoopJoinNode::gatherBuildColumn(ColumnVector& dst, int bc, int start, int count) {
    const int32_t* rows = out_build_rows_.data() + start;
    resetValidity(dst);
    std::visit([](auto& vec) { vec.clear(); }, dst.data);
    for (int i = 0; i < count; ++i) {
        appendColumnValue(dst, build_rows_[rows[i]][bc]);
    }
}

void VecSimdLoopJoinNode::fillOutChunk(int start, int count) {
    out_chunk_.num_rows = count;
    out_chunk_.filter_applied = false;
    out_chunk_.sel.indices.clear();
    out_chunk_.sel.size = 0;

    // The columns are BUILT ONCE and refilled thereafter — their types come from
    // output_schema_ and never change — so the string buffers a chunk allocates
    // are still there for the next one. Rebuilding them per call threw those away.
    if (static_cast<int>(out_chunk_.columns.size()) != output_schema_.size()) {
        out_chunk_.columns.clear();
        for (int c = 0; c < output_schema_.size(); ++c) {
            out_chunk_.columns.push_back(makeColumnVector(output_schema_.column(c).type));
        }
    }

    // The cells are read HERE, from the probe chunk and build_rows_, rather than
    // copied out of a per-output-row Row that was itself a copy. One traversal per
    // output column, so each cell is copied exactly once and the writes are
    // sequential.
    //
    // Which side a column comes from is pure arithmetic on the two bases open()
    // computed; the [FROM, JOIN] order is theirs to encode and this loop never
    // consults swapped_.
    const int probe_width = probe_child_->outputSchema().size();
    for (int c = 0; c < output_schema_.size(); ++c) {
        ColumnVector& dst = out_chunk_.columns[c];
        const int pc = c - probe_out_base_;
        if (pc >= 0 && pc < probe_width) {
            gatherProbeColumn(dst, probe_chunk_->columns[pc], start, count);
        } else {
            gatherBuildColumn(dst, c - build_out_base_, start, count);
        }
    }
}

DataChunk* VecSimdLoopJoinNode::nextChunk() {
    while (output_cursor_ >= static_cast<int>(out_probe_rows_.size())) {
        // Pending output exhausted, pull the next probe chunk. Clearing HERE and
        // nowhere else is what makes probe_chunk_ safe to read at fill time: the
        // row ids in out_probe_rows_ only ever name the chunk this loop last
        // pulled, and it is not pulled again until they have all been emitted.
        // vec_types.h says the child reuses that buffer on its next call, so this
        // ordering is the whole lifetime argument.
        out_probe_rows_.clear();
        out_build_rows_.clear();
        output_cursor_ = 0;

        DataChunk* probe_chunk = probe_child_->nextChunk();
        if (!probe_chunk) return nullptr; // probe side exhausted
        probe_chunk_ = probe_chunk;

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

        const ColumnVector& probe_key_col = probe_chunk->columns[probe_key_idx_];
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

            // The matching pairs are NAMED, in the same order the Rows were
            // pushed: probe rows in probe order (the loop above) and, within one
            // probe row, build rows in increasing build index (probeKeySimd and
            // probeKeyScalar both append ascending). Output row order is
            // observable without a total ORDER BY, so that sequence is the
            // contract and it is unchanged.
            //
            // The [FROM, JOIN] reordering swapped_ used to do per Row now lives
            // entirely in open()'s two base offsets, applied once per output
            // COLUMN in fillOutChunk.
            for (int b : matches) emitRow(r, b);
        }

        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

        // if no matches from this probe chunk, loop to pull the next one
    }

    // emit one BATCH_SIZE slice of the pending output; materialization is real
    // work and counts toward this node's time
    int batch = std::min(BATCH_SIZE, static_cast<int>(out_probe_rows_.size()) - output_cursor_);
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
    probe_chunk_ = nullptr;
}

const Schema& VecSimdLoopJoinNode::outputSchema() const {
    return output_schema_;
}

std::string VecSimdLoopJoinNode::explain() const {
    // Names come from the children's schemas, so --explain prints columns rather
    // than the indices this node holds; an ambiguous name on EITHER side carries
    // its relation slot, and the operands read in logical [FROM, JOIN] order
    // whichever side physically builds — see VecHashJoinNode::explain.
    const VecPlanNode* from_side = swapped_ ? build_child_.get() : probe_child_.get();
    const VecPlanNode* join_side = swapped_ ? probe_child_.get() : build_child_.get();
    const int from_idx = swapped_ ? build_key_idx_ : probe_key_idx_;
    const int join_idx = swapped_ ? probe_key_idx_ : build_key_idx_;
    std::string s = "VecSimdLoopJoin ["
        + qualifyIfAmbiguous(from_side->outputSchema(), from_idx) + " = "
        + qualifyIfAmbiguous(join_side->outputSchema(), join_idx) + "] (materialize)";
    if (!cost_decision_.empty()) s += " " + cost_decision_;
    return s;
}

std::vector<VecPlanNode*> VecSimdLoopJoinNode::children() const {
    return {probe_child_.get(), build_child_.get()};
}
