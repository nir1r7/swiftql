#include "logical_plan.h"
#include "join_condition.h"
#include "validator.h"
#include "subquery_lowering.h"
#include "subquery_decorrelation.h"
#include "parser/expr_utils.h"
#include <unordered_set>


// collecting column names from expressions
static void collectCols(const Expr* expr, std::unordered_set<std::string>& out){
    if (!expr) return;
    if (auto* cr = dynamic_cast<const ColumnRef*>(expr)){
        out.insert(cr->column_name);
        return;
    }

    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)){
        collectCols(bin->left.get(), out);
        collectCols(bin->right.get(), out);
        return;
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)){
        collectCols(agg->argument.get(), out);
        return;
    }
    if (auto* isnull = dynamic_cast<const IsNullExpr*>(expr)){
        collectCols(isnull->operand.get(), out);
        return;
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)){
        collectCols(un->operand.get(), out);
        return;
    }
    if (auto* in = dynamic_cast<const InExpr*>(expr)){
        collectCols(in->operand.get(), out);   // values are literals
        return;
    }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)){
        collectCols(lk->operand.get(), out);
        return;
    }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)){
        for (const auto& w : c->when_clauses){
            collectCols(w.condition.get(), out);
            collectCols(w.result.get(), out);
        }
        collectCols(c->else_expr.get(), out);
        return;
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)){
        collectCols(sub->operand.get(), out);
        collectCols(sub->start.get(), out);
        collectCols(sub->length.get(), out);   // nullptr-safe
        return;
    }
    // Week 30 — DISPATCH SITE 2. Descend into the IN operand, which is written
    // in THIS query, and not into the body: those names are a different scope's
    // range table, and narrowing here is by BARE NAME against one flat schema.
    // A CORRELATED ref inside the body does name an outer column that must
    // survive narrowing, and buildScanSchema handles that conservatively by
    // declining to narrow at all when the statement uses a subquery (see there).
    if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)){
        collectCols(sq->operand.get(), out);
        return;
    }
    // IntervalLiteral: a constant, and folded away before this runs
}

// build schema using only required columns
// copies whole ColumnDefs so order and relation_slot stamps are preserved
static Schema narrowSchema(const Schema& full, const std::unordered_set<std::string>& required) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < full.size(); ++i) {
        if (required.count(full.column(i).name))
            cols.push_back(full.column(i));
    }
    return Schema(cols);
}


TypeId aggregateResultType(const std::string& function, TypeId arg_type) {
    if (function == "COUNT") return TypeId::INT;
    if (function == "MIN" || function == "MAX") return arg_type;
    return TypeId::DOUBLE;   // SUM, AVG — see the header for why SUM is not INT
}


// Argument type of an aggregate spec, for aggregateResultType. Prefers the
// general argument expression; falls back to the plain-ColumnRef fast path
// (spec.column), resolved slot-first like everywhere else. COUNT(*) and specs
// whose argument cannot be resolved report INT, which only COUNT consumes.
static TypeId specArgType(const AggregateSpec& spec, const Schema& table_schema) {
    if (spec.is_star) return TypeId::INT;
    if (spec.argument) return inferExprType(spec.argument, table_schema);
    if (spec.column.empty()) return TypeId::INT;
    int idx = spec.id.isResolved()
        ? table_schema.indexOf(spec.column,
                               spec.id.localSlot("aggregate argument")) : -1;
    if (idx < 0) idx = table_schema.indexOf(spec.column);
    return idx >= 0 ? table_schema.column(idx).type : TypeId::INT;
}


TypeId inferExprType(const Expr* expr, const Schema& schema) {
    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        // Week 31. A null Literal is a materialized scalar subquery that
        // returned zero rows, or one NULL row — the first constant NULL this
        // engine has ever had, since the grammar has no NULL literal and
        // foldNode declines to produce one. Value::type() throws on null and
        // there is nothing here to re-derive the type from, so the node carries
        // the type the subquery's own output schema gave it (ast.h).
        if (lit->value.isNull()) return lit->null_type;
        return lit->value.type();
    }
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // slot-first with bare-name fallback — same contract as
        // resolveColumnIndex() in evaluator.cc
        int idx = col->id.isResolved()
            ? schema.indexOf(col->column_name,
                             col->id.localSlot("inferExprType")) : -1;
        if (idx < 0) idx = schema.indexOf(col->column_name);
        if (idx < 0) throw std::runtime_error(
            "column not found: '" + col->column_name + "'");
        return schema.column(idx).type;
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        // Post-aggregate schemas already carry this aggregate's output column
        // with its resolved type (buildAggregateSchema is the single source of
        // truth). Prefer it so inference can never disagree with what the
        // aggregate node actually produced — this is the path taken by
        // expressions over aggregates, e.g. SUM(a) / SUM(b) in a projection.
        int idx = schema.indexOf(aggregateOutputName(agg));
        if (idx >= 0) return schema.column(idx).type;
        // pre-aggregate context (hand-built trees in tests): derive from the
        // argument against the input schema
        TypeId arg = (agg->is_star || !agg->argument)
            ? TypeId::INT : inferExprType(agg->argument.get(), schema);
        return aggregateResultType(agg->function_name, arg);
    }
    if (dynamic_cast<const IsNullExpr*>(expr)) {
        return TypeId::INT;   // boolean-as-INT convention
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        TypeId t = inferExprType(un->operand.get(), schema);
        if (t == TypeId::STRING)
            throw std::runtime_error("unary '-' requires a numeric operand");
        return t;
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        // recurse into both children first so ill-typed subtrees surface even
        // under comparisons ((team + 1) = 5 must be rejected)
        TypeId l = inferExprType(bin->left.get(), schema);
        TypeId r = inferExprType(bin->right.get(), schema);
        const std::string& op = bin->op;
        if (op == "+" || op == "-" || op == "*" || op == "/") {
            if (l == TypeId::STRING || r == TypeId::STRING)
                throw std::runtime_error("'" + op + "' requires numeric operands");
            // INT/INT stays INT (SQLite truncating division); any DOUBLE promotes
            return (l == TypeId::INT && r == TypeId::INT) ? TypeId::INT : TypeId::DOUBLE;
        }
        return TypeId::INT;   // comparison / AND / OR
    }
    if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        TypeId op_t = inferExprType(in->operand.get(), schema);
        const bool op_str = (op_t == TypeId::STRING);
        for (const Value& c : in->values) {
            // Value's comparison operators throw on STRING vs numeric; reject
            // that shape here so the failure is a plan-time SQL error rather
            // than a per-row throw from inside the scan loop
            if ((c.type() == TypeId::STRING) != op_str)
                throw std::runtime_error(
                    "IN: list values must be comparable with the operand "
                    "(cannot mix STRING and numeric)");
        }
        return TypeId::INT;   // boolean-as-INT convention
    }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        if (inferExprType(lk->operand.get(), schema) != TypeId::STRING)
            throw std::runtime_error("LIKE requires a STRING operand");
        return TypeId::INT;
    }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        // Conditions are predicates. Rejecting a non-INT here is stricter than
        // WHERE (which lets asInt() raise per row), but this type feeds output
        // column pre-allocation, so it has to be decided at plan time.
        for (const auto& w : c->when_clauses) {
            if (inferExprType(w.condition.get(), schema) != TypeId::INT)
                throw std::runtime_error("CASE: WHEN condition must be a predicate");
        }
        // The parser guarantees at least one WHEN, so an empty list is a
        // hand-built tree — say so instead of indexing out of bounds.
        if (c->when_clauses.empty())
            throw std::runtime_error("internal: CASE with no WHEN clauses");
        // Unify the result branches with the same promotion rule as arithmetic
        // above: INT+INT stays INT, any DOUBLE promotes, STRING may not mix.
        TypeId result = inferExprType(c->when_clauses[0].result.get(), schema);
        auto unify = [&result](TypeId t) {
            if (t == result) return;
            if (t == TypeId::STRING || result == TypeId::STRING)
                throw std::runtime_error(
                    "CASE: result branches must be all numeric or all STRING");
            result = TypeId::DOUBLE;
        };
        for (size_t i = 1; i < c->when_clauses.size(); ++i)
            unify(inferExprType(c->when_clauses[i].result.get(), schema));
        // A missing ELSE contributes NULL, which is typeless here (Value::type()
        // throws on null) and so cannot change the unified type.
        if (c->else_expr) unify(inferExprType(c->else_expr.get(), schema));
        return result;
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        if (inferExprType(sub->operand.get(), schema) != TypeId::STRING)
            throw std::runtime_error("SUBSTRING requires a STRING operand");
        if (inferExprType(sub->start.get(), schema) != TypeId::INT)
            throw std::runtime_error("SUBSTRING: start position must be an integer");
        if (sub->length && inferExprType(sub->length.get(), schema) != TypeId::INT)
            throw std::runtime_error("SUBSTRING: length must be an integer");
        // Constant out-of-domain arguments are decided HERE, not per row. After
        // foldConstants every realistic SUBSTRING has literal positions, so this
        // is where the user actually finds out — a plan-time SQL error instead
        // of a throw from inside the scan loop that aborts a partly-executed
        // query. A computed position still raises at execution; see
        // substringOf() in evaluator.cc.
        if (auto* lit = dynamic_cast<const Literal*>(sub->start.get())) {
            if (!lit->value.isNull() && lit->value.asInt() < 1)
                throw std::runtime_error(
                    "SUBSTRING: start position must be >= 1 (SwiftQL does not "
                    "support SQLite's 0 and negative start positions)");
        }
        if (sub->length) {
            if (auto* lit = dynamic_cast<const Literal*>(sub->length.get())) {
                if (!lit->value.isNull() && lit->value.asInt() < 0)
                    throw std::runtime_error(
                        "SUBSTRING: length must be >= 0 (SwiftQL does not support "
                        "SQLite's negative-length form)");
            }
        }
        return TypeId::STRING;
    }
    if (dynamic_cast<const IntervalLiteral*>(expr)) {
        // Unfolded interval: the query is malformed. Loud at plan time, by design.
        throw std::runtime_error(
            "INTERVAL is only valid in constant date arithmetic, "
            "e.g. date '1994-01-01' + interval '1' year");
    }
    if (dynamic_cast<const SubqueryExpr*>(expr)) {
        // DISPATCH SITE 12, closed in Week 31 — as an INTERNAL invariant, not as
        // a feature. Every subquery is replaced by a constant before planning
        // (materializeSubqueries, run by main.cc after Validator::validate), and
        // a correlated one is refused by the Validator. Reaching this therefore
        // means the materialization walker (dispatch site 19) missed an Expr
        // subtype, or the pass was not run at all.
        //
        // That throw is exactly what makes site 19 a LOUD dispatch site instead
        // of the eleventh silent one — do not delete it. This site is also the
        // contract the vectorized path pre-allocates output columns from, so it
        // must never be reached by accident.
        //
        // The real type rule a scalar subquery needed turned out to live on the
        // Literal it becomes: see the null_type branch above.
        throw std::runtime_error(
            "internal: a subquery reached type inference without being "
            "materialized (materializeSubqueries must run before planning)");
    }
    throw std::runtime_error("inferExprType(): unknown Expr subtype");
}


Schema buildScanSchema(const SelectStatement& stmt, const Schema& full_schema) {
    if (stmt.select_star) return full_schema;
    // Week 30. Narrowing is by bare name over one flat schema, and a CORRELATED
    // reference inside a subquery names an outer column that collectCols
    // deliberately does not descend into (dispatch site 2: the body's names
    // belong to another scope's range table). Widening is the safe direction —
    // a narrowed-away column dies later with "column not found", far from the
    // cause. Costs projection pushdown for subquery queries; Week 33 replaces
    // this with the correlated columns actually referenced.
    if (stmt.has_subquery) return full_schema;
    std::unordered_set<std::string> required;
    for (const auto& expr : stmt.select_list) collectCols(expr.get(), required);
    collectCols(stmt.where.get(), required);
    for (const auto& g : stmt.group_by) {
        // expression keys reference columns only inside their tree — a miss
        // here would narrow them out of the scan
        if (g.expr) { collectCols(g.expr.get(), required); continue; }
        required.insert(g.column_name);
    }
    collectCols(stmt.having.get(), required);
    for (const auto& item : stmt.order_by) collectCols(item.expr.get(), required);
    // every ON condition: a key narrowed out of a scan schema dies later with
    // "column not found", far from the cause
    for (const auto& j : stmt.joins) collectCols(j.condition.get(), required);
    return narrowSchema(full_schema, required);
}


Schema buildProjectSchema(const SelectStatement& stmt, const Schema& table_schema){
    std::vector<ColumnDef> cols;

    for (const auto& expr : stmt.select_list) {
        if (auto* col = dynamic_cast<ColumnRef*>(expr.get())) {
            // resolve slot-first (correct side on a join with shared names),
            // falling back to bare name against post-aggregate schemas
            int idx = col->id.isResolved()
                ? table_schema.indexOf(col->column_name,
                                       col->id.localSlot("buildProjectSchema"))
                : -1;
            if (idx < 0) idx = table_schema.indexOf(col->column_name);
            if (idx >= 0) {
                cols.push_back(table_schema.column(idx));
            } else {
                cols.push_back({col->column_name, TypeId::STRING});
            }
        } else if (auto* agg = dynamic_cast<AggregateExpr*>(expr.get())) {
            // the aggregate node below already emitted this column with its
            // resolved result type; reuse it so project and aggregate can never
            // disagree. table_schema here is the POST-aggregate schema, so
            // re-inferring from agg->argument would fail — the argument's base
            // column no longer exists at this level.
            std::string agg_name = aggregateOutputName(agg);
            int agg_idx = table_schema.indexOf(agg_name);
            TypeId result_type = agg_idx >= 0
                ? table_schema.column(agg_idx).type
                : aggregateResultType(agg->function_name, TypeId::DOUBLE);
            cols.push_back({agg_name, result_type});
        } else {
            // general expression (arithmetic, expr over aggregates): name from
            // exprToString, type from inference — never STRING-by-default
            cols.push_back({exprToString(expr.get()),
                            inferExprType(expr.get(), table_schema)});
        }
        // an alias renames only this projected output column; the aggregate
        // node's schema underneath keeps canonical aggregateOutputName names
        if (!expr->alias.empty()) cols.back().name = expr->alias;
    }

    return Schema(cols);
}


// Consumes the specs extractAggregates produced so schema and node can never
// disagree on column order or names.
Schema buildAggregateSchema(const std::vector<GroupByColumn>& group_by,
                            const std::vector<AggregateSpec>& aggregates,
                            const Schema& table_schema){
    // plan-time type check of expression arguments (SUM(speed * 2)): both
    // builders route through here, so ill-typed arguments fail before
    // execution. Plain-ColumnRef SUM/AVG args are already validator-checked.
    for (const auto& spec : aggregates) {
        if (spec.is_star || !spec.argument) continue;
        TypeId t = inferExprType(spec.argument, table_schema);
        if ((spec.function == "SUM" || spec.function == "AVG") && t == TypeId::STRING) {
            throw std::runtime_error(spec.function + "() requires a numeric argument");
        }
    }

    std::vector<ColumnDef> cols;

    // group-by columns in order, resolved slot-first so a qualified GROUP BY
    // picks the named join side even when both sides share the column name.
    // Expression keys become computed output columns named by exprToString —
    // the same string substituteGroupKeyRefs rewrites references into.
    for (const auto& g : group_by) {
        if (g.expr) {
            cols.push_back({exprToString(g.expr.get()),
                            inferExprType(g.expr.get(), table_schema)});
            continue;
        }
        // Week 30. A group key the Binder resolved to an ENCLOSING block carries
        // a slot that indexes THAT block's range table, while `table_schema` is
        // this one's. The lookup below would not miss — it would HIT the wrong
        // relation whenever the column name is shared (`team`, `driver_id` are,
        // on the shipped catalog), so `EXISTS (SELECT COUNT(*) FROM drivers d
        // GROUP BY l.team)` would group by `drivers.team` instead of the
        // correlated `laps.team`. Wrong groups, no error, in both engines, and
        // neither the bare-name fallback nor the `idx < 0` throw below fires.
        //
        // THROW, not decline: grouping is not an optimization and a correlated
        // key has no correct local fallback — its value comes from the outer
        // row. Week 33: the Validator refusal that made this unreachable is
        // gone, and it STAYS A THROW. Decorrelation promotes every correlated
        // reference into a join key before the body is planned, and a body with
        // a GROUP BY is refused outright (subquery_decorrelation.cc), so a
        // correlated group key arriving here means the rewrite left one behind
        // — a planner defect, which is what it says. It remains the single
        // guard for the whole GroupByColumn consumer set.
        //
        // This is the single guard for the whole GroupByColumn consumer set:
        // HashAggregateNode, VecHashAggregateNode and CardinalityEstimator all
        // read `g.relation_slot` too, and all three run on a plan whose schema
        // was built here — if it throws, they never see the key.
        if (!g.id.isLocal()) {
            throw std::runtime_error(
                "internal: a correlated GROUP BY key ('" + g.column_name
                + "') reached plan construction; its slot names an enclosing "
                  "query's range table");
        }
        int idx = g.id.isResolved()
            ? table_schema.indexOf(g.column_name,
                                   g.id.localSlot("buildAggregateSchema"))
            : -1;
        if (idx < 0) idx = table_schema.indexOf(g.column_name);
        if (idx < 0) {
            // Both executors push one key Value per group-by column, so silently
            // dropping the column here would leave the schema one column narrower
            // than every row — silent corruption. Unreachable today (Validator
            // and buildScanSchema guarantee resolution), so this is the shape of
            // a planner bug, not a user error.
            throw std::runtime_error(
                "internal: GROUP BY column '" + g.column_name
                + "' not present in the aggregate's input schema");
        }
        cols.push_back(table_schema.column(idx));
    }

    // one output column per aggregate, in spec order (SELECT-list aggregates
    // first, then hidden HAVING/ORDER-BY-only aggregates at the tail)
    for (const auto& spec : aggregates) {
        ColumnDef def{spec.output_name,
                      aggregateResultType(spec.function, specArgType(spec, table_schema))};
        def.hidden = spec.hidden;
        cols.push_back(def);
    }

    return Schema(cols);
}


std::vector<AggregateSpec> extractAggregates(const SelectStatement& stmt){
    std::vector<AggregateSpec> specs;

    auto makeSpec = [](const AggregateExpr* agg, bool hidden) {
        AggregateSpec spec;
        spec.function = agg->function_name;
        spec.is_star = agg->is_star;
        spec.output_name = aggregateOutputName(agg);
        spec.hidden = hidden;
        if (!agg->is_star && agg->argument) {
            spec.argument = agg->argument.get();  // non-owning; see AggregateSpec
            if (auto* col = dynamic_cast<const ColumnRef*>(agg->argument.get())) {
                spec.column = col->column_name;
                spec.id = col->id;   // carry join side, e.g. AVG(l2.speed)
            }
        }
        return spec;
    };

    // Dedupe by output_name everywhere — the name is how evaluate() finds the
    // column, so name-identical references share one output column.
    auto known = [&specs](const std::string& name) {
        for (const auto& s : specs) {
            if (s.output_name == name) return true;
        }
        return false;
    };

    // recursive collection: SELECT AVG(speed) * 2 contains an aggregate
    // without being one at the top level
    std::vector<const AggregateExpr*> in_select;
    for (const auto& expr : stmt.select_list) collectAggregates(expr.get(), in_select);
    for (const AggregateExpr* agg : in_select) {
        if (!known(aggregateOutputName(agg))) specs.push_back(makeSpec(agg, /*hidden=*/false));
    }

    // aggregates referenced only in HAVING or ORDER BY are computed too, as
    // hidden tail columns the final projection drops
    std::vector<const AggregateExpr*> referenced;
    collectAggregates(stmt.having.get(), referenced);
    for (const auto& item : stmt.order_by) collectAggregates(item.expr.get(), referenced);
    for (const AggregateExpr* agg : referenced) {
        if (!known(aggregateOutputName(agg))) specs.push_back(makeSpec(agg, /*hidden=*/true));
    }

    return specs;
}


// One expression GROUP BY key, in both identities: `canonical` (exprKey, used
// for matching so a qualifier difference does not defeat it) and `display`
// (exprToString, used for the output column name users see).
struct GroupKeyName {
    std::string canonical;
    std::string display;
};

// depth-first replacement for substituteGroupKeyRefs; stops at AggregateExpr
// (arguments evaluate pre-aggregate) and at leaves (a plain ColumnRef can
// never match — expression keys are non-ColumnRef by construction)
static void substituteInto(std::unique_ptr<Expr>& expr,
                           const std::vector<GroupKeyName>& keys) {
    if (!expr) return;
    if (dynamic_cast<AggregateExpr*>(expr.get())) return;
    if (dynamic_cast<ColumnRef*>(expr.get()) || dynamic_cast<Literal*>(expr.get())) return;

    // Match on the canonical key (slot-based), but name the synthesized ref with
    // the GROUP BY expression's display name. That is what lets
    // SELECT laps.season - 1 ... GROUP BY season - 1 resolve: the two render
    // differently under exprToString but identically under exprKey, and the
    // output column is named once, from the group key.
    std::string s = exprKey(expr.get());
    for (const auto& key : keys) {
        if (key.canonical == s) {
            auto ref = std::make_unique<ColumnRef>();
            ref->column_name = key.display;   // matches buildAggregateSchema's output name
            ref->alias = expr->alias;         // preserve a select-item alias
            expr = std::move(ref);
            return;
        }
    }

    if (auto* bin = dynamic_cast<BinaryExpr*>(expr.get())) {
        substituteInto(bin->left, keys);
        substituteInto(bin->right, keys);
        return;
    }
    if (auto* un = dynamic_cast<UnaryExpr*>(expr.get())) {
        substituteInto(un->operand, keys);
        return;
    }
    if (auto* isn = dynamic_cast<IsNullExpr*>(expr.get())) {
        substituteInto(isn->operand, keys);
        return;
    }
    if (auto* in = dynamic_cast<InExpr*>(expr.get())) {
        substituteInto(in->operand, keys);
        return;
    }
    if (auto* lk = dynamic_cast<LikeExpr*>(expr.get())) {
        substituteInto(lk->operand, keys);
        return;
    }
    if (auto* c = dynamic_cast<CaseExpr*>(expr.get())) {
        for (auto& w : c->when_clauses) {
            substituteInto(w.condition, keys);
            substituteInto(w.result, keys);
        }
        substituteInto(c->else_expr, keys);
        return;
    }
    if (auto* sub = dynamic_cast<SubstringExpr*>(expr.get())) {
        substituteInto(sub->operand, keys);
        substituteInto(sub->start, keys);
        substituteInto(sub->length, keys);   // nullptr-safe
        return;
    }
    // Week 30 — DISPATCH SITE 6. The IN operand is this query's and is
    // rewritten; the body is NOT. A post-aggregate group-key rewrite is
    // scope-local, so pointing an inner reference at an outer aggregate's
    // output column would be a wrong answer with no error.
    if (auto* sq = dynamic_cast<SubqueryExpr*>(expr.get())) {
        substituteInto(sq->operand, keys);
        return;
    }
}

void substituteGroupKeyRefs(SelectStatement& stmt) {
    std::vector<GroupKeyName> keys;
    for (const auto& g : stmt.group_by) {
        if (g.expr) keys.push_back({exprKey(g.expr.get()), exprToString(g.expr.get())});
    }
    if (keys.empty()) return;

    for (auto& e : stmt.select_list) substituteInto(e, keys);
    substituteInto(stmt.having, keys);
    for (auto& item : stmt.order_by) substituteInto(item.expr, keys);
}


// LogicalScan
std::string LogicalScan::explain() const {
    return "LogicalScan [" + table_name + ", " + std::to_string(output_schema.columns().size()) + " columns]";
}


// LogicalJoin — single-key output is byte-identical to the pre-Week-26 form,
// so existing --explain assertions keep passing
std::string LogicalJoin::explain() const {
    // The left input's merged schema can hold one column name at several
    // relation slots, and a key resolved to the wrong one still returns rows —
    // so an ambiguous name is printed with the slot the key actually carries
    // (`team@1`). Without it the correct and the incorrect plan render
    // identically, on the surface used to debug them. Unambiguous names stay
    // bare, so a single-key single-join plan is unchanged.
    //
    // Week 29: the node NAME carries the join type. A suffix or a bracketed flag
    // would have changed every inner-join plan string; a distinct name leaves all
    // of them byte-identical and is unmissable in a plan dump. It also keeps the
    // substring "Join", which python_tools/test_new_queries.py greps for when it
    // sums the rows a plan's joins materialize.
    // Week 32: same argument as Week 29's — the node NAME carries the kind, so
    // every pre-existing plan string stays byte-identical, the substring "Join"
    // survives for test_new_queries.py's join-row accounting, and a semi-join is
    // unmissable in a plan dump.
    std::string s = "LogicalJoin [";
    if (semantics == JoinSemantics::SEMI)      s = "LogicalSemiJoin [";
    else if (semantics == JoinSemantics::ANTI) s = "LogicalAntiJoin [";
    else if (join_type == JoinType::LEFT)      s = "LogicalLeftJoin [";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) s += " AND ";
        const Schema& left = children[0]->output_schema;
        int idx = keys[i].from_slot >= 0
            ? left.indexOf(keys[i].from_col, keys[i].from_slot) : -1;
        s += (idx >= 0 ? qualifyIfAmbiguous(left, idx) : keys[i].from_col)
           + " = " + keys[i].join_col;
    }
    s += "]";
    // Week 29: outer joins only — an inner join's residuals are in the WHERE
    // conjunction and print on their own filter node, as they always have.
    if (on_residual) s += " residual=" + exprToString(on_residual.get());
    // Week 28: the join-order decision, on the top join of an enumerated tree
    // only. Empty everywhere else, so every pre-existing plan string — single
    // joins, --no-optimize, hand-built test trees — is unchanged.
    if (!order_decision.empty()) s += " " + order_decision;
    return s;
}


// LogicalFilter
std::string LogicalFilter::explain() const {
    return "LogicalFilter [" + exprToString(predicate.get()) + "]";
}


// LogicalAggregate
std::string LogicalAggregate::explain() const {
    std::string s = "LogicalAggregate [";
    if (!group_by.empty()) {
        for (size_t i = 0; i < group_by.size(); ++i) {
            if (i) s += ", ";
            if (group_by[i].expr) {
                s += exprToString(group_by[i].expr.get());
                continue;
            }
            if (!group_by[i].table_name.empty()) s += group_by[i].table_name + ".";
            s += group_by[i].column_name;
        }
        s += " | ";
    }
    for (size_t i = 0; i < aggregates.size(); ++i) {
        if (i) s += ", ";
        s += aggregates[i].column.empty() && aggregates[i].argument && !aggregates[i].is_star
            ? aggregates[i].output_name
            : aggregates[i].function + "(" + (aggregates[i].is_star ? "*" : aggregates[i].column) + ")";
    }
    return s + "]";
}


// LogicalProject
std::string LogicalProject::explain() const {
    std::string s = "LogicalProject [";
    for (int i = 0; i < output_schema.size(); ++i) {
        if (i) s += ", ";
        s += output_schema.column(i).name;
    }
    return s + "]";
}


// LogicalSort
std::string LogicalSort::explain() const {
    std::string s = "LogicalSort [";
    for (size_t i = 0; i < order_by.size(); ++i) {
        if (i) s += ", ";
        s += exprToString(order_by[i].expr.get());
        if (order_by[i].desc) s += " DESC";
    }
    return s + "]";
}


// LogicalDistinct
std::string LogicalDistinct::explain() const {
    return "LogicalDistinct";
}


// LogicalLimit
std::string LogicalLimit::explain() const {
    return "LogicalLimit [" + std::to_string(limit) + "]";
}


// LogicalPlanBuilder
std::unique_ptr<LogicalPlanNode> LogicalPlanBuilder::build(SelectStatement stmt, const Catalog& catalog) {
    Validator::validate(stmt, catalog);

    // expression GROUP BY keys: rewrite post-aggregate references to the
    // aggregate's group-key output columns (no-op without expression keys)
    substituteGroupKeyRefs(stmt);

    // FROM scan, narrowed to only the columns the query actually needs
    std::unique_ptr<LogicalPlanNode> node = std::make_unique<LogicalScan>(
        stmt.from_table,
        buildScanSchema(stmt, catalog.getTable(stmt.from_table).schema));

    // ON conjuncts that are not equi-join keys, cloned out of the statement's
    // join trees (which die with `stmt` at the end of this function) and folded
    // into the WHERE conjunction below.
    std::vector<std::unique_ptr<Expr>> on_residuals;

    // joins, folded left-deep in written order: joins[i] attaches relation i+1
    for (size_t i = 0; i < stmt.joins.size(); ++i) {
        const auto& jc = stmt.joins[i];
        const int join_slot = static_cast<int>(i) + 1;   // range-table position

        const TableMetadata& join_meta = catalog.getTable(jc.join_table);
        auto join_scan = std::make_unique<LogicalScan>(
            jc.join_table,
            buildScanSchema(stmt, join_meta.schema));

        // classifyJoinCondition routes keys by binder-assigned slot — the only
        // way to disambiguate a self-join's occurrences of the same table —
        // and hands back every non-key conjunct as a residual (Week 27).
        JoinCondition on = classifyJoinCondition(jc.condition.get(), join_slot);
        std::vector<JoinKey> keys = std::move(on.keys);

        // Week 29: where the residuals go is decided by the join type, and only
        // here. INNER: R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S), so they join the WHERE
        // conjunction below and inherit pushdown for free (Week 27). LEFT: that
        // identity is false — moving q out of the ON deletes the left rows it
        // rejects instead of null-extending them — so they stay attached to this
        // join and become part of its match test.
        std::unique_ptr<Expr> on_pred;
        if (jc.type == JoinType::LEFT) {
            std::vector<std::unique_ptr<Expr>> parts;
            for (const Expr* r : on.residuals) parts.push_back(cloneExpr(r));
            if (!parts.empty()) on_pred = conjoinAll(std::move(parts));
        } else {
            for (const Expr* r : on.residuals) on_residuals.push_back(cloneExpr(r));
        }

        // Output schema order is always [relation 0 columns, relation 1, ...] —
        // fixed logical order. The left child already carries slots
        // 0..join_slot-1 from the joins beneath it; only the newly added side is
        // stamped, so qualified references resolve to the correct relation even
        // when several share a column name. By-value loop var: a reference would
        // mutate the join scan's own schema.
        std::vector<ColumnDef> merged_cols = node->output_schema.columns();
        for (ColumnDef col : join_scan->output_schema.columns()) {
            col.relation_slot = join_slot;
            merged_cols.push_back(col);
        }

        // no build/probe swap decision here — that's a physical concern
        // (Week 18/22), not part of the logical plan.
        node = std::make_unique<LogicalJoin>(std::move(node), std::move(join_scan), std::move(keys), join_slot, Schema(merged_cols));

        if (jc.type == JoinType::LEFT) {
            auto* lj = static_cast<LogicalJoin*>(node.get());
            lj->join_type = JoinType::LEFT;
            if (on_pred) {
                // plan-time type check against the MERGED schema, exactly as the
                // WHERE conjunction is checked below — an ill-typed residual must
                // fail here, not per row inside the probe loop. It has to be the
                // merged schema: a residual may span both sides.
                inferExprType(on_pred.get(), lj->output_schema);
                lj->on_residual = std::move(on_pred);
            }
        }
    }

    // Residual ON conjuncts join the WHERE conjunction rather than getting their
    // own LogicalFilter. For an inner join that is semantics-preserving
    // (R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S)), and it is not merely tidier: PredicatePushdown
    // only rewrites a FILTER whose DIRECT child is a JOIN, so a second stacked
    // filter would leave the WHERE unpushed and silently cost every joined query
    // its pushdown. One filter, one push, every conjunct routed by soleSlot().
    // The conjunction is type-checked and pushed exactly like a written WHERE.
    if (!on_residuals.empty()) {
        if (stmt.where) on_residuals.push_back(std::move(stmt.where));
        stmt.where = conjoinAll(std::move(on_residuals));
    }

    // Week 32 — set-membership lowering, BETWEEN the spine and the WHERE filter.
    // Position is load-bearing in both directions: the semi-join's probe input
    // must be the whole FROM/JOIN spine (so the operand's binder slot resolves
    // in the same domain leftKeyIndices() uses), and the IN node must leave the
    // predicate BEFORE inferExprType walks it below — dispatch site 12 still
    // throws on a surviving SubqueryExpr, deliberately, so a missed nesting
    // position is loud rather than silently wrong.
    if (stmt.where) {
        std::vector<std::unique_ptr<Expr>> conjuncts;
        splitConjuncts(std::move(stmt.where), conjuncts);
        InLoweringResult lowered = lowerInSubqueries(std::move(node), conjuncts, catalog);
        // Week 33 — decorrelation, at the SAME site and on the same conjunct
        // vector, for the same two reasons: the semi-join's probe input must be
        // the whole FROM/JOIN spine, and the node must leave the predicate
        // before inferExprType walks it below.
        ExistsLoweringResult decorrelated =
            lowerExistsSubqueries(std::move(lowered.plan), conjuncts, catalog);
        node = std::move(decorrelated.plan);
        stmt.where = conjoinAll(std::move(conjuncts));
        // Anything left holding an IN node is a shape lowering cannot express —
        // an IN under an OR, most of all. Refuse by name here rather than let
        // site 12 report it as a materialization defect.
        refuseUnloweredIn(stmt.where.get(), "a non-top-level position");
        refuseUnloweredCorrelated(stmt.where.get(), "a non-top-level position");
    }

    // filter (WHERE)
    if (stmt.where) {
        // plan-time type check: reject STRING arithmetic here instead of
        // per-row "Type mismatch" throws during execution
        inferExprType(stmt.where.get(), node->output_schema);
        node = std::make_unique<LogicalFilter>(std::move(node), std::move(stmt.where));
    }

    // aggregate (GROUP BY + aggregates) — detection must recurse:
    // AVG(speed) * 2 is not a top-level AggregateExpr but still aggregates
    bool has_aggs = false;
    {
        std::vector<const AggregateExpr*> found;
        for (auto& expr : stmt.select_list) collectAggregates(expr.get(), found);
        has_aggs = !found.empty();
    }

    if (!stmt.group_by.empty() || has_aggs) {
        std::vector<AggregateSpec> agg_specs = extractAggregates(stmt);
        Schema agg_schema = buildAggregateSchema(stmt.group_by, agg_specs, node->output_schema);
        node = std::make_unique<LogicalAggregate>(std::move(node), stmt.group_by, std::move(agg_specs), agg_schema);
    }

    // HAVING — filter above the aggregate; no dedicated node
    if (stmt.having) {
        // Week 32: HAVING's IN is not lowered. The join would have to sit ABOVE
        // LogicalAggregate — legal, but no TPC-H query needs it (Q11's HAVING
        // subquery is scalar), and lowering only WHERE is the minimum code that
        // solves the problem. Stated in the README dialect table and pinned in
        // the rejection suite, because the diffed oracle cannot hold a query
        // that errors.
        refuseUnloweredIn(stmt.having.get(), "HAVING");
        refuseUnloweredCorrelated(stmt.having.get(), "HAVING");
        inferExprType(stmt.having.get(), node->output_schema);
        node = std::make_unique<LogicalFilter>(std::move(node), std::move(stmt.having));
    }

    // sort (ORDER BY) — must evaluate against pre-projection schema
    if (!stmt.order_by.empty()) {
        for (const auto& item : stmt.order_by) {
            inferExprType(item.expr.get(), node->output_schema);
        }
        node = std::make_unique<LogicalSort>(std::move(node), std::move(stmt.order_by));
    }

    // project; SELECT list — placed after Sort so sort expressions resolve against full schema
    if (stmt.select_star) {
        const Schema& child_schema = node->output_schema;
        std::vector<std::unique_ptr<Expr>> star_exprs;
        std::vector<ColumnDef> star_cols;
        for (const auto& col : child_schema.columns()) {
            if (col.hidden) continue; // HAVING/ORDER-BY-only aggregates never reach output
            auto ref = std::make_unique<ColumnRef>();
            ref->column_name = col.name;
            ref->id = ColumnId::local(col.relation_slot);  // schema slot -> local id:
            // preserve side so SELECT * on a self-join emits both sides
            star_exprs.push_back(std::move(ref));
            star_cols.push_back(col);
        }
        node = std::make_unique<LogicalProject>(std::move(node), std::move(star_exprs), Schema(star_cols));
    } else {
        Schema proj_schema = buildProjectSchema(stmt, node->output_schema);
        node = std::make_unique<LogicalProject>(std::move(node), std::move(stmt.select_list), proj_schema);
    }

    // DISTINCT — runs on projected rows
    if (stmt.distinct) {
        node = std::make_unique<LogicalDistinct>(std::move(node));
    }

    // limit
    if (stmt.limit.has_value()) {
        node = std::make_unique<LogicalLimit>(std::move(node), stmt.limit.value());
    }

    return node;
}