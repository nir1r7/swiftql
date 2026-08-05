#include "execution/columnar_eval.h"
#include "execution/evaluator.h"
#include <functional>
#include <numeric>

// tight loop scanner
// T must match the underlying vector element type stored in chunk.columns[col_idx]
// Cmp is a standard comparator (std::equal_to, std::less)
// input_sel: when non-null, only rows in input_sel->indices are evaluated
template <typename T, typename Cmp>
static SelectionVector scanColumn(const DataChunk& chunk, int col_idx, const T& threshold, Cmp cmp,
                                  const SelectionVector* input_sel) {
    const ColumnVector& cv = chunk.columns[col_idx];
    const auto& data = std::get<std::vector<T>>(cv.data);
    SelectionVector sv;
    // SQL: any comparison against NULL is unknown, so a NULL row never passes.
    // The all_valid branch keeps the common case (scan output, which can never
    // hold NULL) on the branch-free loop; the guarded loop only runs above an
    // operator that actually produced NULLs, e.g. HAVING over a NULL aggregate.
    if (cv.all_valid) {
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
    } else if (input_sel) {
        sv.indices.reserve(input_sel->size);
        for (int r : input_sel->indices) {
            if (cv.validity[r] && cmp(data[r], threshold)) sv.indices.push_back(r);
        }
    } else {
        sv.indices.reserve(chunk.num_rows);
        for (int r = 0; r < chunk.num_rows; ++r) {
            if (cv.validity[r] && cmp(data[r], threshold)) sv.indices.push_back(r);
        }
    }
    sv.size = static_cast<int>(sv.indices.size());
    return sv;
}

// OR: both inputs have indices in ascending order (preserved from forward
// scan), so the union is an O(n+m) merge. AND no longer needs a set intersect —
// it cascades the left operand's SelectionVector into the right (see
// evalPredicate), which is equivalent for AND but evaluates the right over
// survivors only.
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
//
// Preferred path: compile the subtree once (cached on the filter node) and
// evaluate it a chunk at a time. Only when there is no cache, or compile()
// declines the shape, does this reconstruct a Row per row and call the scalar
// evaluate() — the 231ms-per-million-rows path this replaces.
//
// input_sel: when non-null, only rows in input_sel->indices are evaluated
static SelectionVector evalFallback(const Expr* pred, const DataChunk& chunk, const Schema& schema,
                                    const SelectionVector* input_sel,
                                    PredicateExecutorCache* cache) {
    SelectionVector sv;

    // the row indices under consideration, in order
    const std::vector<int>* indices_ptr = nullptr;
    std::vector<int> all_indices;
    if (input_sel) {
        indices_ptr = &input_sel->indices;
    } else {
        all_indices.resize(chunk.num_rows);
        std::iota(all_indices.begin(), all_indices.end(), 0);
        indices_ptr = &all_indices;
    }

    if (cache) {
        ExpressionExecutor* exec = cache->get(pred, schema);
        // A predicate evaluates to the INT-as-boolean convention. Anything else
        // (e.g. `WHERE speed`, a bare DOUBLE) goes to evaluate(), which raises
        // from asInt() exactly as it did before.
        if (exec && exec->type() == TypeId::INT) {
            const ColumnVector& out = exec->execute(chunk, *indices_ptr);
            const auto& flags = std::get<std::vector<int64_t>>(out.data);
            const int n = static_cast<int>(indices_ptr->size());
            sv.indices.reserve(n);
            for (int i = 0; i < n; ++i) {
                // NULL is not true: an unknown predicate rejects the row
                if (!out.isNull(i) && flags[i] != 0) sv.indices.push_back((*indices_ptr)[i]);
            }
            sv.size = static_cast<int>(sv.indices.size());
            return sv;
        }
    }

    for (int r : *indices_ptr) {
        Row tmp;
        tmp.reserve(chunk.columns.size());
        for (const auto& cv : chunk.columns){
            tmp.push_back(valueAt(cv, r));
        }
        Value v = evaluate(pred, tmp, schema);
        if (!v.isNull() && v.asInt() != 0) sv.indices.push_back(r);
    }
    sv.size = static_cast<int>(sv.indices.size());
    return sv;
}

// main dispatch
SelectionVector evalPredicate(const Expr* pred, const DataChunk& chunk, const Schema& schema,
                              const SelectionVector* input_sel,
                              PredicateExecutorCache* cache) {
    const auto* bin = dynamic_cast<const BinaryExpr*>(pred);

    // not a BinaryExpr (IsNullExpr, AggregateExpr, ...), scalar fallback
    if (!bin) return evalFallback(pred, chunk, schema, input_sel, cache);

    // AND: cascade the selection vector — evaluate the right operand only over
    // rows the left kept. Equivalent to intersect for AND, but the right touches
    // only survivors, so ordering the most-selective conjunct left (Week 21
    // pushdown) pays off. OR must stay a union — it cannot cascade.
    if (bin->op == "AND"){
        SelectionVector left = evalPredicate(bin->left.get(), chunk, schema, input_sel, cache);
        return evalPredicate(bin->right.get(), chunk, schema, &left, cache);
    }
    if (bin->op == "OR"){
        return sv_union(evalPredicate(bin->left.get(), chunk, schema, input_sel, cache),
                        evalPredicate(bin->right.get(), chunk, schema, input_sel, cache));
    }

    // comparison, attempt fast path for (ColumnRef) op (Literal)
    const auto* cr = dynamic_cast<const ColumnRef*>(bin->left.get());
    const auto* lit = dynamic_cast<const Literal*>(bin->right.get());

    if (!cr || !lit || lit->value.isNull()){
        return evalFallback(pred, chunk, schema, input_sel, cache);
    }

    int col_idx = resolveColumnIndex(*cr, schema);
    if (col_idx < 0){
        return evalFallback(pred, chunk, schema, input_sel, cache);
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
    return evalFallback(pred, chunk, schema, input_sel, cache);
}
