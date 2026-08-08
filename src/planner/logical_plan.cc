#include "logical_plan.h"
#include "join_condition.h"
#include "validator.h"
#include "subquery_lowering.h"
#include "subquery_decorrelation.h"
#include "subquery_materialization.h"   // forEachSubqueryConst (dispatch site 19)
#include "parser/expr_utils.h"
#include "parser/expr_totality.h"   // exprMayRaise / firstMayRaise — applyLimit
                                    // below, and guardLoweredConjunctPrefix
#include <unordered_set>

// See the header for the rule, the measurement and the bounded decline.
std::unique_ptr<LogicalPlanNode> guardLoweredConjunctPrefix(
        std::unique_ptr<LogicalPlanNode> spine,
        std::vector<std::unique_ptr<Expr>>& prefix) {
    if (prefix.empty()) return spine;
    // The prefix is written against the spine's output schema — for a semi/anti
    // join that IS its left child's, so a second extraction screens against the
    // same columns the first one did.
    if (firstMayRaise(prefix, spine->output_schema) == prefix.size()) return spine;
    for (const auto& c : prefix) {
        bool holds_subquery = false;
        forEachSubqueryConst(c.get(), [&](const SubqueryExpr&) { holds_subquery = true; });
        if (holds_subquery) return spine;
    }
    auto guarded = std::move(prefix);
    prefix.clear();
    return std::make_unique<LogicalFilter>(std::move(spine), conjoinAll(std::move(guarded)));
}



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
        // Week 33. A CORRELATED ref indexes an ENCLOSING block's range table,
        // so the slot lookup below would compare two numbering domains. Skip
        // straight to the bare-name fallback, which is byte-identical to what
        // the pre-ColumnId code did whenever that lookup missed — and the fact
        // that it could also HIT, on a shared column name, is the silent wrong
        // answer ColumnId's throw exposed here. Trusting the type of the
        // same-named column is the same "a lower layer already established
        // this" move validateExpr (site 4) and validateJoinCondition (site 18)
        // make; the Binder verified the ref against the scope that supplies it.
        int idx = (col->id.isResolved() && col->id.isLocal())
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
        //
        // !! AND THIS THROW IS WHY `foldConstants` IS NOT OPTIONAL (seam audit
        // pass 3, L-2). `foldDateInterval` is the ONLY thing that removes an
        // IntervalLiteral, so this line fires on every well-formed
        // `date ± interval` query — which is every TPC-H date-range predicate —
        // the moment folding does not run before LogicalPlanBuilder::build.
        // That ORDERING DEPENDENCY is guaranteed today only by folding being the
        // last step of Binder::bind; nothing asserts it. A reader who took
        // constant_folding.h's old word "canonicalization" at face value and
        // gated the pass would land here, on the `--no-optimize` leg only.
        throw std::runtime_error(
            "INTERVAL is only valid in constant date arithmetic, "
            "e.g. date '1994-01-01' + interval '1' year");
    }
    if (dynamic_cast<const SubqueryExpr*>(expr)) {
        // DISPATCH SITE 12, closed in Week 31 — as an INTERNAL invariant, not as
        // a feature. Every UNCORRELATED subquery is replaced by a constant
        // before planning (materializeSubqueries, run by main.cc after
        // Validator::validate).
        //
        // Week 33: this comment used to add "and a correlated one is refused by
        // the Validator". That refusal is gone. A correlated node now SURVIVES
        // materialization deliberately, and three refusals stand between it and
        // this throw, none of them the Validator's: lowerExistsSubqueries
        // consumes the decorrelatable shapes, and refuseUnloweredCorrelated runs
        // on the WHERE residue (:836) and on HAVING (:868). The remaining
        // positions — SELECT list, GROUP BY, ORDER BY, ON — hold no subquery at
        // all, correlated or not: Validator's POSITION rule (WHERE and HAVING
        // only, ast.h) and validateJoinCondition refuse them outright, and that
        // rule Week 33 did NOT touch. Verified rather than assumed:
        // `SELECT d.name, EXISTS (...) FROM drivers d` reports "SELECT:
        // subqueries are supported in WHERE and HAVING only", not this throw.
        //
        // Reaching here therefore still means the materialization walker
        // (dispatch site 19) missed an Expr subtype, or the pass was not run at
        // all — but by a different argument than the one first written down.
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
    // Week 33, Task 8. STILL WIDENS for a correlated query, and that is a
    // measured cost rather than an oversight. Week 31 gave projection pushdown
    // back to MATERIALIZED subquery queries by clearing has_subquery once every
    // node had become a constant; decorrelation cannot do the same, because this
    // function runs while the FROM/JOIN spine is being built and the EXISTS
    // conjunct is still in stmt.where — the extraction happens later, at
    // lowerExistsSubqueries. So every correlated query scans its outer relation
    // at full width.
    //
    // Narrowing correctly means collecting the outer columns the body's
    // correlated refs name (they must survive) plus the ones the outer query
    // uses, which is a second walker over the body — the precise-collectSlots
    // work Week 30 handed to this week. It is a PERFORMANCE change with a real
    // wrong-answer failure mode (a narrowed-away correlated column dies later as
    // "column not found", far from the cause), so it is left for a week that can
    // land it against a benchmark rather than squeezed in beside the semantics.
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


Schema derivedRelationSchema(Schema body_schema, const TableRef& ref) {
    std::vector<ColumnDef> cols = body_schema.columns();

    if (!ref.columnAliases().empty()) {
        if (ref.columnAliases().size() != cols.size()) {
            throw std::runtime_error(
                "derived table '" + ref.alias() + "' has "
                + std::to_string(cols.size()) + " columns but "
                + std::to_string(ref.columnAliases().size())
                + " column aliases were supplied");
        }
        for (size_t i = 0; i < cols.size(); ++i) cols[i].name = ref.columnAliases()[i];
    }

    for (auto& col : cols) {
        col.relation_slot = 0;   // a leaf's own schema; see the header
        // A body's HAVING/ORDER-BY-only aggregate is computed inside the body
        // and is not a column of the RELATION. build()'s star expansion already
        // drops them at the body's own project, so this cannot fire today — but
        // stating it here is what makes "the derived relation's columns are
        // exactly its select list" true by construction rather than by accident.
        col.hidden = false;
    }

    for (size_t i = 0; i < cols.size(); ++i) {
        for (size_t j = i + 1; j < cols.size(); ++j) {
            if (cols[i].name != cols[j].name) continue;
            throw std::runtime_error(
                "derived table '" + ref.alias() + "': column '" + cols[i].name
                + "' is produced twice; give one of them an alias");
        }
    }
    return Schema(cols);
}


Schema blockOutputSchema(const SelectStatement& stmt, const Catalog& catalog) {
    // 1. The spine, in written order: FROM then joins[i] at slot i+1. Same
    //    stamping rule build() uses — a leaf's own columns keep slot 0 and only
    //    the newly added side is stamped — so a name that appears on both sides
    //    resolves by slot exactly as it will in the real plan.
    auto relationSchemaFor = [&](const TableRef& ref) {
        if (!ref.isDerived())
            return buildScanSchema(stmt, catalog.getTable(
                ref.tableName("blockOutputSchema relation")).schema);
        // A derived table inside a derived table: recurse, then apply the same
        // normalization buildRelation applies (rename, flatten to slot 0), so
        // the two agree by construction rather than by inspection.
        return derivedRelationSchema(blockOutputSchema(*ref.body(), catalog), ref);
    };

    // NAMED locals, not `for (col : relationSchemaFor(...).columns())`. That form
    // ranges over a reference INTO a temporary Schema, and a range-for extends
    // the lifetime only of the range-init expression's own top-level temporary —
    // `.columns()` returns a reference, so the Schema dies before the first
    // iteration. It read freed memory and surfaced as `basic_string::_M_create`
    // from a garbage column name, on the first joining derived body tried.
    const Schema from_schema = relationSchemaFor(stmt.from);
    std::vector<ColumnDef> spine = from_schema.columns();
    for (size_t i = 0; i < stmt.joins.size(); ++i) {
        const Schema join_schema = relationSchemaFor(stmt.joins[i].relation);
        for (ColumnDef col : join_schema.columns()) {
            col.relation_slot = static_cast<int>(i) + 1;
            spine.push_back(col);
        }
    }
    Schema schema(spine);

    // 2. Aggregate, when there is one. Same detection as build(): recursive,
    //    because AVG(speed) * 2 aggregates without being an AggregateExpr.
    bool has_aggs = false;
    for (const auto& e : stmt.select_list) {
        std::vector<const AggregateExpr*> found;
        collectAggregates(e.get(), found);
        if (!found.empty()) { has_aggs = true; break; }
    }
    if (!stmt.group_by.empty() || has_aggs) {
        schema = buildAggregateSchema(stmt.group_by, extractAggregates(stmt), schema);
    }

    // 3. Project. SELECT * drops hidden columns, exactly as build()'s star
    //    expansion does — a body's HAVING/ORDER-BY-only aggregate is not a
    //    column of the derived RELATION.
    //
    //    !! THIS IS WHY THIS FUNCTION NEED NOT MODEL SUBQUERY LOWERING, and the
    //    reason is worth stating because it looks like an omission. Steps 1-2
    //    model the spine and the aggregate only; the join lowerCorrelatedScalars
    //    grafts is invisible here. For a NAMED select list that is harmless (the
    //    project schema is written from the list). For a `SELECT *` body it
    //    would NOT be — the star would expand over the widened schema in build()
    //    and over the narrow one here, and the drift check in buildRelation
    //    would report an internal defect for legal SQL. That was seam audit pass
    //    2's B-1, and it is closed at the OTHER end: the lowering marks its
    //    synthetic columns hidden, so build()'s star drops exactly the columns
    //    this function never had. The two agree by construction, and this
    //    function stays a model of what the USER wrote.
    if (stmt.select_star) {
        std::vector<ColumnDef> star;
        for (const auto& col : schema.columns()) {
            if (!col.hidden) star.push_back(col);
        }
        return Schema(star);
    }
    return buildProjectSchema(stmt, schema);
}


std::vector<AggregateSpec> extractAggregates(const SelectStatement& stmt){
    std::vector<AggregateSpec> specs;

    auto makeSpec = [](const AggregateExpr* agg, bool hidden) {
        AggregateSpec spec;
        spec.function = agg->function_name;
        spec.is_star = agg->is_star;
        spec.distinct = agg->distinct;   // Week 34; also encoded in output_name
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

std::string LogicalDerived::explain() const {
    // The decline suffix is appended only when there IS one; see the field.
    return "LogicalDerived [" + alias + ", "
         + std::to_string(output_schema.columns().size()) + " columns]"
         + (pushdown_decision.empty() ? "" : " " + pushdown_decision);
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
    // Printed distinctly: the two differ only in their NULL rule, so a plan
    // that prints the same for both is a plan that cannot show the bug.
    else if (semantics == JoinSemantics::ANTI_NOT_IN) s = "LogicalAntiJoin [NOT IN, ";
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

// Week 36 — see the long note at kResidualBuildSlot (logical_plan.h). The build
// half is re-stamped so the residual's body-side refs resolve to it by SLOT and
// can never fall through to a same-named probe column. `hidden` is cleared for
// the same reason it is set elsewhere: this schema feeds evaluate() and two
// static screens, never a `SELECT *` expansion, so carrying the flag would only
// invite a reader to think it meant something here.
Schema joinResidualSchema(const Schema& probe, const Schema& build) {
    std::vector<ColumnDef> cols = probe.columns();
    cols.reserve(cols.size() + build.columns().size());
    for (ColumnDef c : build.columns()) {
        c.relation_slot = kResidualBuildSlot;
        c.hidden = false;
        cols.push_back(std::move(c));
    }
    return Schema(std::move(cols));
}

Schema joinResidualSchema(const LogicalJoin& join) {
    if (join.semantics == JoinSemantics::STANDARD) return join.output_schema;
    return joinResidualSchema(join.children[0]->output_schema,
                              join.children[1]->output_schema);
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
    // No declared keys = the deterministic cut a LIMIT over a plan-dependent
    // order gets (deterministicCut, above): the comparator falls through to the
    // canonical whole-row tie-break. Say so, rather than printing "[]".
    if (order_by.empty()) return "LogicalSort [canonical row order]";
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
namespace {

// One relation of the block being planned. Week 34 — either a catalog scan or a
// DERIVED TABLE, and the two must be built by one function because everything
// above them (the merged schema, slot stamping, key routing) treats them alike.
std::unique_ptr<LogicalPlanNode> buildRelation(TableRef& ref,
                                               const SelectStatement& outer,
                                               const Catalog& catalog) {
    if (!ref.isDerived()) {
        return std::make_unique<LogicalScan>(
            ref.tableName("buildRelation scan"),
            buildScanSchema(outer, catalog.getTable(
                ref.tableName("buildRelation schema")).schema));
    }

    // The body is a query BLOCK: build() it exactly as a top-level statement.
    // That is what makes its refs level 0 against its OWN range table and its
    // Validator errors the ones the body is entitled to.
    //
    // Projection pushdown into the body is deliberately NOT attempted.
    // buildScanSchema narrows a CATALOG schema by bare name over one flat
    // schema; a derived relation's schema IS its body's select list, so
    // narrowing it means rewriting that list — which changes the body's output
    // schema and therefore the range entry the enclosing block was BOUND
    // against. A real feature with a real wrong-answer failure mode, and not
    // this week's; the cost is a stated one, in README Limitations, so it lands
    // in Week 37's numbers as an expectation rather than a surprise.
    // Computed BEFORE the body is moved out: this is the schema the BINDER
    // resolved this block's references against.
    Schema expected = derivedRelationSchema(blockOutputSchema(*ref.body(), catalog), ref);

    // SelectStatement is move-only; build() takes it by value. The unique_ptr is
    // released here so the TableRef stops owning a statement that has been
    // emptied — a second buildRelation on the same ref would otherwise plan a
    // husk, which is the shape Week 33's use_count() > 1 refusal guards for
    // SubqueryExpr.
    std::unique_ptr<SelectStatement> body = ref.takeBody();
    auto body_plan = LogicalPlanBuilder::build(std::move(*body), catalog);
    Schema normalized = derivedRelationSchema(body_plan->output_schema, ref);

    // THE DRIFT CHECK. blockOutputSchema and build() are two different code
    // paths, so this can GENUINELY FAIL — unlike the assertion Week 33 deleted,
    // which compared a copy of an object with the object. If it fires, the two
    // derivations have diverged, which is the two-paths failure sharing the
    // helpers was meant to prevent, and every indexOf(name, slot) above this
    // graft is resolving against a schema the Binder never saw. Loud beats a
    // clean hit on the wrong column. It runs once per derived relation.
    if (expected.size() != normalized.size()) {
        throw std::runtime_error(
            "internal: derived table '" + ref.alias() + "' was bound against a "
            + std::to_string(expected.size()) + "-column schema but planned to "
            + std::to_string(normalized.size())
            + " columns (blockOutputSchema and LogicalPlanBuilder::build disagree)");
    }
    for (int i = 0; i < expected.size(); ++i) {
        if (expected.column(i).name == normalized.column(i).name
            && expected.column(i).type == normalized.column(i).type) continue;
        throw std::runtime_error(
            "internal: derived table '" + ref.alias() + "' column " + std::to_string(i)
            + " was bound as '" + expected.column(i).name + "' but planned as '"
            + normalized.column(i).name
            + "' (blockOutputSchema and LogicalPlanBuilder::build disagree)");
    }

    return std::make_unique<LogicalDerived>(std::move(body_plan), ref.alias(),
                                            std::move(normalized));
}

// Is this subtree's OUTPUT ROW ORDER a function of the query alone, or of the
// plan the optimizer happened to choose?
//
// Seam audit pass 3, engine E-8 / optimizer B3-1. A `LIMIT` is a CUT: it turns
// its input's row ORDER into the answer's row SET, so a plan-dependent order
// below a `LIMIT` is a plan-dependent ANSWER — and it is not merely "unspecified
// SQL", because `materializeSubqueries` threads the optimize flag into the
// nested runner, so the same cut inside a scalar subquery substitutes a
// DIFFERENT CONSTANT into a query that has no order-dependence at all.
//
// Measured at HEAD before this: a plain `customer JOIN orders WHERE
// o_orderstatus='F' AND o_orderpriority='1-URGENT' LIMIT 3` returned DISJOINT
// row sets optimized vs `--no-optimize` (two ordinary conjuncts move orders'
// estimate below customer's raw count, the build side flips, the probe order
// reverses), and carried into a scalar subquery as `COUNT(*)` 11 vs 10.
//
// The two answers, per node kind:
//
//   SCAN      stable. Storage order, and the row and columnar legs reconstruct
//             the same sequence.
//   SORT      stable. That is what the comparator is for — total on
//             distinguishable rows, and rows it leaves tied are equal in every
//             column of its schema, so they project identically.
//   JOIN      NOT stable, and this is the whole finding. A hash join emits
//             probe-major; which side probes is a cost decision (and the two
//             engines make it by different rules), and `JoinEnumeration` also
//             permutes the spine. Semi/anti joins are `LogicalJoin` too and are
//             treated the same rather than argued about.
//   AGGREGATE / DISTINCT
//             stable IFF the child is. Both engines emit groups and distinct
//             representatives in FIRST-ENCOUNTER order (plan_nodes.cc's
//             `group_order`, vec_hash_aggregate_node.cc's `group_order_`), which
//             is a function of the input order and of nothing else.
//   FILTER / PROJECT / LIMIT / DERIVED
//             stable iff the child is — each is order-preserving row by row.
bool orderIsPlanStable(const LogicalPlanNode* node) {
    switch (node->type) {
    case LogicalNodeType::SCAN:
    case LogicalNodeType::SORT:
        return true;
    case LogicalNodeType::JOIN:
        return false;
    case LogicalNodeType::DERIVED:
    case LogicalNodeType::FILTER:
    case LogicalNodeType::AGGREGATE:
    case LogicalNodeType::PROJECT:
    case LogicalNodeType::DISTINCT:
    case LogicalNodeType::LIMIT:
        return orderIsPlanStable(node->children[0].get());
    }
    return false;
}

// Give a cut a determined input order, and only where it does not already have
// one. A `LogicalSort` with NO declared keys is exactly that: `rowLess` falls
// straight through to the canonical whole-row tie-break
// (execution/sort_comparator.h), which is a function of the row's values and of
// each column's `(relation_slot, name)` identity — nothing the optimizer can
// permute. It is inserted directly beneath the `LIMIT`, i.e. ABOVE the
// projection, so it orders the OUTPUT row: fewer columns to compare, and the
// projected schema's own order is a function of the SELECT list.
//
// WHAT THIS DELIBERATELY CHANGES. `LIMIT n` with no `ORDER BY` over a join now
// returns the n canonically-smallest output rows rather than the first n the
// plan happened to produce. SQL specifies neither; the project asserts
// `optimized == --no-optimize` and cross-engine agreement, and only one of the
// two can be had. SQLite's own arbitrary choice is a third answer again, which
// is why compare_against_sqlite.py's `run_engine_agreement_suite` already says
// "SQLite cannot adjudicate a tie at a LIMIT cut".
std::unique_ptr<LogicalPlanNode> deterministicCut(std::unique_ptr<LogicalPlanNode> node, int limit) {
    if (orderIsPlanStable(node.get())) return node;
    auto sort = std::make_unique<LogicalSort>(std::move(node), std::vector<OrderByItem>{});
    // BOUNDED. Only `limit` rows survive, so only `limit` need ordering — which
    // is what keeps this from turning a streaming LIMIT into a full
    // materialization of whatever reaches it. On the --no-optimize leg that can
    // be the un-pushed join product; measured at 1.8s streaming versus 74s
    // sorted on a `driver_id` self-join over `laps` (5,000,000 rows).
    sort->row_cap = limit;
    return sort;
}

// Place the LIMIT so that the PLAN, not the engine, says how many rows the
// projection beneath it is evaluated on.
//
// SEAM AUDIT PASS 4, E-13's SECOND MECHANISM. Volcano is pull-based: `LimitNode`
// asks `ProjectNode` for n rows and the projection is evaluated n times. The
// vectorized path evaluates a whole `DataChunk` in `VecProjectNode` and
// truncates afterwards in `VecLimitNode`. Since per-row evaluation is not total,
// that is not an implementation detail — it is two different answers:
//
//   SELECT SUBSTRING(name, age - 34, 2) FROM drivers LIMIT 3
//     Volcano     Dr | Dr | Dr
//     vectorized  Error: SUBSTRING: start position must be >= 1
//
//   and WITHOUT the LIMIT all four modes error, so adding a clause that can only
//   REMOVE rows made two of the four start answering.
//
// Neither engine can be talked out of its own laziness — Volcano's pipelining is
// the point of Volcano, and a chunk engine cannot evaluate exactly n rows when n
// falls inside a chunk. So the LOGICAL PLAN settles it: σ-free, row-wise π
// commutes with LIMIT (`LIMIT n (π(R)) ≡ π(LIMIT n (R))` — same rows, same
// order, because π is 1:1 and order-preserving), and putting the LIMIT BELOW the
// projection makes "n rows" a property both engines read off the plan.
//
// ONLY when the projection can raise, and that is deliberate: the rewrite is an
// equivalence, but it is also a plan change, and a plan change on every LIMIT
// query is not what this fix is for. Everything else keeps the plan it had.
//
// WHAT THIS DOES NOT COVER, stated so it is not mistaken for coverage: the swap
// applies only where the LIMIT sits DIRECTLY above the projection. With
// `deterministicCut`'s sort or a DISTINCT in between, the child is a pipeline
// breaker or a blocking operator, the projection beneath it is genuinely
// evaluated on every row, and both engines agree on that — E-17's "adding a JOIN
// turns an answer into an error" is that case, and under this rule it is the
// plan being honest rather than a defect.
std::unique_ptr<LogicalPlanNode> applyLimit(std::unique_ptr<LogicalPlanNode> node, int limit) {
    if (node->type == LogicalNodeType::PROJECT) {
        auto* project = static_cast<LogicalProject*>(node.get());
        const Schema& input = project->children[0]->output_schema;
        for (const auto& e : project->exprs) {
            if (!exprMayRaise(e.get(), input)) continue;
            project->children[0] =
                std::make_unique<LogicalLimit>(std::move(project->children[0]), limit);
            return node;   // the projection is 1:1, so one LIMIT is enough
        }
    }
    return std::make_unique<LogicalLimit>(std::move(node), limit);
}

} // namespace

std::unique_ptr<LogicalPlanNode> LogicalPlanBuilder::build(SelectStatement stmt, const Catalog& catalog) {
    Validator::validate(stmt, catalog);

    // expression GROUP BY keys: rewrite post-aggregate references to the
    // aggregate's group-key output columns (no-op without expression keys)
    substituteGroupKeyRefs(stmt);

    // FROM relation: a scan narrowed to the columns the query needs, or — Week
    // 34 — a whole derived subtree normalized into one relation of this block.
    std::unique_ptr<LogicalPlanNode> node = buildRelation(stmt.from, stmt, catalog);

    // ON conjuncts that are not equi-join keys, cloned out of the statement's
    // join trees (which die with `stmt` at the end of this function) and folded
    // into the WHERE conjunction below.
    std::vector<std::unique_ptr<Expr>> on_residuals;

    // joins, folded left-deep in written order: joins[i] attaches relation i+1
    for (size_t i = 0; i < stmt.joins.size(); ++i) {
        const auto& jc = stmt.joins[i];
        const int join_slot = static_cast<int>(i) + 1;   // range-table position

        // Week 34: the SAME helper as the FROM position. The merged-schema loop
        // below re-stamps this side with join_slot whatever it is, so a derived
        // join needs no new stamping code — which is the payoff of normalizing
        // the derived subtree's own schema to slot 0.
        auto join_scan = buildRelation(
            const_cast<TableRef&>(jc.relation), stmt, catalog);

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
        // Week 34 — correlated SCALAR subqueries (Q17), at the same site and for
        // the same two reasons: the join's probe input must be the whole
        // FROM/JOIN spine so the correlated operand's binder slot resolves in the
        // domain leftKeyIndices() uses, and the node must leave the predicate
        // before inferExprType walks it below.
        //
        // AFTER the other two, deliberately. A scalar node is not a whole
        // conjunct, so this pass walks INTO each conjunct; running it first would
        // have it descend into an IN or EXISTS operand that the earlier passes
        // are about to remove wholesale.
        //
        // range_table_size is 1 + joins.size(): the slot this pass hands its
        // derived relation is one past the last relation the Binder issued, and
        // JoinEnumeration's out-of-range test must be able to explain it.
        ScalarLoweringResult scalars = lowerCorrelatedScalars(
            std::move(decorrelated.plan), conjuncts,
            static_cast<int>(stmt.joins.size()) + 1, catalog);
        node = std::move(scalars.plan);
        stmt.where = conjoinAll(std::move(conjuncts));
        // Anything left holding an IN node is a shape lowering cannot express —
        // an IN under an OR, most of all. Refuse by name here rather than let
        // site 12 report it as a materialization defect.
        //
        // CORRELATION IS TESTED FIRST, and the order is a diagnostic decision,
        // not a stylistic one. A correlated IN is left in `conjuncts` on
        // purpose (subquery_lowering.cc declines it), so it IS a whole top-level
        // conjunct — refuseUnloweredIn would report "not a whole top-level WHERE
        // conjunct" about a query that is exactly that, naming the wrong cause
        // for the right refusal. refuseUnloweredCorrelated declines to fire when
        // nothing is correlated, so an ordinary IN under an OR still reaches the
        // message that owns it.
        refuseUnloweredCorrelated(stmt.where.get(), "a non-top-level position");
        refuseUnloweredIn(stmt.where.get(), "a non-top-level position");
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
        // Same order, same reason as the WHERE site above.
        refuseUnloweredCorrelated(stmt.having.get(), "HAVING");
        refuseUnloweredIn(stmt.having.get(), "HAVING");
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
            // TWO kinds of column are skipped here, not one: a HAVING/ORDER-BY-
            // only aggregate, and (seam audit pass 2, B-1) the columns of a
            // synthetic `$scalarN` relation that lowerCorrelatedScalars grafted
            // into this block. `*` means "the columns of the relations the user
            // wrote", and this expansion runs LAST — over a schema the subquery
            // lowerings have already widened. See common/schema.h on `hidden`.
            if (col.hidden) continue;
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

    // limit — over a determined input order; see deterministicCut above
    if (stmt.limit.has_value()) {
        node = deterministicCut(std::move(node), stmt.limit.value());
        node = applyLimit(std::move(node), stmt.limit.value());
    }

    // Seam audit pass 3, B3-2 — THE JOIN-KEY TYPE RULE, once, on the finished
    // tree. This is the only point at which all four JoinKey producers converge:
    // the `stmt.joins` fold above, lowerInSubqueries, lowerExistsSubqueries and
    // lowerCorrelatedScalars have all run, and every node they built is in this
    // tree. Checking at the producers instead is writing the rule four times,
    // which is how it came to be missing three times.
    //
    // LAST, not first, and both halves of that matter. It needs `semantics` and
    // the projected body schemas, which only exist after the lowerings; and a
    // genuine query defect (a missing column, an ill-typed WHERE) must still
    // outrank an engine rule, which the checks above deliver by running before
    // it. Nested blocks validate their own subtree in their own build() and are
    // then re-walked here as part of this one — idempotent, and the inner
    // message is the more local one.
    Validator::validateJoinKeyTypes(*node);

    return node;
}