#include "execution/evaluator.h"
#include <stdexcept>


Value evaluate(const Expr* expr, const Row& row, const Schema& schema){
    // literal
    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        return lit->value;
    }

    // column reference
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        int idx = schema.indexOf(col->column_name);
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

    // null expression
    if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) {
        Value val = evaluate(isn->operand.get(), row, schema);
        bool result = isn->is_not_null ? !val.isNull() : val.isNull();
        return Value(static_cast<int64_t>(result));
    }

    // aggregate expression
    // note: aggregated values are pre-computed
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        // reconstruct the canonical name, i.e, AVG(speed) or COUNT(*)
        std::string col_name;
        if (agg->is_star) {
            col_name = agg->function_name + "(*)";
        } else {
            // agg->argument must be a ColumnRef
            auto* inner = dynamic_cast<const ColumnRef*>(agg->argument.get());
            col_name = agg->function_name + "(" + inner->column_name + ")";
        }
        int idx = schema.indexOf(col_name);
        return row[idx];
    }
    throw std::runtime_error("evaluate(): unknown Expr subtype");
}


