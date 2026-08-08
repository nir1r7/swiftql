#pragma once

#include "planner/vec_plan_node.h"
#include "storage/columnar_table.h"
#include "parser/ast.h"

class VecScanNode : public VecPlanNode {
    public:
        // `hint_schema` is the schema `pruning_where` was WRITTEN against — the
        // child schema of the filter that handed the hint down, which over a
        // join is the join's merged schema and not this scan's. ChunkPruner
        // screens each conjunct for "can raise" in it; screening in the scan's
        // own schema was a wrong answer rather than a conservative one, because
        // staticTypeOf's bare-name fallback resolves a foreign ref against the
        // scanned table's same-named column (chunk_pruner.h states the repro).
        // nullptr = "the same as `schema`", the single-relation case.
        VecScanNode(std::string table_name, ColumnarTable columnar_table, Schema schema,
                    const Expr* pruning_where = nullptr, const Schema* hint_schema = nullptr);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

    private:
        std::string table_name_;
        ColumnarTable columnar_table_;
        Schema schema_;
        int row_cursor_ = 0;
        DataChunk current_chunk_;
        const Expr* pruning_where_ = nullptr; // non-owning; caller must ensure the Expr outlives this node
        Schema hint_schema_{{}};              // schema pruning_where_ was written against
        int skipped_chunks_ = 0;
        bool executed_ = false;  // gates chunks_skipped in explain(): the counter is only real after open()
};