#include "execution/vec_hash_join_node.h"
#include "execution/chunk_key.h"
#include "execution/key_encoding.h"
#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include <algorithm>
#include <chrono>
#include <functional>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <variant>

VecHashJoinNode::VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::vector<int> probe_key_indices, std::vector<int> build_key_indices, Schema output_schema, bool swapped, bool left_outer, std::unique_ptr<Expr> on_residual, JoinSemantics semantics, Schema residual_schema) : probe_child_(std::move(probe_child)), build_child_(std::move(build_child)), probe_key_idx_(std::move(probe_key_indices)), build_key_idx_(std::move(build_key_indices)), output_schema_(std::move(output_schema)), swapped_(swapped), left_outer_(left_outer), on_residual_(std::move(on_residual)), semantics_(semantics), residual_schema_(std::move(residual_schema)) {
    // Loud rather than latent: with swapped_ the build block is the LEFT half of
    // the output row, so fillOutChunk's trailing-NULL block would null the
    // PRESERVED side's own columns and return rows that look like data.
    // Unreachable from the builder (Week 29 forces the side), so this is the
    // shape of a planner bug, and it says so like every other check of its kind.
    if (left_outer_ && swapped_) {
        throw std::runtime_error(
            "internal: a left outer join requires the preserved side on the probe input");
    }
    // Week 32. Same stance, same reason: the surviving side of a semi/anti join
    // is the PROBE input, and its output schema IS the probe schema. Every one
    // of these is unreachable from VectorizedPlanBuilder, which forces the side,
    // so each is the shape of a planner bug and says so.
    if (semantics_ != JoinSemantics::STANDARD) {
        if (swapped_) {
            throw std::runtime_error(
                "internal: a semi/anti join requires the probed side on the probe input");
        }
        if (left_outer_) {
            throw std::runtime_error(
                "internal: a semi/anti join cannot also be a left outer join");
        }
        // Week 36 — WAS an unconditional "a semi/anti join takes no ON residual".
        // SEMI and ANTI may now carry one (TPC-H q21). ANTI_NOT_IN may not, and
        // the reason is specific rather than caution: its
        // build_had_unmatchable_key_ short-circuit answers "S contains a NULL, so
        // `x NOT IN S` is never TRUE" — a claim about the KEY column that a
        // residual makes untrue, because a build row with a NULL key can no
        // longer stand for "some row matched". NOT IN never produces a residual,
        // so this is a containment and not a limitation.
        if (on_residual_ && semantics_ == JoinSemantics::ANTI_NOT_IN) {
            throw std::runtime_error(
                "internal: a NOT IN anti-join takes no ON residual");
        }
        // THE OUTPUT SCHEMA ASSERTION STAYS, unchanged and load-bearing. The
        // residual is evaluated against residual_schema_, which is PRIVATE: no
        // body column enters output_schema_, so nothing above the join can name
        // one. This is the line that says the residual did not widen the
        // containment on its way in.
        if (output_schema_.size() != probe_child_->outputSchema().size()) {
            throw std::runtime_error(
                "internal: a semi/anti join's output schema must be the probe schema");
        }
        // The pairing, checked both ways so neither a residual without its
        // schema nor a schema without its residual can sit here looking
        // harmless. The width is exact because it is what the assembled row will
        // be: the probe row followed by the whole build row.
        if (on_residual_) {
            if (residual_schema_.size()
                != probe_child_->outputSchema().size() + build_child_->outputSchema().size()) {
                throw std::runtime_error(
                    "internal: a semi/anti join's residual schema must be its probe "
                    "schema followed by its build schema");
            }
        } else if (residual_schema_.size() != 0) {
            throw std::runtime_error(
                "internal: a semi/anti join was given a residual schema without a residual");
        }
    }
    // A LEFT join's residual resolves in output_schema_, which IS its merged
    // schema, so it must not be handed a second one — two schemas for one
    // expression is the drift joinResidualSchema exists to prevent.
    if (semantics_ == JoinSemantics::STANDARD && residual_schema_.size() != 0) {
        throw std::runtime_error(
            "internal: a standard join's ON residual resolves in its output schema");
    }
    // Week 38 — ARM the Bloom pushdown. The filter cannot exist until open() has
    // consumed the build side, but plain --explain never executes the plan, so
    // the probe pipeline is told HERE that a filter is coming and the scan can
    // print `bloom=on` off a plan that never ran. A node that cannot use one
    // ignores both calls.
    if (bloomPushdownApplies()) {
        probe_child_->pushBloomFilter(probe_key_idx_, nullptr);
    }
}

// WHICH JOINS MAY DROP A NON-MATCHING PROBE ROW. This is the whole correctness
// argument for the pushdown, and it is about semantics, not about performance.
//
// A Bloom filter answers "certainly not present" or "possibly present". So a row
// it rejects certainly has NO match on the build side, and dropping it early is
// sound EXACTLY where a probe row with no match contributes nothing to the
// output:
//
//   INNER (STANDARD, !left_outer_)  a probe row with no match emits nothing.
//                                   Dropping it is invisible.  APPLIES.
//   SEMI                            emits a probe row only when a match exists.
//                                   A rejected row would not have been emitted.
//                                   APPLIES.
//   LEFT OUTER (STANDARD + left_outer_)
//                                   a probe row with no match MUST still be
//                                   emitted, null-extended. Dropping it DELETES
//                                   OUTPUT ROWS.  NEVER.
//   ANTI / ANTI_NOT_IN              non-matching probe rows are EXACTLY the
//                                   result. Dropping them deletes the whole
//                                   answer.  NEVER.
//
// TPC-H q13 is the LEFT case and q21/q22 the ANTI cases, and each would come
// back short rather than wrong-looking — which is why the gate is an explicit
// test on the semantics enum rather than a property inferred at the push site.
// tests/test_vectorized.cc asserts, per excluded semantics, that no filter is
// installed.
//
// The residual is NOT part of this. A residual can only make FEWER rows match,
// so a key the filter rejects still cannot match; a semi join carrying one is
// still a semi join.
bool VecHashJoinNode::bloomPushdownApplies() const {
    if (semantics_ == JoinSemantics::SEMI) return true;
    return semantics_ == JoinSemantics::STANDARD && !left_outer_;
}

// ONE key column, INT on BOTH sides — the case where BOTH the Bloom filter and
// (Week 39) the join's own hash table can skip the string entirely. Decided from
// the SCHEMAS, before a chunk is read, because the build loop needs to know
// which key representation it is filling; every chunk column's type comes from
// its producer's schema, so the schema answer is the chunk answer, and
// serializeKey already makes the same INT ⟹ vector<int64_t> assumption.
//
// "BOTH SIDES" IS THE CORRECTNESS CLAUSE, for both readers. The serialized
// encoding renders INT 7 and DOUBLE 7.0 to the same text deliberately, so a
// DOUBLE probe column joins an INT build column; raw int64 bits do not have
// that property, so taking this path with one side DOUBLE would reject a row
// that matches. The full argument is at BloomKeyMode.
bool VecHashJoinNode::singleIntKeyMode() const {
    return probe_key_idx_.size() == 1
        && probe_child_->outputSchema().column(probe_key_idx_[0]).type == TypeId::INT
        && build_child_->outputSchema().column(build_key_idx_[0]).type == TypeId::INT;
}

// Every build key, summarized. Called once, at the end of the build phase, from
// the key arena — which since Week 39 is the ONE place a build key is kept,
// whether or not the join also stored the rows under it.
//
// The keys are the same keys the probe side will test, by construction: in
// SERIALIZED mode both go through chunk_key::serializeKey, and in INT64 mode
// both hash the int64 the encoding is injective on. A build row whose key was
// unmatchable never reached the arena and is not summarized: it can match
// nothing, so nothing is lost by leaving it out.
void VecHashJoinNode::buildAndPushBloomFilter() {
    const size_t key_count = buildKeyCount();
    // A FILTER WITH MORE KEYS THAN THE PROBE SIDE HAS ROWS CANNOT PAY FOR
    // ITSELF, and the cost it cannot recover is this construction: one hash and
    // three scattered writes per build key, over an array too large to stay in
    // cache. TPC-H q4 is the case — 379809 lineitem keys summarized for a probe
    // side of ~5.5k orders rows, which rejected 7.9% and cost 3.2ms to prepare.
    // The scan's own give-up (kBloomSampleRows) cannot see this: by the time it
    // samples, the filter has already been built.
    //
    // The comparison is against the ESTIMATE, which is a performance decision
    // made on a performance number — the same estimate Week 22 already uses to
    // pick the build side. A wrong estimate costs an optimization, never an
    // answer. -1 is "no estimator ran" (--no-optimize), where the pushdown
    // simply always happens, as it did before this guard.
    const double probe_est = probe_child_->estimated_rows;
    if (probe_est >= 0.0 && static_cast<double>(key_count) > probe_est) return;
    if (int_key_mode_) {
        auto filter = std::make_shared<BloomFilter>(key_count, BloomKeyMode::INT64);
        // Week 39 — build_int_keys_ IS the build side's key list in this mode,
        // filled by the ordinary build loop for the join's own table. Week 38
        // had to collect a separate copy in a pass of its own, because a semi
        // join with no residual stored neither the rows nor the keys.
        for (int64_t v : intKeys()) filter->addInt(v);
        probe_child_->pushBloomFilter(probe_key_idx_, std::move(filter));
        return;
    }
    auto filter = std::make_shared<BloomFilter>(key_count);
    for (size_t i = 0; i < build_hash_.size(); ++i) filter->add(keyAt(static_cast<int32_t>(i)));
    probe_child_->pushBloomFilter(probe_key_idx_, std::move(filter));
}

namespace {

// WEEK 38 — serializeKey and hashKey MOVED to execution/chunk_key.h, unchanged.
// The Bloom filter pushdown builds keys here and tests them in VecScanNode, and
// "a Bloom filter has no false negatives" holds only if both sides produce the
// same bytes; one definition is the only way to keep that true. Pulled into this
// namespace so every call site below reads exactly as it did.
using chunk_key::serializeKey;
using chunk_key::hashKey;

// WEEK 39 — the hash for a raw int64 key. splitmix64's finalizer.
//
// The key ITSELF is not usable as the hash, and that is the mistake this exists
// to avoid rather than a stylistic preference: `& mask` keeps only the low bits,
// and TPC-H's generated keys are strided — every l_orderkey at SF=1 is 1, 2, 3
// or 4 modulo 8 — so the raw value collapses a table onto a fraction of its
// slots and turns linear probing into a scan. The finalizer mixes every input
// bit down into the low ones. It is NOT interchangeable with hashKey: that one
// hashes the SERIALIZED bytes, and the two modes never share a table.
inline uint64_t hashInt(int64_t v) {
    uint64_t x = static_cast<uint64_t>(v);
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27; x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// A reused output column, emptied for `count` fresh cells WITHOUT giving up the
// buffers it owns. resize() rather than clear() is the whole point: a STRING
// column's elements survive, so the assignments below reuse the character
// buffers this node allocated on its first output chunk instead of allocating a
// fresh one per cell per chunk.
inline void resetColumn(ColumnVector& cv, int count) {
    cv.all_valid = true;
    cv.validity.clear();
    std::visit([count](auto& vec) { vec.resize(static_cast<size_t>(count)); }, cv.data);
}

// Cell i is NULL. This mirrors appendColumnValue's bookkeeping EXACTLY — the
// all-valid prefix is back-filled on the first NULL so validity stays
// index-aligned with the dense typed vector, and a placeholder is written so no
// stale value from the previous chunk sits under the mask.
inline void setNullAt(ColumnVector& cv, int i) {
    if (cv.all_valid) {
        cv.validity.assign(static_cast<size_t>(i), 1);
        cv.all_valid = false;
    }
    cv.validity.push_back(0);
    switch (cv.type) {
        case TypeId::INT:    std::get<std::vector<int64_t>>(cv.data)[i] = 0;   break;
        case TypeId::DOUBLE: std::get<std::vector<double>>(cv.data)[i] = 0.0;  break;
        case TypeId::STRING: std::get<std::vector<std::string>>(cv.data)[i].clear(); break;
    }
}

// The other half of that bookkeeping: once a column has seen a NULL, every
// later cell has to record that it is not one.
inline void markValidAt(ColumnVector& cv) {
    if (!cv.all_valid) cv.validity.push_back(1);
}

// One cell appended from another column of the SAME storage type — the build
// store's own append. Type equality is by construction (the store's columns are
// made from the build chunks' own types), so this is a typed copy and never the
// `Value` round trip appendColumnValue would need; the NULL bookkeeping is
// appendColumnValue's, via the two helpers above.
inline void appendCellFrom(ColumnVector& dst, const ColumnVector& src, int row) {
    if (src.isNull(row)) {
        if (dst.all_valid) {
            dst.validity.assign(static_cast<size_t>(dst.size()), 1);
            dst.all_valid = false;
        }
        dst.validity.push_back(0);
        // 0 / 0.0 / "" — appendColumnValue's own placeholders, keeping the typed
        // vector dense under the mask
        std::visit([](auto& vec) { vec.emplace_back(); }, dst.data);
        return;
    }
    markValidAt(dst);
    std::visit([&](auto& d) {
        using Vec = std::decay_t<decltype(d)>;
        d.push_back(std::get<Vec>(src.data)[row]);
    }, dst.data);
}

} // namespace

// Thread every stored row onto its bucket's chain, BACK TO FRONT so each chain
// runs in ascending row id. That ordering is not cosmetic: the map this replaced
// handed a key's matches back in build order, and an inner join's output row
// order is observable wherever the query has no total ORDER BY.
//
// Sized to twice the row count, rounded up to a power of two, so `& mask` is the
// whole bucket computation and chains stay short at a 0.5 load factor.
void VecHashJoinNode::buildIndex() {
    const size_t n = build_hash_.size();
    size_t buckets = 16;
    while (buckets < n * 2) buckets <<= 1;
    bucket_mask_ = buckets - 1;
    bucket_head_.assign(buckets, -1);
    build_next_.assign(n, -1);
    for (size_t i = n; i-- > 0;) {
        const size_t b = static_cast<size_t>(build_hash_[i] & bucket_mask_);
        build_next_[i] = bucket_head_[b];
        bucket_head_[b] = static_cast<int32_t>(i);
    }
}

// WEEK 39 — the build side's int64 keys, in build order. Kept ONCE: wherever the join
// stores rows, the row store's key column IS this list — copying it into a
// second vector cost 600572 push_backs and 4.8MB on TPC-H q9 for data already
// in hand — and build_int_keys_ holds it only for the keys-only semi/anti join,
// which stores no rows at all.
const std::vector<int64_t>& VecHashJoinNode::intKeys() const {
    if (build_cols_.empty()) return build_int_keys_;
    return std::get<std::vector<int64_t>>(build_cols_[build_key_idx_[0]].data);
}

// The INT64 mode's index: the SAME chain the serialized mode uses, threaded
// over the key column itself.
//
// ORDER IS PART OF THE CONTRACT, and it is kept exactly as buildIndex() keeps
// it — BACK TO FRONT, so every chain runs in ascending build row and a key's
// matches are emitted in build order. An inner join's output row order is
// observable wherever the query has no total ORDER BY.
//
// No hash is cached beside the chain, and that is not an omission: the cached
// hash exists in the serialized mode to reject a chain neighbour without
// touching the arena, and here the key IS eight bytes in one array, so the
// authoritative compare is already the cheap one.
void VecHashJoinNode::buildIntIndex() {
    int_keys_ = &intKeys();
    const size_t n = int_keys_->size();
    size_t buckets = 16;
    while (buckets < n * 2) buckets <<= 1;
    bucket_mask_ = buckets - 1;
    bucket_head_.assign(buckets, -1);
    build_next_.assign(n, -1);
    for (size_t i = n; i-- > 0;) {
        const size_t b = static_cast<size_t>(hashInt((*int_keys_)[i]) & bucket_mask_);
        build_next_[i] = bucket_head_[b];
        bucket_head_[b] = static_cast<int32_t>(i);
    }
}

void VecHashJoinNode::open() {
    probe_child_->open();
    build_child_->open();

    // Where each side's block sits inside output_schema_'s fixed [FROM, JOIN]
    // order — read off the schemas the operator was actually given, never
    // derived from a chunk's column count: the builder already checks that
    // lowered inputs match their logical schemas, and these are the values that
    // must agree with output_schema_.
    //
    // The one place swapped_ is translated, so neither the probe loop nor
    // fillOutChunk has to know about it. This also replaced Week 29's separate
    // build_width_: for a semi/anti join output_schema_ IS the probe schema, so
    // the build block comes out EMPTY here for the same reason build_width_ was
    // forced to zero there, and the null-extension width is no longer a second
    // number that could disagree with the schema.

    const int build_width = build_child_->outputSchema().size();
    probe_out_base_ = swapped_ ? build_width : 0;
    build_out_base_ = swapped_ ? 0 : probe_child_->outputSchema().size();

    build_cols_.clear();
    build_key_arena_.clear();
    build_key_off_.clear();
    build_key_off_.push_back(0);
    build_hash_.clear();
    build_next_.clear();
    bucket_head_.clear();
    build_int_keys_.clear();
    int_keys_ = nullptr;
    // WHICH KEY REPRESENTATION THIS NODE USES, decided once, before the build
    // loop, because it governs what that loop fills and what the probe reads.
    // Both sides must be a single INT column; everything else keeps the
    // serialized key. See singleIntKeyMode for why "both" is correctness.
    int_key_mode_ = singleIntKeyMode();
    // The ESTIMATE-based half of the Bloom cost guard (the exact-count half is
    // in buildAndPushBloomFilter): a build side estimated LARGER than the probe
    // side cannot pay for the filter. Decided here because it is a decision
    // about the whole build phase.
    const double probe_est = probe_child_->estimated_rows;
    const double build_est = build_child_->estimated_rows;
    bloom_wanted_ = bloomPushdownApplies()
        && !(probe_est >= 0.0 && build_est >= 0.0 && build_est > probe_est);
    build_had_unmatchable_key_ = false;
    probe_chunk_ = nullptr;
    out_probe_rows_.clear();
    out_build_rows_.clear();
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
        // Week 32: a SEMI/ANTI join never emits a build-side row, so only the
        // key is kept.
        //
        // Week 36 — UNLESS IT CARRIES A RESIDUAL. "Never emits" is still true;
        // "never READS" is what stops being true. A residual is a predicate over
        // build columns, which a key alone cannot answer, so such a node keeps
        // the rows too.
        //
        // Week 39 — AND THAT IS NOW THE ONLY DIFFERENCE THE FLAG MAKES. The key
        // is stored either way; `keys_only` skips the per-column append and
        // nothing else, so a semi join without a residual is the row store WITH
        // NO ROWS UNDER IT rather than a second container. The hash set it used
        // to fill cost a malloc per distinct key and 2-3 dependent misses per
        // probe.
        const bool keys_only = semantics_ != JoinSemantics::STANDARD && !on_residual_;

        // The row store's per-row append, shared by both key modes.
        //
        // COLUMN-WISE, and that is a cache decision rather than a taste one.
        // fillOutChunk gathers one output column at a time, so a row-major store
        // made every column's pass re-walk the WHOLE store — 17 passes over 10MB
        // on q18 to emit 10MB. Column-wise, each pass touches only its own array.
        //
        // The store's columns take the CHUNK's storage types, not the schema's,
        // so appendCellFrom is a typed copy and a stored cell is bit-for-bit the
        // Value the old `vector<Row>` held. Nothing here reinterprets a type,
        // which is what keeps the residual — the one consumer that reads these
        // back as Values — seeing exactly what it did before.
        auto append_build_row = [&](int r) {
            if (build_cols_.empty()) {
                for (const auto& cv : chunk->columns) {
                    build_cols_.push_back(makeColumnVector(cv.type));
                }
            }
            for (size_t c = 0; c < chunk->columns.size(); ++c) {
                appendCellFrom(build_cols_[c], chunk->columns[c], r);
            }
        };

        // WEEK 39 — THE INT64 BUILD LOOP. No key is serialized, no arena grows
        // and no hash of a byte string is taken: the key IS the int64, kept in
        // build order for buildIntIndex() below (and for the Bloom filter, which
        // used to need a collection pass of its own here).
        //
        // An INT column's only unmatchable value is NULL — isUnmatchableKey's
        // second clause is `type() == DOUBLE && isnan`, which an INT column
        // cannot satisfy — so this is serializeKey's NULL rule with the NaN case
        // structurally absent rather than dropped. The long note in the
        // serialized loop below covers what the flag means for ANTI.
        if (int_key_mode_) {
            const ColumnVector& kc = chunk->columns[build_key_idx_[0]];
            const auto& kd = std::get<std::vector<int64_t>>(kc.data);
            for (int r : *indices_ptr) {
                if (kc.isNull(r)) {
                    build_had_unmatchable_key_ = true;
                    continue;
                }
                // The key is kept ONCE. With a row store the key column IS the
                // key list, in this order, and buildIntIndex reads it there;
                // build_int_keys_ exists for the keys-only join that has no
                // store, and for the Bloom filter, which needs the same list.
                if (keys_only) build_int_keys_.push_back(kd[r]);
                else           append_build_row(r);
            }
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            continue;
        }

        for (int r : *indices_ptr) {
            // serialize the key tuple with the same '\x01' sentinel Volcano's
            // HashJoinNode uses; a NULL member makes the row unmatchable
            if (!serializeKey(*chunk, build_key_idx_, r, key_buf_)) {
                // Week 32: for ANTI, a NULL here is what makes `x NOT IN S`
                // never TRUE, so record it and short-circuit the probe.
                //
                // The flag is named for what it actually holds: serializeKey
                // fails on any UNMATCHABLE key, and isUnmatchableKey
                // (key_encoding.h) counts NaN as well as NULL. The ANTI collapse
                // is only justified for the NULL half — a NaN in S is a value
                // that simply never compares equal, so `3.0 NOT IN {1.0, NaN}`
                // is relationally TRUE while this branch drops it. Deliberately
                // not special-cased — but not for the reason first given here.
                // "SQLite converts NaN to NULL on storage" covers a COMPUTED
                // NaN only; a `nan` cell in a CSV is imported by SQLite as the
                // TEXT 'nan', which is not NULL, and there SQLite answers TRUE
                // where this drops. See key_encoding.h. It stays uncorrected
                // because no committed dataset holds such a cell and the real
                // close is a columnar validity mask (README, Week 35), not
                // because the two engines agree everywhere.
                build_had_unmatchable_key_ = true;
                continue;
            }

            // Append the row to the store and its key to the arena. Week 37 —
            // the same cells the per-key `vector<Row>` held, in the same order,
            // without the Row's own allocation; the index over them is threaded
            // once the row count is known, in buildIndex() below.
            if (!keys_only) append_build_row(r);
            build_key_arena_ += key_buf_;
            build_key_off_.push_back(build_key_arena_.size());
            build_hash_.push_back(hashKey(key_buf_));
        }
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    {
        auto t0 = std::chrono::high_resolution_clock::now();
        if (int_key_mode_) buildIntIndex(); else buildIndex();
        // Week 38 — sideways information passing. The build side is complete
        // here and nothing has been probed yet, which is the only moment the
        // filter can both be finished and still reach the probe pipeline before
        // it produces its first chunk. Summarizing is this node's own work, so
        // it is charged to this node's self-time like the index build above.
        if (bloom_wanted_) buildAndPushBloomFilter();
        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
    }

    build_child_->close();
}

// One output column gathered from the probe chunk at the row ids
// out_probe_rows_[start .. start+count).
//
// THE FAST PATH IS A TYPED COPY, not a `Value` round trip, and the difference is
// two string allocations per cell: valueAt() constructs a Value (copying the
// characters into it) and appendColumnValue then copies them out again. Straight
// `dst[i] = src[row]` on the underlying vectors copies once, and because
// resetColumn resized rather than cleared, `operator=` reuses the destination
// string's existing buffer.
//
// It is taken only when the output column's storage type IS the source's and the
// source has no NULLs, which is exactly the case where appendColumnValue reduces
// to that same assignment: INT is `asInt()`, STRING is `asString()`, and DOUBLE
// reaching narrowToDoubleColumn as a DOUBLE returns its own value at every
// magnitude (the INT arms cannot fire, and a freshly made column is RENDERED, so
// neither refusal is armed). Anything else — a NULL anywhere, or a source stored
// as a different type — falls back to appendColumnValue itself rather than to a
// second copy of its rules.
void VecHashJoinNode::gatherProbeColumn(ColumnVector& dst, const ColumnVector& src,
                                        int start, int count) {
    const int32_t* rows = out_probe_rows_.data() + start;
    if (dst.type == src.type && src.all_valid) {
        std::visit([&](auto& d) {
            using Vec = std::decay_t<decltype(d)>;
            const Vec& s = std::get<Vec>(src.data);
            for (int i = 0; i < count; ++i) d[i] = s[rows[i]];
        }, dst.data);
        return;
    }
    std::visit([](auto& vec) { vec.clear(); }, dst.data);
    for (int i = 0; i < count; ++i) appendColumnValue(dst, valueAt(src, rows[i]));
}

// One output column gathered from the build store's column `bc`, at the row ids
// out_build_rows_[start .. start+count).
//
// Same shape as the probe gather above and the same typed fast path, with one
// extra condition on it: br == -1 is the LEFT join's unmatched preserved-side
// row — NULL across the whole build block, which is what its null-extension
// always meant — and only a LEFT join ever writes one. (A SEMI/ANTI join writes
// -1 too, but its build block is empty, so this function is never called for
// one.) So !left_outer_ is exactly "no -1 can appear here", and the fast path
// needs no per-cell test for it.
void VecHashJoinNode::gatherBuildColumn(ColumnVector& dst, int bc, int start, int count) {
    const int32_t* rows = out_build_rows_.data() + start;
    // NOT MERELY DEFENSIVE — build_cols_ is created from the first build chunk
    // that CONTRIBUTES a row, so it stays empty both when the build side is empty
    // and when every one of its rows had a NULL key. A LEFT join still emits its
    // whole probe side there, null-extended, and indexing an empty vector to
    // discover that is undefined behaviour (caught by UBSan on
    // VecLeftHashJoin.EmptyBuildSideEmitsEveryProbeRow, where it happened to be
    // harmless because the reference was never read).
    if (build_cols_.empty()) {
        std::visit([](auto& vec) { vec.clear(); }, dst.data);
        for (int i = 0; i < count; ++i) appendColumnValue(dst, Value::null());
        return;
    }
    const ColumnVector& src = build_cols_[bc];
    if (!left_outer_ && dst.type == src.type && src.all_valid) {
        std::visit([&](auto& d) {
            using Vec = std::decay_t<decltype(d)>;
            const Vec& s = std::get<Vec>(src.data);
            for (int i = 0; i < count; ++i) d[i] = s[rows[i]];
        }, dst.data);
        return;
    }
    std::visit([](auto& vec) { vec.clear(); }, dst.data);
    for (int i = 0; i < count; ++i) {
        const int32_t br = rows[i];
        appendColumnValue(dst, br < 0 ? Value::null() : valueAt(src, br));
    }
}

void VecHashJoinNode::fillOutChunk(int start, int count) {
    out_chunk_.num_rows = count;
    out_chunk_.filter_applied = false;
    out_chunk_.sel.indices.clear();
    out_chunk_.sel.size = 0;

    // The columns are BUILT ONCE and refilled thereafter — their types come from
    // output_schema_ and never change — so the string buffers a chunk allocates
    // are still there for the next one. Rebuilding them per call threw those
    // away, which for a 33-column join is most of the materialization cost.
    if (static_cast<int>(out_chunk_.columns.size()) != output_schema_.size()) {
        out_chunk_.columns.clear();
        for (int c = 0; c < output_schema_.size(); ++c) {
            out_chunk_.columns.push_back(makeColumnVector(output_schema_.column(c).type));
        }
    }

    // Week 37 — the cells are read HERE, from the probe chunk and the build
    // store, rather than copied out of a per-output-row Row that was itself a
    // copy. One traversal per output column, so each cell is copied exactly
    // once and the writes are sequential.
    //
    // Which side a column comes from is pure arithmetic on the two bases open()
    // computed; the [FROM, JOIN] order is theirs to encode and this loop never
    // consults swapped_.
    const int probe_width = probe_child_->outputSchema().size();
    for (int c = 0; c < output_schema_.size(); ++c) {
        ColumnVector& dst = out_chunk_.columns[c];
        resetColumn(dst, count);
        const int pc = c - probe_out_base_;
        if (pc >= 0 && pc < probe_width) {
            gatherProbeColumn(dst, probe_chunk_->columns[pc], start, count);
        } else {
            gatherBuildColumn(dst, c - build_out_base_, start, count);
        }
    }
}

// Week 36 — THE SEMI/ANTI MATCH TEST, for a probe key that serialized into
// key_buf_. One question — "does SOME build row under this key satisfy the
// residual?" — asked of the one index open() filled.
//
// Week 39 — "whichever container" was two containers, and is now one. WITHOUT a
// residual the walk stops at the first key match, which is the same yes/no the
// hash set answered; WITH one it goes on to test that candidate.
//
// It scans the bucket and BREAKS ON THE FIRST PASS. The break is not an
// optimization: a semi join emits each probe row AT MOST ONCE — R ⋉ S is π_R(R ⋈
// S) with duplicates COLLAPSED, not an inner join with a projection on top — and
// a loop that kept going would be indistinguishable here but is the shape that
// grows into the duplicate bug the operator exists to prevent.
//
// SEMI AND ANTI ASK THE SAME QUESTION AND DIFFER ONLY IN THE VERDICT, which is
// why this returns "matched" rather than "emit". A residual that evaluates to
// NULL is UNKNOWN, and `passes()`'s `!v.isNull() && v.asInt() != 0` counts it as
// NOT passing on BOTH sides — correct for both, and not a sign flip away from
// wrong: EXISTS is two-valued, so a candidate that is UNKNOWN simply is not a
// witness, and NOT EXISTS keeps the probe row exactly as it would with no
// candidate at all. (The Week 33 round-2 failure was the opposite mistake:
// applying NOT IN's three-valued collapse to an anti-join.)
//
// The assembled row is probe ⊕ build, matching residual_schema_'s construction
// in joinResidualSchema. residual_row_ is reused across candidates, so the loop
// allocates nothing per pair.
bool VecHashJoinNode::candidatePassesResidual(const Row& probe_row, int32_t b) {
    residual_row_.assign(probe_row.begin(), probe_row.end());
    for (const ColumnVector& bc : build_cols_) residual_row_.push_back(valueAt(bc, b));
    Value v = evaluate(on_residual_.get(), residual_row_, residual_schema_);
    return !v.isNull() && v.asInt() != 0;
}

bool VecHashJoinNode::buildSideMatchesInt(int64_t key, const Row& probe_row) {
    for (int32_t i = firstCandidate(hashInt(key)); i >= 0; i = build_next_[i]) {
        if ((*int_keys_)[i] != key) continue;
        if (!on_residual_ || candidatePassesResidual(probe_row, i)) return true;
    }
    return false;
}

bool VecHashJoinNode::buildSideMatches(const Row& probe_row) {
    const std::string_view key(key_buf_);
    const uint64_t h = hashKey(key);
    for (int32_t i = firstCandidate(h); i >= 0; i = build_next_[i]) {
        // The cached hash rejects a chain neighbour under a different key
        // without touching the arena; the arena compare is what actually decides
        // equality, over the injective encoding.
        if (build_hash_[i] != h || keyAt(i) != key) continue;
        // Week 39 — with no residual this IS the old `build_keys_.count()`:
        // "the key is present" is what the set answered and what reaching this
        // line means. The break is still where it was.
        if (!on_residual_ || candidatePassesResidual(probe_row, i)) return true;
    }
    return false;
}

DataChunk* VecHashJoinNode::nextChunk() {
    while (output_cursor_ >= static_cast<int>(out_probe_rows_.size())) {
        // Pending output exhausted, pull the next probe chunk. Clearing HERE and
        // nowhere else is what makes probe_chunk_ safe to read at fill time: the
        // row ids in out_probe_rows_ only ever name the chunk this loop last
        // pulled, and it is not pulled again until they have all been emitted.
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

        // The ON residual, for a left outer join. Boolean-as-INT with an explicit
        // null test, the same three-valued rule the filter path uses: UNKNOWN is
        // not a match. Scalar evaluate() against the assembled row is the
        // correct-but-slow fallback the project already uses where a chunk kernel
        // cannot apply — only outer joins that HAVE a residual pay for it.
        auto passes = [&](const Row& row) {
            if (!on_residual_) return true;
            Value v = evaluate(on_residual_.get(), row, output_schema_);
            return !v.isNull() && v.asInt() != 0;
        };

        // Week 32 — the NULL rule, and it is NOT symmetric. `x NOT IN S` is
        // never TRUE when S holds a NULL: FALSE where x matches, UNKNOWN
        // elsewhere, and a WHERE keeps neither. So an ANTI join over a build
        // side that saw one NULL key emits NOTHING AT ALL — do not probe. The
        // positive form needs no special case, because a NULL simply never
        // matches. This is the collapse Week 31 proved sound at the
        // substitution site: UNKNOWN and FALSE are indistinguishable to every
        // consumer reachable from a WHERE, and nothing this week adds a general
        // NOT. See docs/week-32-plan.md 8.
        // ANTI_NOT_IN ONLY. A decorrelated NOT EXISTS is JoinSemantics::ANTI
        // and must NOT take this branch: EXISTS is never UNKNOWN, so a NULL in
        // the BODY's key column makes that body row fail to match and nothing
        // more. Taking it for ANTI returned ZERO ROWS for the whole query where
        // SQLite returns a full set (docs/week-33-plan.md, round 2).
        if (semantics_ == JoinSemantics::ANTI_NOT_IN && build_had_unmatchable_key_) {
            stats.rows_in += static_cast<int>(indices_ptr->size());
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            continue;   // pull the next probe chunk; this one contributes nothing
        }

        // Week 32 — SEMI/ANTI probe. Structurally a FILTER: output_schema_ IS
        // the probe schema, so the probe row is emitted verbatim, ONCE, and no
        // build row is read. Emitting once is the whole point — an inner join
        // with a projection on top would emit the probe row per match.
        if (semantics_ != JoinSemantics::STANDARD) {
            // Week 36 — assembled ONCE per probe row when a residual needs it,
            // outside the candidate scan. Without a residual nothing reads it
            // before the emit path builds its own, so it stays empty and the
            // no-residual node does exactly the work it did before.
            Row probe_row;
            // Week 39 — the INT64 mode's key column, bound ONCE per chunk. The
            // per-row branch below is on a member bool the branch predictor
            // settles on immediately; what must not be per-row is the variant
            // access.
            const ColumnVector* int_kc = nullptr;
            const std::vector<int64_t>* int_kd = nullptr;
            if (int_key_mode_) {
                int_kc = &probe_chunk->columns[probe_key_idx_[0]];
                int_kd = &std::get<std::vector<int64_t>>(int_kc->data);
            }
            for (int r : *indices_ptr) {
                stats.rows_in++;
                // A NULL probe key emits nothing, for SEMI and ANTI alike:
                // `NULL IN S` and `NULL NOT IN S` are both UNKNOWN whenever S is
                // non-empty, and both are FALSE-equivalent under a WHERE. When S
                // is EMPTY, `x NOT IN ()` is TRUE even for a NULL x — there is
                // nothing to compare against — which is why the empty case is
                // tested on the stored key count rather than folded in here.
                // In INT64 mode "the key did not serialize" is exactly "the key
                // is NULL": an INT column cannot hold the other unmatchable
                // value (a NaN), so the two tests answer the same question and
                // every branch below is reached on the same rows.
                const bool key_ok = int_key_mode_
                    ? !int_kc->isNull(r)
                    : serializeKey(*probe_chunk, probe_key_idx_, r, key_buf_);
                if (!key_ok) {
                    // A NULL probe key matches nothing. The three semantics part
                    // HERE, and this is the whole reason ANTI and ANTI_NOT_IN
                    // are separate enumerators:
                    //   SEMI          — `NULL IN S` is UNKNOWN, and EXISTS over
                    //                   an unsatisfiable equality is FALSE.
                    //                   Emits nothing either way.
                    //   ANTI          — NOT EXISTS, two-valued. The correlated
                    //                   equality is UNKNOWN for every body row,
                    //                   so the body yields none, EXISTS is FALSE
                    //                   and NOT EXISTS is TRUE. The row is
                    //                   emitted UNCONDITIONALLY — whether the
                    //                   build side is empty has nothing to do
                    //                   with it.
                    //   ANTI_NOT_IN   — three-valued. `NULL NOT IN S` is UNKNOWN
                    //                   for a non-empty S, which a WHERE drops;
                    //                   `x NOT IN ()` is TRUE for any x, which
                    //                   is why the empty case is tested on the
                    //                   stored key count rather than folded in.
                    //
                    // Week 39: `buildKeyCount() == 0` is what `build_keys_
                    // .empty()` was — "not one build row produced a key". The
                    // container changed; the question did not, and it is asked
                    // in this one place only.
                    const bool emit =
                        semantics_ == JoinSemantics::ANTI
                        || (semantics_ == JoinSemantics::ANTI_NOT_IN
                            && buildKeyCount() == 0 && !build_had_unmatchable_key_);
                    // Week 37 — the probe row is named, not copied. The build
                    // block is empty for a semi/anti join (output_schema_ IS the
                    // probe schema), so the -1 is only there to keep the two
                    // lists parallel and is never read.
                    if (emit) emitRow(r, -1);
                    continue;
                }
                if (on_residual_) {
                    probe_row.clear();
                    probe_row.reserve(probe_chunk->columns.size());
                    for (const auto& cv : probe_chunk->columns) probe_row.push_back(valueAt(cv, r));
                }
                const bool hit = int_key_mode_
                    ? buildSideMatchesInt((*int_kd)[r], probe_row)
                    : buildSideMatches(probe_row);
                if (hit != (semantics_ == JoinSemantics::SEMI)) continue;
                emitRow(r, -1);
            }
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            // NOT `continue`: the while-loop head CLEARS the pending lists, so a
            // continue here would discard the rows just written. Falling off the
            // end re-tests the loop condition, which is what the standard path
            // has always done.
            if (!out_probe_rows_.empty()) break;
            continue;
        }

        // ONE CANDIDATE PAIR, judged and emitted. Written once and shared by the
        // two key modes below rather than copied into each: this is where the ON
        // residual's rule lives, and two copies of it is how they drift.
        //
        // A candidate failing the residual is NOT a match, and the return value
        // is what says so — otherwise the probe row is neither emitted joined nor
        // null-extended, and it vanishes from the result.
        //
        // Assembling the row is the ONLY thing that still needs Values here, and
        // only a join with a residual pays for it — output row order always
        // matches output_schema_'s fixed [FROM, JOIN] logical order, so the build
        // block leads when the FROM table ended up on the build side (swapped_).
        auto tryCandidate = [&](int r, int32_t b) {
            if (on_residual_) {
                assembled_row_.clear();
                auto append_probe = [&]() {
                    for (const auto& cv : probe_chunk->columns) {
                        assembled_row_.push_back(valueAt(cv, r));
                    }
                };
                auto append_build = [&]() {
                    for (const ColumnVector& bc : build_cols_) {
                        assembled_row_.push_back(valueAt(bc, b));
                    }
                };
                if (swapped_) { append_build(); append_probe(); }
                else          { append_probe(); append_build(); }
                if (!passes(assembled_row_)) return false;
            }
            emitRow(r, b);
            return true;
        };

        // Week 39 — the INT64 mode's probe key column, bound once per chunk.
        const ColumnVector* int_kc = nullptr;
        const std::vector<int64_t>* int_kd = nullptr;
        if (int_key_mode_) {
            int_kc = &probe_chunk->columns[probe_key_idx_[0]];
            int_kd = &std::get<std::vector<int64_t>>(int_kc->data);
        }

        for (int r : *indices_ptr) {
            stats.rows_in++;
            bool matched = false;

            if (int_key_mode_) {
                // A NULL key matches nothing — which for a LEFT join is
                // precisely the row that must still be emitted. An INT column's
                // only unmatchable value is NULL, so this IS the serialized
                // path's test with the NaN case structurally absent.
                if (int_kc->isNull(r)) {
                    if (left_outer_) emitRow(r, -1);
                    continue;
                }
                // Week 39 — the same chain walk as below, over the key column
                // itself: no key is rendered, no arena is touched, and equality
                // is one int64 compare. buildIntIndex() threads the chains
                // backwards, so this visits a key's rows in ASCENDING BUILD ROW
                // exactly as the serialized walk does.
                const int64_t k = (*int_kd)[r];
                for (int32_t b = firstCandidate(hashInt(k)); b >= 0; b = build_next_[b]) {
                    if ((*int_keys_)[b] != k) continue;
                    if (tryCandidate(r, b)) matched = true;
                }
            } else {
                // An unmatchable key (a NULL member, or a NaN) matches nothing —
                // which for a LEFT join is precisely the row that must still be
                // emitted. Week 27's bare `continue` here was a correct comment
                // attached to the wrong action, and it is the one line in this
                // operator that drops rows without erroring.
                if (!serializeKey(*probe_chunk, probe_key_idx_, r, key_buf_)) {
                    if (left_outer_) emitRow(r, -1);
                    continue;
                }

                // Week 37 — the chain walk that replaced hash_table_.find(). It
                // visits a bucket's rows in ASCENDING row id (buildIndex threads
                // the chains backwards to make that true), so a key's matches are
                // emitted in build order exactly as the per-key `vector<Row>` did.
                const std::string_view key(key_buf_);
                const uint64_t h = hashKey(key);
                for (int32_t b = firstCandidate(h); b >= 0; b = build_next_[b]) {
                    // The cached hash rejects a chain neighbour under a different
                    // key without touching the arena; the arena compare over the
                    // injective encoding is what actually decides equality.
                    if (build_hash_[b] != h || keyAt(b) != key) continue;
                    if (tryCandidate(r, b)) matched = true;
                }
            }

            if (left_outer_ && !matched) emitRow(r, -1);
        }

        stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

        // if no matches from this probe chunk, loop to pull the next one
    }

    // emit one BATCH_SIZE slice of the pending output; materialization is real
    // work and counts toward this node's time
    int batch = std::min(BATCH_SIZE, static_cast<int>(out_probe_rows_.size()) - output_cursor_);
    auto t0 = std::chrono::high_resolution_clock::now();
    fillOutChunk(output_cursor_, batch);
    stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();

    stats.rows_out += batch;
    output_cursor_ += batch;
    return &out_chunk_;
}

void VecHashJoinNode::close() {
    probe_child_->close();
    build_cols_.clear();
    build_key_arena_.clear();
    build_key_off_.clear();
    build_hash_.clear();
    build_next_.clear();
    bucket_head_.clear();
    build_int_keys_.clear();
    int_keys_ = nullptr;
    probe_chunk_ = nullptr;
}

const Schema& VecHashJoinNode::outputSchema() const {
    return output_schema_;
}

std::string VecHashJoinNode::explain() const {
    // Names come from the children's schemas, so --explain still prints columns
    // rather than the integers this node holds. A one-key join renders exactly
    // the pre-Week-27 string.
    //
    // EITHER side can be a merged join schema holding one name at several
    // relation slots, and that is exactly the case where resolving a key by name
    // instead of by slot silently joins the wrong relation. Printing a bare
    // `team = team` for both the right and the wrong plan makes the defect
    // invisible on the surface used to debug it, so an ambiguous name carries
    // its slot (`team@1`) on whichever side it appears. Unambiguous names stay
    // bare, so every pre-existing plan string is unchanged.
    //
    // Rendered in logical [FROM, JOIN] order, which is the build side first when
    // the join is swapped: the physical probe/build order is a cost decision and
    // reversing the operands with it leaves the reader unable to tell which
    // relation is which.
    const VecPlanNode* from_side  = swapped_ ? build_child_.get() : probe_child_.get();
    const VecPlanNode* join_side  = swapped_ ? probe_child_.get() : build_child_.get();
    const std::vector<int>& from_idx = swapped_ ? build_key_idx_ : probe_key_idx_;
    const std::vector<int>& join_idx = swapped_ ? probe_key_idx_ : build_key_idx_;

    // Week 29: the node NAME carries the join type, so every inner-join plan
    // string is byte-identical and an outer join is unmissable. It keeps the
    // substring "Join", which the python harness greps for.
    std::string s = "VecHashJoin [";
    if (semantics_ == JoinSemantics::SEMI)      s = "VecSemiHashJoin [";
    else if (semantics_ == JoinSemantics::ANTI) s = "VecAntiHashJoin [";
    else if (semantics_ == JoinSemantics::ANTI_NOT_IN) s = "VecAntiHashJoin [NOT IN, ";
    else if (left_outer_)                       s = "VecLeftHashJoin [";
    for (size_t i = 0; i < from_idx.size(); ++i) {
        if (i) s += " AND ";
        s += qualifyIfAmbiguous(from_side->outputSchema(), from_idx[i]) + " = "
           + qualifyIfAmbiguous(join_side->outputSchema(), join_idx[i]);
    }
    s += "] (materialize)";
    if (on_residual_) s += " residual=" + exprToString(on_residual_.get());
    if (!cost_decision_.empty()) s += " " + cost_decision_;
    return s;
}

std::vector<VecPlanNode*> VecHashJoinNode::children() const {
    return {probe_child_.get(), build_child_.get()};
}
