#include "execution/columnar_eval.h"
#include "execution/evaluator.h"
#include <functional>

// tight loop scanner
// T must match the underlying vector element type stored in chunk.columns[col_idx]
// Cmp is a standard comparator (std::equal_to, std::less)
// input_sel: when non-null, only rows in input_sel->indices are evaluated
template <typename T, typename Cmp>
static SelectionVector scanColumn(const DataChunk& chunk, int col_idx, const T& threshold, Cmp cmp,
                                  const SelectionVector* input_sel) {
    const auto& data = std::get<std::vector<T>>(chunk.columns[col_idx].data);
    SelectionVector sv;
    if (input_sel) {
        sv.indices.reserve(input_sel->size);
        for (int r : input_sel->indices) {
            if (cmp(data[r], threshold)) sv.indices.push_back(r);
        }
    } else {
        sv.indices.reserve(chunk.num_rows);
        for (int r = 0; r < chunk.num_rows; ++r) {
            if (cmp(data[r], threshold)) sv.indices.push_back(r);
        }
    }
    sv.size = static_cast<int>(sv.indices.size());
    return sv;
}

// AND/OR
// both inputs have indices in ascending order (preserved from forward scan),
// so intersection and union are O(n+m) merge operations
SelectionVector sv_intersect(const SelectionVector& a, const SelectionVector& b) {
    SelectionVector result;
    int i = 0, j = 0;
    while (i < a.size && j < b.size) {
        if (a.indices[i] == b.indices[j]){
            result.indices.push_back(a.indices[i++]); ++j;
        }
        else if (a.indices[i] <  b.indices[j]){
            ++i;
        }
        else{
            ++j;
        }
    }
    result.size = static_cast<int>(result.indices.size());
    return result;
}

static SelectionVector sv_union(const SelectionVector& a, const SelectionVector& b) {
    SelectionVector result;
    int i = 0, j = 0;
    while (i < a.size && j < b.size) {
        if (a.indices[i] < b.indices[j]){
            result.indices.push_back(a.indices[i++]);
        }
        else if (a.indices[i] > b.indices[j]){
            result.indices.push_back(b.indices[j++]);
        }
        else{
            result.indices.push_back(a.indices[i++]);
            ++j;
        }
    }
    while (i < a.size) result.indices.push_back(a.indices[i++]);
    while (j < b.size) result.indices.push_back(b.indices[j++]);
    result.size = static_cast<int>(result.indices.size());
    return result;
}

// fallback
// handles IS NULL, arithmetic subexpressions, col op col, and any other shape the fast path does not cover
// reconstructs a temporary Row per row index and delegates to the scalar evaluator.
// input_sel: when non-null, only rows in input_sel->indices are evaluated
static SelectionVector evalFallback(const Expr* pred, const DataChunk& chunk, const Schema& schema,
                                    const SelectionVector* input_sel) {
    SelectionVector sv;
    auto eval_row = [&](int r) {
        Row tmp;
        tmp.reserve(chunk.columns.size());
        for (const auto& cv : chunk.columns){
            std::visit([&](const auto& vec){ tmp.push_back(Value(vec[r])); }, cv.data);
        }
        Value v = evaluate(pred, tmp, schema);
        if (!v.isNull() && v.asInt() != 0) sv.indices.push_back(r);
    };
    if (input_sel) {
        for (int r : input_sel->indices) eval_row(r);
    } else {
        for (int r = 0; r < chunk.num_rows; ++r) eval_row(r);
    }
    sv.size = static_cast<int>(sv.indices.size());
    return sv;
}

// main dispatch
SelectionVector evalPredicate(const Expr* pred, const DataChunk& chunk, const Schema& schema,
                              const SelectionVector* input_sel) {
    const auto* bin = dynamic_cast<const BinaryExpr*>(pred);

    // not a BinaryExpr (IsNullExpr, AggregateExpr, ...), scalar fallback
    if (!bin) return evalFallback(pred, chunk, schema, input_sel);

    // AND/OR, recurse on each side with the same input_sel, then compose
    if (bin->op == "AND"){
        return sv_intersect(evalPredicate(bin->left.get(), chunk, schema, input_sel),
                            evalPredicate(bin->right.get(), chunk, schema, input_sel));
    }
    if (bin->op == "OR"){
        return sv_union(evalPredicate(bin->left.get(), chunk, schema, input_sel),
                        evalPredicate(bin->right.get(), chunk, schema, input_sel));
    }

    // comparison, attempt fast path for (ColumnRef) op (Literal)
    const auto* cr = dynamic_cast<const ColumnRef*>(bin->left.get());
    const auto* lit = dynamic_cast<const Literal*>(bin->right.get());

    if (!cr || !lit || lit->value.isNull()){
        return evalFallback(pred, chunk, schema, input_sel);
    }

    int col_idx = resolveColumnIndex(*cr, schema);
    if (col_idx < 0){
        return evalFallback(pred, chunk, schema, input_sel);
    }

    TypeId col_type = chunk.columns[col_idx].type;
    TypeId lit_type = lit->value.type();
    const std::string& op = bin->op;

    // INT column, INT literal
    if (col_type == TypeId::INT && lit_type == TypeId::INT) {
        int64_t t = lit->value.asInt();
        if (op == "=") return scanColumn<int64_t>(chunk, col_idx, t, std::equal_to<int64_t>{}, input_sel);
        if (op == "!=") return scanColumn<int64_t>(chunk, col_idx, t, std::not_equal_to<int64_t>{}, input_sel);
        if (op == "<") return scanColumn<int64_t>(chunk, col_idx, t, std::less<int64_t>{}, input_sel);
        if (op == ">") return scanColumn<int64_t>(chunk, col_idx, t, std::greater<int64_t>{}, input_sel);
        if (op == "<=") return scanColumn<int64_t>(chunk, col_idx, t, std::less_equal<int64_t>{}, input_sel);
        if (op == ">=") return scanColumn<int64_t>(chunk, col_idx, t, std::greater_equal<int64_t>{}, input_sel);
    }

    // DOUBLE column, DOUBLE literal, or DOUBLE column, INT literal (coerce)
    if (col_type == TypeId::DOUBLE &&
        (lit_type == TypeId::DOUBLE || lit_type == TypeId::INT)) {
        double t = (lit_type == TypeId::INT) ? static_cast<double>(lit->value.asInt()) : lit->value.asDouble();
        if (op == "=") return scanColumn<double>(chunk, col_idx, t, std::equal_to<double>{}, input_sel);
        if (op == "!=") return scanColumn<double>(chunk, col_idx, t, std::not_equal_to<double>{}, input_sel);
        if (op == "<") return scanColumn<double>(chunk, col_idx, t, std::less<double>{}, input_sel);
        if (op == ">") return scanColumn<double>(chunk, col_idx, t, std::greater<double>{}, input_sel);
        if (op == "<=") return scanColumn<double>(chunk, col_idx, t, std::less_equal<double>{}, input_sel);
        if (op == ">=") return scanColumn<double>(chunk, col_idx, t, std::greater_equal<double>{}, input_sel);
    }

    // STRING column, STRING literal
    if (col_type == TypeId::STRING && lit_type == TypeId::STRING) {
        const std::string& t = lit->value.asString();
        if (op == "=") return scanColumn<std::string>(chunk, col_idx, t, std::equal_to<std::string>{}, input_sel);
        if (op == "!=") return scanColumn<std::string>(chunk, col_idx, t, std::not_equal_to<std::string>{}, input_sel);
        if (op == "<") return scanColumn<std::string>(chunk, col_idx, t, std::less<std::string>{}, input_sel);
        if (op == ">") return scanColumn<std::string>(chunk, col_idx, t, std::greater<std::string>{}, input_sel);
        if (op == "<=") return scanColumn<std::string>(chunk, col_idx, t, std::less_equal<std::string>{}, input_sel);
        if (op == ">=") return scanColumn<std::string>(chunk, col_idx, t, std::greater_equal<std::string>{}, input_sel);
    }

    // type mismatch or unknown operator, scalar fallback
    return evalFallback(pred, chunk, schema, input_sel);
}
