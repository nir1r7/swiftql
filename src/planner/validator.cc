#include "validator.h"
#include "join_condition.h"
#include <stdexcept>
#include "parser/expr_utils.h"

namespace {

// Every ColumnRef reachable outside an aggregate must be a GROUP BY column,
// or sit inside a subtree matching a GROUP BY expression under exprKey — the
// slot-based canonical identity, so SELECT laps.season - 1 matches
// GROUP BY season - 1 (both slot 0). Matching on exprToString instead would
// compare the as-typed qualifier and reject that pair, while the plain-column
// path below already matches by (relation_slot, column_name). The planner's
// group-key substitution uses the same identity.
// AggregateExpr terminates the walk: its argument is evaluated pre-grouping.
void checkGroupedRefs(const Expr* expr, const std::vector<GroupByColumn>& group_by) {
    if (!expr) return;
    if (dynamic_cast<const AggregateExpr*>(expr)) return;
    for (const auto& g : group_by) {
        if (g.expr && exprKey(g.expr.get()) == exprKey(expr)) return;
    }
    // Week 30 — DISPATCH SITE 5. A subquery is refused in a select list by
    // validateExpr, so this is unreachable from the outer query; it IS reachable
    // inside a grouped subquery, via validateExpr's recursive validateQuery.
    // Grouping is scope-local, so the body is not this query's to check.
    if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        checkGroupedRefs(sq->operand.get(), group_by);
        return;
    }
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // Week 30. A correlated ref is supplied by an enclosing query, so it is
        // constant within every group of THIS one and needs no GROUP BY entry.
        if (col->query_level > 0) return;
        for (const auto& g : group_by) {
            if (g.expr) continue;
            // name match plus slot compatibility: SELECT a.grp with
            // GROUP BY b.grp is a different column, not a match.
            // Unbound slots (-1) stay name-only for direct-validate callers.
            if (g.column_name == col->column_name &&
                (col->relation_slot < 0 || g.relation_slot < 0 ||
                 col->relation_slot == g.relation_slot)) {
                return;
            }
        }
        throw std::runtime_error("SELECT column '" + col->column_name + "' must appear in GROUP BY or be used in an aggregate function");
    }
    // A subtree that matches a group key except for a literal's TYPE is the most
    // confusing near-miss: exprToString renders the DOUBLE 1.0 as "1", so
    // `SELECT season - 1.0` with `GROUP BY season - 1` reads as identical text but
    // is a different expression with a different result type. Treating them as one
    // key made the projection read the INT group column and truncate — `(season -
    // 1.0) / 2` returned 1010 instead of 1010.5. Say what actually differs.
    for (const auto& g : group_by) {
        if (g.expr && exprToString(g.expr.get()) == exprToString(expr)) {
            throw std::runtime_error(
                "GROUP BY expression '" + exprToString(g.expr.get())
                + "' does not match the SELECT expression: they differ in a literal's "
                  "type (INT vs DOUBLE), which changes the result type. Write the same "
                  "literal in both clauses.");
        }
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        checkGroupedRefs(bin->left.get(), group_by);
        checkGroupedRefs(bin->right.get(), group_by);
        return;
    }
    if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) {
        checkGroupedRefs(isn->operand.get(), group_by);
        return;
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        checkGroupedRefs(un->operand.get(), group_by);
        return;
    }
    // Week 25 nodes. This is a SEPARATE dispatch site from validateExpr below,
    // and it fails silently: a subtype missed here lets an ungrouped column
    // reference through validation, and the query then dies at execution with
    // "Column not found in schema" against the post-aggregate schema.
    if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        checkGroupedRefs(in->operand.get(), group_by);
        return;
    }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        checkGroupedRefs(lk->operand.get(), group_by);
        return;
    }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        for (const auto& w : c->when_clauses) {
            checkGroupedRefs(w.condition.get(), group_by);
            checkGroupedRefs(w.result.get(), group_by);
        }
        checkGroupedRefs(c->else_expr.get(), group_by);
        return;
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        checkGroupedRefs(sub->operand.get(), group_by);
        checkGroupedRefs(sub->start.get(), group_by);
        checkGroupedRefs(sub->length.get(), group_by);   // nullptr-safe
        return;
    }
    // Literal / IntervalLiteral: fine
}

} // namespace

void Validator::validate(const SelectStatement& stmt, const Catalog& catalog){
    validateQuery(stmt, catalog);

    // Week 30 — LAST, after every semantic check, including the nested query's
    // own (validateQuery runs those through validateExpr). A genuine query
    // defect outranks a temporary engine limitation, which is the discipline
    // that placed Week 26's multi-key refusal past the plan-time type checks.
    //
    // Unlike Week 26, both engines are equally incapable here: neither can
    // lower a subquery, so there is no capability difference to preserve and no
    // reason for two failure points. ONE check, four modes, one message —
    // Planner::plan and LogicalPlanBuilder::build both call validate() first,
    // so the two engines are equivalent BY CONSTRUCTION rather than by two
    // copies of a guard that can drift, which is what Week 29's audit rounds
    // were about.
    //
    // What this placement guarantees: every PARSE, BIND and VALIDATE error a
    // query is entitled to fires first — a bad nested table, a bad nested
    // column, an ungrouped reference, a wrong arity, a disallowed position.
    // What it does not: a PLAN-TIME type check (inferExprType on the WHERE,
    // buildProjectSchema on the select list) runs later than Validator, so for
    // a query that is both a subquery query and ill-typed, this message wins.
    // The wording is the same either way — dispatch site 12 emits this exact
    // string — so the only difference is which fault the user fixes first, and
    // the alternative is a second refusal site in each planner.
    if (stmt.has_subquery) {
        throw std::runtime_error(
            "subqueries are parsed and bound but not yet executable (Week 31)");
    }
}

void Validator::validateQuery(const SelectStatement& stmt, const Catalog& catalog){
    // FROM table must exist
    if (!catalog.hasTable(stmt.from_table)) {
        throw std::runtime_error(
            "Table not found: '" + stmt.from_table + "'");
    }
    const Schema& schema = catalog.getTable(stmt.from_table).schema;

    // SELECT list columns must exist (skip for SELECT *)
    if (!stmt.select_star) {
        for (const auto& expr : stmt.select_list) {
            validateExpr(expr.get(), schema, "SELECT", catalog);
        }
    }

    // aggregate functions must be applied to compatible column types, wherever
    // the aggregate appears (SELECT, HAVING, ORDER BY) and whichever join side
    // its argument resolves to
    {
        std::vector<const AggregateExpr*> aggs;
        for (const auto& expr : stmt.select_list) collectAggregates(expr.get(), aggs);
        collectAggregates(stmt.having.get(), aggs);
        for (const auto& item : stmt.order_by) collectAggregates(item.expr.get(), aggs);

        for (const AggregateExpr* agg : aggs) {
            if (agg->is_star || !agg->argument) continue;
            if (agg->function_name != "SUM" && agg->function_name != "AVG") continue;
            auto* col = dynamic_cast<const ColumnRef*>(agg->argument.get());
            if (!col) continue;

            // pick the schema the argument resolves against: binder slot when
            // bound, table-name match for unbound qualified refs, FROM otherwise
            const Schema* target = nullptr;
            if (col->relation_slot > 0
                && col->relation_slot <= static_cast<int>(stmt.joins.size())) {
                // slot k > 0 is joins[k-1]'s relation — the one arithmetic
                // identity the whole multi-way generalization rests on
                target = &catalog.getTable(stmt.joins[col->relation_slot - 1].join_table).schema;
            } else if (col->relation_slot == 0 || col->table_name.empty()) {
                target = &schema;
            } else if (col->table_name == stmt.from_table) {
                target = &schema;
            } else {
                for (const auto& j : stmt.joins) {
                    if (col->table_name != j.join_table) continue;
                    target = &catalog.getTable(j.join_table).schema;
                    break;
                }
            }
            if (!target || !target->hasColumn(col->column_name)) continue;

            TypeId t = target->column(target->indexOf(col->column_name)).type;
            if (t == TypeId::STRING){
                throw std::runtime_error(agg->function_name + "() requires a numeric column, but '" + col->column_name + "' is of type STRING");
            }
        }
    }

    // JOIN tables must exist (if present)
    if (!stmt.joins.empty()) {
        // Keyed by the name a qualified reference can actually use — the alias
        // when there is one, the table name otherwise — mirroring the Binder's
        // range table. Keying by table name made every aliased `ON` reference
        // fall through validateJoinCondition's "unknown qualifier" escape, so
        // its column-existence check did nothing for exactly the shapes Week 26
        // adds (a self-join cannot be written without aliases).
        std::vector<std::pair<std::string, const Schema*>> relations{
            {stmt.from_alias.empty() ? stmt.from_table : stmt.from_alias, &schema}};
        for (const auto& j : stmt.joins) {
            if (!catalog.hasTable(j.join_table)) {
                throw std::runtime_error(
                    "Join table not found: '" + j.join_table + "'");
            }
            relations.push_back({j.alias.empty() ? j.join_table : j.alias,
                                 &catalog.getTable(j.join_table).schema});
        }
        for (size_t i = 0; i < stmt.joins.size(); ++i) {
            if (!stmt.joins[i].condition) continue;
            // shape first (at least one equi-join key, no forward reference),
            // then column existence — a shape error is the more useful message
            // when both are wrong. The returned keys/residuals are rebuilt by
            // the planners; only the throw matters here.
            JoinCondition on = classifyJoinCondition(stmt.joins[i].condition.get(),
                                                     static_cast<int>(i) + 1);
            validateJoinCondition(stmt.joins[i].condition.get(), relations);

            // Week 29 (deferred from the Week 27 audit). A join key is compared as
            // TEXT, which carries no type tag: a STRING "7" matches an INT 7 while
            // "007" does not, while the identical predicate in a WHERE clause
            // throws Type mismatch — half a match, with no error either way, and
            // both halves reachable on the shipped catalog (drivers.team vs
            // laps.lap_id). Under an outer join the unmatched half comes back as
            // null-extended rows rather than as missing ones, which is why it is
            // closed here. Also makes the int_keys SIMD gate's assumption explicit.
            //
            // Coarse on purpose — both STRING or both numeric — matching
            // Value::operator==, which coerces INT/DOUBLE and throws only across
            // the STRING boundary, and keyFieldText's numeric affinity (7.0 and 7
            // must keep joining, as they do in SQLite).
            //
            // `relations` is in range-table order, so relations[slot] is the schema
            // a JoinKey slot addresses. Both engines route through this function,
            // so one check covers all four modes with one message.
            for (const JoinKey& k : on.keys) {
                if (k.from_slot < 0) continue;   // unbound: positional routing, no
                                                 // relation identity to be exact about
                if (k.from_slot >= static_cast<int>(relations.size())) continue;
                const Schema* left_schema  = relations[k.from_slot].second;
                const Schema* right_schema = relations[i + 1].second;
                int li = left_schema->indexOf(k.from_col);
                int ri = right_schema->indexOf(k.join_col);
                if (li < 0 || ri < 0) continue;  // existence is validateJoinCondition's
                const bool l_str = left_schema->column(li).type  == TypeId::STRING;
                const bool r_str = right_schema->column(ri).type == TypeId::STRING;
                if (l_str != r_str) {
                    throw std::runtime_error(
                        "JOIN ON: cannot join a STRING column with a numeric one ('"
                        + k.from_col + "' and '" + k.join_col + "')");
                }
            }
        }
    }

    // WHERE columns must exist; aggregates are not allowed in WHERE
    if (stmt.where) {
        validateExpr(stmt.where.get(), schema, "WHERE", catalog,
                     /*allow_aggregates=*/false, /*allow_subqueries=*/true);
    }

    // Column ordinals (ORDER BY 1 / GROUP BY 2) are not supported. Rejecting is
    // the point: a bare integer parses as a Literal, so ORDER BY 1 used to sort
    // every row by the same constant and return them unsorted with no error,
    // and GROUP BY 1 failed with a message about the SELECT list. SQLite treats
    // both as references to output column 1.
    for (const auto& item : stmt.order_by) {
        if (auto* lit = dynamic_cast<const Literal*>(item.expr.get())) {
            if (!lit->value.isNull() && lit->value.type() == TypeId::INT) {
                throw std::runtime_error(
                    "ORDER BY " + lit->value.toString() + ": column ordinals are not "
                    "supported; use a column name or a select-list alias");
            }
        }
    }
    for (const auto& g : stmt.group_by) {
        if (!g.expr) continue;
        if (auto* lit = dynamic_cast<const Literal*>(g.expr.get())) {
            if (!lit->value.isNull() && lit->value.type() == TypeId::INT) {
                throw std::runtime_error(
                    "GROUP BY " + lit->value.toString() + ": column ordinals are not "
                    "supported; use a column name or a select-list alias");
            }
        }
    }

    // GROUP BY columns must exist. Binder-resolved entries (slot stamped) were
    // already verified; the rest check against the FROM table or, when a join
    // is present, the joined table.
    for (const auto& g : stmt.group_by) {
        if (g.expr) {
            // expression group key: no aggregates inside, and every column
            // it references must exist
            std::vector<const AggregateExpr*> in_key;
            collectAggregates(g.expr.get(), in_key);
            if (!in_key.empty()) {
                throw std::runtime_error("GROUP BY: aggregate functions are not allowed in GROUP BY");
            }
            validateExpr(g.expr.get(), schema, "GROUP BY", catalog, /*allow_aggregates=*/true);
            continue;
        }
        if (g.relation_slot >= 0 && !g.table_name.empty()) continue; // binder verified
        bool found;
        if (!g.table_name.empty()) {
            // qualified but unbound (validator-only callers that skip the Binder)
            found = (g.table_name == stmt.from_table && schema.hasColumn(g.column_name));
            for (const auto& j : stmt.joins) {
                if (found) break;
                found = g.table_name == j.join_table
                     && catalog.getTable(j.join_table).schema.hasColumn(g.column_name);
            }
        } else {
            found = schema.hasColumn(g.column_name);
            for (const auto& j : stmt.joins) {
                if (found) break;
                found = catalog.getTable(j.join_table).schema.hasColumn(g.column_name);
            }
        }
        if (!found) {
            throw std::runtime_error(
                "GROUP BY column not found: '" + g.column_name + "'");
        }
    }

    // HAVING requires GROUP BY
    if (stmt.having && stmt.group_by.empty()) {
        throw std::runtime_error("HAVING requires GROUP BY");
    }

    // HAVING columns (and aggregate arguments) must exist
    if (stmt.having) {
        validateExpr(stmt.having.get(), schema, "HAVING", catalog,
                     /*allow_aggregates=*/true, /*allow_subqueries=*/true);
    }

    // non aggregated SELECT column references must appear in GROUP BY.
    // Detection recurses (AVG(speed) * 2 aggregates without being a top-level
    // AggregateExpr), and the check now also runs for GROUP BY without
    // aggregates — previously that shape slipped through and died at runtime.
    bool has_aggregates = false;
    {
        std::vector<const AggregateExpr*> found;
        for (const auto& expr : stmt.select_list) collectAggregates(expr.get(), found);
        has_aggregates = !found.empty();
    }
    if ((has_aggregates || !stmt.group_by.empty()) && !stmt.select_star) {
        for (const auto& expr : stmt.select_list) {
            checkGroupedRefs(expr.get(), stmt.group_by);
        }
    }

    // ORDER BY: validate ColumnRef nodes against the base table schema.
    // Aggregate expressions (e.g. COUNT(*)) resolve against the post-aggregate
    // output schema at execution time and are not checked here.
    for (const auto& item : stmt.order_by) {
        // Week 30. ORDER BY is not routed through validateExpr (only its
        // ColumnRef nodes are checked), so the WHERE/HAVING-only position rule
        // needs its own line here rather than an allow_subqueries flag.
        if (dynamic_cast<const SubqueryExpr*>(item.expr.get())) {
            throw std::runtime_error(
                "ORDER BY: subqueries are supported in WHERE and HAVING only");
        }
        if (auto* col = dynamic_cast<const ColumnRef*>(item.expr.get())) {
            if (col->table_name.empty() && !schema.hasColumn(col->column_name)) {
                throw std::runtime_error("ORDER BY column not found: '" + col->column_name + "'");
            }
        }
    }

    // an ORDER BY aggregate needs an aggregation context to be computed in
    if (stmt.group_by.empty() && !has_aggregates) {
        std::vector<const AggregateExpr*> order_aggs;
        for (const auto& item : stmt.order_by) {
            collectAggregates(item.expr.get(), order_aggs);
        }
        if (!order_aggs.empty()) {
            throw std::runtime_error(
                "ORDER BY aggregate requires GROUP BY or an aggregated SELECT list");
        }
    }
}


// recursively validate an expression and its sub expressions
void Validator::validateExpr(const Expr* expr, const Schema& schema, const std::string& context,
                             const Catalog& catalog, bool allow_aggregates, bool allow_subqueries) {
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // Week 30. A ref the Binder resolved to an ENCLOSING query names a
        // relation this scope's schema does not hold, so checking it here would
        // be a false "column not found" on a legal correlated reference. The
        // Binder already verified it against that scope's range table — the
        // same reason validateJoinCondition trusts a bound ref's slot instead
        // of re-deriving the relation from table_name.
        if (col->query_level > 0) return;
        // skip validation for qualified refs (table.column)
        // full resolution handled when join schema is merged
        if (col->table_name.empty() && !schema.hasColumn(col->column_name)) {
            throw std::runtime_error(context + ": column not found: '" + col->column_name + "'");
        }
    }
    else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        validateExpr(bin->left.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
        validateExpr(bin->right.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
    }
    else if (auto* isnull = dynamic_cast<const IsNullExpr*>(expr)) {
        validateExpr(isnull->operand.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
    }
    else if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        validateExpr(un->operand.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
    }
    else if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        if (!allow_aggregates) {
            throw std::runtime_error(
                context + ": aggregate functions are not allowed in WHERE clause; use HAVING instead");
        }
        if (!agg->is_star && agg->argument) {
            // Aggregates cannot nest. expr_utils.h's collectAggregates() stops
            // walking at an AggregateExpr on that assumption, so a nested one is
            // never collected as a spec and SUM(AVG(speed)) used to reach
            // execution and die with "Column not found in schema: AVG(speed)".
            std::vector<const AggregateExpr*> nested;
            collectAggregates(agg->argument.get(), nested);
            if (!nested.empty()) {
                throw std::runtime_error(
                    context + ": aggregate functions cannot be nested — '"
                    + agg->function_name + "(" + nested.front()->function_name + "(...))'");
            }
            validateExpr(agg->argument.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
        }
    }
    else if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        validateExpr(in->operand.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
    }
    else if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        validateExpr(lk->operand.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
    }
    else if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        for (const auto& w : c->when_clauses) {
            validateExpr(w.condition.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
            validateExpr(w.result.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
        }
        if (c->else_expr) validateExpr(c->else_expr.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
    }
    else if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        validateExpr(sub->operand.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
        validateExpr(sub->start.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
        if (sub->length) validateExpr(sub->length.get(), schema, context, catalog, allow_aggregates, allow_subqueries);
    }
    // Week 30 — DISPATCH SITE 4.
    else if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        if (!allow_subqueries) {
            throw std::runtime_error(
                context + ": subqueries are supported in WHERE and HAVING only");
        }

        // The IN operand is written in THIS query, so it is checked here,
        // against this schema, under this clause's aggregate rule.
        if (sq->operand) {
            validateExpr(sq->operand.get(), schema, context, catalog,
                         allow_aggregates, allow_subqueries);
        }
        if (!sq->subquery) return;

        // ARITY is decidable now, from the select list alone; CARDINALITY is
        // not — "scalar subquery returned more than one row" is Week 31's
        // runtime check. EXISTS has no arity rule at all: TPC-H Q4 and Q21 both
        // write `select *`, and EXISTS never reads the values.
        if (sq->kind != SubqueryExpr::Kind::EXISTS) {
            if (sq->subquery->select_star || sq->subquery->select_list.size() != 1) {
                throw std::runtime_error(
                    std::string(sq->kind == SubqueryExpr::Kind::IN ? "IN" : "scalar")
                    + " subquery must return exactly one column");
            }
        }

        // The body is a DIFFERENT scope: a different FROM schema, and its own
        // aggregate rule — `WHERE x > (SELECT AVG(y) ...)` is legal SQL, but
        // this walk carries allow_aggregates=false for a WHERE and would
        // reject the subquery's own aggregate. So do NOT descend from here;
        // hand the statement to a fresh validation against its own schema.
        //
        // validateQuery, not validate: the "not yet executable" refusal belongs
        // to the whole statement once, at the very end, not to each nesting
        // level on the way down.
        validateQuery(*sq->subquery, catalog);
    }
    // literal / interval nodes need no validation
}


void Validator::validateJoinCondition(const Expr* expr,
        const std::vector<std::pair<std::string, const Schema*>>& relations){
    if (!expr) return;

    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // A BOUND ref carries the relation the Binder resolved it against, so
        // check that relation by slot. `relations` is built in range-table
        // order, so index == slot.
        //
        // Matching a bound ref on table_name cannot work: Binder::resolveColumnRef
        // rewrites an unqualified ref's table_name to the TABLE name of the
        // relation it resolved to, while this list is keyed by REF name. When
        // another relation is aliased to exactly that table name, the name match
        // lands on the wrong entry and rejects a legal query —
        // `FROM drivers x JOIN laps drivers ON age = drivers.driver_id` checked
        // `age` against laps.
        if (col->relation_slot >= 0 &&
            col->relation_slot < static_cast<int>(relations.size())) {
            const auto& rel = relations[col->relation_slot];
            if (!rel.second->hasColumn(col->column_name)) {
                throw std::runtime_error("JOIN ON: column '" + col->column_name
                    + "' not found in table '" + rel.first + "'");
            }
            return;
        }

        // Unbound (validator-only callers that skip the Binder): resolve by the
        // name as typed, against the ref names a qualified reference may use.
        if (col->table_name.empty()) {
            for (const auto& rel : relations) {
                if (rel.second->hasColumn(col->column_name)) return;
            }
            throw std::runtime_error(
                "JOIN ON: column '" + col->column_name + "' not found in any joined table");
        }
        for (const auto& rel : relations) {
            if (rel.first != col->table_name) continue;
            if (!rel.second->hasColumn(col->column_name)){
                throw std::runtime_error("JOIN ON: column '" + col->column_name
                    + "' not found in table '" + rel.first + "'");
            }
            return;
        }
        return; // qualified ref with unknown table prefix: an alias, resolved by the Binder
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        validateJoinCondition(bin->left.get(),  relations);
        validateJoinCondition(bin->right.get(), relations);
        return;
    }
    // DISPATCH SITE 18. Silent on an unhandled subtype: no column-existence
    // check inside it. Dormant until Week 26 relaxed classifyJoinCondition to
    // accept AND-chains; LIVE since Week 27, which routes every non-key ON
    // conjunct as a residual, so the shapes below actually arrive.
    //
    // For a residual this is the ONLY column check there is: Validator::validate
    // runs before the planners fold residuals into the WHERE conjunction, so
    // validateExpr never sees them. A gap here surfaces as a far-from-the-cause
    // "column not found" out of inferExprType. Keep in lockstep with
    // validateExpr above.
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr))   { validateJoinCondition(un->operand.get(), relations); return; }
    if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) { validateJoinCondition(isn->operand.get(), relations); return; }
    if (auto* in = dynamic_cast<const InExpr*>(expr))      { validateJoinCondition(in->operand.get(), relations); return; }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr))    { validateJoinCondition(lk->operand.get(), relations); return; }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        for (const auto& w : c->when_clauses) {
            validateJoinCondition(w.condition.get(), relations);
            validateJoinCondition(w.result.get(), relations);
        }
        validateJoinCondition(c->else_expr.get(), relations);   // nullptr-safe
        return;
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        validateJoinCondition(sub->operand.get(), relations);
        validateJoinCondition(sub->start.get(), relations);
        validateJoinCondition(sub->length.get(), relations);    // nullptr-safe
        return;
    }
    if (dynamic_cast<const SubqueryExpr*>(expr)) {
        // Week 30. No TPC-H query puts a subquery in an ON clause, and a
        // residual carrying one would be handed to a probe loop that cannot
        // evaluate it (an outer join) or folded into the WHERE conjunction and
        // routed by a relation slot it does not have (an inner one). Decline,
        // in the same shape as the AggregateExpr branch below.
        //
        // classifyJoinCondition runs one line EARLIER in validateQuery, so a
        // subquery that ALSO forward-references a later relation reports the
        // forward reference first. That is the stated order — shape before
        // contents — not a defect.
        throw std::runtime_error(
            "JOIN ON: subqueries are not supported in a join condition");
    }
    if (dynamic_cast<const AggregateExpr*>(expr)) {
        // meaningless in a join condition, and loud beats silent
        throw std::runtime_error(
            "JOIN ON: aggregate functions are not allowed in a join condition");
    }
    // Literal / IntervalLiteral: nothing to check
}