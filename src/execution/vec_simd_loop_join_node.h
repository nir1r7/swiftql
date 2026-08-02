#pragma once
#include "planner/vec_plan_node.h"
#include "common/schema.h"
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
    VecSimdLoopJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::string probe_join_col, std::string build_join_col, Schema output_schema, bool swapped = false, bool use_simd = true);

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
    std::string probe_join_col_;
    std::string build_join_col_;
    Schema output_schema_;
    bool swapped_;
    bool use_simd_;

    // build phase output: keys in a flat SIMD-probeable buffer, payload rows
    // in a parallel array (build_rows_[i] is the row whose key is build_keys_[i])
    std::vector<int64_t> build_keys_;
    std::vector<Row> build_rows_;

    // rolling output buffer, filled per probe chunk, emitted in BATCH_SIZE slices
    std::vector<Row> output_buffer_;
    int output_cursor_ = 0;
    DataChunk out_chunk_;

    // append indices of build_keys_ equal to key, in increasing order
    void probeKeySimd(int64_t key, std::vector<int>& matches) const;
    void probeKeyScalar(int64_t key, std::vector<int>& matches) const;
    void fillOutChunk(int start, int count);
};
