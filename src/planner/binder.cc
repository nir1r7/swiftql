#include "binder.h"
#include "constant_folding.h"
#include "parser/expr_utils.h"
#include <stdexcept>

void Binder::bind(SelectStatement& stmt, const Catalog& catalog) {
    bindQuery(stmt, catalog, /*parent=*/nullptr);
}

bool Binder::bindQuery(SelectStatement& stmt, const Catalog& catalog, Scope* parent) {
    // table existence is Validator's error to raise (preserves its message)
    // without a valid range table there is nothing safe to resolve here
    if (!catalog.hasTable(stmt.from_table)) return false;
    for (const auto& j : stmt.joins) {
        if (!catalog.hasTable(j.join_table)) return false;
    }

    Scope scope;
    scope.parent = parent;
    scope.stmt = &stmt;

    const Schema& from_schema = catalog.getTable(stmt.from_table).schema;
    scope.range_table.push_back({
        stmt.from_alias.empty() ? stmt.from_table : stmt.from_alias,
        stmt.from_table,
        &from_schema
    });

    // One range-table entry per JOIN, in written order: joins[i] lands at slot
    // i+1. resolveColumnRef already loops over the range table by index, so
    // widening it here is the whole of the N-relation generalization.
    for (const auto& j : stmt.joins) {
        const Schema& join_schema = catalog.getTable(j.join_table).schema;
        scope.range_table.push_back({
            j.alias.empty() ? j.join_table : j.alias,
            j.join_table,
            &join_schema
        });

        // Two relations sharing a ref name are unresolvable — every qualified
        // reference is ambiguous. Compare against EVERY prior entry, not just
        // the previous one: with three relations the clash can be between
        // entries 0 and 2, which never form the [0]/[1] pair.
        //
        // Which diagnostic to give turns on whether aliases were supplied (an
        // entry is aliased when its ref name differs from its table name), not
        // on table-name equality: "add aliases" is only right advice when
        // neither side has one. Choosing on the table name told a user who had
        // written `FROM sj a JOIN sj b ... JOIN sj a` — every relation aliased,
        // the fault being that `a` is used twice — to add what they already had.
        //
        // Week 30: still PER SCOPE. An inner alias may legally repeat an outer
        // one — shadowing it is what SQL scoping means — so this loop must not
        // walk the parent chain.
        const RangeEntry& added = scope.range_table.back();
        for (size_t prior = 0; prior + 1 < scope.range_table.size(); ++prior) {
            const RangeEntry& earlier = scope.range_table[prior];
            if (earlier.ref_name != added.ref_name) continue;
            bool neither_aliased = earlier.ref_name == earlier.table_name
                                && added.ref_name == added.table_name;
            if (earlier.table_name == added.table_name && neither_aliased) {
                throw std::runtime_error(
                    "self-join requires table aliases to disambiguate the two references to '"
                    + added.table_name + "'");
            }
            throw std::runtime_error(
                "duplicate table alias '" + added.ref_name
                + "': each side of a join needs a distinct name");
        }
    }

    if (!stmt.select_star) {
        for (auto& expr : stmt.select_list) bindExpr(expr.get(), scope, catalog);
    }
    bindExpr(stmt.where.get(), scope, catalog);
    // GROUP BY items resolve through the same machinery as column refs:
    // qualified names pick their side, ambiguous unqualified names throw.
    // A bare name matching no input column falls back to a select-list alias
    // (input columns take precedence, matching SQLite GROUP BY scoping);
    // expression items just bind their column references.
    for (auto& g : stmt.group_by) {
        if (!g.expr && g.table_name.empty() && !g.column_name.empty()) {
            bool is_column = false;
            for (const auto& rte : scope.range_table) {
                if (rte.schema->hasColumn(g.column_name)) { is_column = true; break; }
            }
            if (!is_column) {
                for (const auto& sel : stmt.select_list) {
                    if (!sel->alias.empty() && sel->alias == g.column_name) {
                        // GROUP BY <alias>: substitute the (already bound)
                        // aliased expression; a plain-column alias keeps the
                        // column fast path
                        auto clone = cloneExpr(sel.get());
                        if (auto* cr = dynamic_cast<ColumnRef*>(clone.get())) {
                            g.table_name = cr->table_name;
                            g.column_name = cr->column_name;
                            g.relation_slot = cr->relation_slot;
                            g.query_level = cr->query_level;
                        } else {
                            g.expr = std::move(clone);
                        }
                        break;
                    }
                }
            }
        }
        if (g.expr) {
            bindExpr(g.expr.get(), scope, catalog);
            continue;
        }
        ColumnRef tmp;
        tmp.table_name = g.table_name;
        tmp.column_name = g.column_name;
        // Week 30: carry the stamp the alias substitution above may already
        // have written. Without it resolveColumnRef sees an unresolved ref with
        // a table_name that was rewritten to the TABLE name and takes the
        // qualified path against a range table keyed on REF names, so
        // `SELECT name AS n FROM laps l JOIN drivers d ... GROUP BY n` failed
        // with "unknown table qualifier: 'drivers'".
        tmp.relation_slot = g.relation_slot;
        tmp.query_level = g.query_level;
        resolveColumnRef(&tmp, scope);
        g.table_name = tmp.table_name;
        g.relation_slot = tmp.relation_slot;
        // Week 30 round 1: the level travels with the slot. resolveColumnRef
        // walks OUT, so a group key can resolve in an enclosing block, and a
        // slot stored without its level indexes the wrong range table.
        g.query_level = tmp.query_level;
    }
    bindExpr(stmt.having.get(), scope, catalog);
    // SQLite scoping: ORDER BY names resolve against select-list aliases
    // first, then base columns. Substitute a clone of the aliased expression —
    // Sort evaluates below Project, where the alias name is not a column.
    // The select list is already bound, so clones carry relation_slot stamps
    // and the re-bind below is a genuine no-op (resolveColumnRef is idempotent).
    for (auto& item : stmt.order_by) {
        if (auto* col = dynamic_cast<ColumnRef*>(item.expr.get())) {
            if (col->table_name.empty()) {
                for (const auto& sel : stmt.select_list) {
                    if (!sel->alias.empty() && sel->alias == col->column_name) {
                        item.expr = cloneExpr(sel.get());
                        break;
                    }
                }
            }
        }
        bindExpr(item.expr.get(), scope, catalog);   // no-op on already-stamped clones
    }
    for (auto& j : stmt.joins) bindExpr(j.condition.get(), scope, catalog);

    // Fold constant arithmetic last, so every downstream pass — Validator, the
    // logical planner, pushdown, cardinality estimation, the chunk pruner, and
    // the columnar comparison fast path — sees `season = 2024` rather than
    // `season = 2020 + 4`. Unconditional: folding cannot change results, so it
    // is canonicalization rather than a cost-based decision, and both execution
    // paths and `--no-optimize` get it.
    //
    // Week 30: this runs PER SCOPE. A subquery's own constants were folded by
    // its own bindQuery, and foldNode declines a SubqueryExpr (dispatch site
    // 14) rather than descending into an already-folded body.
    foldConstants(stmt);
    return scope.correlated;
}

void Binder::markCorrelated(Scope& scope, int level) {
    Scope* s = &scope;
    for (int i = 0; i < level && s; ++i, s = s->parent) s->correlated = true;
}

void Binder::checkCorrelatedAggregateArg(const AggregateExpr* agg, const Scope& scope) {
    // Mirrors Validator's local rule exactly — same functions, same shape (a
    // bare ColumnRef argument), same message — for the one case Validator had
    // to decline: an argument resolved to an enclosing block. It reported the
    // right answer only when the INNER query's join list happened to hold a
    // column of that name at the outer slot, and skipped the check otherwise.
    //
    // Resolving the type here is exact rather than coincidental: (query_level,
    // relation_slot) addresses one range entry, and resolveColumnRef already
    // verified the column exists in it.
    if (agg->function_name != "SUM" && agg->function_name != "AVG") return;
    auto* col = dynamic_cast<const ColumnRef*>(agg->argument.get());
    if (!col || col->query_level <= 0 || col->relation_slot < 0) return;

    const Scope* s = &scope;
    for (int i = 0; i < col->query_level && s; ++i) s = s->parent;
    if (!s || col->relation_slot >= static_cast<int>(s->range_table.size())) return;

    const Schema* sch = s->range_table[col->relation_slot].schema;
    int idx = sch->indexOf(col->column_name);
    if (idx < 0) return;   // existence is resolveColumnRef's, and it threw already
    if (sch->column(idx).type == TypeId::STRING) {
        throw std::runtime_error(agg->function_name
            + "() requires a numeric column, but '" + col->column_name
            + "' is of type STRING");
    }
}

void Binder::bindExpr(Expr* expr, Scope& scope, const Catalog& catalog) {
    if (!expr) return;

    if (auto* col = dynamic_cast<ColumnRef*>(expr)) {
        resolveColumnRef(col, scope);
    } else if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        bindExpr(bin->left.get(), scope, catalog);
        bindExpr(bin->right.get(), scope, catalog);
    } else if (auto* isnull = dynamic_cast<IsNullExpr*>(expr)) {
        bindExpr(isnull->operand.get(), scope, catalog);
    } else if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
        bindExpr(un->operand.get(), scope, catalog);
    } else if (auto* agg = dynamic_cast<AggregateExpr*>(expr)) {
        if (!agg->is_star) {
            bindExpr(agg->argument.get(), scope, catalog);
            // After binding, so the argument carries its (level, slot).
            //
            // Being in the Binder means this outranks every Validator rule,
            // including the one forbidding an aggregate in WHERE — so an
            // illegal-position aggregate with a CORRELATED STRING argument is
            // diagnosed by type rather than by position, where the same
            // aggregate with a local or numeric argument is diagnosed by
            // position. Both are refused either way; only the wording differs,
            // and moving the check back to Validator is what re-opens the
            // wrong-relation lookup it exists to close.
            checkCorrelatedAggregateArg(agg, scope);
        }
    } else if (auto* in = dynamic_cast<InExpr*>(expr)) {
        bindExpr(in->operand.get(), scope, catalog);   // values are literals
    } else if (auto* lk = dynamic_cast<LikeExpr*>(expr)) {
        bindExpr(lk->operand.get(), scope, catalog);
    } else if (auto* c = dynamic_cast<CaseExpr*>(expr)) {
        for (auto& w : c->when_clauses) {
            bindExpr(w.condition.get(), scope, catalog);
            bindExpr(w.result.get(), scope, catalog);
        }
        bindExpr(c->else_expr.get(), scope, catalog);
    } else if (auto* sub = dynamic_cast<SubstringExpr*>(expr)) {
        bindExpr(sub->operand.get(), scope, catalog);
        bindExpr(sub->start.get(), scope, catalog);
        bindExpr(sub->length.get(), scope, catalog);   // nullptr-safe
    } else if (auto* sq = dynamic_cast<SubqueryExpr*>(expr)) {
        // Week 30 — DISPATCH SITE 3.
        //
        // The IN operand is written in the ENCLOSING query, so it binds against
        // THIS scope. Binding it inside the subquery's scope would resolve it
        // against the subquery's relations — a wrong answer with no error.
        bindExpr(sq->operand.get(), scope, catalog);

        if (scope.stmt) scope.stmt->has_subquery = true;

        // The body opens a new scope whose parent is this one. Binding it HERE,
        // rather than in a separate pass, is what makes resolution lexical: the
        // enclosing range table is exactly the one in scope at this point.
        //
        // A shared statement (cloneExpr shares the shared_ptr) can arrive here
        // twice; that is safe because resolveColumnRef is idempotent.
        if (sq->subquery) sq->correlated = bindQuery(*sq->subquery, catalog, &scope);
    }
    // literal / interval, nothing to bind
}

void Binder::resolveColumnRef(ColumnRef* col, Scope& scope) {
    // Week 30. Binding is IDEMPOTENT: a ref that already carries a slot has
    // been resolved and is returned untouched.
    //
    // Three callers re-bind a clone of an already-bound expression — GROUP BY
    // <alias>, ORDER BY <alias>, and any cloneExpr'd subtree (BETWEEN's
    // desugaring, the residual ON clone) — and for an unqualified ref the
    // second pass was a WRONG ANSWER, not a no-op: the unqualified branch below
    // rewrites table_name to the TABLE name while the range table is keyed on
    // the REF name, so `SELECT name AS n FROM laps l JOIN drivers d ... ORDER BY n`
    // failed with "unknown table qualifier: 'drivers'".
    //
    // Writing ref_name back instead would also resolve it, and would change the
    // OUTPUT COLUMN NAME of every aggregate over an unqualified column in an
    // aliased query — aggregateOutputName IS exprToString, which renders
    // table_name.column_name, so AVG(laps.speed) would become AVG(l.speed).
    // Not re-resolving what is already resolved changes no name at all.
    //
    // It is also the precondition for SubqueryExpr's shared statement (ast.h):
    // two nodes may share one SelectStatement, so the Binder may walk it twice.
    //
    // Week 30 round 1: idempotent means "same result", not "does nothing".
    // Correlation is derived from the stamp rather than from a side effect of
    // resolution, because the stamp is what survives a second walk. Returning
    // early WITHOUT marking made bindQuery report `false` on every re-walk, so
    // SubqueryExpr::correlated was overwritten with false — and that is
    // reachable inside ONE bind(): BETWEEN's desugaring clones its left operand
    // before binding and cloneExpr SHARES the statement, so two SubqueryExpr
    // nodes reach one SelectStatement and the second was left marked
    // uncorrelated. collectSlots then contributes no -1 for it, soleSlot
    // returns a single slot, and pushdown pushes a CORRELATED conjunct onto one
    // relation's scan — the wrong answer the sentinel exists to prevent, and
    // the precondition restampSlots' safety argument rests on.
    //
    // query_level is relative to the block the ref is written in, which is
    // exactly `scope` on every walk, so re-deriving it here cannot drift.
    if (col->relation_slot >= 0) {
        if (col->query_level > 0) markCorrelated(scope, col->query_level);
        return;
    }

    // Innermost scope first, then out. `level` is what lands in query_level:
    // 0 = this block's own range table, >= 1 = a correlated reference.
    int level = 0;
    for (Scope* s = &scope; s; s = s->parent, ++level) {
        if (!col->table_name.empty()) {
            // qualified reference: the qualifier must name exactly one relation
            // (its alias if it has one, otherwise its table name)
            for (int slot = 0; slot < static_cast<int>(s->range_table.size()); ++slot) {
                const RangeEntry& rte = s->range_table[slot];
                if (rte.ref_name != col->table_name) continue;
                if (!rte.schema->hasColumn(col->column_name)) {
                    throw std::runtime_error(
                        "column '" + col->column_name + "' not found in '" + rte.ref_name + "'");
                }
                // keep the as-typed qualifier (alias or table name): aggregate
                // output names are built from it, and self-join occurrences
                // are only distinguishable by their aliases. Routing uses the
                // (level, slot) pair, never the qualifier text.
                col->relation_slot = slot;        // range-table position
                col->query_level = level;
                markCorrelated(scope, level);
                return;
            }
            continue;   // qualifier names no relation here: try the enclosing block
        }

        // unqualified: resolve across this scope's relations and reject
        // ambiguity — the matching side determines the slot. Ambiguity is a
        // PER-SCOPE question: a name matching one relation here and another in
        // an enclosing block is not ambiguous, the inner one shadows the outer.
        int matches = 0;
        int resolved_slot = -1;
        std::string resolved_table;
        for (int slot = 0; slot < static_cast<int>(s->range_table.size()); ++slot) {
            if (s->range_table[slot].schema->hasColumn(col->column_name)) {
                ++matches;
                resolved_slot = slot;
                resolved_table = s->range_table[slot].table_name;
            }
        }
        if (matches > 1) {
            throw std::runtime_error("ambiguous column reference: '" + col->column_name + "'");
        }
        if (matches == 1) {
            // Qualify only in a MULTI-relation block, which is what the
            // pre-Week-30 `range_table.size() < 2` shortcut did by taking slot
            // 0 before it ever reached this loop. The distinction is
            // schema-visible: aggregateOutputName IS exprToString, which renders
            // table_name.column_name, so qualifying here would rename every
            // single-relation aggregate's output column (SUM((speed * 2)) ->
            // SUM((laps.speed * 2))).
            //
            // The shortcut itself could not survive nesting: a single-relation
            // subquery would have swallowed EVERY unqualified name at slot 0
            // and never walked out, so no correlated reference could resolve.
            if (s->range_table.size() >= 2) col->table_name = resolved_table;
            col->relation_slot = resolved_slot;
            col->query_level = level;
            markCorrelated(scope, level);
            return;
        }
        // matches == 0 in this scope: try the enclosing one
    }

    // Nothing in the chain matched. Fall back to the INNERMOST scope's
    // pre-Week-30 behaviour, unchanged, so every existing error message stays
    // byte-identical: a qualified ref throws "unknown table qualifier", and an
    // unqualified one in a single-relation block takes slot 0 with existence
    // left to Validator (which owns that message).
    if (!col->table_name.empty()) {
        throw std::runtime_error("unknown table qualifier: '" + col->table_name + "'");
    }
    if (scope.range_table.size() < 2) {
        col->relation_slot = 0;
        col->query_level = 0;
        return;
    }
    // several relations and no match: leave unresolved; Validator reports
    // "column not found"
}
