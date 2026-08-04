#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include <stdexcept>


int resolveColumnIndex(const ColumnRef& col, const Schema& schema){
    if (col.relation_slot >= 0) {
        int idx = schema.indexOf(col.column_name, col.relation_slot);
        if (idx != -1) return idx;
        // slot-qualified miss (e.g. post-aggregate/projected schema that no
        // longer carries the original slot): fall back to bare name.
    }
    return schema.indexOf(col.column_name);
}

Value evaluate(const Expr* expr, const Row& row, const Schema& schema){
    // literal
    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        return lit->value;
    }

    // column reference
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        int idx = resolveColumnIndex(*col, schema);
        if (idx == -1)
            throw std::runtime_error("Column not found in schema: " + col->column_name);
        return row[idx];
    }

    // binary expression
    // note: using 0 and 1 to represent false and true for boolean results
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        Value left = evaluate(bin->left.get(), row, schema);
        Value right = evaluate(bin->right.get(), row, schema);

        // any null operand returns a null result
        if (left.isNull() || right.isNull()) return Value::null();

        const std::string& op = bin->op;
        if (op == "=")  return Value(static_cast<int64_t>(left == right));
        if (op == "!=") return Value(static_cast<int64_t>(left != right));
        if (op == "<") return Value(static_cast<int64_t>(left < right));
        if (op == ">") return Value(static_cast<int64_t>(left > right));
        if (op == "<=") return Value(static_cast<int64_t>(left <= right));
        if (op == ">=") return Value(static_cast<int64_t>(left >= right));

        if (op == "+" || op == "-" || op == "*" || op == "/") {
            // plan-time inferExprType already rejects STRING operands; this
            // throw only guards hand-built ASTs in tests
            if (left.type() == TypeId::STRING || right.type() == TypeId::STRING)
                throw std::runtime_error("'" + op + "' requires numeric operands");

            if (left.type() == TypeId::INT && right.type() == TypeId::INT) {
                int64_t l = left.asInt(), r = right.asInt();
                if (op == "+") return Value(l + r);
                if (op == "-") return Value(l - r);
                if (op == "*") return Value(l * r);
                // SQLite semantics: INT/INT truncates, x/0 is NULL
                return r == 0 ? Value::null() : Value(l / r);
            }
            double l = left.toNumeric(), r = right.toNumeric();
            if (op == "+") return Value(l + r);
            if (op == "-") return Value(l - r);
            if (op == "*") return Value(l * r);
            return r == 0.0 ? Value::null() : Value(l / r);
        }

        if (op == "AND") {
            bool l = left.asInt() != 0;
            bool r = right.asInt() != 0;
            return Value(static_cast<int64_t>(l && r));
        }
        if (op == "OR") {
            bool l = left.asInt() != 0;
            bool r = right.asInt() != 0;
            return Value(static_cast<int64_t>(l || r));
        }

        throw std::runtime_error("Unknown binary operator: " + op);
    }

    // unary minus
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        Value v = evaluate(un->operand.get(), row, schema);
        if (v.isNull()) return Value::null();
        if (v.type() == TypeId::INT)    return Value(-v.asInt());
        if (v.type() == TypeId::DOUBLE) return Value(-v.asDouble());
        throw std::runtime_error("unary '-' requires a numeric operand");
    }

    // null expression
    if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) {
        Value val = evaluate(isn->operand.get(), row, schema);
        bool result = isn->is_not_null ? !val.isNull() : val.isNull();
        return Value(static_cast<int64_t>(result));
    }

    // aggregate expression
    // note: aggregated values are pre-computed; the output column is found by
    // the shared name contract (see aggregateOutputName)
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        std::string col_name = aggregateOutputName(agg);
        int idx = schema.indexOf(col_name);
        if (idx == -1)
            throw std::runtime_error("Column not found in schema: " + col_name);
        return row[idx];
    }
    throw std::runtime_error("evaluate(): unknown Expr subtype");
}


