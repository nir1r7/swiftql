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
    // Relation identity assigned by the Binder: the range-table position —
    // 0 = FROM, then one per JOIN in written order (joins[i] -> i+1);
    // -1 = unresolved. Distinguishes self-join occurrences (l1 vs l2) that
    // share a canonical table_name. Evaluation resolves by (slot, name) when
    // slot >= 0, falling back to bare name otherwise.
    int relation_slot = -1;
    // Week 30. How many query blocks OUT from the block this ref is written in
    // the relation lives: 0 = this query's own range table (the default, so
    // every pre-existing ref and every hand-built test tree keeps its meaning),
    // 1 = the immediately enclosing query, and so on. RELATIVE, like Postgres's
    // varlevelsup, so a subquery's tree means the same thing wherever it sits.
    //
    // !! relation_slot is a position in the range table of the scope this many
    // steps OUT. Reading a slot without its level compares two different
    // numbering domains — an inner slot 1 and an outer slot 1 are different
    // relations. Every walker that routes by slot must test the level first.
    int query_level = 0;
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

// Defined at the bottom of this file; a subquery expression holds a whole one.
struct SelectStatement;

// A nested query in an expression position (Week 30).
//
// ONE node with a kind tag, not three: the three forms differ only in whether
// there is a left-hand operand and whether the result is a value or a
// predicate. Three structs would cost three branches at each of the eighteen
// dispatch sites (development.md) and give three chances to miss one.
//
// OWNERSHIP: `subquery` is a shared_ptr because SelectStatement holds
// unique_ptr members and is therefore move-only, while cloneExpr (dispatch site
// 11) must copy any Expr. Sharing rather than deep-copying is the same choice
// GroupByColumn::expr makes, for the same reason, and it is safe because the
// bound AST is read-only after binding — which holds only because
// Binder::resolveColumnRef is IDEMPOTENT (binder.cc). Two nodes sharing one
// statement is a real state: BETWEEN's desugaring clones its left operand
// before binding, and the GROUP BY / ORDER BY alias substitution clones an
// already-bound select item.
//
// POSITION: legal in WHERE and HAVING only. Validator refuses every other
// clause and validateJoinCondition (dispatch site 18) refuses ON — no TPC-H
// query puts a subquery anywhere else, and allowing one in the select list
// would mean buildProjectSchema must type it and aggregateOutputName must name
// an output column after it. That is a restriction on POSITION, not on
// representation: all three kinds are represented. FROM is Week 34.
struct SubqueryExpr : Expr {
    enum class Kind {
        SCALAR,   // (SELECT one_column FROM ...) — a value
        EXISTS,   // EXISTS (SELECT ...)          — a predicate
        IN        // x IN (SELECT one_column ...) — a predicate over `operand`
    };
    Kind kind = Kind::SCALAR;
    bool negated = false;                 // NOT EXISTS / NOT IN
    // IN only; nullptr for SCALAR and EXISTS. Belongs to the ENCLOSING scope,
    // never to the subquery's — the Binder binds it against the outer range
    // table and every walker must descend into it there.
    std::unique_ptr<Expr> operand;
    std::shared_ptr<SelectStatement> subquery;
    // Set by the Binder: true when some ColumnRef inside `subquery` resolved to
    // an enclosing scope. Read by collectSlots (dispatch site 8) to decide
    // whether this node can be routed by relation slot at all; Week 33
    // decorrelates on it.
    bool correlated = false;
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

// Week 29. Per-JOIN-CLAUSE, not per-statement: a query may mix inner and outer
// joins (A JOIN B ... LEFT JOIN C ...), and predicate pushdown and join
// enumeration both have to ask the question one join at a time. INNER is the
// default so every pre-existing brace-init and hand-built test tree keeps its
// meaning. RIGHT/FULL are out of scope: a RIGHT join is not a flag flip, since
// swapping the operands would change the merged schema's column order, which
// invariant 1 forbids.
enum class JoinType { INNER, LEFT };

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
    int relation_slot = -1;   // range-table position (0 = FROM); -1 = unresolved
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
        // Week 29. LAST field, so positional brace-inits that predate it stay
        // valid — the same discipline AggregateSpec::argument and
        // GroupByColumn::expr follow.
        JoinType type = JoinType::INNER;
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

    // Week 30. Set by the Binder when it binds a SubqueryExpr directly inside
    // THIS statement. It exists so the "not yet executable" refusal and
    // buildScanSchema's conservative widening need no nineteenth walker over
    // the statement to find out. Any statement containing a subquery at any
    // depth contains one DIRECTLY, so the top-level flag is always the right
    // test for "this query uses a subquery".
    bool has_subquery = false;
};