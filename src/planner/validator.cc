#include "validator.h"
#include "logical_plan.h"   // blockOutputSchema / derivedRelationSchema (Week 34)
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
        if (!col->id.isLocal()) return;
        for (const auto& g : group_by) {
            if (g.expr) continue;
            // name match plus slot compatibility: SELECT a.grp with
            // GROUP BY b.grp is a different column, not a match.
            // Unbound slots (-1) stay name-only for direct-validate callers.
            // Week 30: the LEVEL is part of the identity — a slot compared
            // across two levels is a slot compared across two range tables.
            // `col` is always level 0 here (the guard above returns for a
            // correlated ref), so this rejects a correlated GROUP BY key as a
            // match for a local column of the same name.
            if (g.column_name == col->column_name &&
                col->id.couldBeSameRelation(g.id)) {
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

// THE JOIN-KEY TYPE RULE — one comparison, one message, for all four JoinKey
// producers (validator.h explains why it converges on the plan rather than on
// the producers).
//
// WHAT IS WRONG, stated because a refusal that does not name its cause gets
// deleted by the next person who meets it. A join key is serialized to TEXT
// (execution/key_encoding.h) and the texts are compared. Text carries no type
// tag, so a STRING key matches a numeric one only when the STRING already IS the
// number's canonical rendering: '16' matches the INT 16, while '016', ' 16',
// '+16' and '16.0' do not. SQLite applies numeric affinity and matches all five.
// So the engine returns a strict SUBSET of the right rows for a positive test
// and a strict SUPERSET for a negated one, with no error either way — the
// "half a match" Week 29 named.
//
// REFUSING RATHER THAN IMPLEMENTING AFFINITY, deliberately, and the reason is
// not effort:
//   - `Value::operator==` throws `Type mismatch` across the STRING boundary, so
//     the identical predicate in a WHERE clause is an error. And an INNER join's
//     ON conjuncts that are NOT keys are folded into the WHERE conjunction by
//     LogicalPlanBuilder::build. Give keys affinity and one written predicate
//     answers differently depending on whether classifyJoinCondition made it a
//     key — the same conjunct, two semantics, decided by a planner detail.
//   - the encoding could not implement it locally anyway: keyFieldText sees ONE
//     Value and would need the OPPOSING column's type threaded through six
//     serializers and both engines.
//   - the encoding is NOT the defect. keyFieldText's numeric affinity (an
//     integral DOUBLE takes the integer path, so 7.0 joins 7) is correct
//     everywhere, including in all three lowerings — seam audit pass 3, C3-4,
//     measured it. The defect was only ever the missing guard.
//
// Coarse on purpose — both STRING or both numeric — matching `Value::operator==`,
// which coerces INT/DOUBLE and throws only across the STRING boundary.
void requireJoinKeyTypes(const char* context,
                         const Schema& left, int li,
                         const Schema& right, int ri,
                         const std::string& from_col,
                         const std::string& join_col) {
    const bool l_str = left.column(li).type  == TypeId::STRING;
    const bool r_str = right.column(ri).type == TypeId::STRING;
    if (l_str == r_str) return;
    throw std::runtime_error(
        std::string(context) + ": cannot join a STRING column with a numeric one ('"
        + from_col + "' and '" + join_col + "'). Keys are matched as serialized "
        "text, which carries no type tag, so the STRING would match only when it "
        "is already the number's canonical rendering ('16' matches the INT 16, "
        "'016' does not) while SQLite's affinity converts and matches both. "
        "Make the two columns the same type.");
}

// Which construct produced this node's keys, for the message only. SEMI is the
// one that cannot be narrowed: lowerInSubqueries (IN) and lowerExistsSubqueries
// (EXISTS) both build it and nothing on the node tells them apart, so the
// message names both rather than guessing one — naming the wrong cause for the
// right refusal is a defect this tree has logged three times.
const char* joinKeyContext(const LogicalJoin& join) {
    switch (join.semantics) {
        case JoinSemantics::SEMI:          return "IN / EXISTS subquery";
        case JoinSemantics::ANTI:          return "NOT EXISTS subquery";
        case JoinSemantics::ANTI_NOT_IN:   return "NOT IN subquery";
        // A written JOIN ON reaches the AST loop in validate() first, so what
        // normally arrives here as STANDARD is the correlated-scalar rewrite's
        // `$scalarN` LEFT join. NOT named as such: the AST loop legitimately
        // skips an unresolved key, so a written join CAN arrive here too, and a
        // prefix that asserted otherwise would be a wrong-cause diagnostic.
        case JoinSemantics::STANDARD:      break;
    }
    return "join key";
}

} // namespace

void Validator::validateJoinKeyTypes(const LogicalPlanNode& plan) {
    for (const auto& child : plan.children) validateJoinKeyTypes(*child);
    if (plan.type != LogicalNodeType::JOIN) return;
    const auto& join = static_cast<const LogicalJoin&>(plan);
    const Schema& left  = join.children[0]->output_schema;
    const Schema& right = join.children[1]->output_schema;

    // RESOLVED EXACTLY AS THE PHYSICAL BUILDER RESOLVES, so the check is
    // guaranteed to be about the columns the probe will actually compare:
    // leftKeyIndices honours from_slot (a merged left schema can hold `team` at
    // two slots), and rightKeyIndices is POSITIONAL for a semi/anti join —
    // whose build input the lowering has already arranged to BE the key tuple,
    // in key order — and by name otherwise. See vectorized_plan_builder.cc.
    const bool positional = join.semantics != JoinSemantics::STANDARD;
    for (size_t k = 0; k < join.keys.size(); ++k) {
        const JoinKey& key = join.keys[k];
        const int li = (key.from_slot >= 0) ? left.indexOf(key.from_col, key.from_slot)
                                            : left.indexOf(key.from_col);
        const int ri = positional ? static_cast<int>(k) : right.indexOf(key.join_col);
        // A MISS IS NOT THIS RULE'S TO REPORT: leftKeyIndices and rightKeyIndices
        // throw on it by name at lowering, with the message that owns it.
        if (li < 0 || ri < 0 || ri >= right.size()) continue;
        requireJoinKeyTypes(joinKeyContext(join), left, li, right, ri,
                            key.from_col, key.join_col);
    }
}

void Validator::validate(const SelectStatement& stmt, const Catalog& catalog){
    validateQuery(stmt, catalog);

    // Week 30, NARROWED IN WEEK 31 — LAST, after every semantic check, including
    // the nested query's own (validateQuery runs those through validateExpr). A
    // genuine query defect outranks a temporary engine limitation, which is the
    // discipline that placed Week 26's multi-key refusal past the plan-time type
    // checks. Nothing about the PLACEMENT changed this week; only the condition
    // and the message did.
    //
    // Week 31 narrowed the CONDITION. An UNCORRELATED subquery is
    // loop-invariant — its value cannot depend on the outer row — so
    // materializeSubqueries runs it once and substitutes a constant before
    // either planner sees the statement (subquery_materialization.h). A
    // CORRELATED one has a different value per outer row, which is
    // decorrelation, and that is Week 33.
    //
    // Still ONE check, four modes, one message. Planner::plan and
    // LogicalPlanBuilder::build both call validate() first, and the
    // materialization pass sits ABOVE both engines, so the four modes are
    // equivalent BY CONSTRUCTION rather than by copies of a guard that can drift
    // — which is what Week 29's audit rounds were about.
    //
    // What this placement guarantees: every PARSE, BIND and VALIDATE error a
    // query is entitled to fires first — a bad nested table, a bad nested
    // column, an ungrouped reference, a wrong arity, a disallowed position. It
    // is also what the materialization pass relies on: the pass TRUSTS the arity
    // rule (it reads column 0 of the result) and this refusal (it never has to
    // ask a scope question), and main.cc therefore calls validate() before it.
    //
    // Everything above this line describes the refusal as it stood through Week
    // 32 and is kept as history, not as a live guarantee. In particular the
    // containment development.md's slot-consumer table rested on — "a ColumnRef
    // with query_level > 0 exists ONLY inside a correlated subquery, and those
    // are refused here" — NO LONGER HOLDS AT THIS SITE. It is now held by the
    // TYPE (common/column_id.h): a level cannot be dropped silently because a
    // bare int is not assignable where a ColumnId is required, and every
    // consumer that narrows to a slot names itself when it does.
    //
    // Week 33 REMOVED the refusal that stood here. A correlated subquery is now
    // decorrelated into the Week 32 semi/anti join
    // (planner/subquery_decorrelation.h), and the shapes that rewrite cannot
    // express are refused AT THAT SITE, by name, so the message says which shape
    // it declined instead of naming a week.
    //
    // Nothing replaces it here on purpose. Week 30's placement rule still holds
    // for what remains: a DIALECT rule belongs in this one check that both
    // planner entry points reach, and a CAPABILITY difference belongs at the
    // engine that lacks it (Planner::plan) — which is where the Volcano refusal
    // for a decorrelated join lives, beside Week 32's identical one for IN.
}

void Validator::validateQuery(const SelectStatement& stmt, const Catalog& catalog){
    // Week 34 — ONE range table for the whole function, built once, keyed
    // exactly as the Binder's is (the alias when there is one, the table name
    // otherwise). It replaces four inline recomputations of the same thing:
    // the FROM schema, the SUM/AVG argument check's slot arithmetic, the
    // `relations` vector for join conditions, and the GROUP BY existence check.
    // Keeping them separate is what let Week 26's join keying and Week 30's
    // SUM/AVG slot lookup each disagree with the Binder in its own way.
    //
    // A DERIVED entry's schema is computed by the SAME shared helpers the Binder
    // and the plan builder use, and is OWNED here (the catalog owns the others).
    std::vector<std::unique_ptr<Schema>> owned_schemas;
    std::vector<std::pair<std::string, const Schema*>> relations;

    auto addRelation = [&](const TableRef& ref, bool is_join) {
        if (ref.isDerived()) {
            // The body is a query block of its own: validate it in its own scope
            // first, so a defect inside it reports against the body's relations
            // rather than surfacing as a missing column of the derived table.
            validateQuery(*ref.body(), catalog);
            owned_schemas.push_back(std::make_unique<Schema>(
                derivedRelationSchema(blockOutputSchema(*ref.body(), catalog), ref)));
            relations.push_back({ref.refName(), owned_schemas.back().get()});
            return;
        }
        const std::string& t = ref.tableName("Validator relation existence");
        if (!catalog.hasTable(t)) {
            throw std::runtime_error(
                (is_join ? "Join table not found: '" : "Table not found: '") + t + "'");
        }
        relations.push_back({ref.refName(), &catalog.getTable(t).schema});
    };

    addRelation(stmt.from, /*is_join=*/false);
    for (const auto& j : stmt.joins) addRelation(j.relation, /*is_join=*/true);

    // The FROM relation's schema, under its historical name. Every use below is
    // of relation 0, which is what `schema` always meant.
    const Schema& schema = *relations[0].second;

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
            // Week 30 round 2. `stmt.joins` and `schema` below are THIS block's,
            // and validateQuery recurses into every nested statement, so a
            // correlated argument would index the inner join list with an outer
            // slot. Measured: the same illegal `SUM(d.name)` inside a subquery
            // was caught or silently skipped depending on the order of the
            // INNER query's own joins. The check that belongs to the block
            // owning the relation is made by the Binder, which is the only
            // layer holding the scope chain — see checkCorrelatedAggregateArg.
            if (!col->id.isLocal()) continue;

            // pick the schema the argument resolves against: binder slot when
            // bound, table-name match for unbound qualified refs, FROM otherwise
            //
            // Week 34: resolved against the RANGE TABLE built above rather than
            // against `stmt.joins[slot-1].join_table`. That index arithmetic is
            // still right — slot k > 0 IS joins[k-1]'s relation — but its next
            // step, catalog.getTable(name), has no answer for a derived
            // relation. The range table answers uniformly for both, which is the
            // same reason the CORRELATED half of this check lives in the Binder
            // (Week 30 round 2): the layer that owns slot -> schema is the only
            // one that can resolve it.
            //
            // The unqualified/table-name fallbacks now match on the REF NAME,
            // which is what a qualified reference actually writes. Keying on the
            // table name was already wrong for an aliased relation and worked
            // only because the slot branch caught those first.
            const Schema* target = nullptr;
            const int col_slot = col->id.localSlot("SUM/AVG argument check");
            if (col_slot >= 0 && col_slot < static_cast<int>(relations.size())) {
                target = relations[col_slot].second;
            } else if (col->table_name.empty()) {
                target = &schema;
            } else {
                for (const auto& r : relations) {
                    if (col->table_name != r.first) continue;
                    target = r.second;
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

    // JOIN conditions (existence was checked by addRelation above, in written
    // order, so a missing join table still reports before any ON clause is read)
    if (!stmt.joins.empty()) {
        for (size_t i = 0; i < stmt.joins.size(); ++i) {
            if (!stmt.joins[i].condition) continue;
            // shape first (at least one equi-join key, no forward reference),
            // then column existence — a shape error is the more useful message
            // when both are wrong. The returned keys/residuals are rebuilt by
            // the planners; only the throw matters here.
            JoinCondition on = classifyJoinCondition(stmt.joins[i].condition.get(),
                                                     static_cast<int>(i) + 1);
            validateJoinCondition(stmt.joins[i].condition.get(), relations);

            // Week 29 (deferred from the Week 27 audit), rescoped by seam audit
            // pass 3 (B3-2). The rule itself is requireJoinKeyTypes above, and
            // the reason it is refused rather than given affinity is stated
            // there. What this loop is, precisely:
            //
            // !! IT COVERS `stmt.joins` AND NOTHING ELSE. Week 29's comment stood
            // here saying the defect "is closed here" and claimed one check
            // covered all four modes. It covered one of the FOUR PRODUCERS of a
            // JoinKey — the written JOIN — and the three that arrived after it
            // (IN / NOT IN lowering, EXISTS / NOT EXISTS decorrelation, the
            // correlated-scalar rewrite) went uncovered for four weeks and
            // returned wrong row sets in both directions. Validator::
            // validateJoinKeyTypes is where all four converge; this loop is not
            // that containment and must never be read as it again.
            //
            // WHY IT IS KEPT ANYWAY, rather than deleted in favour of the walk:
            // Planner::plan (Volcano) builds its HashJoinNode straight from
            // `stmt.joins` and never builds a logical plan, so the walk cannot
            // see it. `stmt.joins` is also the only producer Volcano HAS — it
            // refuses IN, correlated and derived shapes outright — so this loop
            // is exactly that path's cover, and no more than it. On the
            // vectorized path it merely fires first, with the same message.
            //
            // `relations` is in range-table order, so relations[slot] is the schema
            // a JoinKey slot addresses.
            for (const JoinKey& k : on.keys) {
                if (k.from_slot < 0) continue;   // unbound: positional routing, no
                                                 // relation identity to be exact about
                if (k.from_slot >= static_cast<int>(relations.size())) continue;
                const Schema* left_schema  = relations[k.from_slot].second;
                const Schema* right_schema = relations[i + 1].second;
                int li = left_schema->indexOf(k.from_col);
                int ri = right_schema->indexOf(k.join_col);
                if (li < 0 || ri < 0) continue;  // existence is validateJoinCondition's
                requireJoinKeyTypes("JOIN ON", *left_schema, li, *right_schema, ri,
                                    k.from_col, k.join_col);
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
    //
    // !! Week 37. The test is `written_ordinal`, stamped by the PARSER
    // (ordinalAsWritten), and NOT `dynamic_cast<Literal*>` on the tree in front
    // of us — which is what it used to be, and which was WRONG in a way an
    // audit refuted by execution.
    //
    // The rule is SYNTACTIC, and SQLite's own is too: a term is an ordinal iff
    // it is an integer literal under any number of parentheses and unary signs.
    // BINARY ARITHMETIC IS NOT — SQLite evaluates `1 + 1` and sorts on the
    // constant, leaving every row tied. See ordinalAsWritten (parser.cc) for
    // the measured table that separates the three behaviours.
    //
    // By the time the Validator runs, two rewrites have manufactured Literals
    // in exactly these two positions out of source text that was never an
    // ordinal — constant folding (binder.cc, `ORDER BY 1 + 1` -> Literal(2))
    // and the select-alias substitution just above it (`SELECT 1 AS one FROM
    // laps ORDER BY one` -> Literal(1)). Testing the tree refused both of those
    // legal queries, and quoted back "ORDER BY 2" / "ORDER BY 1", an ordinal
    // the user had not typed. The message now quotes the parser's own record,
    // so it cannot invent one.
    //
    // What this deliberately did NOT change: `-1`, `0`, `(1)` and `- -1` are
    // all still refused. Every one of them is an ordinal to SQLite as well, so
    // refusing them is agreement rather than extra strictness — the scope of
    // this fix is exactly the terms SQLite does NOT treat as ordinals.
    for (const auto& item : stmt.order_by) {
        if (!item.written_ordinal.empty()) {
            throw std::runtime_error(
                "ORDER BY " + item.written_ordinal + ": column ordinals are not "
                "supported; use a column name or a select-list alias");
        }
    }
    for (const auto& g : stmt.group_by) {
        if (!g.written_ordinal.empty()) {
            throw std::runtime_error(
                "GROUP BY " + g.written_ordinal + ": column ordinals are not "
                "supported; use a column name or a select-list alias");
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
        // Week 30 round 1. A group key the Binder resolved to an ENCLOSING block
        // is not this scope's to check: its slot indexes that block's range
        // table, and the Binder verified the column there. Grouping by a
        // correlated constant is legal SQL.
        //
        // This has to be tested on the LEVEL, not on `!g.table_name.empty()`
        // below: the binder only writes a qualifier back for a block holding two
        // or more relations, so keying the skip on the qualifier made the
        // outcome depend on how many relations the ENCLOSING query has —
        // `EXISTS (SELECT COUNT(*) FROM drivers d GROUP BY season)` was refused
        // with "GROUP BY column not found: 'season'" under a one-relation outer
        // query and accepted under a two-relation one, for the same subquery.
        if (!g.id.isLocal()) continue;
        if (g.id.isResolved() && !g.table_name.empty()) continue; // binder verified
        bool found;
        if (!g.table_name.empty()) {
            // qualified but unbound (validator-only callers that skip the Binder)
            // Week 34: matched against the range table's REF names, which is
            // what a qualified GROUP BY item actually writes, and which answers
            // for a derived relation as well as a base one.
            found = false;
            for (const auto& r : relations) {
                if (found) break;
                found = g.table_name == r.first && r.second->hasColumn(g.column_name);
            }
        } else {
            found = false;
            for (const auto& r : relations) {
                if (found) break;
                found = r.second->hasColumn(g.column_name);
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
        if (auto* col = dynamic_cast<const ColumnRef*>(item.expr.get())) {
            if (col->id.isLocal() && col->table_name.empty()
                && !schema.hasColumn(col->column_name)) {
                throw std::runtime_error("ORDER BY column not found: '" + col->column_name + "'");
            }
        }
        // Week 30 round 1. The WHERE/HAVING-only position rule was a one-line
        // test on the ROOT node, so `ORDER BY lap_id + (SELECT ...)` slipped
        // through — SELECT and GROUP BY have no such hole because they route
        // through validateExpr, whose allow_subqueries=false default is checked
        // at EVERY node. Route ORDER BY through it too rather than growing a
        // bespoke recursive walker, which would be a nineteenth dispatch site
        // and silent on a subtype it missed.
        //
        // It runs AFTER the bare-ColumnRef check above so that check keeps
        // owning its message; for anything else, validateExpr's recursion is
        // what makes the rule actually hold. This week the hole only changed
        // which message the user got; since Week 31 the blanket refusal is
        // gone and nothing else enforces the restriction ast.h justifies.
        validateExpr(item.expr.get(), schema, "ORDER BY", catalog,
                     /*allow_aggregates=*/true, /*allow_subqueries=*/false);
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
        if (!col->id.isLocal()) return;
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
        // not — "scalar subquery returned more than one row" needs data, and is
        // raised where the subquery runs (subquery_materialization.cc, Week 31).
        // This check is that one's precondition: it reads column 0 of the
        // result and would read it from a two-column subquery otherwise. EXISTS has no arity rule at all: TPC-H Q4 and Q21 both
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
        // Week 30 round 1. A ref the Binder resolved to an ENCLOSING query
        // carries a slot that is a position in THAT scope's range table, while
        // `relations` is this one's — indexing it compares two numbering
        // domains, which is what ast.h says every slot consumer must not do.
        // The Binder already verified the column against the scope that
        // supplies it, so there is nothing to check here.
        //
        // Reachable because validateQuery recurses into a nested statement's
        // own ON clauses: inside a subquery a correlated ref is an ordinary
        // top-level ref of that expression. Without this,
        // `... EXISTS (SELECT 1 FROM drivers d JOIN drivers d2 ON d.driver_id =
        // d2.driver_id AND d.age = l.lap_id)` reported
        // "JOIN ON: column 'lap_id' not found in table 'd'" — an error the
        // query is not entitled to, against a relation it never named.
        if (!col->id.isLocal()) return;

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
        const int slot = col->id.localSlot("validateJoinCondition");
        if (slot >= 0 && slot < static_cast<int>(relations.size())) {
            const auto& rel = relations[slot];
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