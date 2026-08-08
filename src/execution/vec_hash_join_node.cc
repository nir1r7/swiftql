#include "execution/vec_hash_join_node.h"
#include "execution/key_encoding.h"
#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include <algorithm>
#include <chrono>
#include <numeric>
#include <stdexcept>

VecHashJoinNode::VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::vector<int> probe_key_indices, std::vector<int> build_key_indices, Schema output_schema, bool swapped, bool left_outer, std::unique_ptr<Expr> on_residual, JoinSemantics semantics, Schema residual_schema) : probe_child_(std::move(probe_child)), build_child_(std::move(build_child)), probe_key_idx_(std::move(probe_key_indices)), build_key_idx_(std::move(build_key_indices)), output_schema_(std::move(output_schema)), swapped_(swapped), left_outer_(left_outer), on_residual_(std::move(on_residual)), semantics_(semantics), residual_schema_(std::move(residual_schema)) {
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

            // Week 32: a SEMI/ANTI join never emits a build-side row, so only the
            // key is kept.
            //
            // Week 36 — UNLESS IT CARRIES A RESIDUAL. "Never emits" is still
            // true; "never READS" is what stops being true. A residual is a
            // predicate over build columns, and a set of serialized keys cannot
            // answer one, so such a node falls through to the STANDARD path
            // below and fills hash_table_ with the rows. A ROUTING CHANGE, not a
            // new data structure: the machinery below is the one the ordinary
            // equi-join has always used, and exactly one of the two containers
            // is ever populated.
            if (semantics_ != JoinSemantics::STANDARD && !on_residual_) {
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
// Week 36 — THE SEMI/ANTI MATCH TEST, for a probe key that serialized into
// key_buf_. One question — "does SOME build row under this key satisfy the
// residual?" — asked of whichever container open() filled.
//
// WITHOUT a residual it is `build_keys_.count()`, byte for byte the pre-Week-36
// test, and hash_table_ is empty.
//
// WITH one it scans the bucket and BREAKS ON THE FIRST PASS. The break is not an
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
bool VecHashJoinNode::buildSideMatches(const Row& probe_row) {
    if (!on_residual_) return build_keys_.count(key_buf_) > 0;

    auto it = hash_table_.find(key_buf_);
    if (it == hash_table_.end()) return false;
    for (const Row& build_row : it->second) {
        residual_row_.assign(probe_row.begin(), probe_row.end());
        residual_row_.insert(residual_row_.end(), build_row.begin(), build_row.end());
        Value v = evaluate(on_residual_.get(), residual_row_, residual_schema_);
        if (!v.isNull() && v.asInt() != 0) return true;
    }
    return false;
}

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
            for (int r : *indices_ptr) {
                stats.rows_in++;
                // A NULL probe key emits nothing, for SEMI and ANTI alike:
                // `NULL IN S` and `NULL NOT IN S` are both UNKNOWN whenever S is
                // non-empty, and both are FALSE-equivalent under a WHERE. When S
                // is EMPTY, `x NOT IN ()` is TRUE even for a NULL x — there is
                // nothing to compare against — which is why the empty case is
                // tested on build_keys_ rather than folded in here.
                if (!serializeKey(*probe_chunk, probe_key_idx_, r, key_buf_)) {
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
                    //                   is why the empty case is tested on
                    //                   build_keys_ rather than folded in.
                    //
                    // Week 36: `build_keys_` is still the RIGHT container to ask
                    // here, and not by luck — the constructor refuses a residual
                    // on ANTI_NOT_IN, which is the only semantics this line
                    // reads, so open() cannot have filled hash_table_ instead.
                    const bool emit =
                        semantics_ == JoinSemantics::ANTI
                        || (semantics_ == JoinSemantics::ANTI_NOT_IN
                            && build_keys_.empty() && !build_had_unmatchable_key_);
                    if (emit) {
                        Row out_row;
                        out_row.reserve(output_schema_.size());
                        for (const auto& cv : probe_chunk->columns) out_row.push_back(valueAt(cv, r));
                        output_buffer_.push_back(std::move(out_row));
                    }
                    continue;
                }
                if (on_residual_) {
                    probe_row.clear();
                    probe_row.reserve(probe_chunk->columns.size());
                    for (const auto& cv : probe_chunk->columns) probe_row.push_back(valueAt(cv, r));
                }
                const bool hit = buildSideMatches(probe_row);
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
