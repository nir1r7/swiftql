#include "join_condition.h"

#include <stdexcept>

JoinConditionKeys classifyJoinCondition(const Expr* condition) {
    auto* bin = dynamic_cast<const BinaryExpr*>(condition);
    if (!bin) {
        throw std::runtime_error(
            "JOIN ON: condition must be a single equality between one column from each table");
    }
    if (bin->op == "AND" || bin->op == "OR") {
        throw std::runtime_error(
            "JOIN ON: compound join conditions are not supported; "
            "use a single equality between one column from each table");
    }
    if (bin->op != "=") {
        throw std::runtime_error(
            "JOIN ON: non-equality join conditions are not supported (got '" + bin->op + "')");
    }
    auto* lc = dynamic_cast<const ColumnRef*>(bin->left.get());
    auto* rc = dynamic_cast<const ColumnRef*>(bin->right.get());
    if (!lc || !rc) {
        throw std::runtime_error(
            "JOIN ON: both sides of the join equality must be column references");
    }
    if (lc->relation_slot >= 0 && rc->relation_slot >= 0) {
        if (lc->relation_slot == rc->relation_slot) {
            throw std::runtime_error(
                "JOIN ON: condition must compare a column from each joined table; "
                "both sides reference '" + lc->table_name + "'");
        }
        if (lc->relation_slot == 0) return {lc->column_name, rc->column_name};
        return {rc->column_name, lc->column_name};
    }
    return {lc->column_name, rc->column_name};
}
