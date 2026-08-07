#include "execution/vec_hash_join_node.h"
#include "execution/key_encoding.h"
#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <stdexcept>

VecHashJoinNode::VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::vector<int> probe_key_indices, std::vector<int> build_key_indices, Schema output_schema, bool swapped, bool left_outer, std::unique_ptr<Expr> on_residual, JoinSemantics semantics) : probe_child_(std::move(probe_child)), build_child_(std::move(build_child)), probe_key_idx_(std::move(probe_key_indices)), build_key_idx_(std::move(build_key_indices)), output_schema_(std::move(output_schema)), swapped_(swapped), left_outer_(left_outer), on_residual_(std::move(on_residual)), semantics_(semantics) {
    // Loud rather than latent: with swapped_ the build block is the LEFT half of
    // the output row, so emitNullExtended's trailing-NULL assembly would null the
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
        if (on_residual_) {
            throw std::runtime_error(
                "internal: a semi/anti join takes no ON residual");
        }
        if (output_schema_.size() != probe_child_->outputSchema().size()) {
            throw std::runtime_error(
                "internal: a semi/anti join's output schema must be the probe schema");
        }
    }
}

namespace {

// Serialize one row's k-key tuple into `out`, reusing its capacity. The encoding
// itself lives in key_encoding.h, shared with Volcano's HashJoinNode.
//
// Returns false when any key column is NULL. SQL's NULL equals nothing, so with
// k keys the rule composes: one NULL member makes the whole tuple unmatchable,
// on either side. Dropping such rows keeps them out of the hash table instead of
// bucketing them under toString()'s "NULL", which would make NULL = NULL match.
bool serializeKey(const DataChunk& chunk, const std::vector<int>& key_idx, int r, std::string& out) {
    const bool prefixed = key_idx.size() > 1;
    out.clear();
    for (int c : key_idx) {
        // valueAt, never the typed vector directly: a raw read bypasses the
        // validity mask and turns a NULL into the placeholder underneath it
        Value v = valueAt(chunk.columns[c], r);
        if (isUnmatchableKey(v)) return false;
        appendJoinKeyField(out, v, prefixed);
    }
    return true;
}

} // namespace

void VecHashJoinNode::open() {
    probe_child_->open();
    build_child_->open();

    // Width of the NULL block for a left outer join. Read off the schema the
    // operator was actually given, never derived from a probe chunk's column
    // count: the builder already checks that lowered inputs match their logical
    // schemas, and this is the value that must agree with output_schema_.
    // Meaningless for a semi/anti join — nothing is null-extended and no build
    // column is emitted — so it is left at zero rather than silently computing a
    // width nothing uses.
    build_width_ = (semantics_ == JoinSemantics::STANDARD)
                 ? build_child_->outputSchema().size() : 0;

    hash_table_.clear();
    build_keys_.clear();
    build_had_unmatchable_key_ = false;
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
                // not special-cased: key_encoding.h records that SQLite converts
                // NaN to NULL on storage, so the oracle agrees with this
                // engine's answer and there is no reachable divergence to fix.
                build_had_unmatchable_key_ = true;
                continue;
            }

            // Week 32: a SEMI/ANTI join never emits a build-side row, so only the
            // key is kept.
            if (semantics_ != JoinSemantics::STANDARD) {
                build_keys_.insert(key_buf_);
                continue;
            }

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

// One preserved-side row with no surviving match. The build block is the
// TRAILING columns of output_schema_ (guaranteed by !swapped_, which the
// constructor enforces), so the assembly is the probe row followed by
// build_width_ NULLs.
//
// appendColumnValue (vec_types.h) back-fills the validity prefix on the first
// NULL, so fillOutChunk turns these into REAL nulls rather than the placeholder
// underneath them — the Week 24 validity mask doing the whole job. This operator
// needs no materialization change at all.
void VecHashJoinNode::emitNullExtended(const DataChunk& probe_chunk, int r) {
    Row out_row;
    out_row.reserve(output_schema_.size());
    for (const auto& cv : probe_chunk.columns) out_row.push_back(valueAt(cv, r));
    for (int i = 0; i < build_width_; ++i) out_row.push_back(Value::null());
    output_buffer_.push_back(std::move(out_row));
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
        if (semantics_ == JoinSemantics::ANTI && build_had_unmatchable_key_) {
            stats.rows_in += static_cast<int>(indices_ptr->size());
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            continue;   // pull the next probe chunk; this one contributes nothing
        }

        // Week 32 — SEMI/ANTI probe. Structurally a FILTER: output_schema_ IS
        // the probe schema, so the probe row is emitted verbatim, ONCE, and no
        // build row is read. Emitting once is the whole point — an inner join
        // with a projection on top would emit the probe row per match.
        if (semantics_ != JoinSemantics::STANDARD) {
            for (int r : *indices_ptr) {
                stats.rows_in++;
                // A NULL probe key emits nothing, for SEMI and ANTI alike:
                // `NULL IN S` and `NULL NOT IN S` are both UNKNOWN whenever S is
                // non-empty, and both are FALSE-equivalent under a WHERE. When S
                // is EMPTY, `x NOT IN ()` is TRUE even for a NULL x — there is
                // nothing to compare against — which is why the empty case is
                // tested on build_keys_ rather than folded in here.
                if (!serializeKey(*probe_chunk, probe_key_idx_, r, key_buf_)) {
                    if (semantics_ == JoinSemantics::ANTI && build_keys_.empty()
                        && !build_had_unmatchable_key_) {
                        Row out_row;
                        out_row.reserve(output_schema_.size());
                        for (const auto& cv : probe_chunk->columns) out_row.push_back(valueAt(cv, r));
                        output_buffer_.push_back(std::move(out_row));
                    }
                    continue;
                }
                const bool hit = build_keys_.count(key_buf_) > 0;
                if (hit != (semantics_ == JoinSemantics::SEMI)) continue;
                Row out_row;
                out_row.reserve(output_schema_.size());
                for (const auto& cv : probe_chunk->columns) out_row.push_back(valueAt(cv, r));
                output_buffer_.push_back(std::move(out_row));
            }
            stats.elapsed_us += std::chrono::duration<double, std::micro>(std::chrono::high_resolution_clock::now() - t0).count();
            // NOT `continue`: the while-loop head CLEARS output_buffer_, so a
            // continue here would discard the rows just written. Falling off the
            // end re-tests the loop condition, which is what the standard path
            // has always done.
            if (!output_buffer_.empty()) break;
            continue;
        }

        for (int r : *indices_ptr) {
            stats.rows_in++;

            // An unmatchable key (a NULL member, or a NaN) matches nothing — which
            // for a LEFT join is precisely the row that must still be emitted.
            // Week 27's bare `continue` here was a correct comment attached to the
            // wrong action, and it is the one line in this operator that drops
            // rows without erroring.
            if (!serializeKey(*probe_chunk, probe_key_idx_, r, key_buf_)) {
                if (left_outer_) emitNullExtended(*probe_chunk, r);
                continue;
            }

            bool matched = false;
            auto it = hash_table_.find(key_buf_);
            if (it == hash_table_.end()) {
                if (left_outer_) emitNullExtended(*probe_chunk, r);
                continue;
            }

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
                // A candidate failing the ON residual is NOT a match, so it must
                // not set `matched`: otherwise the probe row is neither emitted
                // joined nor null-extended, and it vanishes from the result.
                if (!passes(out_row)) continue;
                matched = true;
                output_buffer_.push_back(std::move(out_row));
            }

            if (left_outer_ && !matched) emitNullExtended(*probe_chunk, r);
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
    build_keys_.clear();
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
