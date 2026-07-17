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
    VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::string probe_join_col, std::string build_join_col, Schema output_schema, bool swapped = false);

    void open() override;
    DataChunk* nextChunk() override;
    void close() override;
    const Schema& outputSchema() const override;
    std::string explain() const override;
    std::vector<VecPlanNode*> children() const override;

private:
    std::unique_ptr<VecPlanNode> probe_child_;
    std::unique_ptr<VecPlanNode> build_child_;
    std::string probe_join_col_;
    std::string build_join_col_;
    Schema output_schema_;
    bool swapped_;

    // build phase hash table: serialized join key -> matching build side rows
    std::unordered_map<std::string, std::vector<Row>> hash_table_;

    // rolling output buffer, filled per probe chunk, emitted in BATCH_SIZE slices
    std::vector<Row> output_buffer_;
    int output_cursor_ = 0;
    DataChunk out_chunk_;

    void fillOutChunk(int start, int count);
};
