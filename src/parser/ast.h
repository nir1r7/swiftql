#pragma once
#include "common/value.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>

// base expression node
struct Expr {
    virtual ~Expr() = default;
};

// reference to a column
struct ColumnRef : Expr {
    std::string table_name; 
    std::string column_name;
};

// literal or constant
struct Literal : Expr {
    Value value;
    explicit Literal(Value v) : value(std::move(v)) {}
};

// binary express; two expressions joined by an operator
// expr (operator) expr
struct BinaryExpr : Expr {
    std::string op; // "=", "!=", "<", ">", "<=", ">=", "AND", "OR"
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
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

// one ORDER BY entry: an expression plus sort direction
struct OrderByItem {
    std::unique_ptr<Expr> expr;
    bool desc = false;
};

// the full parsed query
struct SelectStatement {
    bool distinct = false;
    bool select_star = false;  // true when SELECT *

    std::vector<std::unique_ptr<Expr>> select_list;

    std::string from_table;

    // optional JOIN
    struct JoinClause {
        std::string join_table;
        std::unique_ptr<Expr> condition; // the ON expression
    };
    std::optional<JoinClause> join;

    // optional WHERE
    std::unique_ptr<Expr> where; // nullptr if no WHERE

    // optional GROUP BY
    std::vector<std::string> group_by;

    // optional HAVING
    std::unique_ptr<Expr> having; // nullptr if no HAVING

    // optional ORDER BY
    std::vector<OrderByItem> order_by;

    // optional LIMIT
    std::optional<int> limit;
};