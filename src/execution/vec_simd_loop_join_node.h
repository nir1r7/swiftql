#pragma once
#include "planner/vec_plan_node.h"
#include "common/schema.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Week 23.5 — SIMD small-build loop join. Inner equi-join over INT keys only:
// the build side's keys are held in one flat contiguous buffer and every probe
// key is compared against all of them with SIMD (NEON on ARM, AVX2 on x86,
// scalar fallback otherwise). O(build_rows * probe_rows), so the planner only
// selects it when the costed build side is small (cost_model.h).
class VecSimdLoopJoinNode : public VecPlanNode {
public:
    // Same contract as VecHashJoinNode: output_schema is always in fixed
    // logical order [FROM-table columns, JOIN-table columns]; swapped = true
    // means the FROM table ended up as build_child, so nextChunk() reorders.
    // use_simd = false forces the scalar reference path (tests + calibration).
    //
    // ONE key, as a resolved column index per side. One because a composite key
    // cannot live in the flat int64 buffer this operator probes with SIMD — the
    // planner declines multi-key here and lowers to the hash join instead. An
    // index rather than a name because the probe side may be a merged join
    // schema, where a bare name can match several relations (Week 27).
    VecSimdLoopJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, int probe_key_index, int build_key_index, Schema output_schema, bool swapped = false, bool use_simd = true);

    void open() override;
    DataChunk* nextChunk() override;
    void close() override;
    const Schema& outputSchema() const override;
    std::string explain() const override;
    std::vector<VecPlanNode*> children() const override;

    // Week 23.5: builder-supplied cost-decision summary, appended to explain().
    // Same convention as VecHashJoinNode — only VectorizedPlanBuilder knows
    // the cost numbers; bare constructions leave it empty and print nothing.
    void setCostDecision(std::string decision) { cost_decision_ = std::move(decision); }

private:
    std::string cost_decision_;
    std::unique_ptr<VecPlanNode> probe_child_;
    std::unique_ptr<VecPlanNode> build_child_;
    int probe_key_idx_;
    int build_key_idx_;
    Schema output_schema_;
    bool swapped_;
    bool use_simd_;

    // build phase output: keys in a flat SIMD-probeable buffer, payload rows
    // in a parallel array (build_rows_[i] is the row whose key is build_keys_[i])
    //
    // Still a `vector<Row>`, deliberately, where Week 37 turned the hash join's
    // equivalent into a column store. The reason that rewrite was a CACHE
    // decision does not apply here: the hash join stores one row per build row of
    // a table it may have chosen to build from at any size, and a row-major store
    // made every output column's gather pass re-walk the whole thing — 17 passes
    // over 10MB on TPC-H q18. This operator's probe term is QUADRATIC, so the
    // cost model only ever prefers it for a small build side (cost_model.h; a
    // modelled crossover, not a hard cap) and the store stays in L1 however many
    // passes cross it.
    //
    // NOT MEASURED AGAINST THE ALTERNATIVE, and this comment says so rather than
    // implying it was. What IS measured is that the store is no longer where the
    // time goes: on the 1M-row F1 `laps JOIN drivers` join, against a 20-row
    // build side, this operator's self-time is ~19ms with one INT column per side
    // and ~23ms with four output columns one of which is a STRING — so the entire
    // per-cell gather of the two extra columns, build side included, is ~4ms of
    // it. The other ~19ms is the O(build_rows * probe_rows) compare pass, which no
    // storage layout changes.
    std::vector<int64_t> build_keys_;
    std::vector<Row> build_rows_;

    // ===== WEEK 37 (applied here in Week 40): output rows are NAMED, not built =====
    //
    // This WAS `std::vector<Row> output_buffer_` — one heap-allocated
    // `vector<Value>` per OUTPUT row, every cell of which fillOutChunk then
    // copied a SECOND time into the output ColumnVectors. On the F1
    // `laps JOIN drivers` shape, where all 1M probe rows match, that is 1M
    // allocations and two copies of every cell to emit 4M of them; the operator
    // spent 60ms of a 93ms query there while VecHashJoinNode — which got this
    // treatment in Week 37 — did the same join in 9.8ms.
    //
    // The pair (out_probe_rows_[i], out_build_rows_[i]) names row i of the
    // pending output instead, and fillOutChunk reads the cells straight from the
    // probe chunk and build_rows_, one output COLUMN at a time, copying each cell
    // exactly once.
    //
    // THE PROBE CHUNK POINTER IS THE ONE NEW LIFETIME OBLIGATION, and it is the
    // same one the hash join carries. vec_types.h documents that nextChunk()
    // returns a buffer the child REUSES on its next call, so the row ids in
    // out_probe_rows_ are only meaningful while probe_chunk_ still holds those
    // rows. It does: the lists are cleared, and the probe child pulled, ONLY at
    // the head of nextChunk()'s loop, which is reached only once output_cursor_
    // has drained every pending row. So every BATCH_SIZE slice of a given list is
    // emitted before the chunk underneath it can be refilled.
    //
    // Unlike the hash join there is no -1 sentinel: this operator is inner-only,
    // so every pending row names a real build row.
    const DataChunk* probe_chunk_ = nullptr;
    std::vector<int32_t> out_probe_rows_;
    std::vector<int32_t> out_build_rows_;
    int output_cursor_ = 0;
    DataChunk out_chunk_;

    // Where each side's block starts inside output_schema_, computed once in
    // open() from swapped_. [FROM..., JOIN...] is fixed, so this is the ONLY
    // place the physical probe/build order is translated into it — fillOutChunk
    // never consults swapped_, exactly as VecHashJoinNode's does not.
    int probe_out_base_ = 0;
    int build_out_base_ = 0;

    // append indices of build_keys_ equal to key, in increasing order
    void probeKeySimd(int64_t key, std::vector<int>& matches) const;
    void probeKeyScalar(int64_t key, std::vector<int>& matches) const;
    void fillOutChunk(int start, int count);
    void gatherProbeColumn(ColumnVector& dst, const ColumnVector& src, int start, int count);
    void gatherBuildColumn(ColumnVector& dst, int bc, int start, int count);
    // One pending output row, named rather than materialized: probe row `r`
    // joined to build row `b`.
    void emitRow(int r, int32_t b) {
        out_probe_rows_.push_back(r);
        out_build_rows_.push_back(b);
    }
};
