#pragma once

#include "plan_node.h"
#include "parser/ast.h"
#include "storage/csv_loader.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <optional>

// read rows one at a time from the loaded row vector (read data)
class SeqScanNode : public PlanNode {
    public:
        SeqScanNode(std::vector<Row> rows, Schema schema);

        void open() override; // initialize cursor
        Row* next() override; // get next row
        void close() override; // cleanup
        const Schema& outputSchema() const override;
    private:
        std::vector<Row> rows_;
        Schema schema_;
        int cursor_; //current position in rows_
};


// evaluate a predicate (bool expr) and discard non matching rows (apply WHERE)
class FilterNode : public PlanNode{
    public:
        FilterNode(std::unique_ptr<PlanNode> child, std::unique_ptr<Expr> predicate);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
    private:
        std::unique_ptr<PlanNode> child_;
        std::unique_ptr<Expr> predicate_;
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
    private:
        std::unique_ptr<PlanNode> child_;
        std::vector<std::unique_ptr<Expr>> expressions_; // the select list
        Schema output_schema_;
        Row current_row_;
};


// aggregate specification for a single aggregate function 
struct AggregateSpec {
    std::string function; // i.e, AVG, SUM, ...
    std::string column; // empty for star param
    bool is_star;
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
    private:
        std::unique_ptr<PlanNode> child_;
        std::unordered_set<std::string> seen_; // seen rows (serialized as strings for easy hashing)
};


// sort rows by specified columns (apply ORDER BY)
class SortNode : public PlanNode {
    public:
        SortNode(std::unique_ptr<PlanNode> child, std::vector<std::string> sorts_cols);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
    private:
        std::unique_ptr<PlanNode> child_;
        std::vector<std::string> sort_cols_; // columns to sort by (in order of precedence)
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

    private:
        std::unique_ptr<PlanNode> child_;
        int limit_;
        int count_;
};


// perform inner join (apply JOIN)
// not to be implemented yet
class HashJoinNode : public PlanNode {
    public:
        HashJoinNode(std::unique_ptr<PlanNode> left,
            std::unique_ptr<PlanNode> right,
            std::string left_col,
            std::string right_col,
            Schema output_schema);

        void open() override;
        Row* next() override;
        void close() override;
        const Schema& outputSchema() const override;
    private:
        std::unique_ptr<PlanNode> left_; // left child node to pull rows from
        std::unique_ptr<PlanNode> right_; // right child node to pull rows from
        std::string left_col_; // join column from left table
        std::string right_col_; // join column from right table
        Schema output_schema_;
};
