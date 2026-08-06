#pragma once
#include "planner/vec_plan_node.h"
#include "common/schema.h"
#include <memory>
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
    VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::vector<int> probe_key_indices, std::vector<int> build_key_indices, Schema output_schema, bool swapped = false);

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
};
