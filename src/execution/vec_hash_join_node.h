#pragma once
#include "planner/vec_plan_node.h"
#include "common/schema.h"
#include "planner/logical_plan.h"   // JoinSemantics
#include "parser/ast.h"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class VecHashJoinNode : public VecPlanNode {
public:
    // probe_child: larger table
    // build_child: smaller table
    // output_schema is always in fixed logical order [FROM-table columns, JOIN-table columns].
    // swapped = true means the FROM table ended up as build_child (i.e. physical
    // probe-then-build order is the reverse of logical order) — nextChunk() must
    // swap column order when assembling output rows.
    //
    // Keys arrive as resolved column INDICES, one per equi-join key, paired
    // positionally (probe_key_indices[k] matches build_key_indices[k]). Week 27:
    // names would not do, because the probe side's schema can be a MERGED join
    // schema holding the same column name at several relation slots — only the
    // planner knows which slot a key meant. Indices also take the per-chunk
    // indexOf() out of the probe loop.
    //
    // Week 29 — left_outer: emit every probe row at least once, null-extended
    // across the build block when nothing matched. Legal ONLY with
    // swapped == false, because the PRESERVED side must be the probe input: a
    // build-side-preserved outer join needs a matched flag per build row plus an
    // end-of-probe drain, so VectorizedPlanBuilder FORCES the side instead of
    // costing it — the one place Week 22's build-side decision does not apply.
    // The constructor throws on the illegal combination rather than emitting rows
    // with the preserved side nulled out.
    //
    // on_residual: the non-key ON conjuncts, conjoined (LogicalJoin::on_residual,
    // moved in). It filters the MATCH TEST, so a probe row whose every candidate
    // fails it is null-extended (LEFT) or counts as no match (SEMI/ANTI), never
    // silently dropped. nullptr on every inner join — an inner join's residuals
    // live in the WHERE conjunction (Week 27).
    //
    // residual_schema: the schema the residual resolves in, and since Week 36 it
    // is NOT always output_schema_. For a LEFT join the two ARE the same merged
    // schema and this is left empty. For a SEMI/ANTI join output_schema_ IS the
    // probe schema (Week 32's containment, which does not move) while the
    // residual also names the BODY's projected columns — so it is probe ⊕ build,
    // built once by joinResidualSchema (planner/logical_plan.h) and handed to
    // every consumer that must agree about it. Empty is REQUIRED, not merely
    // allowed, wherever there is no semi/anti residual: the constructor checks
    // the pairing rather than letting a stale schema sit beside a null predicate.
    // Week 32 — semantics: SEMI emits each probe row AT MOST ONCE when a match
    // exists, ANTI when none does. Both are structurally FILTERS: output_schema
    // IS the probe schema, no build-side column is ever emitted, and the build
    // side is therefore stored as a hash SET rather than the multimap. Storing
    // the rows would waste the memory AND invite an implementation that emits
    // one output row per match — the duplicate bug the operator exists to
    // prevent (R semi S is pi_R(R join S) with duplicates COLLAPSED, not an
    // inner join with a projection on top).
    //
    // Legal ONLY with swapped == false and left_outer == false: a hash semi-join
    // emits PROBE-side rows, so the surviving side must be the probe input, and
    // VectorizedPlanBuilder FORCES it rather than costing it — the same place
    // Week 22's build-side decision does not apply that left_outer already carved
    // out. The constructor throws on every illegal combination rather than
    // emitting plausible-looking wrong rows.
    //
    // WEEK 36 — "AND NO RESIDUAL" WAS THE THIRD ITEM IN THAT LIST AND IS GONE,
    // for SEMI and ANTI. TPC-H q21 decorrelates to a semi join and an anti join
    // that each carry `l2.l_suppkey != l1.l_suppkey` beside their key, and such a
    // residual cannot be folded into the WHERE: `R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S)` is
    // an INNER-join identity, and a semi join has already collapsed its matching
    // build rows to a yes/no answer by the time `q` could apply.
    //
    // Two consequences inside this operator, both of them routing and neither a
    // new data structure:
    //   * the build side keeps ROWS, not keys. A key on its own cannot answer a
    //     predicate over build columns, so a semi/anti join WITH a residual
    //     fills the ROW STORE (build_cols_ + its index) — the STANDARD path's
    //     own machinery — and one WITHOUT a residual fills the same index with
    //     no columns under it (Week 39; it used to have a hash set of its own).
    //   * the match test becomes "SOME candidate passes", with an early break.
    //     A semi join emits each probe row AT MOST ONCE, which is the whole
    //     reason this operator exists, and the loop must not become an inner
    //     join's multiply-emitting one.
    //
    // ANTI_NOT_IN STILL TAKES NO RESIDUAL, and that is a containment rather than
    // an omission: build_had_unmatchable_key_ short-circuits the entire probe on
    // the claim "S contains a NULL, so `x NOT IN S` is never TRUE" — a statement
    // about the KEY column that a residual makes untrue, since a build row with a
    // NULL key can no longer stand for "some row matched". `NOT IN` produces no
    // residual (subquery_lowering.cc builds no residual at all), so the
    // constraint costs nothing and the constructor enforces it.
    //
    // Trailing and defaulted so every existing construction and hand-built test
    // tree compiles unchanged, the same after-the-fact discipline left_outer and
    // on_residual used.
    VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::vector<int> probe_key_indices, std::vector<int> build_key_indices, Schema output_schema, bool swapped = false, bool left_outer = false, std::unique_ptr<Expr> on_residual = nullptr, JoinSemantics semantics = JoinSemantics::STANDARD, Schema residual_schema = Schema({}));

    void open() override;
    DataChunk* nextChunk() override;
    void close() override;
    const Schema& outputSchema() const override;
    std::string explain() const override;
    std::vector<VecPlanNode*> children() const override;

    // Week 23: builder-supplied cost-decision summary ("build=<table> cost=...
    // (alt=...)"), appended to explain(). Only VectorizedPlanBuilder knows the
    // hashJoinCost numbers; bare constructions leave it empty and print nothing.
    void setCostDecision(std::string decision) { cost_decision_ = std::move(decision); }

private:
    std::string cost_decision_;
    std::unique_ptr<VecPlanNode> probe_child_;
    std::unique_ptr<VecPlanNode> build_child_;
    std::vector<int> probe_key_idx_;
    std::vector<int> build_key_idx_;
    Schema output_schema_;
    bool swapped_;
    bool left_outer_;
    std::unique_ptr<Expr> on_residual_;
    JoinSemantics semantics_;
    // Week 36 — probe ⊕ build, for a SEMI/ANTI residual only. Empty everywhere
    // else, including every LEFT join, whose residual resolves in output_schema_
    // because there the two are the same schema. PRIVATE, and that word is the
    // containment: output_schema_ never widens, so no body column is in scope
    // above this node however many the residual reads.
    Schema residual_schema_;
    // The concatenated probe ⊕ build row the residual is evaluated against,
    // reused across candidates so the probe loop allocates once per probe row
    // rather than once per pair. Same discipline as key_buf_.
    Row residual_row_;

    // WEEK 39 — `std::unordered_set<std::string> build_keys_` IS GONE, and with
    // it the last node-per-key container in this operator. Week 32 gave the
    // SEMI/ANTI build side its own set because such a join keeps keys and no
    // payload; Week 37 then rewrote the STANDARD side into the arena + chained
    // index below and left the set alone. The set cost one malloc per distinct
    // key for the node and another for the std::string inside it, and every
    // probe was 2-3 dependent cache misses — on TPC-H q4 at SF=1, over ~3.2M
    // build keys, VecSemiHashJoin was 44.1% of the query.
    //
    // A KEYS-ONLY BUILD SIDE IS NOW THE ROW STORE WITH NO ROWS. The arena, the
    // offsets, the cached hashes and the chain are filled exactly as they are
    // for a STANDARD join; only the per-column append is skipped, because there
    // are no build columns to emit and no residual to read them. So there is one
    // container, one lookup, and `buildSideMatches` is one chain walk whose last
    // step is the residual test or nothing.
    //
    // Two consequences worth stating, neither a semantic change:
    //   * the arena holds one entry per build ROW, where the set held one per
    //     DISTINCT key. Chains under a duplicated key are longer, but a
    //     semi/anti probe breaks on its first match and the bucket count still
    //     scales with the row count, so the load factor is unchanged.
    //   * "was any key stored" is `buildKeyCount() == 0`, not `set.empty()`. It
    //     is read in exactly one place, and that place is NOT INTERCHANGEABLE
    //     with anything else — see build_had_unmatchable_key_ below and the
    //     ANTI_NOT_IN branch in nextChunk().
    // !! The one piece of state that is easy to forget and impossible to notice.
    // `x NOT IN S` is never TRUE when S contains a NULL — FALSE where x matches,
    // UNKNOWN elsewhere, and a WHERE keeps neither. A semi-join has no
    // substitution site the way Week 31's materialization did, so the fact that
    // the build side held a NULL must be CARRIED OUT of the build phase as a
    // flag and short-circuit the whole ANTI probe. Without it, NOT IN over a
    // nullable column returns rows SQLite does not. See docs/week-32-plan.md 8.
    //
    // Named UNMATCHABLE, not NULL: it is set wherever serializeKey fails, and
    // isUnmatchableKey (key_encoding.h) counts NaN too. The over-collapse that
    // gives is unreachable through the oracle — SQLite stores NaN as NULL — and
    // the reasoning is at the set site.
    bool build_had_unmatchable_key_ = false;

    // ===== build side: a row STORE plus a chained hash INDEX over it =====
    //
    // WEEK 37 — this WAS `unordered_map<std::string, std::vector<Row>>`, which
    // cost roughly three heap allocations per build row (the node, the per-key
    // vector's growth, and the Row's own `vector<Value>`) and was the single
    // hottest structure in the benchmark. Nothing about the ENCODING changed —
    // the keys are still the `<len>:<bytes>` + '\x01' tuples key_encoding.h
    // defines, still injective, and a row whose key fails serializeKey is still
    // dropped before it reaches any of this. What changed is where the bytes
    // live:
    //
    //   * build_cols_ holds the rows COLUMN-WISE, one ColumnVector per build
    //     column, in the build chunks' own storage types. Cells are copied out
    //     of the chunk EXACTLY as the old Row held them — same type, same
    //     null-ness — so nothing downstream can tell the two apart. Column-wise
    //     because fillOutChunk gathers one output column at a time: row-major,
    //     every column's pass re-walked the WHOLE store.
    //   * build_key_arena_ holds every row's serialized key end to end, with
    //     build_key_off_[i]..build_key_off_[i+1] delimiting row i's. One growing
    //     buffer instead of one std::string per row.
    //   * the index is the textbook chain: bucket_head_[h & bucket_mask_] is the
    //     first candidate row and build_next_[i] the next. build_hash_[i] caches
    //     the full hash so a chain walk rejects a foreign key without touching
    //     the arena.
    //
    // ORDER IS PART OF THE CONTRACT. `vector<Row>` handed matches back in build
    // order, and an inner join's output row order is observable wherever the
    // query has no total ORDER BY. buildIndex() therefore threads the chains
    // BACK TO FRONT, so every chain runs in ASCENDING row id and a walk sees the
    // same rows in the same order the old per-key vector did.
    std::vector<ColumnVector> build_cols_;
    std::string build_key_arena_;
    std::vector<size_t> build_key_off_;
    std::vector<uint64_t> build_hash_;
    std::vector<int32_t> build_next_;
    std::vector<int32_t> bucket_head_;
    uint64_t bucket_mask_ = 0;

    // ===== WEEK 39: the INT64 key path =====
    //
    // ONE key column, INT on BOTH sides. There the key IS an int64 and needs no
    // encoding at all: the arena, the offsets and the cached hashes above are
    // not built, and the SAME chained index runs over the key column itself. A
    // probe hashes an integer, walks one chain and settles equality with an
    // int64 compare — against a serialized path that renders the key as decimal
    // text, hashes those bytes, and compares an arena slice that lives wherever
    // the build order put it. TPC-H q4, q9, q13, q17, q18 and q21 join on int64
    // foreign keys and nothing else.
    //
    // AN OPEN-ADDRESSED {key, row} TABLE WAS TRIED FIRST AND LOST, which is
    // worth recording because it is the obvious design and it is one memory
    // access shorter per candidate. 16-byte slots at a 0.5 load factor make the
    // table 4x the size of a chain's bucket array, and the BUILD pays for that
    // in full: on TPC-H q9's `l_partkey = p_partkey` join — 600572 build rows
    // against a 1075-row probe — scattering 600572 sixteen-byte writes over
    // 33MB cost 15.8ms more than the chained index it replaced, and 11.7ms more
    // after the table was resized to 1.5x. The probe it accelerates asks 1075
    // questions. Chaining keeps the 4-byte bucket array and reads the key
    // column in place, so nothing is copied and nothing is enlarged.
    //
    // BOTH SIDES MUST BE INT, and that is a CORRECTNESS rule, not a
    // conservative gate. The serialized encoding renders INT 7 and DOUBLE 7.0 to
    // the same text ON PURPOSE, so they join; hashing raw bits would put them in
    // different buckets and delete a matching row. singleIntKeyMode() is the
    // same predicate — and the same argument — the Bloom pushdown already uses,
    // which is why it is asked once and read by both.
    //
    // ORDER IS PART OF THE CONTRACT here exactly as it is for the serialized
    // mode, and it is kept the same way: buildIntIndex() threads the chains
    // BACK TO FRONT so every chain runs in ascending build row.
    //
    // build_int_keys_ holds the keys ONLY for a keys-only semi/anti join, which
    // stores no rows; everywhere else the row store's key column is the list and
    // intKeys() returns it. int_keys_ is bound to whichever, once, after the
    // build phase — it is read once per candidate and must not re-run that
    // choice per row.
    std::vector<int64_t> build_int_keys_;
    const std::vector<int64_t>* int_keys_ = nullptr;
    bool int_key_mode_ = false;

    // scratch buffer for the serialized key, reused across rows so neither the
    // build loop nor the probe loop allocates per row
    std::string key_buf_;

    // ===== output side: row IDs now, Values only at fill time =====
    //
    // WEEK 37 — this WAS `std::vector<Row> output_buffer_`, one heap-allocated
    // `vector<Value>` per OUTPUT row, every cell of which fillOutChunk then
    // copied a second time into the ColumnVectors. On q18's 60175-row × 33-column
    // inner join that is 2M Value copies and 60k allocations to emit 1M cells.
    //
    // The pair (out_probe_rows_[i], out_build_rows_[i]) names row i of the
    // pending output instead, and fillOutChunk reads the cells straight from the
    // probe chunk and the build store — each cell copied ONCE, column at a time.
    // out_build_rows_[i] == -1 is the null-extended row a LEFT join emits for an
    // unmatched preserved-side row; fillOutChunk writes Value::null() across the
    // build block for it, which is byte for byte what the old null-extension
    // pushed. For SEMI/ANTI the build block is empty (output_schema_ IS the probe
    // schema) so the -1 is never read.
    //
    // THE PROBE CHUNK POINTER IS THE ONE NEW LIFETIME OBLIGATION. Cells are read
    // at FILL time, not at match time, so probe_chunk_ must still hold the rows
    // out_probe_rows_ names. It does: nextChunk() only pulls probe_child_ when
    // the pending list is exhausted, so every BATCH_SIZE slice of a given list is
    // emitted before the chunk underneath it can be refilled.
    const DataChunk* probe_chunk_ = nullptr;
    std::vector<int32_t> out_probe_rows_;
    std::vector<int32_t> out_build_rows_;
    int output_cursor_ = 0;
    DataChunk out_chunk_;

    // Where each side's block starts inside output_schema_, computed once in
    // open() from swapped_. [FROM..., JOIN...] is fixed, so this is the ONLY
    // place the physical probe/build order is translated into it.
    int probe_out_base_ = 0;
    int build_out_base_ = 0;

    // probe ⊕ build (or build ⊕ probe when swapped_) in output_schema_ order, for
    // a STANDARD join's ON residual only. Reused across pairs, same discipline as
    // key_buf_ and residual_row_.
    Row assembled_row_;

    void buildIndex();
    void buildIntIndex();
    // The INT64 half of buildSideMatches, for a probe key that is not NULL.
    bool buildSideMatchesInt(int64_t key, const Row& probe_row);
    // Week 38 — the Bloom pushdown. The gate is a statement about SEMANTICS and
    // its full argument is at the definition: only a join whose non-matching
    // probe rows produce no output may drop them early.
    bool bloomPushdownApplies() const;
    // ONE key column, INT on BOTH sides — read off the SCHEMAS, before a chunk
    // exists. Week 38 introduced it as `bloomIntKeyMode` for the filter alone;
    // Week 39 gave the join's own hash table the same fast path off the same
    // predicate, so it is asked once in open() and both features read the
    // answer. The "both sides" argument is at the definition and is the same
    // argument for both readers.
    bool singleIntKeyMode() const;
    void buildAndPushBloomFilter();
    bool bloom_wanted_ = false;
    // first candidate row id for a key hash, or -1 when the table is empty
    int32_t firstCandidate(uint64_t h) const {
        return bucket_head_.empty() ? -1 : bucket_head_[h & bucket_mask_];
    }
    std::string_view keyAt(int32_t i) const {
        return std::string_view(build_key_arena_.data() + build_key_off_[i],
                                build_key_off_[i + 1] - build_key_off_[i]);
    }
    void fillOutChunk(int start, int count);
    void gatherProbeColumn(ColumnVector& dst, const ColumnVector& src, int start, int count);
    void gatherBuildColumn(ColumnVector& dst, int bc, int start, int count);
    // One pending output row, NAMED rather than materialized: probe row `r`
    // joined to build row `b`. b == -1 is the LEFT join's preserved-side row with
    // no surviving match, null-extended across the build block at fill time — and
    // it is also what a SEMI/ANTI join emits, whose build block is empty, meaning
    // "this probe row, verbatim".
    void emitRow(int r, int32_t b) {
        out_probe_rows_.push_back(r);
        out_build_rows_.push_back(b);
    }
    // Week 36 — the SEMI/ANTI match test, for a key that serialized. "Does SOME
    // build row under this key satisfy the residual?" — which with no residual
    // degenerates to "is this key present", the pre-Week-36 question.
    bool buildSideMatches(const Row& probe_row);
    // One candidate, judged by the ON residual, for a SEMI/ANTI join. Split out
    // so the match tests share it rather than each carrying a copy of the
    // three-valued rule (a residual that is NULL is UNKNOWN, and UNKNOWN is not
    // a witness — for SEMI and for ANTI alike).
    bool candidatePassesResidual(const Row& probe_row, int32_t b);
    // The build side's int64 keys, in build order. Kept ONCE: the row store's
    // key column IS this list wherever there is a store, and build_int_keys_
    // holds it only for a keys-only semi/anti join, which stores no rows.
    // INT64 mode only — the serialized modes have no such column.
    const std::vector<int64_t>& intKeys() const;
    // How many build keys were stored, from whichever of the two key
    // representations open() chose. ONE reader that matters: the ANTI_NOT_IN
    // collapse, which needs "S is empty" and not "S has no rows of interest".
    size_t buildKeyCount() const {
        return int_key_mode_ ? intKeys().size() : build_hash_.size();
    }
};
