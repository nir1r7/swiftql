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
            std::vector<std::string> group_by_cols,
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
        std::vector<std::string> group_by_cols_; // columns to group by
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
        HashJoinNode(std::unique_ptr<PlanNode> left, std::unique_ptr<PlanNode> right, std::string left_col, std::string right_col, Schema output_schema, bool swapped = false);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
        std::string explain() const override;
        std::vector<PlanNode*> children() const override;
    private:
        std::unique_ptr<PlanNode> left_; // left child node to pull rows from
        std::unique_ptr<PlanNode> right_; // right child node to pull rows from
        std::string left_col_; // join column from left table
        std::string right_col_; // join column from right table
        Schema output_schema_;
        bool swapped_; // true: right_ (build) is logically first; see class comment

        std::unordered_map<std::string, std::vector<Row>> hash_table_;
        
        Row* current_probe_row_ = nullptr;
        int bucket_idx_;

        Row current_row_;
};
