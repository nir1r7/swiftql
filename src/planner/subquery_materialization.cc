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

// ===================================================================
// WHICH SCALAR SUBQUERIES ARE DIVIDED BY AN INTEGER
//
// The vectorized path stores a chunk column as ONE type, so a body whose select
// item mixes INTEGER and REAL branches (a CASE is the general route) hands back
// its INT flattened to REAL. That is correct for the body's own plan — the
// value is the same number and prints the same — and it is a WRONG ANSWER for
// `7 / (SELECT ...)`, because Volcano and SQLite truncate INTEGER/INTEGER and
// the flattened REAL does not: seam subquery pass 4 measured 10000 rows where
// both Volcano modes and SQLite return 0.
//
// The engine already refuses this shape when it is ONE plan (a derived table, a
// correlated body decorrelated into the same tree). It could not see this one
// because materializeSubqueries cuts the query into two independent builds and
// the arming walk runs once per build — the body's plan holds no `/` and the
// outer plan holds no INT. So the question is asked HERE, before the cut, where
// both halves are still in one AST, and the answer is handed to the runner.
//
// It is asked at the AST, so the operand types come from the catalog rather
// than from a plan schema. Only ONE thing has to be decided: can the OTHER
// operand of the `/` be an INTEGER? A REAL other operand cannot truncate in
// either engine, so `speed / (SELECT ...)` and `x / 2.0` must NOT arm — that
// half of the rule is the one fix round 3 left out, and it cost `x / 2.0` its
// answer (E-14). Unknown answers INTEGER, which over-refuses rather than
// under-refuses. What is left unknown is therefore worth keeping small, and the
// two remaining cases are named rather than discovered: a correlated reference
// (this walk has no enclosing scope) and a derived relation nested more than
// MAX_TYPE_DEPTH levels deep. Week 37: the second of those is now TRUE. It used
// to be a claim rather than a fact — exprMayBeInt dropped the catalog, so the
// budget was spent but the second level had nothing to resolve against and
// answered INTEGER regardless. See exprMayBeInt.
// ===================================================================

// The statement's own range table in slot order. A catalog relation contributes
// its schema; a DERIVED one contributes its BODY, whose select list is walked
// for the one column being asked about — `FROM (SELECT l.speed AS s FROM laps)`
// is a REAL column, and calling it unknown (and therefore INTEGER) refuses a
// query that is right today. That was measured, not imagined: without the
// derived arm, `t.s / (SELECT <mixed CASE>)` over exactly that body went from
// 10000 rows to a refusal.
struct RangeEntry {
    const Schema* schema = nullptr;          // a catalog relation
    const SelectStatement* body = nullptr;   // a derived one
};
using RangeTable = std::vector<RangeEntry>;

// How many derived-relation levels the type question will descend before
// answering INTEGER. Three is past every shape in the corpus (TPC-H's deepest is
// one) and the budget exists to bound the walk, not to express a limit.
constexpr int MAX_TYPE_DEPTH = 3;

RangeTable rangeTableOf(const SelectStatement& stmt, const Catalog* catalog);
bool exprMayBeInt(const Expr* e, const RangeTable& rt, const Catalog* catalog, int depth);

RangeTable rangeTableOf(const SelectStatement& stmt, const Catalog* catalog) {
    RangeTable rt;
    if (!catalog) return rt;
    auto push = [&](const TableRef& ref) {
        if (ref.isDerived()) { rt.push_back({nullptr, ref.body()}); return; }
        const std::string& name = ref.tableName("subquery division operand typing");
        rt.push_back({catalog->hasTable(name) ? &catalog->getTable(name).schema : nullptr,
                      nullptr});
    };
    push(stmt.from);
    for (const auto& j : stmt.joins) push(j.relation);
    return rt;
}

// The output-column names of a derived body, by the same three rules
// buildProjectSchema uses (alias wins; then a ColumnRef's own name; then
// aggregateOutputName). A `SELECT *` body has no select list to match, so it is
// resolved against the body's OWN range table instead.
bool derivedColumnMayBeInt(const SelectStatement& body, const std::string& name,
                           const Catalog* catalog, int depth) {
    if (depth <= 0) return true;   // nested derived tables: stop, conservatively
    const RangeTable inner = rangeTableOf(body, catalog);
    if (body.select_star || body.select_list.empty()) {
        ColumnRef probe;
        probe.column_name = name;
        return exprMayBeInt(&probe, inner, catalog, depth - 1);
    }
    for (const auto& e : body.select_list) {
        std::string out = e->alias;
        if (out.empty()) {
            if (auto* cr = dynamic_cast<const ColumnRef*>(e.get())) out = cr->column_name;
            else if (auto* agg = dynamic_cast<const AggregateExpr*>(e.get()))
                out = aggregateOutputName(agg);
            else continue;   // exprToString names it; not worth re-deriving here
        }
        if (out == name) return exprMayBeInt(e.get(), inner, catalog, depth - 1);
    }
    return true;
}

// Same resolution order every consumer uses: by (slot, name) when the Binder
// resolved it, by bare name otherwise. Unresolvable — a correlated reference, an
// empty range table, a name that is in no relation this walk can see — is
// INTEGER, the conservative answer.
bool columnMayBeInt(const ColumnRef& cr, const RangeTable& rt,
                    const Catalog* catalog, int depth) {
    auto lookIn = [&](const RangeEntry& e) -> int {
        // -1 = not here, 0 = here and not INT, 1 = here and may be INT
        if (e.schema) {
            const int idx = e.schema->indexOf(cr.column_name);
            return idx < 0 ? -1 : (e.schema->column(idx).type == TypeId::INT ? 1 : 0);
        }
        if (e.body) return derivedColumnMayBeInt(*e.body, cr.column_name, catalog, depth) ? 1 : 0;
        return -1;
    };
    if (rt.empty()) return true;
    if (cr.id.isResolved() && cr.id.isLocal()) {
        const int slot = cr.id.localSlot("subquery division operand typing");
        if (slot >= 0 && slot < static_cast<int>(rt.size())) {
            const int r = lookIn(rt[slot]);
            if (r >= 0) return r == 1;
        }
        return true;
    }
    for (const RangeEntry& e : rt) {
        const int r = lookIn(e);
        if (r >= 0) return r == 1;
    }
    return true;
}

// The SCALAR subqueries whose VALUE flows into this expression's value, plus
// whether that value can be an INTEGER in Volcano. Mirrors taintWalk in
// vectorized_plan_builder.cc, one AST level up: same `/` rule, same
// "arithmetic is INT only when both operands are" propagation, same death of
// the taint at a comparison.
struct DivTaint {
    // The BODIES, not the nodes: the ResultCache is keyed by statement and
    // cloneExpr shares a body between two SubqueryExpr nodes, so a set of nodes
    // would let the undivided node of a shared pair run the body first and
    // decide the flag for both.
    std::vector<const SelectStatement*> carried;
    bool may_be_int = false;
};

void addCarried(std::vector<const SelectStatement*>& into,
                const std::vector<const SelectStatement*>& from) {
    for (const SelectStatement* s : from) {
        if (std::find(into.begin(), into.end(), s) == into.end()) into.push_back(s);
    }
}

DivTaint divWalk(const Expr* e, const RangeTable& rt,
                 std::unordered_set<const SelectStatement*>& observed,
                 const Catalog* catalog, int depth) {
    DivTaint out;
    if (!e) return out;

    if (auto* lit = dynamic_cast<const Literal*>(e)) {
        out.may_be_int = lit->value.isNull() ? lit->null_type == TypeId::INT
                                             : lit->value.type() == TypeId::INT;
        return out;
    }
    if (auto* cr = dynamic_cast<const ColumnRef*>(e)) {
        out.may_be_int = columnMayBeInt(*cr, rt, catalog, depth);
        return out;
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(e)) {
        // COUNT is INT-typed; SUM and AVG accumulate into a double and emit a
        // DOUBLE whatever the input was; MIN/MAX hand back an element of the
        // input domain, so they inherit the argument's answer.
        //
        // !! EVERY ARM WALKS THE ARGUMENT, and the two that answer about their
        // OWN type still walk it FOR THE SIDE EFFECT. `observed` is a side
        // channel — the set of bodies an INT-partnered `/` divides — and it is
        // independent of what this node's type is. Through the previous round
        // SUM and AVG returned here without walking, and
        // `HAVING SUM(l.round / (SELECT MAX(CASE ... 2 ELSE 0.5 END)))` was a
        // SILENT WRONG ANSWER: 7 teams where both Volcano modes and SQLite give
        // 4 (seam subquery pass 5, B-1). The discriminator that named the arm
        // rather than the shape: MAX over the IDENTICAL body, from the same
        // clause, refuses — because the MIN/MAX arm below recursed and these two
        // did not. The plan-level walk this one is written to mirror
        // (collectIntOrigins' AGGREGATE case, vectorized_plan_builder.cc) walks
        // `spec.argument` for EVERY aggregate, outside its own order_stat test.
        // Four other non-arithmetic arms of this same walk (IsNullExpr, InExpr,
        // LikeExpr, SubstringExpr) already recurse for the side effect alone and
        // discard the result; these are now the fifth and sixth.
        const DivTaint arg = divWalk(agg->argument.get(), rt, observed, catalog, depth);
        if (agg->function_name == "COUNT") { out.may_be_int = true; return out; }
        if (agg->function_name == "SUM" || agg->function_name == "AVG") return out;
        return arg;   // MIN/MAX inherit the argument's answer, type and carried
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(e)) {
        return divWalk(un->operand.get(), rt, observed, catalog, depth);
    }
    if (auto* sq = dynamic_cast<const SubqueryExpr*>(e)) {
        // Its operand belongs to this block and is walked for its own sake.
        divWalk(sq->operand.get(), rt, observed, catalog, depth);
        // Only a SCALAR node is replaced by a value. EXISTS becomes a 0/1
        // literal — an INT in both engines — and IN is lowered to a semi-join
        // and never materialized at all.
        if (sq->kind == SubqueryExpr::Kind::SCALAR && !sq->correlated && sq->subquery) {
            out.carried.push_back(sq->subquery.get());
        }
        out.may_be_int = true;   // its body's type is exactly what is undecided
        return out;
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(e)) {
        DivTaint l = divWalk(bin->left.get(), rt, observed, catalog, depth);
        DivTaint r = divWalk(bin->right.get(), rt, observed, catalog, depth);
        const std::string& op = bin->op;
        if (op == "+" || op == "-" || op == "*" || op == "/") {
            const bool both = l.may_be_int && r.may_be_int;
            if (op == "/" && both) {
                for (const SelectStatement* s : l.carried) observed.insert(s);
                for (const SelectStatement* s : r.carried) observed.insert(s);
            }
            if (both) { addCarried(out.carried, l.carried); addCarried(out.carried, r.carried); }
            out.may_be_int = both;
            return out;
        }
        out.may_be_int = true;   // comparison / AND / OR: boolean-as-INT
        return out;
    }
    if (auto* isn = dynamic_cast<const IsNullExpr*>(e)) {
        divWalk(isn->operand.get(), rt, observed, catalog, depth);
        out.may_be_int = true;
        return out;
    }
    if (auto* in = dynamic_cast<const InExpr*>(e)) {
        divWalk(in->operand.get(), rt, observed, catalog, depth);
        out.may_be_int = true;
        return out;
    }
    if (auto* lk = dynamic_cast<const LikeExpr*>(e)) {
        divWalk(lk->operand.get(), rt, observed, catalog, depth);
        out.may_be_int = true;
        return out;
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(e)) {
        divWalk(sub->operand.get(), rt, observed, catalog, depth);
        divWalk(sub->start.get(), rt, observed, catalog, depth);
        divWalk(sub->length.get(), rt, observed, catalog, depth);
        return out;   // STRING, never an INT
    }
    if (auto* c = dynamic_cast<const CaseExpr*>(e)) {
        for (const auto& w : c->when_clauses) {
            divWalk(w.condition.get(), rt, observed, catalog, depth);
            DivTaint br = divWalk(w.result.get(), rt, observed, catalog, depth);
            addCarried(out.carried, br.carried);
            out.may_be_int = out.may_be_int || br.may_be_int;
        }
        if (c->else_expr) {
            DivTaint br = divWalk(c->else_expr.get(), rt, observed, catalog, depth);
            addCarried(out.carried, br.carried);
            out.may_be_int = out.may_be_int || br.may_be_int;
        }
        return out;
    }
    out.may_be_int = true;
    return out;
}

// "Can this expression's Volcano Value be an INT?", with no interest in the
// subqueries under it. One definition of the type question, used by the derived
// arm of columnMayBeInt above and by divWalk itself.
bool exprMayBeInt(const Expr* e, const RangeTable& rt, const Catalog* catalog, int depth) {
    std::unordered_set<const SelectStatement*> ignored;
    // THE CATALOG IS NEEDED HERE, and the comment that used to say otherwise was
    // the defect (seam subquery pass 5, B-4). It read: "the catalog reached this
    // far through the RangeTable already; a body one level down is resolved
    // against `rt`'s own entries". A body one level down is NOT resolved against
    // `rt`: it needs its OWN range table, which derivedColumnMayBeInt builds with
    // rangeTableOf(body, catalog) — and rangeTableOf returns an EMPTY table at
    // its first line when catalog is null, on which columnMayBeInt answers
    // "may be INT" for every name. So the SECOND derived level always answered
    // INTEGER and MAX_TYPE_DEPTH = 3 was really 1: `t.s / (SELECT <mixed>)` over
    // a REAL column answered at one derived level and was refused at two, with
    // no working mode (Volcano refuses derived tables by capability).
    return divWalk(e, rt, ignored, catalog, depth).may_be_int;
}

} // namespace

void collectQueryTables(const SelectStatement& stmt, std::vector<std::string>& out) {
    // A self-join names one table twice and main.cc keys its loaded data by
    // table name, so dedupe here rather than at every caller.
    // Week 34: a DERIVED relation names no catalog table of its own; the tables
    // its body scans are collected by recursing into the body, exactly as this
    // walker already does for a SubqueryExpr. Skipping it here rather than
    // calling tableName() is the difference between "there is nothing to add"
    // and an internal-defect throw on a legitimate query.
    if (!stmt.from.isDerived())
        addTable(out, stmt.from.tableName("collectQueryTables FROM"));
    else
        collectQueryTables(*stmt.from.body(), out);
    for (const auto& j : stmt.joins) {
        if (!j.relation.isDerived())
            addTable(out, j.relation.tableName("collectQueryTables JOIN"));
        else
            collectQueryTables(*j.relation.body(), out);
    }

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
// SelectStatement DURING planning breaks it, and the fix is to hold the
// shared_ptr in the cache VALUE, which pins the address for the pass's lifetime.
//
// Week 34 was named here as the candidate and IS NOT ONE, checked rather than
// assumed: a derived table's body is allocated by the parser like every other
// statement (parseTableRef calls make_unique at parse time), and
// LogicalPlanBuilder MOVES it rather than allocating a second. This pass also
// runs before planning and never meets a TableRef at all. The invariant holds
// unchanged, and the pin is still a field no reachable input needs.
using ResultCache = std::unordered_map<const SelectStatement*, SubqueryResult>;

const SubqueryResult& runOnce(SubqueryExpr* sq, const SubqueryRunner& run,
                              ResultCache& cache, const Catalog* catalog,
                              bool int_type_observable) {
    auto it = cache.find(sq->subquery.get());
    if (it != cache.end()) return it->second;

    // Innermost first: a body cannot run while it still holds a subquery of its
    // own. This must happen BEFORE the move below — materializing the moved-from
    // husk is a silent no-op that leaves the inner node in place and surfaces as
    // site 12's throw from inside a nested run.
    materializeSubqueries(*sq->subquery, run, catalog);

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
    // The flag is keyed by BODY, exactly as this cache is, so a body shared by
    // two SubqueryExpr nodes (BETWEEN clones its left operand) carries the
    // union of what those nodes need — whichever of them reaches here first.
    auto ins = cache.emplace(sq->subquery.get(), run(std::move(body), int_type_observable));
    return ins.first->second;
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
        // Week 32 — UNREACHABLE, and deliberately loud rather than deleted.
        // materializeSubqueries now SKIPS every Kind::IN node (see below): the
        // set-membership form is lowered to a semi/anti join by
        // subquery_lowering.h instead, which is what let Week 31's
        // MAX_MATERIALIZED_IN_VALUES cap be removed outright — nothing is
        // materialized, so there is no linear InExpr scan left to bound.
        //
        // The three-valued IN reasoning this branch used to carry moved WITH the
        // rule, to the probe loops of VecHashJoinNode and HashJoinNode: `x NOT
        // IN S` is never TRUE when S holds a NULL, and a semi-join has no
        // substitution site, so that fact is carried out of the build phase as a
        // flag. See docs/week-32-plan.md §8.
        throw std::runtime_error(
            "internal: an IN subquery reached materialization (Week 32 lowers "
            "it to a semi-join)");
    }
    }
    throw std::runtime_error("internal: unknown SubqueryExpr::Kind");
}

} // namespace

bool needsSubqueryMaterialization(const SelectStatement& stmt) {
    if (stmt.has_subquery) return true;
    // Recurse into derived bodies only — the same structure collectQueryTables
    // walks, and for the same reason. A SubqueryExpr's own body is not walked
    // here: a statement holding one has has_subquery set directly.
    if (stmt.from.isDerived() && stmt.from.body()
        && needsSubqueryMaterialization(*stmt.from.body())) {
        return true;
    }
    for (const auto& j : stmt.joins) {
        if (j.relation.isDerived() && j.relation.body()
            && needsSubqueryMaterialization(*j.relation.body())) {
            return true;
        }
    }
    return false;
}

void materializeSubqueries(SelectStatement& stmt, const SubqueryRunner& run,
                           const Catalog* catalog) {
    // Week 35 — A DERIVED TABLE'S BODY IS A STATEMENT, and its subqueries are as
    // unmaterialized as any other. `collectQueryTables`, ten lines above in this
    // same file, WAS extended to descend into a body when Week 34 landed derived
    // tables (`if (!stmt.from.isDerived()) ... else collectQueryTables(*body)`).
    // This pass was not. So a scalar subquery inside `FROM (SELECT ...)` reached
    // type inference unsubstituted and tripped the internal guard with
    // "a subquery reached type inference without being materialized" — an
    // INTERNAL error surfaced to the user, on TPC-H Q22.
    //
    // Two walkers over the same structure, one updated: exactly the shape the
    // standing sweep rule exists for. Found by the Week 35 TPC-H harness, which
    // is the first thing in the project that runs a query of this shape.
    //
    // The recursion runs BEFORE the has_subquery fast path on purpose: that flag
    // describes THIS statement's own expression slots, and an outer query whose
    // only subquery lives inside a derived body has it clear. Putting the
    // recursion after it would leave the bug in place for precisely the failing
    // case.
    if (stmt.from.isDerived() && stmt.from.body()) {
        materializeSubqueries(*stmt.from.body(), run, catalog);
    }
    for (auto& j : stmt.joins) {
        if (j.relation.isDerived() && j.relation.body()) {
            materializeSubqueries(*j.relation.body(), run, catalog);
        }
    }

    // One bool for the 99% case: every query in the engine calls this.
    if (!stmt.has_subquery) return;

    // BEFORE anything is replaced, because the question is about the expression
    // the subquery SITS IN and `visit` below destroys it. Two passes over the
    // same slots; the second one is the substitution.
    const RangeTable range_table = rangeTableOf(stmt, catalog);
    std::unordered_set<const SelectStatement*> divided_by_int;
    forEachStatementExpr(stmt, [&](std::unique_ptr<Expr>& e) {
        divWalk(e.get(), range_table, divided_by_int, catalog, MAX_TYPE_DEPTH);
    });
    for (const auto& g : stmt.group_by) {
        divWalk(g.expr.get(), range_table, divided_by_int, catalog, MAX_TYPE_DEPTH);
    }

    ResultCache cache;
    // Week 32: a Kind::IN node SURVIVES this pass. It is the one shape lowered
    // to a plan node (a semi/anti join) rather than substituted with a
    // constant, and a plan node cannot be produced from an AST rewrite — see
    // subquery_lowering.h for the routing and for why keeping both productions
    // for one shape was rejected.
    bool node_survives = false;
    auto visit = [&](std::unique_ptr<Expr>& slot) {
        auto* sq = dynamic_cast<SubqueryExpr*>(slot.get());
        if (!sq) return;
        // Week 33 — a CORRELATED node survives this pass, for the same reason a
        // Kind::IN node does and one stronger: it has no value to substitute.
        // Its body names a column of the ENCLOSING row, so "run it once and
        // replace it with a constant" is not an optimisation of it — it is a
        // different query. Running it here executed the body with the outer ref
        // resolved by bare name against the BODY's own schema
        // (`l.driver_id = d.driver_id` became `laps.driver_id = laps.driver_id`,
        // a tautology), folded the whole predicate to the literal 1 or 0, and
        // returned a plausible wrong answer for every correlated query in the
        // engine — while making lowerExistsSubqueries unreachable from the CLI,
        // because the SubqueryExpr it decorrelates had already been consumed.
        //
        // The header's precondition "no correlated subquery anywhere in stmt"
        // was true only while Validator::validate refused one. Week 33 deleted
        // that refusal, and this pass — which the header says TRUSTS the
        // Validator rather than re-checking — kept trusting a rule that no
        // longer existed. The trust is now placed where it can be honoured: the
        // routing decision is made HERE, off the Binder's own `correlated` flag,
        // not off a rule enforced in another file.
        //
        // What happens to the survivor is the planner's business:
        // lowerExistsSubqueries decorrelates a Kind::EXISTS one into a semi/anti
        // join, and refuseUnloweredCorrelated refuses every other shape by name.
        // Both are loud; neither substitutes a value.
        if (sq->correlated) {
            // The BODY is still this pass's business, exactly as for IN: an
            // uncorrelated subquery nested inside a correlated body is Week 31's
            // and must still be materialized before the body is planned. Same
            // caveats as the IN arm below — not moved, not limit-capped, not
            // cached.
            materializeSubqueries(*sq->subquery, run, catalog);
            node_survives = true;
            return;
        }
        if (sq->kind == SubqueryExpr::Kind::IN) {
            // The NODE survives, but its BODY is still this pass's business:
            // `x IN (SELECT k FROM t WHERE c > (SELECT AVG(c) FROM t))` is legal
            // SQL Week 31 answered. The recursion that handles it lives inside
            // runOnce ("Innermost first"), which this arm returns before — so it
            // is repeated here. Nothing else of runOnce applies: the body is NOT
            // moved, NOT limit-capped and NOT cached, because it is planned by
            // the semi-join's build side rather than run for a value.
            //
            // NOT CACHED IS NOT FREE — name what makes it safe, because neither
            // guarantee lives in this file. cloneExpr shares the body's
            // shared_ptr rather than deep-copying it, so two SubqueryExpr nodes
            // over ONE body is a state the engine already reaches (BETWEEN
            // clones its left operand). This arm recurses unconditionally,
            // outside the statement-keyed ResultCache the non-IN path uses, so
            // a body reached twice is walked twice. It is harmless only because
            // (a) a walked body normally has has_subquery cleared on the first
            // pass, so the second call returns at the top, and (b) any body that
            // is genuinely shared is refused outright by planBody's
            // use_count() > 1 check in subquery_lowering.cc.
            //
            // (a) does NOT hold for every body: an IN nested inside an IN body
            // leaves the body's has_subquery TRUE, so a second visit would
            // re-walk it. Nothing produces a second visit today; if anything
            // ever does, guard this call on cache.count(sq->subquery.get())
            // rather than re-deriving the argument.
            //
            // Without this, the body's own SCALAR node reaches
            // LogicalPlanBuilder and inferExprType (dispatch site 12) throws its
            // "the materialization walker missed a subtype" message — an
            // internal-defect report for a perfectly ordinary query.
            materializeSubqueries(*sq->subquery, run, catalog);
            node_survives = true;
            return;
        }
        const SubqueryResult& res =
            runOnce(sq, run, cache, catalog,
                    divided_by_int.count(sq->subquery.get()) != 0);
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

    // The flag means "a SubqueryExpr is still in this tree". Week 32: an IN node
    // may still be one, and the flag MUST stay set while it is, because the
    // semi-join's probe key is the IN operand and dropping it breaks the join.
    //
    // **Week 37 changed HOW that protection works, and this comment used to
    // state the old mechanism as a guarantee.** buildScanSchema no longer widens
    // to the full schema when the flag is set — it narrows to the union of the
    // block's own referenced columns and the outer columns nested bodies name
    // (`collectOuterRefs`). The IN operand survives because `collectCols`
    // collects it, not because the schema is widened. The flag still gates
    // Planner::plan's refusal message, so it must still be accurate; what it no
    // longer does is turn projection pushdown off wholesale.
    stmt.has_subquery = node_survives;

    // A substituted constant can sit under arithmetic — `> (SELECT ...) * 2` —
    // and three fast paths pattern-match on ColumnRef op Literal. Folding
    // touches arithmetic only, never comparisons, and declines any fold that
    // evaluates to NULL, so it cannot change a result or manufacture a second
    // null Literal. Same argument as constant_folding.h.
    foldConstants(stmt);
}
