
#pragma once

#include "common/schema.h"
#include "parser/ast.h"
#include "catalog/catalog.h"
#include <memory>
#include <string>
#include <vector>

enum class LogicalNodeType {
    SCAN, JOIN, FILTER, AGGREGATE, PROJECT, SORT, DISTINCT, LIMIT
};

// aggregate specification for a single aggregate function
struct AggregateSpec {
    std::string function;    // AVG, SUM, etc..
    std::string column;      // empty for star param
    bool is_star;
    // relation slot of the argument column (binder-assigned): distinguishes
    // join sides that share a column name, e.g. AVG(l2.speed) on a self-join.
    // -1 = unresolved/single-relation (resolve by bare name).
    int relation_slot = -1;
};

// execution independent plan node
// no table data and no execution state, just describes what to compute
// children[0] = FROM/left input, children[1] = JOIN input (join only)
struct LogicalPlanNode {
    LogicalNodeType type;
    Schema output_schema;
    std::vector<std::unique_ptr<LogicalPlanNode>> children;

    LogicalPlanNode(LogicalNodeType t, Schema schema) : type(t), output_schema(std::move(schema)) {}
    virtual ~LogicalPlanNode() = default;
    virtual std::string explain() const = 0;
};

// read one table
struct LogicalScan : LogicalPlanNode {
    std::string table_name;  // catalog name (binder normalized)

    LogicalScan(std::string table, Schema narrowed_schema) : LogicalPlanNode(LogicalNodeType::SCAN, std::move(narrowed_schema)), table_name(std::move(table)) {}
    std::string explain() const override;
};

// inner equi-join
// from_col to children[0], join_col to children[1]
struct LogicalJoin : LogicalPlanNode {
    std::string from_col;
    std::string join_col;

    LogicalJoin(std::unique_ptr<LogicalPlanNode> from_child, std::unique_ptr<LogicalPlanNode> join_child, std::string from_c, std::string join_c, Schema merged) : LogicalPlanNode(LogicalNodeType::JOIN, std::move(merged)), from_col(std::move(from_c)), join_col(std::move(join_c)) {
        children.push_back(std::move(from_child));
        children.push_back(std::move(join_child));
    }
    std::string explain() const override;
};

// evaluate a predicate (bool expr) and discard non matching rows
// serves both WHERE and HAVING; position encodes which (below aggregate = WHERE)
struct LogicalFilter : LogicalPlanNode {
    std::unique_ptr<Expr> predicate;

    LogicalFilter(std::unique_ptr<LogicalPlanNode> child, std::unique_ptr<Expr> pred) : LogicalPlanNode(LogicalNodeType::FILTER, child->output_schema), predicate(std::move(pred)) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// group rows by specified columns and compute aggregates
struct LogicalAggregate : LogicalPlanNode {
    std::vector<std::string> group_by;  // empty for global aggregates
    std::vector<AggregateSpec> aggregates;

    LogicalAggregate(std::unique_ptr<LogicalPlanNode> child, std::vector<std::string> group_by, std::vector<AggregateSpec> aggregates, Schema output_schema) : LogicalPlanNode(LogicalNodeType::AGGREGATE, std::move(output_schema)), group_by(std::move(group_by)), aggregates(std::move(aggregates)) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// evaluate the SELECT list
struct LogicalProject : LogicalPlanNode {
    std::vector<std::unique_ptr<Expr>> exprs;  // SELECT list, or synthesized ColumnRefs for SELECT *

    LogicalProject(std::unique_ptr<LogicalPlanNode> child, std::vector<std::unique_ptr<Expr>> exprs, Schema output_schema) : LogicalPlanNode(LogicalNodeType::PROJECT, std::move(output_schema)), exprs(std::move(exprs)) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// sort rows by specified expressions
struct LogicalSort : LogicalPlanNode {
    std::vector<OrderByItem> order_by;  // owns the sort exprs

    LogicalSort(std::unique_ptr<LogicalPlanNode> child, std::vector<OrderByItem> order_by) : LogicalPlanNode(LogicalNodeType::SORT, child->output_schema), order_by(std::move(order_by)) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// remove duplicate rows
struct LogicalDistinct : LogicalPlanNode {
    LogicalDistinct(std::unique_ptr<LogicalPlanNode> child) : LogicalPlanNode(LogicalNodeType::DISTINCT, child->output_schema) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// limit the number of output rows
struct LogicalLimit : LogicalPlanNode {
    int limit;

    LogicalLimit(std::unique_ptr<LogicalPlanNode> child, int limit) : LogicalPlanNode(LogicalNodeType::LIMIT, child->output_schema), limit(limit) {
        children.push_back(std::move(child));
    }
    std::string explain() const override;
};

// validates the statement, then builds an execution-independent logical plan tree
// moves expressions out of stmt (select list, predicates, order by) rather than copying
// precondition: Binder::bind has run on stmt; relation slots drive join-key routing,
// falling back to positional routing when unbound, matching Planner::plan
class LogicalPlanBuilder {
    public:
        static std::unique_ptr<LogicalPlanNode> build(SelectStatement stmt, const Catalog& catalog);
};