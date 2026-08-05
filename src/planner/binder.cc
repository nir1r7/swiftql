#include "binder.h"
#include "constant_folding.h"
#include "parser/expr_utils.h"
#include <stdexcept>

void Binder::bind(SelectStatement& stmt, const Catalog& catalog) {
    // table existence is Validator's error to raise (preserves its message)
    // without a valid range table there is nothing safe to resolve here
    if (!catalog.hasTable(stmt.from_table)) return;
    if (stmt.join.has_value() && !catalog.hasTable(stmt.join->join_table)) return;

    std::vector<RangeEntry> range_table;
    const Schema& from_schema = catalog.getTable(stmt.from_table).schema;
    range_table.push_back({
        stmt.from_alias.empty() ? stmt.from_table : stmt.from_alias,
        stmt.from_table,
        &from_schema
    });

    if (stmt.join.has_value()) {
        const Schema& join_schema = catalog.getTable(stmt.join->join_table).schema;
        range_table.push_back({
            stmt.join->alias.empty() ? stmt.join->join_table : stmt.join->alias,
            stmt.join->join_table,
            &join_schema
        });

        // Two relations sharing a ref name are unresolvable — every qualified
        // reference is ambiguous. Distinguish the aliasless self-join (needs
        // aliases, matching SQLite) from a duplicated alias across tables.
        if (range_table[0].ref_name == range_table[1].ref_name) {
            if (range_table[0].table_name == range_table[1].table_name) {
                throw std::runtime_error(
                    "self-join requires table aliases to disambiguate the two references to '"
                    + stmt.from_table + "'");
            }
            throw std::runtime_error(
                "duplicate table alias '" + range_table[0].ref_name
                + "': each side of a join needs a distinct name");
        }
    }

    if (!stmt.select_star) {
        for (auto& expr : stmt.select_list) bindExpr(expr.get(), range_table);
    }
    bindExpr(stmt.where.get(), range_table);
    // GROUP BY items resolve through the same machinery as column refs:
    // qualified names pick their side, ambiguous unqualified names throw.
    // A bare name matching no input column falls back to a select-list alias
    // (input columns take precedence, matching SQLite GROUP BY scoping);
    // expression items just bind their column references.
    for (auto& g : stmt.group_by) {
        if (!g.expr && g.table_name.empty() && !g.column_name.empty()) {
            bool is_column = false;
            for (const auto& rte : range_table) {
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
                        } else {
                            g.expr = std::move(clone);
                        }
                        break;
                    }
                }
            }
        }
        if (g.expr) {
            bindExpr(g.expr.get(), range_table);
            continue;
        }
        ColumnRef tmp;
        tmp.table_name = g.table_name;
        tmp.column_name = g.column_name;
        resolveColumnRef(&tmp, range_table);
        g.table_name = tmp.table_name;
        g.relation_slot = tmp.relation_slot;
    }
    bindExpr(stmt.having.get(), range_table);
    // SQLite scoping: ORDER BY names resolve against select-list aliases
    // first, then base columns. Substitute a clone of the aliased expression —
    // Sort evaluates below Project, where the alias name is not a column.
    // The select list is already bound, so clones carry relation_slot stamps.
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
        bindExpr(item.expr.get(), range_table);   // no-op on already-stamped clones
    }
    if (stmt.join.has_value()) bindExpr(stmt.join->condition.get(), range_table);

    // Fold constant arithmetic last, so every downstream pass — Validator, the
    // logical planner, pushdown, cardinality estimation, the chunk pruner, and
    // the columnar comparison fast path — sees `season = 2024` rather than
    // `season = 2020 + 4`. Unconditional: folding cannot change results, so it
    // is canonicalization rather than a cost-based decision, and both execution
    // paths and `--no-optimize` get it.
    foldConstants(stmt);
}

void Binder::bindExpr(Expr* expr, const std::vector<RangeEntry>& range_table) {
    if (!expr) return;

    if (auto* col = dynamic_cast<ColumnRef*>(expr)) {
        resolveColumnRef(col, range_table);
    } else if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        bindExpr(bin->left.get(), range_table);
        bindExpr(bin->right.get(), range_table);
    } else if (auto* isnull = dynamic_cast<IsNullExpr*>(expr)) {
        bindExpr(isnull->operand.get(), range_table);
    } else if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
        bindExpr(un->operand.get(), range_table);
    } else if (auto* agg = dynamic_cast<AggregateExpr*>(expr)) {
        if (!agg->is_star) bindExpr(agg->argument.get(), range_table);
    }
    // literal, nothing to bind
}

void Binder::resolveColumnRef(ColumnRef* col, const std::vector<RangeEntry>& range_table) {
    if (!col->table_name.empty()) {
        // qualified reference: the qualifier must name exactly one relation
        // (its alias if it has one, otherwise its table name)
        for (int slot = 0; slot < static_cast<int>(range_table.size()); ++slot) {
            const RangeEntry& rte = range_table[slot];
            if (rte.ref_name == col->table_name) {
                if (!rte.schema->hasColumn(col->column_name)) {
                    throw std::runtime_error(
                        "column '" + col->column_name + "' not found in '" + rte.ref_name + "'");
                }
                // keep the as-typed qualifier (alias or table name): aggregate
                // output names are built from it, and self-join occurrences
                // are only distinguishable by their aliases. Routing uses the
                // slot, never the qualifier text.
                col->relation_slot = slot;        // 0 = FROM, 1 = JOIN
                return;
            }
        }
        throw std::runtime_error("unknown table qualifier: '" + col->table_name + "'");
    }

    // unqualified: single relation resolves to slot 0 (existence stays
    // Validator's job); with two relations, resolve across both and reject
    // ambiguity — the matching side determines the slot
    if (range_table.size() < 2) {
        col->relation_slot = 0;
        return;
    }

    int matches = 0;
    int resolved_slot = -1;
    std::string resolved_table;
    for (int slot = 0; slot < static_cast<int>(range_table.size()); ++slot) {
        if (range_table[slot].schema->hasColumn(col->column_name)) {
            ++matches;
            resolved_slot = slot;
            resolved_table = range_table[slot].table_name;
        }
    }
    if (matches > 1) {
        throw std::runtime_error("ambiguous column reference: '" + col->column_name + "'");
    }
    if (matches == 1) {
        col->table_name = resolved_table; // fully qualify
        col->relation_slot = resolved_slot;
    }
    // matches == 0: leave unresolved; Validator reports "column not found"
}
