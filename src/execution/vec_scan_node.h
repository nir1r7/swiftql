#pragma once

#include "planner/vec_plan_node.h"
#include "storage/columnar_table.h"
#include "parser/ast.h"
#include <memory>
#include <string>
#include <vector>

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
        //
        // THE TABLE IS SHARED, NOT OWNED. Every use of it below is a READ
        // (num_rows, columns.at(), zone_maps), so a scan never needed its own
        // image — and the by-value member this replaced made the lowering site
        // deep-copy the whole table once per extra reference: a self-join, a
        // three-way self-join, or a subquery body naming a table the outer query
        // also reads. The cost is not the vector headers but the
        // DictionaryEncoder's `vector<string>`: l_comment holds ~4.5M distinct
        // values averaging 26.5 bytes, past SSO, so one copy is millions of
        // mallocs. Measured at SF=1 as plan time (swiftql.ms - swiftql.exec_ms):
        // 344.3ms on q21 (lineitem scanned three times), 145.7ms on q18, 135.0ms
        // on q17, against 0.2-0.7ms for every query with no repeated table.
        VecScanNode(std::string table_name, std::shared_ptr<const ColumnarTable> columnar_table,
                    Schema schema, const Expr* pruning_where = nullptr,
                    const Schema* hint_schema = nullptr);

        // The same, taking sole ownership of a table no one else holds — what a
        // hand-built test tree has. Wraps and forwards; there is one member.
        VecScanNode(std::string table_name, ColumnarTable columnar_table, Schema schema,
                    const Expr* pruning_where = nullptr, const Schema* hint_schema = nullptr);

        void open() override;
        DataChunk* nextChunk() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<VecPlanNode*> children() const override;

        // Week 38 — the receiving end of the join's sideways information pass.
        // Stores the filter, and ONLY if every key index names a column of this
        // scan's own schema; anything else is declined (the call returns without
        // storing) rather than approximated. See VecPlanNode::pushBloomFilter
        // for why the keys are indices and for the nullptr arming call.
        void pushBloomFilter(const std::vector<int>& key_indices,
                             std::shared_ptr<const BloomFilter> filter) override;

    private:
        // Mark the rows of `chunk` that may match the pushed filter, as a
        // SelectionVector — no column data is moved (late materialization).
        void applyBloomFilter(DataChunk& chunk);

        std::string table_name_;
        std::shared_ptr<const ColumnarTable> columnar_table_;
        Schema schema_;
        int row_cursor_ = 0;
        DataChunk current_chunk_;
        const Expr* pruning_where_ = nullptr; // non-owning; caller must ensure the Expr outlives this node
        Schema hint_schema_{{}};              // schema pruning_where_ was written against
        int skipped_chunks_ = 0;
        bool executed_ = false;  // gates chunks_skipped in explain(): the counter is only real after open()

        // ===== Week 38: pushed-down Bloom filter =====
        //
        // bloom_armed_ is set by the plan-time arming call and bloom_ by the
        // real push at the join's open(); the two are separate so plain
        // --explain, which never executes, can still report the pushdown.
        // NEITHER is cleared by open(): the join pushes AFTER opening its probe
        // child, so clearing there would throw away the filter that is about to
        // be used. rows_rejected_ is the runtime counter and does reset.
        std::vector<int> bloom_key_idx_;
        std::shared_ptr<const BloomFilter> bloom_;
        bool bloom_armed_ = false;
        int64_t bloom_rejected_ = 0;
        int64_t bloom_tested_ = 0;
        bool bloom_gave_up_ = false;
        std::string bloom_key_buf_;   // reused across rows, as the join's is

        // THE FILTER PAYS FOR ITSELF ONLY IF IT REJECTS. Testing a row is not
        // free, and on a foreign-key join where every probe row matches it is
        // pure loss: TPC-H q9's lineitem scan tested 600572 rows for a composite
        // key and rejected NONE, costing 7.8ms of the query's 114ms. So the
        // scan samples the first `kBloomSampleRows` rows and stops testing when
        // fewer than 1 in `kBloomMinRejectRatio` were rejected.
        //
        // Giving up can never change an answer — it only stops REMOVING rows,
        // and every row it stops removing is one the join was always prepared
        // to reject itself. The sample is large enough (8 chunks) that a
        // genuinely selective filter cannot be abandoned by chance; a filter
        // whose rejections all sit past row 8192 is abandoned, which costs the
        // optimization and not the result.
        static constexpr int64_t kBloomSampleRows = 8192;
        static constexpr int64_t kBloomMinRejectRatio = 8;
};