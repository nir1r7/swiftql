#pragma once
#include "planner/vec_plan_node.h"
#include "common/schema.h"
#include "planner/logical_plan.h"   // JoinSemantics
#include "parser/ast.h"
#include <memory>
#include <unordered_set>
#include <string>
#include <unordered_map>
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
    //   * the build side keeps ROWS, not keys. `build_keys_` (a set) cannot
    //     answer a predicate over build columns, so a semi/anti join WITH a
    //     residual fills `hash_table_` — the STANDARD path's own machinery —
    //     and one WITHOUT a residual still fills `build_keys_`, unchanged.
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

    // Week 32 — SEMI/ANTI build side: keys only, no payload. Disjoint from
    // hash_table_, which stays empty for these nodes.
    //
    // Week 36 — "for these nodes" is now "for these nodes WITHOUT a residual".
    // A residual is a predicate over BUILD COLUMNS, which a set of serialized
    // keys cannot answer, so a semi/anti join carrying one fills hash_table_
    // instead. Exactly one of the two is populated, decided in open() by
    // `on_residual_`, and the probe reads whichever open() filled — see
    // buildSideMatches().
    std::unordered_set<std::string> build_keys_;
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
    // width of the NULL block, read off build_child_'s schema in open()
    int build_width_ = 0;

    // build phase hash table: serialized join key tuple -> matching build side rows
    std::unordered_map<std::string, std::vector<Row>> hash_table_;

    // scratch buffer for the serialized key, reused across rows so neither the
    // build loop nor the probe loop allocates per row
    std::string key_buf_;

    // rolling output buffer, filled per probe chunk, emitted in BATCH_SIZE slices
    std::vector<Row> output_buffer_;
    int output_cursor_ = 0;
    DataChunk out_chunk_;

    void fillOutChunk(int start, int count);
    // one preserved-side row with no surviving match, null-extended
    void emitNullExtended(const DataChunk& probe_chunk, int r);
    // Week 36 — the SEMI/ANTI match test, for a key that serialized. "Does SOME
    // build row under this key satisfy the residual?" — which with no residual
    // degenerates to "is this key present", the pre-Week-36 question.
    bool buildSideMatches(const Row& probe_row);
};
