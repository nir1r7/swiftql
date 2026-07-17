#include "binder.h"
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

        // A self-join (same table on both sides) is only resolvable if the two
        // occurrences have distinct names to qualify with — otherwise every
        // qualified reference is ambiguous. Require aliases, matching SQLite.
        if (range_table[0].ref_name == range_table[1].ref_name) {
            throw std::runtime_error(
                "self-join requires table aliases to disambiguate the two references to '"
                + stmt.from_table + "'");
        }
    }

    if (!stmt.select_star) {
        for (auto& expr : stmt.select_list) bindExpr(expr.get(), range_table);
    }
    bindExpr(stmt.where.get(), range_table);
    bindExpr(stmt.having.get(), range_table);
    for (auto& item : stmt.order_by) bindExpr(item.expr.get(), range_table);
    if (stmt.join.has_value()) bindExpr(stmt.join->condition.get(), range_table);
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
                col->table_name = rte.table_name; // normalize alias -> canonical table name
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
