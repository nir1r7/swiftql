#pragma once

#include "plan_node.h"
#include "logical_plan.h"
#include "parser/ast.h"
#include "storage/csv_loader.h"
#include "storage/columnar_table.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <optional>

// read rows one at a time from the loaded row vector (read data)
class SeqScanNode : public PlanNode {
    public:
        // row storage
        SeqScanNode(std::string table_name, std::vector<Row> rows, Schema schema);

        // columnar storage (pruning_where = nullptr disables chunk pruning)
        SeqScanNode(std::string table_name, ColumnarTable columnar_table, Schema schema, const Expr* pruning_where = nullptr);

        void open() override; // initialize cursor
        Row* next() override; // get next row
        void close() override; // cleanup
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;
    private:
        Schema schema_;
        int cursor_; //current position in rows_
        std::string table_name_;

        // row path
        std::vector<Row> rows_;

        // columnar path
        ColumnarTable columnar_table_;
        bool use_columnar_ = false;
        Row reconstructed_row_; // reuse on every columnar next()

        const Expr* pruning_where_ = nullptr; // non owning
        int skipped_chunks_ = 0;
        bool executed_ = false;  // gates chunks_skipped in explain(): the counter is only real after open()
};


// evaluate a predicate (bool expr) and discard non matching rows (apply WHERE)
class FilterNode : public PlanNode{
    public:
        FilterNode(std::unique_ptr<PlanNode> child, std::unique_ptr<Expr> predicate);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;
    private:
        std::unique_ptr<Expr> predicate_;
        std::unique_ptr<PlanNode> child_;
};


// evaluate the SELECT list and hold projected rows (apply SELECT)
class ProjectNode : public PlanNode {
    public:
        ProjectNode(std::unique_ptr<PlanNode> child,
            std::vector<std::unique_ptr<Expr>> expressions,
            Schema output_schema);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;
    private:
        std::unique_ptr<PlanNode> child_;
        std::vector<std::unique_ptr<Expr>> expressions_; // the select list
        Schema output_schema_;
        Row current_row_;
};


// group rows by specified columns and compute aggregates (apply GROUP BY)
class HashAggregateNode : public PlanNode {
    public:
        HashAggregateNode(std::unique_ptr<PlanNode> child,
            std::vector<GroupByColumn> group_by_cols,
            std::vector<AggregateSpec> aggregates,
            Schema output_schema);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;
    private:
        std::unique_ptr<PlanNode> child_;
        std::vector<GroupByColumn> group_by_cols_; // columns to group by
        std::vector<AggregateSpec> aggregates_; // aggregate functions to compute
        Schema output_schema_;

        // built during open() by consuming all child rows
        std::vector<Row> results_; // final aggregated results
        int cursor_;
};


// evaluate HAVING predicate on aggregated results and discard non matching groups (apply HAVING)
class HavingNode : public PlanNode {
    public:
        HavingNode(std::unique_ptr<PlanNode> child, std::unique_ptr<Expr> predicate);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;
    private:
        std::unique_ptr<PlanNode> child_;
        std::unique_ptr<Expr> predicate_; // condition expression
};


// remove duplicate rows (apply DISTINCT)
class DistinctNode : public PlanNode {
    public:
        explicit DistinctNode(std::unique_ptr<PlanNode> child);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;
    private:
        std::unique_ptr<PlanNode> child_;
        std::unordered_set<std::string> seen_; // seen rows (serialized as strings for easy hashing)
};


// sort rows by specified expressions (apply ORDER BY)
class SortNode : public PlanNode {
    public:
        SortNode(std::unique_ptr<PlanNode> child, std::vector<OrderByItem> order_by);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;
    private:
        std::unique_ptr<PlanNode> child_;
        std::vector<OrderByItem> order_by_;
        std::vector<Row> sorted_rows_;
        int cursor_;
};


// limit the number of output rows (apply LIMIT)
class LimitNode : public PlanNode {
    public:
        LimitNode(std::unique_ptr<PlanNode> child, int limit);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;

    private:
        std::unique_ptr<PlanNode> child_;
        int limit_;
        int count_;
};


// perform inner join (apply JOIN)
// left_ probes, right_ builds. output_schema stays in logical FROM/JOIN order;
// swapped_ means physical build/probe order is reversed and output must swap.
class HashJoinNode : public PlanNode {
    public:
        // Keys are paired positionally: left_cols[k] joins right_cols[k]. Both
        // children are single-relation scans on this path (Planner::plan builds
        // one join and refuses more), so their schemas cannot repeat a column
        // name and by-name resolution is unambiguous — the vectorized builder,
        // whose left input may be a merged join schema, resolves by slot into
        // indices instead.
        //
        // Week 33, Task 7(2) — is the Volcano semi/anti refusal TOTAL? Week 32's
        // hand-off flagged this as unproven and pointed at
        // src/execution/hash_join_node.cc, which DOES NOT EXIST: the Volcano
        // hash join is this class, in the planner layer. Correcting the pointer
        // is half the finding.
        //
        // The answer is YES, and structurally rather than by a guard that could
        // drift. This operator has NO JoinSemantics parameter, so a semi or anti
        // join is not REPRESENTABLE here — there is no argument to pass and no
        // branch to take. The refusals in Planner::plan (IN, Week 32; correlated,
        // Week 33) therefore make a shape unreachable that could not have been
        // built anyway; they exist to produce a NAMED message instead of a
        // confusing one. That is a stronger containment than the vectorized
        // side's, where JoinSemantics is a constructor argument and
        // build_had_unmatchable_key_ carries NOT IN's three-valued rule.
        //
        // What this costs is the two-mode oracle coverage recorded as Week 33's
        // Task 6 gap. Adding semantics here is the fix; note it needs a Volcano
        // plan shape that can hold a SECOND join, which Planner::plan cannot
        // express today.
        //
        // Week 29 — same contract as VecHashJoinNode. left_outer emits every
        // probe row at least once, null-extended across the build block, and is
        // legal only with swapped == false: the PRESERVED side must be the probe
        // input, so Planner::plan forces the side rather than costing it.
        // on_residual holds the non-key ON conjuncts of an outer join, which
        // filter the MATCH TEST (a probe row whose every candidate fails them is
        // null-extended, not deleted) — nullptr for every inner join, whose
        // residuals are folded into the WHERE conjunction instead.
        HashJoinNode(std::unique_ptr<PlanNode> left, std::unique_ptr<PlanNode> right, std::vector<std::string> left_cols, std::vector<std::string> right_cols, Schema output_schema, bool swapped = false, bool left_outer = false, std::unique_ptr<Expr> on_residual = nullptr);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;
    private:
        std::unique_ptr<PlanNode> left_; // left child node to pull rows from
        std::unique_ptr<PlanNode> right_; // right child node to pull rows from
        std::vector<std::string> left_cols_;  // join columns from left table
        std::vector<std::string> right_cols_; // join columns from right table
        Schema output_schema_;
        bool swapped_; // true: right_ (build) is logically first; see class comment
        bool left_outer_;
        std::unique_ptr<Expr> on_residual_;
        // per probe row, and it must survive across next() calls: the
        // null-extended row is emitted when the bucket drains, one call later
        bool probe_matched_ = false;
        int build_width_ = 0;   // NULL block width, resolved in open()

        std::unordered_map<std::string, std::vector<Row>> hash_table_;

        // resolved once in open(): re-resolving per row cost k lookups a row
        std::vector<int> left_key_idx_;
        std::vector<int> right_key_idx_;

        Row* current_probe_row_ = nullptr;
        int bucket_idx_;
        std::string probe_key_;   // serialized key of current_probe_row_

        Row current_row_;
};
