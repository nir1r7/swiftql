
#pragma once

#include "common/schema.h"
#include "parser/ast.h"
#include "catalog/catalog.h"
#include "planner/join_condition.h"   // JoinKey
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
    //
    // Week 33. WAS a bare `int relation_slot` with NO query level, contained
    // only by Validator::validate refusing every statement with a subquery —
    // the containment Week 33 removes. It now carries the pair, like
    // GroupByColumn, so its two consumers (plan_nodes.cc,
    // vec_hash_aggregate_node.cc), which resolve indexOf(column, slot) against
    // a CHILD schema, cannot read a correlated slot as a local one.
    // See common/column_id.h and development.md's slot-consumer table.
    ColumnId id;
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

// Week 32 — set-membership lowering. Kept OFF JoinType (parser/ast.h) on
// purpose: JoinType is the *syntactic* per-clause kind a user writes on a JOIN
// clause, and SEMI/ANTI are never written — they are lowering artifacts. Adding
// them there would make `stmt.joins[i].join_type == SEMI` representable and
// unreachable, and grow a dead case in every parser and validator switch.
enum class JoinSemantics {
    STANDARD,  // ordinary equi-join; join_type says INNER or LEFT
    SEMI,      // emit each children[0] row AT MOST ONCE, when a match exists
    ANTI       // emit each children[0] row when NO match exists
};

// equi-join, INNER by default. keys[k].from_col resolves against children[0]'s schema,
// keys[k].join_col against children[1]'s. Multi-key since Week 26 (TPC-H Q9).
// join_slot is the binder relation slot of children[1] — it stamps the merged
// schema and is what predicate pushdown routes conjuncts by. Left-deep only:
// children[1] is always a single relation (join enumeration, Week 28, keeps
// that shape).
struct LogicalJoin : LogicalPlanNode {
    std::vector<JoinKey> keys;
    // Binder relation slot of children[1]. Week 32: -1 when semantics !=
    // STANDARD, and -1 there means "children[1] is not a relation of this
    // block's range table" — there is no slot that names a subquery body. Every
    // reader of join_slot must therefore either decline on semantics !=
    // STANDARD or be provably unreachable for such a node (PredicatePushdown
    // declines; JoinEnumeration declines; the lowerings read it only in the
    // STANDARD branch). That is a development.md slot-table row, not a comment.
    int join_slot;
    // Week 28: set by JoinEnumeration on the TOP join of an enumerated tree, and
    // nowhere else. Empty for every single-join plan, every --no-optimize plan
    // and every hand-built test tree, so all pre-existing explain strings stay
    // byte-identical. Same discipline as VecHashJoinNode::cost_decision_ — and
    // the same rule: never print it when estimates did not drive the decision,
    // or --explain claims an optimizer choice that never happened.
    std::string order_decision;

    // Week 29 — LEFT OUTER. Keys, merged schema and slot stamping are identical;
    // what changes is that an unmatched children[0] row is emitted with NULLs
    // across children[1]'s block, and that four passes must now ask which kind
    // of join this is (pushdown, enumeration, estimation, lowering). Set AFTER
    // construction, like order_decision, so the five-argument constructor — and
    // every hand-built test tree that calls it — is unchanged.
    JoinType join_type = JoinType::INNER;

    // Non-key ON conjuncts, conjoined. NON-NULL ONLY FOR AN OUTER JOIN: an inner
    // join's residuals are folded into the WHERE conjunction instead (Week 27),
    // because ON and WHERE are interchangeable there and the fold buys pushdown.
    // For an outer join they are part of the MATCH TEST — a left row whose every
    // candidate fails this predicate is null-extended, not deleted. Resolves
    // against THIS node's merged output_schema, and is MOVED into the physical
    // operator at lowering (like LogicalFilter::predicate).
    std::unique_ptr<Expr> on_residual;

    // Week 32 — set-membership lowering (subquery_lowering.h). Set AFTER
    // construction, like order_decision and join_type, so the five-argument
    // constructor and every hand-built test tree are byte-identical.
    //
    // !! INVARIANT, and the whole containment for the two-range-table problem
    // this node introduces (development.md -> Relation slots and query levels):
    // when semantics != STANDARD, output_schema IS children[0]->output_schema,
    // NOT a merged schema. children[1] is the plan of a subquery BODY, whose
    // relation slots are numbered against the BODY's range table — a second
    // numbering domain at the same query level. Nothing from children[1] is ever
    // in scope above this node, so the two domains never meet. A merged schema
    // here would put a body column at a body slot into scope, and every
    // indexOf(name, slot) above would be a coin flip — the silent
    // wrong-relation class buildAggregateSchema's tripwire exists for.
    //
    // join_type stays INNER for both: an outer semi-join is not a shape this
    // engine can produce, and the two fields are read independently. join_slot
    // is -1 — see below.
    JoinSemantics semantics = JoinSemantics::STANDARD;

    LogicalJoin(std::unique_ptr<LogicalPlanNode> from_child, std::unique_ptr<LogicalPlanNode> join_child, std::vector<JoinKey> keys, int join_slot, Schema merged) : LogicalPlanNode(LogicalNodeType::JOIN, std::move(merged)), keys(std::move(keys)), join_slot(join_slot) {
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

// Result type of an aggregate, as a function of the function AND its argument
// type. COUNT is always INT. MIN/MAX are order statistics — they return an
// element of the input domain, so they preserve the argument type (including
// STRING; typing them DOUBLE made MIN(team) throw bad_variant_access at the
// materialization point). SUM/AVG accumulate in double, a deliberate divergence
// from SQLite's INT-preserving SUM: the harness compares numerically, and one
// accumulator type keeps the aggregate nodes simple.
// `arg_type` is ignored for COUNT and for star aggregates.
TypeId aggregateResultType(const std::string& function, TypeId arg_type);

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