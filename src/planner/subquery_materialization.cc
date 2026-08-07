#include "planner/subquery_materialization.h"

#include "planner/constant_folding.h"
#include "parser/expr_utils.h"        // cloneExpr — bridges the GROUP BY shared_ptr
#include "execution/key_encoding.h"   // keyFieldText — exact value identity

#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// DISPATCH SITE 19. Every container subtype, in the same order sites 4 / 8 / 14
// enumerate them. A subtype added later and missed here means its subqueries are
// never materialized, and inferExprType (site 12) throws — loud, at plan time.
void forEachSubquery(std::unique_ptr<Expr>& expr,
                     const std::function<void(std::unique_ptr<Expr>&)>& fn) {
    if (!expr) return;

    if (auto* bin = dynamic_cast<BinaryExpr*>(expr.get())) {
        forEachSubquery(bin->left, fn);
        forEachSubquery(bin->right, fn);
    } else if (auto* isn = dynamic_cast<IsNullExpr*>(expr.get())) {
        forEachSubquery(isn->operand, fn);
    } else if (auto* un = dynamic_cast<UnaryExpr*>(expr.get())) {
        forEachSubquery(un->operand, fn);
    } else if (auto* agg = dynamic_cast<AggregateExpr*>(expr.get())) {
        if (!agg->is_star) forEachSubquery(agg->argument, fn);
    } else if (auto* in = dynamic_cast<InExpr*>(expr.get())) {
        forEachSubquery(in->operand, fn);     // values are literals
    } else if (auto* lk = dynamic_cast<LikeExpr*>(expr.get())) {
        forEachSubquery(lk->operand, fn);
    } else if (auto* c = dynamic_cast<CaseExpr*>(expr.get())) {
        for (auto& w : c->when_clauses) {
            forEachSubquery(w.condition, fn);
            forEachSubquery(w.result, fn);
        }
        forEachSubquery(c->else_expr, fn);
    } else if (auto* sub = dynamic_cast<SubstringExpr*>(expr.get())) {
        forEachSubquery(sub->operand, fn);
        forEachSubquery(sub->start, fn);
        forEachSubquery(sub->length, fn);     // nullptr-safe
    } else if (dynamic_cast<SubqueryExpr*>(expr.get())) {
        // The IN operand belongs to THIS block and may itself hold a subquery,
        // so it is visited before the node that owns it.
        forEachSubquery(static_cast<SubqueryExpr*>(expr.get())->operand, fn);
        fn(expr);      // `expr` is (usually) REPLACED here...
        return;        // ...so it is no longer a SubqueryExpr: do not fall through
    }
    // ColumnRef / Literal / IntervalLiteral: nothing to visit
}

void forEachSubqueryConst(const Expr* expr,
                          const std::function<void(const SubqueryExpr&)>& fn) {
    if (!expr) return;

    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        forEachSubqueryConst(bin->left.get(), fn);
        forEachSubqueryConst(bin->right.get(), fn);
    } else if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) {
        forEachSubqueryConst(isn->operand.get(), fn);
    } else if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        forEachSubqueryConst(un->operand.get(), fn);
    } else if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        if (!agg->is_star) forEachSubqueryConst(agg->argument.get(), fn);
    } else if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        forEachSubqueryConst(in->operand.get(), fn);
    } else if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        forEachSubqueryConst(lk->operand.get(), fn);
    } else if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        for (const auto& w : c->when_clauses) {
            forEachSubqueryConst(w.condition.get(), fn);
            forEachSubqueryConst(w.result.get(), fn);
        }
        forEachSubqueryConst(c->else_expr.get(), fn);
    } else if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        forEachSubqueryConst(sub->operand.get(), fn);
        forEachSubqueryConst(sub->start.get(), fn);
        forEachSubqueryConst(sub->length.get(), fn);
    } else if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        forEachSubqueryConst(sq->operand.get(), fn);
        fn(*sq);
    }
}

namespace {

// Every expression a statement owns in a unique_ptr SLOT. Position is
// WHERE/HAVING only (validateExpr's allow_subqueries flag enforces it), but
// walking the rest costs four lines and stops this pass depending silently on a
// rule enforced in another file.
//
// GROUP BY is the one clause missing here, and only because its expression is a
// shared_ptr (GroupByColumn must stay copyable) rather than a slot this walk can
// replace through. It is walked by walkGroupKeys below, so the defence has no
// hole — see the comment there.
template <typename Fn>
void forEachStatementExpr(SelectStatement& stmt, Fn&& fn) {
    fn(stmt.where);
    fn(stmt.having);
    for (auto& e : stmt.select_list) fn(e);
    for (auto& i : stmt.order_by)    fn(i.expr);
    for (auto& j : stmt.joins)       fn(j.condition);
}

void addTable(std::vector<std::string>& out, const std::string& name) {
    if (std::find(out.begin(), out.end(), name) == out.end()) out.push_back(name);
}

} // namespace

void collectQueryTables(const SelectStatement& stmt, std::vector<std::string>& out) {
    // A self-join names one table twice and main.cc keys its loaded data by
    // table name, so dedupe here rather than at every caller.
    addTable(out, stmt.from_table);
    for (const auto& j : stmt.joins) addTable(out, j.join_table);

    auto descend = [&out](const SubqueryExpr& sq) {
        if (sq.subquery) collectQueryTables(*sq.subquery, out);
    };
    forEachSubqueryConst(stmt.where.get(), descend);
    forEachSubqueryConst(stmt.having.get(), descend);
    for (const auto& g : stmt.group_by)    forEachSubqueryConst(g.expr.get(), descend);
    for (const auto& e : stmt.select_list) forEachSubqueryConst(e.get(), descend);
    for (const auto& i : stmt.order_by)    forEachSubqueryConst(i.expr.get(), descend);
    for (const auto& j : stmt.joins)       forEachSubqueryConst(j.condition.get(), descend);
}

namespace {

// Identity is the STATEMENT ADDRESS, which is the identity exprKey already uses
// for a subquery (expr_utils.h: "@" + address). cloneExpr SHARES the shared_ptr
// rather than deep-copying — SelectStatement is move-only, and a deep-copy
// walker's omissions would be silent — so two SubqueryExpr nodes over ONE
// statement is a real state: `(SELECT MAX(age) FROM drivers) BETWEEN 1 AND 99`
// produces exactly it, because BETWEEN clones its left operand before binding.
//
// Week 30 handed this week the decision explicitly: one subplan or two. ONE run,
// two substitutions. Two textually identical but DISTINCT subqueries still run
// twice — a missed optimization, not a wrong answer; a structural key is Week
// 37's.
//
// !! A KEY OUTLIVES THE STATEMENT IT NAMES, and is therefore COMPARED ONLY,
// never dereferenced. `visit` replaces the SubqueryExpr through its slot, which
// releases the last shared_ptr to that SelectStatement while its address is
// still a live key for the rest of the walk. Nothing reads through the key —
// `cache.find` compares addresses — so this is not a dangling READ. What it
// would be is an ABA collision: a SelectStatement allocated at the recycled
// address would hit this cache and receive ANOTHER subquery's rows, silently.
// That is unreachable for one reason, which is the invariant to preserve:
// EVERY SelectStatement IS ALLOCATED AT PARSE TIME (parser.cc:355/477/639 are
// the only `make_shared<SelectStatement>` in the engine) and parsing has
// finished before this pass begins; nothing here allocates one — `body` at
// runOnce is a stack local, and cloneExpr shares the shared_ptr rather than
// making a second statement. The first week that heap-allocates a
// SelectStatement DURING planning — Week 34's derived tables are the candidate —
// breaks it, and the fix is to hold the shared_ptr in the cache VALUE, which
// pins the address for the pass's lifetime. Adding that pin now would be a field
// no reachable input needs.
using ResultCache = std::unordered_map<const SelectStatement*, SubqueryResult>;

const SubqueryResult& runOnce(SubqueryExpr* sq, const SubqueryRunner& run,
                              ResultCache& cache) {
    auto it = cache.find(sq->subquery.get());
    if (it != cache.end()) return it->second;

    // Innermost first: a body cannot run while it still holds a subquery of its
    // own. This must happen BEFORE the move below — materializing the moved-from
    // husk is a silent no-op that leaves the inner node in place and surfaces as
    // site 12's throw from inside a nested run.
    materializeSubqueries(*sq->subquery, run);

    SelectStatement body = std::move(*sq->subquery);

    // Bound the work where the shape allows it, without ever widening a LIMIT
    // the query wrote. EXISTS needs one row; SCALAR needs two, because "more
    // than one row" is the cardinality error and two rows prove it — LIMIT 1
    // would silently accept a multi-row scalar. LogicalPlanBuilder puts LIMIT
    // above SORT, so a body with its own ORDER BY still returns the right rows.
    if (sq->kind == SubqueryExpr::Kind::EXISTS) {
        body.limit = body.limit ? std::min(*body.limit, 1) : 1;
    } else if (sq->kind == SubqueryExpr::Kind::SCALAR) {
        body.limit = body.limit ? std::min(*body.limit, 2) : 2;
    }

    // MOVING out of the shared statement is safe ONLY because the cache above is
    // consulted first, so a second node over the same statement never reaches
    // here. Anything added later that reads *sq->subquery past this point reads
    // an empty statement.
    auto ins = cache.emplace(sq->subquery.get(), run(std::move(body)));
    return ins.first->second;
}

// Distinct non-null values of the result's single column, in first-seen order,
// with `has_null` reporting whether any row was NULL.
//
// Deduped by keyFieldText, the EXACT identity text (key_encoding.h), never by
// Value::toString(): %.15g collapsed 3245 distinct sums into 2526 texts over the
// shipped dataset, and this list is about value identity for the same reason a
// GROUP BY key is. It also means the INT 7 and the DOUBLE 7.0 collapse, which is
// what Value::operator== does with them anyway.
//
// The dedupe is not tidiness: evaluate()'s InExpr scans this list LINEARLY per
// row, and driver_id has 20 distinct values in 10000 rows.
std::vector<Value> distinctNonNull(const std::vector<Row>& rows, bool* has_null) {
    std::vector<Value> out;
    std::unordered_set<std::string> seen;
    *has_null = false;
    for (const Row& r : rows) {
        if (r.empty()) continue;                      // arity is Validator's
        if (r[0].isNull()) { *has_null = true; continue; }
        if (seen.insert(keyFieldText(r[0])).second) out.push_back(r[0]);
    }
    return out;
}

// The constant that replaces one uncorrelated SubqueryExpr.
std::unique_ptr<Expr> buildReplacement(SubqueryExpr* sq, const SubqueryResult& res) {
    switch (sq->kind) {

    case SubqueryExpr::Kind::EXISTS: {
        // Only existence matters, never values — which is why the Validator
        // gives EXISTS no arity rule at all (Q4 and Q21 both write SELECT *).
        // Boolean-as-INT, the convention inferExprType reports for a predicate.
        const bool exists = !res.rows.empty();
        return std::make_unique<Literal>(
            Value(static_cast<int64_t>(exists != sq->negated)));
    }

    case SubqueryExpr::Kind::SCALAR: {
        // CARDINALITY — the checkpoint's runtime check. ARITY was decidable at
        // bind time from the select list alone and is the Validator's; this one
        // needs data. runOnce capped the body at LIMIT 2, so two rows is proof.
        if (res.rows.size() > 1) {
            throw std::runtime_error("scalar subquery returned more than one row");
        }
        // Zero rows is NULL in SQL, and a one-row result may itself hold a NULL
        // (SELECT AVG(speed) over an empty selection returns one NULL row). Both
        // reach the same place, and both take their TYPE from the body's single
        // output column: Value has no typed null, and inferExprType must answer.
        if (res.rows.empty() || res.rows[0].empty() || res.rows[0][0].isNull()) {
            auto lit = std::make_unique<Literal>(Value::null());
            // Exactly one output column, guaranteed by Validator's arity rule.
            if (res.schema.size() > 0) lit->null_type = res.schema.column(0).type;
            return lit;
        }
        return std::make_unique<Literal>(res.rows[0][0]);
    }

    case SubqueryExpr::Kind::IN: {
        bool has_null = false;
        std::vector<Value> values = distinctNonNull(res.rows, &has_null);

        if (static_cast<int>(values.size()) > MAX_MATERIALIZED_IN_VALUES) {
            throw std::runtime_error(
                "IN subquery produced " + std::to_string(values.size())
                + " distinct values, above the materialization limit of "
                + std::to_string(MAX_MATERIALIZED_IN_VALUES)
                + " (Week 32 lowers this form to a semi-join)");
        }

        // THREE-VALUED IN. InExpr::values is documented "non-empty, never NULL"
        // because the grammar has no NULL literal — but a subquery result is not
        // the grammar, so preserving that invariant is this week's job:
        //
        //   x IN (S)      TRUE on a match, else FALSE without NULLs / UNKNOWN with
        //   x NOT IN (S)  FALSE on a match, else TRUE without NULLs / UNKNOWN with
        //
        // UNKNOWN and FALSE are indistinguishable to every consumer REACHABLE
        // here, which is what makes the collapses below sound rather than
        // convenient. Precisely: a subquery is legal in WHERE and HAVING only, so
        // the ultimate consumer is a filter, and a filter drops UNKNOWN and FALSE
        // alike; there is no general NOT in the grammar (only NOT IN / NOT LIKE /
        // NOT BETWEEN / IS NOT NULL, none of which applies to a predicate's
        // RESULT); `IS [NOT] NULL` parses at the additive level, so
        // `(x IN S) IS NULL` does not parse; three-valued AND/OR differ between
        // UNKNOWN and FALSE only in cases a filter then drops identically; and
        // CASE WHEN does not take an UNKNOWN branch.
        //
        // ADDING A GENERAL `NOT` BREAKS THIS FIRST.
        if (sq->negated && has_null) {
            // never TRUE: FALSE where the operand matches, UNKNOWN elsewhere.
            // This is the classic NOT-IN defect; returning rows here is wrong.
            return std::make_unique<Literal>(Value(int64_t(0)));
        }
        if (values.empty()) {
            // x IN () is FALSE. x NOT IN () is TRUE — even for a NULL x, since
            // there is nothing to compare against. (A set of only NULLs lands
            // here for the positive form: UNKNOWN, which collapses to FALSE.)
            return std::make_unique<Literal>(
                Value(static_cast<int64_t>(sq->negated ? 1 : 0)));
        }

        auto in = std::make_unique<InExpr>();
        in->operand = std::move(sq->operand);   // this block's, already bound and folded
        in->values  = std::move(values);
        in->negated = sq->negated;              // InExpr implements it; do not pre-apply
        return in;
    }
    }
    throw std::runtime_error("internal: unknown SubqueryExpr::Kind");
}

} // namespace

void materializeSubqueries(SelectStatement& stmt, const SubqueryRunner& run) {
    // One bool for the 99% case: every query in the engine calls this.
    if (!stmt.has_subquery) return;

    ResultCache cache;
    auto visit = [&](std::unique_ptr<Expr>& slot) {
        auto* sq = dynamic_cast<SubqueryExpr*>(slot.get());
        if (!sq) return;
        const SubqueryResult& res = runOnce(sq, run, cache);
        std::unique_ptr<Expr> replacement = buildReplacement(sq, res);
        // An aliased constant keeps its name, for the same reason foldNode
        // preserves the alias. Always empty in WHERE/HAVING; kept anyway so the
        // rule does not depend on the position restriction.
        replacement->alias = slot->alias;
        slot = std::move(replacement);
    };
    forEachStatementExpr(stmt, [&](std::unique_ptr<Expr>& e) {
        forEachSubquery(e, visit);
    });

    // GROUP BY, which forEachStatementExpr cannot reach: GroupByColumn::expr is
    // a shared_ptr, and site 19 replaces a node THROUGH ITS SLOT, which needs a
    // unique_ptr. An expression only lands there by alias substitution from the
    // select list (binder.cc), and the Validator refuses a subquery in the
    // select list — so this is unreachable today. It is walked anyway because
    // the alternative is this pass depending SILENTLY on a rule enforced in
    // another file, which is the one thing forEachStatementExpr's comment says
    // it exists to avoid; the day that rule relaxes, a group key holding a
    // SubqueryExpr would otherwise survive into planning.
    //
    // Cloning is what bridges the two ownership shapes, and costs nothing on any
    // real query: only a key that actually contains a subquery is cloned.
    for (auto& g : stmt.group_by) {
        bool has_subquery = false;
        forEachSubqueryConst(g.expr.get(), [&](const SubqueryExpr&) { has_subquery = true; });
        if (!has_subquery) continue;

        std::unique_ptr<Expr> slot = cloneExpr(g.expr.get());
        forEachSubquery(slot, visit);
        g.expr = std::shared_ptr<Expr>(std::move(slot));
    }

    // The flag means "a SubqueryExpr is still in this tree", and none is. This
    // is what returns projection pushdown to a subquery query: buildScanSchema
    // widens to the full schema for as long as the flag is set (Week 30), which
    // Week 30's own hand-forward note predicted would otherwise show up as a
    // surprise in this week's first benchmark.
    stmt.has_subquery = false;

    // A substituted constant can sit under arithmetic — `> (SELECT ...) * 2` —
    // and three fast paths pattern-match on ColumnRef op Literal. Folding
    // touches arithmetic only, never comparisons, and declines any fold that
    // evaluates to NULL, so it cannot change a result or manufacture a second
    // null Literal. Same argument as constant_folding.h.
    foldConstants(stmt);
}
