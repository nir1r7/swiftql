#pragma once
#include "planner/vec_plan_node.h"
#include "common/schema.h"
#include "parser/ast.h"
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
    // moved in). Evaluated against the ASSEMBLED output row and output_schema_.
    // It filters the MATCH TEST, so a probe row whose every candidate fails it is
    // null-extended rather than dropped. nullptr on every inner join — an inner
    // join's residuals live in the WHERE conjunction (Week 27).
    VecHashJoinNode(std::unique_ptr<VecPlanNode> probe_child, std::unique_ptr<VecPlanNode> build_child, std::vector<int> probe_key_indices, std::vector<int> build_key_indices, Schema output_schema, bool swapped = false, bool left_outer = false, std::unique_ptr<Expr> on_residual = nullptr);

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
};
