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
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
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
    // FROM table must exist
    if (!catalog.hasTable(stmt.from_table)) {
        throw std::runtime_error(
            "Table not found: '" + stmt.from_table + "'");
    }
    const Schema& schema = catalog.getTable(stmt.from_table).schema;

    // SELECT list columns must exist (skip for SELECT *)
    if (!stmt.select_star) {
        for (const auto& expr : stmt.select_list) {
            validateExpr(expr.get(), schema, "SELECT");
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
        std::vector<std::pair<std::string, const Schema*>> relations{{stmt.from_table, &schema}};
        for (const auto& j : stmt.joins) {
            if (!catalog.hasTable(j.join_table)) {
                throw std::runtime_error(
                    "Join table not found: '" + j.join_table + "'");
            }
            relations.push_back({j.join_table, &catalog.getTable(j.join_table).schema});
        }
        for (size_t i = 0; i < stmt.joins.size(); ++i) {
            if (!stmt.joins[i].condition) continue;
            // shape first (equi-join keys), then column existence — a shape
            // error is the more useful message when both are wrong
            classifyJoinCondition(stmt.joins[i].condition.get(), static_cast<int>(i) + 1);
            validateJoinCondition(stmt.joins[i].condition.get(), relations);
        }
    }

    // WHERE columns must exist; aggregates are not allowed in WHERE
    if (stmt.where) {
        validateExpr(stmt.where.get(), schema, "WHERE", /*allow_aggregates=*/false);
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
            validateExpr(g.expr.get(), schema, "GROUP BY", /*allow_aggregates=*/true);
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
        validateExpr(stmt.having.get(), schema, "HAVING", /*allow_aggregates=*/true);
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
void Validator::validateExpr(const Expr* expr, const Schema& schema, const std::string& context, bool allow_aggregates) {
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // skip validation for qualified refs (table.column)
        // full resolution handled when join schema is merged
        if (col->table_name.empty() && !schema.hasColumn(col->column_name)) {
            throw std::runtime_error(context + ": column not found: '" + col->column_name + "'");
        }
    }
    else if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        validateExpr(bin->left.get(), schema, context, allow_aggregates);
        validateExpr(bin->right.get(), schema, context, allow_aggregates);
    }
    else if (auto* isnull = dynamic_cast<const IsNullExpr*>(expr)) {
        validateExpr(isnull->operand.get(), schema, context, allow_aggregates);
    }
    else if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        validateExpr(un->operand.get(), schema, context, allow_aggregates);
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
            validateExpr(agg->argument.get(), schema, context, allow_aggregates);
        }
    }
    else if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        validateExpr(in->operand.get(), schema, context, allow_aggregates);
    }
    else if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        validateExpr(lk->operand.get(), schema, context, allow_aggregates);
    }
    else if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        for (const auto& w : c->when_clauses) {
            validateExpr(w.condition.get(), schema, context, allow_aggregates);
            validateExpr(w.result.get(), schema, context, allow_aggregates);
        }
        if (c->else_expr) validateExpr(c->else_expr.get(), schema, context, allow_aggregates);
    }
    else if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        validateExpr(sub->operand.get(), schema, context, allow_aggregates);
        validateExpr(sub->start.get(), schema, context, allow_aggregates);
        if (sub->length) validateExpr(sub->length.get(), schema, context, allow_aggregates);
    }
    // literal / interval nodes need no validation
}


void Validator::validateJoinCondition(const Expr* expr,
        const std::vector<std::pair<std::string, const Schema*>>& relations){
    if (!expr) return;

    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
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
    // accept AND-chains; Week 27 legalizes residual ON conjuncts, which is when
    // the Week 25 shapes below actually arrive. Keep in lockstep with
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
    if (dynamic_cast<const AggregateExpr*>(expr)) {
        // meaningless in a join condition, and loud beats silent
        throw std::runtime_error(
            "JOIN ON: aggregate functions are not allowed in a join condition");
    }
    // Literal / IntervalLiteral: nothing to check
}