#include "execution/expression_executor.h"
#include "execution/checked_arith.h"
#include "execution/evaluator.h"
#include "parser/expr_utils.h"
#include "planner/logical_plan.h"
#include <algorithm>
#include <stdexcept>
#include <unordered_set>


// Compiled expression node. Dispatch is resolved at compile() time — kind and
// type are fixed, so execute() runs a switch over an enum instead of a
// dynamic_cast chain, once per node per chunk.
struct ExpressionExecutor::Node {
    enum class Kind {
        COLUMN,        // gather from a chunk column (also aggregate output columns)
        CONSTANT,      // broadcast a literal
        CAST_DOUBLE,   // widen an INT child so arithmetic kernels stay homogeneous
        ARITH,         // + - * /
        COMPARE,       // = != < > <= >=
        LOGICAL,       // AND OR
        NEGATE,        // unary -
        IS_NULL,       // IS NULL / IS NOT NULL
        LIKE,          // LIKE / NOT LIKE against a constant pattern
        IN_SET,        // IN / NOT IN against a constant, pre-hashed value list
        SUBSTRING,     // SUBSTRING(x, start [, length])
    };

    Kind kind;
    TypeId type;                                   // == inferExprType of this subtree
    int column_index = -1;                         // COLUMN
    Value constant;                                // CONSTANT
    std::string op;                                // ARITH / COMPARE / LOGICAL
    bool is_not_null = false;                      // IS_NULL
    bool negated = false;                          // LIKE / IN_SET
    std::string pattern;                           // LIKE
    // IN_SET, hashed once at compile time. Split by storage type to mirror
    // Value::operator== exactly: INT-vs-INT compares the variants (exact),
    // while INT-vs-DOUBLE coerces both to double. Collapsing everything into
    // one double set would make 9007199254740993 IN (9007199254740992) true.
    std::unordered_set<int64_t> in_ints;
    std::unordered_set<double> in_doubles;
    std::unordered_set<std::string> in_strings;
    std::vector<std::unique_ptr<Node>> children;
    ColumnVector out;                              // scratch, reused across chunks
};

namespace {

using Node = ExpressionExecutor::Node;

// ===== scratch management =====

// Size a scratch column to n rows of `type` and clear its validity mask.
//
// The data is deliberately NOT zeroed: every kernel writes all n elements
// before anyone reads them, so the previous chunk's contents are dead. That
// makes resize() a no-op on every chunk after the first (they are all
// BATCH_SIZE) instead of a redundant pass over the buffer, and lets STRING
// columns reuse their existing heap allocations.
//
// The validity mask, by contrast, MUST be cleared: a leftover NULL from the
// previous chunk would silently null out a valid row in this one.
void resetOut(ColumnVector& cv, TypeId type, int n) {
    cv.type = type;
    cv.all_valid = true;
    cv.validity.clear();
    switch (type) {
        case TypeId::INT:
            if (!std::holds_alternative<std::vector<int64_t>>(cv.data))
                cv.data = std::vector<int64_t>();
            std::get<std::vector<int64_t>>(cv.data).resize(n);
            break;
        case TypeId::DOUBLE:
            if (!std::holds_alternative<std::vector<double>>(cv.data))
                cv.data = std::vector<double>();
            std::get<std::vector<double>>(cv.data).resize(n);
            break;
        case TypeId::STRING:
            if (!std::holds_alternative<std::vector<std::string>>(cv.data))
                cv.data = std::vector<std::string>();
            std::get<std::vector<std::string>>(cv.data).resize(n);
            break;
    }
}

// Mark row i NULL, materialising the mask on first use.
void markNull(ColumnVector& cv, int i, int n) {
    if (cv.all_valid) {
        cv.validity.assign(static_cast<size_t>(n), 1);
        cv.all_valid = false;
    }
    cv.validity[i] = 0;
}

// Seed `out`'s validity from one or two operands: a row is valid only where
// every operand is. Returns true when at least one NULL was propagated, so
// kernels can skip the per-row validity test when there are none.
bool propagateNulls(ColumnVector& out, const ColumnVector& a, const ColumnVector* b, int n) {
    if (a.all_valid && (!b || b->all_valid)) return false;
    out.validity.assign(static_cast<size_t>(n), 1);
    out.all_valid = false;
    for (int i = 0; i < n; ++i) {
        if (a.isNull(i) || (b && b->isNull(i))) out.validity[i] = 0;
    }
    return true;
}

// ===== kernels =====

// Gather sel rows out of a source column, carrying validity across.
void runColumn(Node& n, const DataChunk& chunk, const std::vector<int>& sel) {
    const ColumnVector& src = chunk.columns[n.column_index];
    const int count = static_cast<int>(sel.size());
    resetOut(n.out, n.type, count);

    switch (n.type) {
        case TypeId::INT: {
            const auto& s = std::get<std::vector<int64_t>>(src.data);
            auto& d = std::get<std::vector<int64_t>>(n.out.data);
            for (int i = 0; i < count; ++i) d[i] = s[sel[i]];
            break;
        }
        case TypeId::DOUBLE: {
            const auto& s = std::get<std::vector<double>>(src.data);
            auto& d = std::get<std::vector<double>>(n.out.data);
            for (int i = 0; i < count; ++i) d[i] = s[sel[i]];
            break;
        }
        case TypeId::STRING: {
            const auto& s = std::get<std::vector<std::string>>(src.data);
            auto& d = std::get<std::vector<std::string>>(n.out.data);
            for (int i = 0; i < count; ++i) d[i] = s[sel[i]];
            break;
        }
    }

    if (!src.all_valid) {
        for (int i = 0; i < count; ++i) {
            if (src.isNull(sel[i])) markNull(n.out, i, count);
        }
    }
}

// compileNode declines a NULL literal, so n.constant is always non-NULL here.
void runConstant(Node& n, int count) {
    resetOut(n.out, n.type, count);
    switch (n.type) {
        case TypeId::INT:
            std::fill_n(std::get<std::vector<int64_t>>(n.out.data).begin(), count,
                        n.constant.asInt());
            break;
        case TypeId::DOUBLE:
            std::fill_n(std::get<std::vector<double>>(n.out.data).begin(), count,
                        n.constant.toNumeric());
            break;
        case TypeId::STRING:
            std::fill_n(std::get<std::vector<std::string>>(n.out.data).begin(), count,
                        n.constant.asString());
            break;
    }
}

void runCastDouble(Node& n, const ColumnVector& src, int count) {
    resetOut(n.out, TypeId::DOUBLE, count);
    const auto& s = std::get<std::vector<int64_t>>(src.data);
    auto& d = std::get<std::vector<double>>(n.out.data);
    for (int i = 0; i < count; ++i) d[i] = static_cast<double>(s[i]);
    propagateNulls(n.out, src, nullptr, count);
}

// Arithmetic. Both operands share n.type by construction (compile() inserts
// CAST_DOUBLE where needed), so each loop is homogeneous and vectorizable.
//
// Rows that are ALREADY NULL must be skipped, not merely overwritten. runColumn
// gathers the real underlying value even for a NULL row, and that value can
// legitimately be INT64_MIN or 0 — computing on it would raise a spurious
// overflow, or divide by a garbage zero, for a row whose result is never read.
// evaluate() has the same short-circuit: it returns NULL before touching either
// operand. `apply` therefore takes the skip decision once, outside the loop.
template <typename T, typename Op>
inline void applyRowwise(Node& n, std::vector<T>& d, int count, bool has_nulls, Op op) {
    if (has_nulls) {
        for (int i = 0; i < count; ++i) {
            if (n.out.isNull(i)) { d[i] = T{0}; continue; }
            d[i] = op(i);
        }
    } else {
        for (int i = 0; i < count; ++i) d[i] = op(i);
    }
}

// DOUBLE: IEEE arithmetic, no overflow trap. Division by 0.0 is NULL, matching
// evaluator.cc (SQLite semantics) rather than producing an infinity.
inline void arithKernelDouble(Node& n, const std::vector<double>& l, const std::vector<double>& r,
                              std::vector<double>& d, int count, bool has_nulls) {
    const std::string& op = n.op;
    if (op == "+") { applyRowwise(n, d, count, has_nulls, [&](int i){ return l[i] + r[i]; }); return; }
    if (op == "-") { applyRowwise(n, d, count, has_nulls, [&](int i){ return l[i] - r[i]; }); return; }
    if (op == "*") { applyRowwise(n, d, count, has_nulls, [&](int i){ return l[i] * r[i]; }); return; }
    for (int i = 0; i < count; ++i) {
        if (has_nulls && n.out.isNull(i)) { d[i] = 0.0; continue; }
        if (r[i] == 0.0) { markNull(n.out, i, count); d[i] = 0.0; continue; }
        d[i] = l[i] / r[i];
    }
}

// INT: every op is overflow-checked, same rules as evaluator.cc's INT branch.
// Not optional — signed overflow is UB, and at -O2 the compiler may assume it
// away (see checked_arith.h). The check is a flag test the branch predictor gets
// right every time, so the loops stay tight.
inline void arithKernelInt(Node& n, const std::vector<int64_t>& l, const std::vector<int64_t>& r,
                           std::vector<int64_t>& d, int count, bool has_nulls) {
    const std::string& op = n.op;
    if (op == "+") { applyRowwise(n, d, count, has_nulls, [&](int i){ return checkedAdd(l[i], r[i]); }); return; }
    if (op == "-") { applyRowwise(n, d, count, has_nulls, [&](int i){ return checkedSub(l[i], r[i]); }); return; }
    if (op == "*") { applyRowwise(n, d, count, has_nulls, [&](int i){ return checkedMul(l[i], r[i]); }); return; }
    for (int i = 0; i < count; ++i) {
        if (has_nulls && n.out.isNull(i)) { d[i] = 0; continue; }
        if (r[i] == 0) { markNull(n.out, i, count); d[i] = 0; continue; }
        d[i] = checkedDiv(l[i], r[i]);   // INT/INT truncates; guards INT64_MIN / -1
    }
}

void runArith(Node& n, const ColumnVector& lhs, const ColumnVector& rhs, int count) {
    resetOut(n.out, n.type, count);
    bool has_nulls = propagateNulls(n.out, lhs, &rhs, count);
    if (n.type == TypeId::INT) {
        arithKernelInt(n, std::get<std::vector<int64_t>>(lhs.data),
                       std::get<std::vector<int64_t>>(rhs.data),
                       std::get<std::vector<int64_t>>(n.out.data), count, has_nulls);
    } else {
        arithKernelDouble(n, std::get<std::vector<double>>(lhs.data),
                          std::get<std::vector<double>>(rhs.data),
                          std::get<std::vector<double>>(n.out.data), count, has_nulls);
    }
}

// Comparison. Output is INT 0/1 (the engine's boolean convention); a NULL
// operand yields NULL, as in evaluate().
template <typename T>
void compareKernel(const std::string& op, const std::vector<T>& l, const std::vector<T>& r,
                   std::vector<int64_t>& d, int count) {
    if (op == "=")  { for (int i = 0; i < count; ++i) d[i] = l[i] == r[i]; return; }
    if (op == "!=") { for (int i = 0; i < count; ++i) d[i] = l[i] != r[i]; return; }
    if (op == "<")  { for (int i = 0; i < count; ++i) d[i] = l[i] <  r[i]; return; }
    if (op == ">")  { for (int i = 0; i < count; ++i) d[i] = l[i] >  r[i]; return; }
    if (op == "<=") { for (int i = 0; i < count; ++i) d[i] = l[i] <= r[i]; return; }
    for (int i = 0; i < count; ++i) d[i] = l[i] >= r[i];   // ">="
}

void runCompare(Node& n, const ColumnVector& lhs, const ColumnVector& rhs, int count) {
    resetOut(n.out, TypeId::INT, count);
    propagateNulls(n.out, lhs, &rhs, count);
    auto& d = std::get<std::vector<int64_t>>(n.out.data);
    switch (lhs.type) {
        case TypeId::INT:
            compareKernel<int64_t>(n.op, std::get<std::vector<int64_t>>(lhs.data),
                                   std::get<std::vector<int64_t>>(rhs.data), d, count);
            break;
        case TypeId::DOUBLE:
            compareKernel<double>(n.op, std::get<std::vector<double>>(lhs.data),
                                  std::get<std::vector<double>>(rhs.data), d, count);
            break;
        case TypeId::STRING:
            compareKernel<std::string>(n.op, std::get<std::vector<std::string>>(lhs.data),
                                       std::get<std::vector<std::string>>(rhs.data), d, count);
            break;
    }
}

// AND/OR over the INT-as-boolean convention, three-valued to match evaluate().
// Deliberately does NOT call propagateNulls: these connectives can produce a
// definite answer from a NULL operand (false AND NULL = false, true OR NULL =
// true), so pre-marking a row NULL from its operands would be wrong.
void runLogical(Node& n, const ColumnVector& lhs, const ColumnVector& rhs, int count) {
    resetOut(n.out, TypeId::INT, count);
    const auto& l = std::get<std::vector<int64_t>>(lhs.data);
    const auto& r = std::get<std::vector<int64_t>>(rhs.data);
    auto& d = std::get<std::vector<int64_t>>(n.out.data);
    const bool is_and = (n.op == "AND");
    for (int i = 0; i < count; ++i) {
        // tri-state: -1 unknown (NULL), 0 false, 1 true
        const int lv = lhs.isNull(i) ? -1 : (l[i] != 0);
        const int rv = rhs.isNull(i) ? -1 : (r[i] != 0);
        int res;
        if (is_and) res = (lv == 0 || rv == 0) ? 0 : ((lv < 0 || rv < 0) ? -1 : 1);
        else        res = (lv == 1 || rv == 1) ? 1 : ((lv < 0 || rv < 0) ? -1 : 0);
        if (res < 0) { markNull(n.out, i, count); d[i] = 0; }
        else         { d[i] = res; }
    }
}

void runNegate(Node& n, const ColumnVector& src, int count) {
    resetOut(n.out, n.type, count);
    bool has_nulls = propagateNulls(n.out, src, nullptr, count);
    if (n.type == TypeId::INT) {
        const auto& s = std::get<std::vector<int64_t>>(src.data);
        auto& d = std::get<std::vector<int64_t>>(n.out.data);
        // skip NULL rows: -INT64_MIN is UB, and a NULL row's gathered value can
        // be INT64_MIN even though its result is never read
        applyRowwise(n, d, count, has_nulls, [&](int i){ return checkedNegate(s[i]); });
    } else {
        const auto& s = std::get<std::vector<double>>(src.data);
        auto& d = std::get<std::vector<double>>(n.out.data);
        applyRowwise(n, d, count, has_nulls, [&](int i){ return -s[i]; });
    }
}

// IS NULL / IS NOT NULL reads the operand's mask and is itself never NULL.
void runIsNull(Node& n, const ColumnVector& src, int count) {
    resetOut(n.out, TypeId::INT, count);
    auto& d = std::get<std::vector<int64_t>>(n.out.data);
    for (int i = 0; i < count; ++i) {
        bool is_null = src.isNull(i);
        d[i] = n.is_not_null ? !is_null : is_null;
    }
}

// LIKE. The matcher itself is evaluator.cc's likeMatch — the same function the
// scalar path calls, so the two cannot disagree about wildcards or case folding.
// What is hoisted out of the row loop is the dispatch, not the matching.
void runLike(Node& n, const ColumnVector& src, int count) {
    resetOut(n.out, TypeId::INT, count);
    bool has_nulls = propagateNulls(n.out, src, nullptr, count);
    const auto& s = std::get<std::vector<std::string>>(src.data);
    auto& d = std::get<std::vector<int64_t>>(n.out.data);
    for (int i = 0; i < count; ++i) {
        // a NULL row's gathered value is a placeholder empty string; matching it
        // would be wasted work and its result is never read
        if (has_nulls && n.out.isNull(i)) { d[i] = 0; continue; }
        bool m = likeMatch(s[i], n.pattern);
        d[i] = n.negated ? !m : m;
    }
}

// IN over the pre-hashed constant set. O(1) per row instead of k comparisons —
// TPC-H Q22 probes a 7-element list with a SUBSTRING on the left.
void runInSet(Node& n, const ColumnVector& src, int count) {
    resetOut(n.out, TypeId::INT, count);
    bool has_nulls = propagateNulls(n.out, src, nullptr, count);
    auto& d = std::get<std::vector<int64_t>>(n.out.data);

    auto emit = [&](int i, bool found) { d[i] = n.negated ? !found : found; };

    switch (src.type) {
        case TypeId::INT: {
            const auto& s = std::get<std::vector<int64_t>>(src.data);
            for (int i = 0; i < count; ++i) {
                if (has_nulls && n.out.isNull(i)) { d[i] = 0; continue; }
                // both sets: INT values compare exactly, DOUBLE values coerce —
                // exactly what Value::operator== does for each pair
                emit(i, n.in_ints.count(s[i]) != 0 ||
                        n.in_doubles.count(static_cast<double>(s[i])) != 0);
            }
            break;
        }
        case TypeId::DOUBLE: {
            const auto& s = std::get<std::vector<double>>(src.data);
            for (int i = 0; i < count; ++i) {
                if (has_nulls && n.out.isNull(i)) { d[i] = 0; continue; }
                emit(i, n.in_doubles.count(s[i]) != 0);
            }
            break;
        }
        case TypeId::STRING: {
            const auto& s = std::get<std::vector<std::string>>(src.data);
            for (int i = 0; i < count; ++i) {
                if (has_nulls && n.out.isNull(i)) { d[i] = 0; continue; }
                emit(i, n.in_strings.count(s[i]) != 0);
            }
            break;
        }
    }
}

// SUBSTRING. children are [operand, start] or [operand, start, length].
// Delegates to evaluator.cc's substringOf, so the 1-based indexing and the
// out-of-domain errors are defined in exactly one place.
void runSubstring(Node& n, const ColumnVector& str, const ColumnVector& start,
                  const ColumnVector* len, int count) {
    resetOut(n.out, TypeId::STRING, count);
    bool has_nulls = propagateNulls(n.out, str, &start, count);
    if (len && !len->all_valid) {
        // third operand: fold its validity in too (propagateNulls takes two)
        for (int i = 0; i < count; ++i) {
            if (len->isNull(i)) { markNull(n.out, i, count); has_nulls = true; }
        }
    }

    const auto& s  = std::get<std::vector<std::string>>(str.data);
    const auto& b  = std::get<std::vector<int64_t>>(start.data);
    const std::vector<int64_t>* l =
        len ? &std::get<std::vector<int64_t>>(len->data) : nullptr;
    auto& d = std::get<std::vector<std::string>>(n.out.data);

    for (int i = 0; i < count; ++i) {
        // skip NULL rows: their gathered start is a placeholder 0, which
        // substringOf would reject with "start position must be >= 1" for a
        // row whose result is never read. evaluate() short-circuits the same way.
        if (has_nulls && n.out.isNull(i)) { d[i].clear(); continue; }
        d[i] = substringOf(s[i], b[i], l != nullptr, l ? (*l)[i] : 0);
    }
}

// Post-order evaluation. One dispatch per node per chunk, not per row.
const ColumnVector& runNode(Node& n, const DataChunk& chunk, const std::vector<int>& sel) {
    const int count = static_cast<int>(sel.size());
    switch (n.kind) {
        case Node::Kind::COLUMN:
            runColumn(n, chunk, sel);
            break;
        case Node::Kind::CONSTANT:
            runConstant(n, count);
            break;
        case Node::Kind::CAST_DOUBLE:
            runCastDouble(n, runNode(*n.children[0], chunk, sel), count);
            break;
        case Node::Kind::ARITH: {
            const ColumnVector& l = runNode(*n.children[0], chunk, sel);
            const ColumnVector& r = runNode(*n.children[1], chunk, sel);
            runArith(n, l, r, count);
            break;
        }
        case Node::Kind::COMPARE: {
            const ColumnVector& l = runNode(*n.children[0], chunk, sel);
            const ColumnVector& r = runNode(*n.children[1], chunk, sel);
            runCompare(n, l, r, count);
            break;
        }
        case Node::Kind::LOGICAL: {
            const ColumnVector& l = runNode(*n.children[0], chunk, sel);
            const ColumnVector& r = runNode(*n.children[1], chunk, sel);
            runLogical(n, l, r, count);
            break;
        }
        case Node::Kind::NEGATE:
            runNegate(n, runNode(*n.children[0], chunk, sel), count);
            break;
        case Node::Kind::IS_NULL:
            runIsNull(n, runNode(*n.children[0], chunk, sel), count);
            break;
        case Node::Kind::LIKE:
            runLike(n, runNode(*n.children[0], chunk, sel), count);
            break;
        case Node::Kind::IN_SET:
            runInSet(n, runNode(*n.children[0], chunk, sel), count);
            break;
        case Node::Kind::SUBSTRING: {
            const ColumnVector& s = runNode(*n.children[0], chunk, sel);
            const ColumnVector& b = runNode(*n.children[1], chunk, sel);
            // runNode returns a reference into each node's own scratch, so the
            // three results coexist safely — different nodes, different buffers
            const ColumnVector* l = n.children.size() > 2
                ? &runNode(*n.children[2], chunk, sel) : nullptr;
            runSubstring(n, s, b, l, count);
            break;
        }
    }
    return n.out;
}

// ===== compilation =====

bool isArithOp(const std::string& op) {
    return op == "+" || op == "-" || op == "*" || op == "/";
}

bool isCompareOp(const std::string& op) {
    return op == "=" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
}

// Wrap an INT node in a widening cast so a binary kernel sees two operands of
// the same type. Returns the node unchanged when it is already DOUBLE.
std::unique_ptr<Node> widenToDouble(std::unique_ptr<Node> child) {
    if (child->type == TypeId::DOUBLE) return child;
    auto cast = std::make_unique<Node>();
    cast->kind = Node::Kind::CAST_DOUBLE;
    cast->type = TypeId::DOUBLE;
    cast->children.push_back(std::move(child));
    return cast;
}

// Returns nullptr for anything without a kernel. Callers treat that as
// "keep using evaluate()", so declining is always safe.
std::unique_ptr<Node> compileNode(const Expr* expr, const Schema& schema) {
    if (!expr) return nullptr;

    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        // The grammar has no NULL literal (NULL appears only in IS NULL), and a
        // null Value has no type() to broadcast. Decline rather than invent one:
        // Value::type() throws on null, and compile() must not throw.
        //
        // Week 31 made this branch REACHABLE for the first time: a materialized
        // scalar subquery that returned zero rows (or one NULL row) substitutes
        // a null Literal. Declining cascades — the enclosing predicate compiles
        // to nullptr and the whole conjunct falls back to evaluate() per row,
        // which is the correct-but-slow path CASE already uses. That is why no
        // kernel has to learn about null constants, and it costs little in
        // practice: the comparison is UNKNOWN for every row, so the query it
        // guards returns nothing anyway. The node still carries the type
        // (Literal::null_type) for inferExprType; broadcasting a constant
        // validity mask is not worth a kernel until a query needs it.
        if (lit->value.isNull()) return nullptr;
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::CONSTANT;
        n->constant = lit->value;
        n->type = lit->value.type();
        return n;
    }

    // A ColumnRef and an AggregateExpr are the same thing at execution time: a
    // read of one already-computed input column. The aggregate's output column
    // is named by aggregateOutputName, which is the shared contract with
    // buildAggregateSchema (see evaluate()'s AggregateExpr case).
    int col_idx = -1;
    if (auto* cr = dynamic_cast<const ColumnRef*>(expr)) {
        col_idx = resolveColumnIndex(*cr, schema);
    } else if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        col_idx = schema.indexOf(aggregateOutputName(agg));
    }
    if (col_idx >= 0) {
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::COLUMN;
        n->column_index = col_idx;
        n->type = schema.column(col_idx).type;
        return n;
    }
    // an unresolved ColumnRef/AggregateExpr is not compilable
    if (dynamic_cast<const ColumnRef*>(expr) || dynamic_cast<const AggregateExpr*>(expr)) {
        return nullptr;
    }

    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        if (un->op != "-") return nullptr;
        auto child = compileNode(un->operand.get(), schema);
        if (!child || child->type == TypeId::STRING) return nullptr;
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::NEGATE;
        n->type = child->type;
        n->children.push_back(std::move(child));
        return n;
    }

    if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) {
        auto child = compileNode(isn->operand.get(), schema);
        if (!child) return nullptr;
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::IS_NULL;
        n->type = TypeId::INT;
        n->is_not_null = isn->is_not_null;
        n->children.push_back(std::move(child));
        return n;
    }

    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        auto lhs = compileNode(bin->left.get(), schema);
        auto rhs = compileNode(bin->right.get(), schema);
        if (!lhs || !rhs) return nullptr;

        if (isArithOp(bin->op)) {
            if (lhs->type == TypeId::STRING || rhs->type == TypeId::STRING) return nullptr;
            auto n = std::make_unique<Node>();
            n->kind = Node::Kind::ARITH;
            n->op = bin->op;
            // INT/INT stays INT (SQLite truncating division); any DOUBLE
            // promotes, and the INT side is widened so the kernel is homogeneous
            n->type = (lhs->type == TypeId::INT && rhs->type == TypeId::INT)
                ? TypeId::INT : TypeId::DOUBLE;
            if (n->type == TypeId::DOUBLE) {
                lhs = widenToDouble(std::move(lhs));
                rhs = widenToDouble(std::move(rhs));
            }
            n->children.push_back(std::move(lhs));
            n->children.push_back(std::move(rhs));
            return n;
        }

        if (isCompareOp(bin->op)) {
            // STRING vs numeric throws in Value's comparison operators; decline
            // so the fallback raises the same error from the same place
            bool l_str = lhs->type == TypeId::STRING;
            bool r_str = rhs->type == TypeId::STRING;
            if (l_str != r_str) return nullptr;
            auto n = std::make_unique<Node>();
            n->kind = Node::Kind::COMPARE;
            n->op = bin->op;
            n->type = TypeId::INT;
            if (!l_str && lhs->type != rhs->type) {
                lhs = widenToDouble(std::move(lhs));
                rhs = widenToDouble(std::move(rhs));
            }
            n->children.push_back(std::move(lhs));
            n->children.push_back(std::move(rhs));
            return n;
        }

        if (bin->op == "AND" || bin->op == "OR") {
            // evaluate() calls asInt() on both operands, which requires INT
            if (lhs->type != TypeId::INT || rhs->type != TypeId::INT) return nullptr;
            auto n = std::make_unique<Node>();
            n->kind = Node::Kind::LOGICAL;
            n->op = bin->op;
            n->type = TypeId::INT;
            n->children.push_back(std::move(lhs));
            n->children.push_back(std::move(rhs));
            return n;
        }

        return nullptr;   // unknown operator
    }

    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        auto child = compileNode(lk->operand.get(), schema);
        if (!child || child->type != TypeId::STRING) return nullptr;
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::LIKE;
        n->type = TypeId::INT;
        n->pattern = lk->pattern;
        n->negated = lk->negated;
        n->children.push_back(std::move(child));
        return n;
    }

    if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        auto child = compileNode(in->operand.get(), schema);
        if (!child) return nullptr;
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::IN_SET;
        n->type = TypeId::INT;
        n->negated = in->negated;
        // Hash the list once. A STRING operand admits only STRING values and a
        // numeric operand only numeric ones — the mixed shape throws in Value's
        // comparison operators, so decline it here and let the fallback raise
        // the same error from the same place.
        for (const Value& v : in->values) {
            const bool v_str = (v.type() == TypeId::STRING);
            if (v_str != (child->type == TypeId::STRING)) return nullptr;
            if (v_str)                          n->in_strings.insert(v.asString());
            else if (v.type() == TypeId::INT)   n->in_ints.insert(v.asInt());
            else                                n->in_doubles.insert(v.asDouble());
        }
        // A DOUBLE operand coerces every comparison to double, so fold the INT
        // values into the double set and probe just one.
        if (child->type == TypeId::DOUBLE) {
            for (int64_t i : n->in_ints) n->in_doubles.insert(static_cast<double>(i));
            n->in_ints.clear();
        }
        n->children.push_back(std::move(child));
        return n;
    }

    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        auto str = compileNode(sub->operand.get(), schema);
        auto beg = compileNode(sub->start.get(), schema);
        if (!str || !beg) return nullptr;
        if (str->type != TypeId::STRING || beg->type != TypeId::INT) return nullptr;
        auto n = std::make_unique<Node>();
        n->kind = Node::Kind::SUBSTRING;
        n->type = TypeId::STRING;
        n->children.push_back(std::move(str));
        n->children.push_back(std::move(beg));
        if (sub->length) {
            auto len = compileNode(sub->length.get(), schema);
            if (!len || len->type != TypeId::INT) return nullptr;
            n->children.push_back(std::move(len));
        }
        return n;
    }

    // CaseExpr is deliberately NOT compiled. evaluate() short-circuits — it
    // never touches an untaken branch — and a chunk kernel cannot, so
    // `CASE WHEN i < 100 THEN i * 1000000000000 ELSE 0 END` would raise a
    // checkedMul overflow here for rows whose branch is discarded. Declining
    // keeps the fallback correct-but-slow, which is the contract; a masked
    // kernel (evaluate each branch only over its own selection) is the fix when
    // TPC-H Q8/Q12/Q14 profiling justifies it.

    return nullptr;   // unknown Expr subtype — a new node type lands here
}

} // namespace


ExpressionExecutor::ExpressionExecutor(std::unique_ptr<Node> root) : root_(std::move(root)) {}

ExpressionExecutor::~ExpressionExecutor() = default;

std::unique_ptr<ExpressionExecutor> ExpressionExecutor::compile(const Expr* expr,
                                                                const Schema& input_schema) {
    auto root = compileNode(expr, input_schema);
    if (!root) return nullptr;

    // Guard the type contract: callers pre-allocate output columns from
    // inferExprType, so a disagreement here would be a silent wrong-type write.
    // inferExprType throws on ill-typed trees; declining is the safe answer.
    try {
        if (inferExprType(expr, input_schema) != root->type) return nullptr;
    } catch (const std::runtime_error&) {
        return nullptr;
    }

    return std::unique_ptr<ExpressionExecutor>(new ExpressionExecutor(std::move(root)));
}

const ColumnVector& ExpressionExecutor::execute(const DataChunk& chunk,
                                                const std::vector<int>& sel) {
    return runNode(*root_, chunk, sel);
}

TypeId ExpressionExecutor::type() const {
    return root_->type;
}
