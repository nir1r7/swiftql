#include "execution/evaluator.h"
#include "execution/checked_arith.h"
#include "parser/expr_utils.h"
#include <stdexcept>


// LIKE pattern matcher: '%' matches any sequence, '_' any single character.
// Two-pointer greedy with one backtrack point — O(n*m) worst case, O(n) for the
// prefix / suffix / contains shapes TPC-H actually uses.
//
// ASCII case-INSENSITIVE, matching SQLite's default LIKE. That keeps
// compare_against_sqlite.py a valid oracle: with case-sensitive matching, any
// test query whose pattern case differs from the data would diverge from the
// reference for a reason that has nothing to do with the engine.
bool likeMatch(const std::string& text, const std::string& pat) {
    // Strict ASCII A-Z, which is what SQLite's LIKE folds. std::tolower is
    // locale-dependent — under an ISO-8859-1 locale it maps 0xC9 to 0xE9 — so
    // using it would make LIKE results depend on the process locale and quietly
    // break the "ASCII case-insensitive" contract on non-ASCII bytes. The engine
    // never calls setlocale today, so this is a latent hazard rather than a live
    // bug; spelling the fold out removes it and costs nothing.
    auto fold = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };

    size_t t = 0, p = 0, star_p = std::string::npos, star_t = 0;
    while (t < text.size()) {
        if (p < pat.size() && (pat[p] == '_' || fold(pat[p]) == fold(text[t]))) {
            ++t; ++p;
        } else if (p < pat.size() && pat[p] == '%') {
            star_p = p++;     // remember the wildcard...
            star_t = t;       // ...and where the text stood when we took it
        } else if (star_p != std::string::npos) {
            p = star_p + 1;   // backtrack: let that % consume one more character
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '%') ++p;   // trailing %s match the empty tail
    return p == pat.size();
}


// SUBSTRING with SQL/SQLite 1-based indexing. has_length == false is an omitted
// FOR clause, meaning "to the end of the string".
//
// SQLite DOES define the out-of-domain cases — substr(x,0,3) drops the phantom
// position 0, a negative start counts from the right, a negative length takes
// the preceding characters. SwiftQL rejects all three instead. That is a
// deliberate documented divergence (see the dialect table in readme.md), not an
// oversight, but it is a real one: unlike column ordinals or unary '+', which
// are parse-time rejections, this can fire per row. inferExprType decides it at
// plan time whenever the arguments are constant, which after foldConstants is
// every realistic query; only a computed position reaches this throw.
//
// Shared with the ExpressionExecutor kernel so the two agree by construction
// rather than by review.
std::string substringOf(const std::string& s, int64_t start, bool has_length, int64_t length) {
    if (start < 1)
        throw std::runtime_error("SUBSTRING: start position must be >= 1");
    if (has_length && length < 0)
        throw std::runtime_error("SUBSTRING: length must be >= 0");

    size_t from = static_cast<size_t>(start - 1);
    if (from >= s.size()) return std::string();   // entirely past the end
    size_t take = has_length ? static_cast<size_t>(length) : std::string::npos;
    return s.substr(from, take);                  // substr clamps to what remains
}


int resolveColumnIndex(const ColumnRef& col, const Schema& schema){
    if (col.id.isResolved() && col.id.isLocal()) {
        int idx = schema.indexOf(col.column_name, col.id.localSlot("evaluate"));
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
        const std::string& op = bin->op;

        // AND/OR are three-valued and must be handled BEFORE the blanket NULL
        // propagation below: they can reach a definite answer with a NULL operand
        // (false AND NULL = false, true OR NULL = true). Propagating NULL instead
        // made Volcano answer `WHERE (season/0) > 0 OR 1 = 1` with zero rows while
        // the vectorized path answered with all of them — two engines disagreeing
        // on the same query, which Phase 5 cannot ship as "a documented dialect".
        // TPC-H Q19 is an OR chain over nullable columns.
        //
        // THIS BRANCH IS EAGER AND STAYS EAGER. It is the VALUE of `a AND b`,
        // which SQL defines with no evaluation order, and a caller that wants
        // the value (a SELECT list, a CASE arm) must get the exact 3VL answer.
        // A FILTER wants something else — the row set each conjunct is
        // evaluated on — and that is evaluatePredicate() at the bottom of this
        // file. Seam audit pass 4's E-13 is what happens when a filter uses this
        // one: it evaluates the right conjunct on rows the left rejected, and
        // the vectorized cascade does not.
        if (op == "AND" || op == "OR") {
            // tri-state: -1 unknown (NULL), 0 false, 1 true
            const int l = left.isNull()  ? -1 : (left.asInt()  != 0);
            const int r = right.isNull() ? -1 : (right.asInt() != 0);
            if (op == "AND") {
                if (l == 0 || r == 0) return Value(static_cast<int64_t>(0));  // false dominates
                if (l < 0 || r < 0)   return Value::null();
                return Value(static_cast<int64_t>(1));
            }
            if (l == 1 || r == 1) return Value(static_cast<int64_t>(1));      // true dominates
            if (l < 0 || r < 0)   return Value::null();
            return Value(static_cast<int64_t>(0));
        }

        // every other operator propagates NULL
        if (left.isNull() || right.isNull()) return Value::null();

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
                // checked: signed overflow is UB, not wraparound (checked_arith.h)
                if (op == "+") return Value(checkedAdd(l, r));
                if (op == "-") return Value(checkedSub(l, r));
                if (op == "*") return Value(checkedMul(l, r));
                // SQLite semantics: INT/INT truncates, x/0 is NULL
                return r == 0 ? Value::null() : Value(checkedDiv(l, r));
            }
            double l = left.toNumeric(), r = right.toNumeric();
            if (op == "+") return Value(l + r);
            if (op == "-") return Value(l - r);
            if (op == "*") return Value(l * r);
            return r == 0.0 ? Value::null() : Value(l / r);
        }

        throw std::runtime_error("Unknown binary operator: " + op);
    }

    // unary minus
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        Value v = evaluate(un->operand.get(), row, schema);
        if (v.isNull()) return Value::null();
        if (v.type() == TypeId::INT)    return Value(checkedNegate(v.asInt()));
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

    // IN over a constant list. The list can never contain NULL (the grammar has
    // no NULL literal and the parser takes literals only), so SQL's three-valued
    // IN rule collapses to this single unknown case.
    if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        Value v = evaluate(in->operand.get(), row, schema);
        if (v.isNull()) return Value::null();
        bool found = false;
        for (const Value& c : in->values) {
            // Value::operator== coerces INT against DOUBLE and throws on
            // STRING-vs-numeric; inferExprType rejects that shape at plan time
            if (v == c) { found = true; break; }
        }
        return Value(static_cast<int64_t>(in->negated ? !found : found));
    }

    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        Value v = evaluate(lk->operand.get(), row, schema);
        if (v.isNull()) return Value::null();          // NULL LIKE x is NULL
        if (v.type() != TypeId::STRING)
            throw std::runtime_error("LIKE requires a STRING operand");
        bool m = likeMatch(v.asString(), lk->pattern);
        return Value(static_cast<int64_t>(lk->negated ? !m : m));
    }

    // CASE short-circuits: an untaken branch is never evaluated. That is why
    // ExpressionExecutor::compileNode deliberately declines CaseExpr — an eager
    // chunk kernel would raise checkedMul overflow on rows whose branch is
    // discarded, and the differential tests would then be right to fail.
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        for (const auto& w : c->when_clauses) {
            Value cond = evaluate(w.condition.get(), row, schema);
            // NULL is not true — an unknown condition falls through, as in WHERE
            if (cond.isNull() || cond.asInt() == 0) continue;
            return evaluate(w.result.get(), row, schema);
        }
        return c->else_expr ? evaluate(c->else_expr.get(), row, schema) : Value::null();
    }

    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        Value s = evaluate(sub->operand.get(), row, schema);
        Value b = evaluate(sub->start.get(), row, schema);
        Value n = sub->length ? evaluate(sub->length.get(), row, schema) : Value::null();
        if (s.isNull() || b.isNull() || (sub->length && n.isNull())) return Value::null();
        if (s.type() != TypeId::STRING)
            throw std::runtime_error("SUBSTRING requires a STRING operand");
        return Value(substringOf(s.asString(), b.asInt(),
                                 sub->length != nullptr, sub->length ? n.asInt() : 0));
    }

    // An interval that reaches execution was never folded, which means it was
    // not part of a constant date expression. Loud by design — see ast.h.
    if (dynamic_cast<const IntervalLiteral*>(expr)) {
        throw std::runtime_error(
            "INTERVAL is only valid in constant date arithmetic, "
            "e.g. date '1994-01-01' + interval '1' year");
    }

    // DISPATCH SITE 13, closed in Week 31 — as an INTERNAL invariant, not as a
    // feature, and in the same commit as site 12 (inferExprType), because
    // evaluate() is the semantic reference the vectorized kernels are checked
    // against and the two must never disagree about what a node means.
    //
    // Every UNCORRELATED subquery is replaced by a constant before planning
    // (materializeSubqueries). Week 33: this used to add "a correlated one is
    // refused by the Validator", which is no longer true — see the same
    // correction at dispatch site 12 (logical_plan.cc) for what holds instead.
    // Reaching this still means the materialization walker (dispatch site 19)
    // missed an Expr subtype. That is what makes site 19 loud — do not delete
    // it.
    if (dynamic_cast<const SubqueryExpr*>(expr)) {
        throw std::runtime_error(
            "internal: a subquery reached evaluation without being "
            "materialized (materializeSubqueries must run before planning)");
    }

    throw std::runtime_error("evaluate(): unknown Expr subtype");
}


// ── PREDICATE EVALUATION — THE VOLCANO HALF OF ONE RULE ─────────────────────
//
// SEAM AUDIT PASS 4, E-13. `evaluate()` above computes BOTH operands of an AND
// before applying the tri-state rule, and `evalPredicate` (columnar_eval.cc)
// CASCADES the selection vector — it evaluates the right conjunct only over the
// rows the left kept. Since per-row evaluation is not total, that is not two
// implementations of one semantics; it is two different semantics, and it was
// measured as such on the shipped `drivers` table:
//
//   SELECT name, age FROM drivers WHERE age > 30 AND SUBSTRING(name, age-30, 3) = 'er_'
//     Volcano     Error: SUBSTRING: start position must be >= 1
//     vectorized  the correct three rows
//
// The vectorized answer is the right one — the guard `age > 30` is exactly what
// makes `age - 30 >= 1` well-defined — and it is the one this function now
// implements, so the ENGINE no longer decides. The rule, stated once in
// parser/expr_totality.h and obeyed by the optimizer and the chunk pruner as
// well:
//
//   A CONJUNCT IS EVALUATED ON THE ROWS FOR WHICH EVERY CONJUNCT WRITTEN BEFORE
//   IT EVALUATED TRUE.
//
// Consequences worth stating rather than discovering:
//
//   * AND IS NOT COMMUTATIVE for error behaviour. `p AND q` and `q AND p` are
//     different programs when q can raise, exactly as they are in C or in
//     SQLite. That is the definition, not a defect — and it is what obliges
//     PredicatePushdown to freeze a raising conjunct in its written position.
//   * OR IS EAGER, in both engines. `sv_union` cannot cascade (it needs both
//     sides over the SAME input rows), so this function evaluates both operands
//     of an OR too. The two engines agree; the asymmetry with AND is a property
//     of the connectives, and `evalPredicate`'s OR branch is its twin.
//   * ONE INEXACTNESS, deliberate. Three-valued AND says NULL AND FALSE = FALSE;
//     this returns UNKNOWN, because the right operand was not evaluated. Both
//     answers REJECT the row, and this function's contract is "is it TRUE", so
//     no caller can observe the difference. `evaluate()` is untouched and still
//     returns the exact 3VL value for `SELECT a AND b`, where it IS observable.
//     Cascading on TRUE rather than on non-FALSE is what makes this identical to
//     the selection-vector cascade, which is the whole point.
int evaluatePredicate(const Expr* pred, const Row& row, const Schema& schema) {
    if (auto* bin = dynamic_cast<const BinaryExpr*>(pred)) {
        if (bin->op == "AND") {
            const int l = evaluatePredicate(bin->left.get(), row, schema);
            if (l != 1) return l;   // FALSE or UNKNOWN: the right is not reached
            return evaluatePredicate(bin->right.get(), row, schema);
        }
        if (bin->op == "OR") {
            const int l = evaluatePredicate(bin->left.get(), row, schema);
            const int r = evaluatePredicate(bin->right.get(), row, schema);
            if (l == 1 || r == 1) return 1;      // true dominates
            if (l < 0 || r < 0) return -1;
            return 0;
        }
    }
    const Value v = evaluate(pred, row, schema);
    return v.isNull() ? -1 : (v.asInt() != 0);
}


