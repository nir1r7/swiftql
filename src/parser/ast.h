#pragma once
#include "common/value.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>

// base expression node
struct Expr {
    virtual ~Expr() = default;
    // output-column alias (SELECT expr AS name); meaningful only on
    // select-list roots, empty elsewhere — DuckDB models it the same way
    std::string alias;
};

// reference to a column
struct ColumnRef : Expr {
    std::string table_name;
    std::string column_name;
    // Relation identity assigned by the Binder: 0 = FROM side, 1 = JOIN side,
    // -1 = unresolved. Distinguishes self-join occurrences (l1 vs l2) that
    // share a canonical table_name. Evaluation resolves by (slot, name) when
    // slot >= 0, falling back to bare name otherwise.
    int relation_slot = -1;
};

// literal or constant
struct Literal : Expr {
    Value value;
    explicit Literal(Value v) : value(std::move(v)) {}
};

// binary express; two expressions joined by an operator
// expr (operator) expr
struct BinaryExpr : Expr {
    std::string op; // "=", "!=", "<", ">", "<=", ">=", "AND", "OR", "+", "-", "*", "/"
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

// unary prefix operator; only "-" for now
struct UnaryExpr : Expr {
    std::string op;   // "-"
    std::unique_ptr<Expr> operand;
};

// IS NULL / IS NOT NULL predicate
struct IsNullExpr : Expr {
    std::unique_ptr<Expr> operand;
    bool is_not_null;   // true = IS NOT NULL, false = IS NULL
};

// aggregate function call
struct AggregateExpr : Expr {
    std::string function_name;    // "AVG", "COUNT", "SUM", "MIN", "MAX"
    std::unique_ptr<Expr> argument; // nullptr when is_star = true
    bool is_star;
};

// x IN (v1, v2, ...) / x NOT IN (...) over a constant value list.
//
// The list is restricted to literals, which is what makes the whole node cheap:
// the executor hashes the set once at compile time instead of comparing k times
// per row, and — since the grammar has no NULL literal — the set can never hold
// a NULL, so SQL's three-valued IN rule collapses to "operand NULL -> NULL".
// IN (subquery) is Week 32 and lowers to a semi-join; it is a different
// production, not an extension of this list.
struct InExpr : Expr {
    std::unique_ptr<Expr> operand;
    std::vector<Value> values;   // non-empty, never NULL
    bool negated = false;        // NOT IN
};

// x LIKE 'pattern' / x NOT LIKE 'pattern'. '%' matches any sequence, '_' any
// single character. ASCII case-INSENSITIVE, matching SQLite's default so
// compare_against_sqlite.py stays the correctness oracle — a documented dialect
// choice. The pattern must be a string literal: TPC-H never computes one, and a
// constant pattern is what lets the executor analyse it once per query.
struct LikeExpr : Expr {
    std::unique_ptr<Expr> operand;
    std::string pattern;
    bool negated = false;
};

// Searched CASE: CASE WHEN c THEN r [WHEN c THEN r ...] [ELSE e] END.
// The simple form (CASE x WHEN v ...) is not supported — no TPC-H query needs
// it. A missing ELSE yields NULL, carried on the validity mask.
struct CaseExpr : Expr {
    struct WhenClause {
        std::unique_ptr<Expr> condition;   // must infer to INT (boolean-as-INT)
        std::unique_ptr<Expr> result;
    };
    std::vector<WhenClause> when_clauses;  // at least one; the parser enforces it
    std::unique_ptr<Expr> else_expr;       // nullptr when omitted
};

// SUBSTRING(x FROM start [FOR length]) / SUBSTRING(x, start [, length]).
// 1-based start, SQLite-compatible. This is the first and only scalar function:
// if a second one arrives, replace this with a FunctionExpr { name, args } plus
// a name -> kernel registry rather than adding another one-off node.
struct SubstringExpr : Expr {
    std::unique_ptr<Expr> operand;
    std::unique_ptr<Expr> start;    // 1-based
    std::unique_ptr<Expr> length;   // nullptr = to the end of the string
};

// INTERVAL 'n' day|month|year.
//
// This node MUST NOT survive planning. foldConstants rewrites `date +/- interval`
// into a plain date Literal before validation, which is what keeps ChunkPruner,
// scanColumn and selectivity() on their ColumnRef-op-Literal fast paths. An
// interval that reaches inferExprType or evaluate() therefore means a malformed
// query, and both throw — the loud failure is the design.
struct IntervalLiteral : Expr {
    enum class Unit { DAY, MONTH, YEAR };
    int64_t count = 0;
    Unit unit = Unit::DAY;
};

// one ORDER BY entry: an expression plus sort direction
struct OrderByItem {
    std::unique_ptr<Expr> expr;
    bool desc = false;
};

// one GROUP BY entry: optionally qualified, slot-stamped by the binder
// (plain value struct — keeps SelectStatement's move semantics simple)
struct GroupByColumn {
    std::string table_name;   // as typed; empty if unqualified
    std::string column_name;
    int relation_slot = -1;   // 0 = FROM, 1 = JOIN; -1 = unresolved
    // non-null for expression grouping (GROUP BY season - 1): the group key
    // is evaluate(expr) per row instead of a column read. shared_ptr (not
    // unique_ptr) keeps the struct copyable — planner/test sites copy
    // group-by vectors, and the expr is read-only after binding, so sharing
    // is safe. Last field so positional brace-inits ({"", "grp"}) stay valid.
    std::shared_ptr<Expr> expr;
};

// the full parsed query
struct SelectStatement {
    bool distinct = false;
    bool select_star = false;  // true when SELECT *

    std::vector<std::unique_ptr<Expr>> select_list;

    std::string from_table;
    std::string from_alias; // empty if FROM table has no alias

    // optional JOINs
    struct JoinClause {
        std::string join_table;
        std::string alias; // empty if JOIN table has no alias
        std::unique_ptr<Expr> condition; // the ON expression
    };
    // Written order is load-bearing: joins[i] attaches relation slot i+1 to the
    // left-deep tree built from relations 0..i. That single identity is what the
    // Binder's range table, the merged join schema, join-key routing and
    // predicate pushdown all derive their slot arithmetic from. Empty = no join.
    // (Was std::optional<JoinClause> through Phase 4 — one join only.)
    std::vector<JoinClause> joins;

    // optional WHERE
    std::unique_ptr<Expr> where; // nullptr if no WHERE

    // optional GROUP BY
    std::vector<GroupByColumn> group_by;

    // optional HAVING
    std::unique_ptr<Expr> having; // nullptr if no HAVING

    // optional ORDER BY
    std::vector<OrderByItem> order_by;

    // optional LIMIT
    std::optional<int> limit;
};