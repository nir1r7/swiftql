
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
    // output-schema column name (aggregateOutputName of the bound expr);
    // empty only for hand-built specs in tests (falls back to FUNC(column))
    std::string output_name;
    // true = referenced only in HAVING/ORDER BY: computed but never projected
    bool hidden = false;
    // general argument expression (e.g. SUM(speed * (1 - x))). Non-owning:
    // points into the statement's AST, whose subtrees move into plan nodes of
    // the same tree — moving a unique_ptr never relocates the Expr (same
    // aliasing argument as the scan pruning hint in vectorized_plan_builder).
    // Execution uses `column` when set (plain ColumnRef fast path) and falls
    // back to evaluating `argument` per row otherwise. Last field so existing
    // positional brace-inits ({"COUNT", "", true}) stay valid.
    const Expr* argument = nullptr;
};

// execution independent plan node
// no table data and no execution state, just describes what to compute
// children[0] = FROM/left input, children[1] = JOIN input (join only)
struct LogicalPlanNode {
    LogicalNodeType type;
    Schema output_schema;
    // estimated output row count (Week 20 CardinalityEstimator);
    // -1 = not estimated (e.g. --no-optimize skips the pass)
    double estimated_rows = -1.0;
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
    std::vector<GroupByColumn> group_by;  // empty for global aggregates
    std::vector<AggregateSpec> aggregates;

    LogicalAggregate(std::unique_ptr<LogicalPlanNode> child, std::vector<GroupByColumn> group_by, std::vector<AggregateSpec> aggregates, Schema output_schema) : LogicalPlanNode(LogicalNodeType::AGGREGATE, std::move(output_schema)), group_by(std::move(group_by)), aggregates(std::move(aggregates)) {
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

// logical schema helpers — shared by LogicalPlanBuilder and the Volcano
// Planner (relocated from Planner in Week 18 so the logical layer no longer
// includes physical operator headers)

// result type of an expression against a schema; throws std::runtime_error on
// ill-typed trees (arithmetic over STRING). Mirrors evaluate()'s dispatch:
// same slot-first column resolution, same INT-as-boolean convention.
TypeId inferExprType(const Expr* expr, const Schema& schema);

// narrowed scan schema for one table: only columns the query references;
// returns full_schema unchanged for SELECT *
Schema buildScanSchema(const SelectStatement& stmt, const Schema& full_schema);

// schema of the project node's output based on the select list
Schema buildProjectSchema(const SelectStatement& stmt, const Schema& table_schema);

// schema of the aggregate node's output: group-by columns, then one column per aggregate
Schema buildAggregateSchema(const std::vector<GroupByColumn>& group_by,
                            const std::vector<AggregateSpec>& aggregates,
                            const Schema& table_schema);

// extract aggregate specifications from the select list
std::vector<AggregateSpec> extractAggregates(const SelectStatement& stmt);

// Rewrite post-aggregate clauses (select list, HAVING, ORDER BY) so any
// subtree textually matching an expression GROUP BY key becomes a ColumnRef
// to the aggregate's group-key output column (named by exprToString). Runs
// AFTER Validator::validate — validation checks the original trees against
// base-table schemas; the synthesized refs resolve only post-aggregate.
// No-op unless expression group keys exist. WHERE is never rewritten (it
// runs pre-aggregate), and AggregateExpr arguments are left intact.
void substituteGroupKeyRefs(SelectStatement& stmt);

// validates the statement, then builds an execution-independent logical plan tree
// moves expressions out of stmt (select list, predicates, order by) rather than copying
// precondition: Binder::bind has run on stmt; relation slots drive join-key routing,
// falling back to positional routing when unbound, matching Planner::plan
class LogicalPlanBuilder {
    public:
        static std::unique_ptr<LogicalPlanNode> build(SelectStatement stmt, const Catalog& catalog);
};