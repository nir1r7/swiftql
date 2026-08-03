#include "cardinality_estimator.h"
#include "parser/ast.h"
#include <algorithm>

const ColumnStatsEntry* StatsContext::find(const std::string& name, int slot) const {
    if (slot >= 0) {
        for (const auto& e : entries)
            if (e.relation_slot == slot && e.name == name) return &e;
    }
    for (const auto& e : entries)
        if (e.name == name) return &e;   // bare-name fallback, first match wins
    return nullptr;
}

// both operands non-null and mutually comparable? Value::operator< throws on
// STRING vs numeric, so guard before touching min/max
static bool comparable(const Value& a, const Value& b) {
    if (a.isNull() || b.isNull()) return false;
    return (a.type() == TypeId::STRING) == (b.type() == TypeId::STRING);
}

double CardinalityEstimator::selectivity(const Expr* pred, const StatsContext& ctx) {
    if (auto* isnull = dynamic_cast<const IsNullExpr*>(pred)) {
        auto* col = dynamic_cast<const ColumnRef*>(isnull->operand.get());
        const ColumnStatsEntry* e = col ? ctx.find(col->column_name, col->relation_slot) : nullptr;
        if (!e || e->table_rows == 0) return FALLBACK_EQ_SELECTIVITY;
        double null_frac = static_cast<double>(e->stats->null_count) / e->table_rows;
        return isnull->is_not_null ? 1.0 - null_frac : null_frac;
    }

    auto* bin = dynamic_cast<const BinaryExpr*>(pred);
    if (!bin) return FALLBACK_SELECTIVITY;

    // conjunction / disjunction under the independence assumption
    if (bin->op == "AND")
        return selectivity(bin->left.get(), ctx) * selectivity(bin->right.get(), ctx);
    if (bin->op == "OR") {
        double l = selectivity(bin->left.get(), ctx);
        double r = selectivity(bin->right.get(), ctx);
        return l + r - l * r;   // inclusion-exclusion under independence
    }

    // normalize to col-op-lit: the grammar allows "300 < speed" too
    auto* col = dynamic_cast<const ColumnRef*>(bin->left.get());
    auto* lit = dynamic_cast<const Literal*>(bin->right.get());
    std::string op = bin->op;
    if (!col || !lit) {
        col = dynamic_cast<const ColumnRef*>(bin->right.get());
        lit = dynamic_cast<const Literal*>(bin->left.get());
        // flipping operands mirrors the comparison: 300 < speed  ==  speed > 300
        if (op == "<") op = ">"; else if (op == ">") op = "<";
        else if (op == "<=") op = ">="; else if (op == ">=") op = "<=";
    }
    if (!col || !lit)
        return (op == "=") ? FALLBACK_EQ_SELECTIVITY : FALLBACK_SELECTIVITY;

    const ColumnStatsEntry* e = ctx.find(col->column_name, col->relation_slot);

    if (op == "=" || op == "!=") {
        double eq = FALLBACK_EQ_SELECTIVITY;
        if (e && e->stats->distinct_count > 0) {
            eq = 1.0 / e->stats->distinct_count;
            // literal outside [min,max] cannot match — ChunkPruner logic
            // at table granularity
            if (comparable(lit->value, e->stats->min_val) &&
                (lit->value < e->stats->min_val || lit->value > e->stats->max_val))
                eq = 0.0;
        }
        return (op == "=") ? eq : 1.0 - eq;
    }

    if (op == "<" || op == "<=" || op == ">" || op == ">=") {
        // interpolation needs a numeric interval; strings fall back
        if (!e || e->stats->min_val.isNull() ||
            e->stats->min_val.type() == TypeId::STRING ||
            lit->value.type() == TypeId::STRING)
            return FALLBACK_RANGE_SELECTIVITY;
        double lo = e->stats->min_val.toNumeric();
        double hi = e->stats->max_val.toNumeric();
        if (hi <= lo) return FALLBACK_RANGE_SELECTIVITY;  // single-value column
        double below = std::clamp((lit->value.toNumeric() - lo) / (hi - lo), 0.0, 1.0);
        return (op == "<" || op == "<=") ? below : 1.0 - below;
    }

    return FALLBACK_SELECTIVITY;
}

void CardinalityEstimator::estimate(LogicalPlanNode& root, const Catalog& catalog) {
    estimateNode(root, catalog);
}

StatsContext CardinalityEstimator::estimateNode(LogicalPlanNode& node, const Catalog& catalog) {
    switch (node.type) {
        case LogicalNodeType::SCAN: {
            auto& scan = static_cast<LogicalScan&>(node);
            StatsContext ctx;
            if (catalog.hasStats(scan.table_name)) {
                const TableStats& ts = catalog.getStats(scan.table_name);
                node.estimated_rows = static_cast<double>(ts.row_count);
                // the scan schema is already narrowed to referenced columns,
                // so the context carries exactly the stats the plan can use
                for (const auto& col : scan.output_schema.columns()) {
                    auto it = ts.columns.find(col.name);
                    if (it != ts.columns.end())
                        ctx.entries.push_back({col.name, col.relation_slot,
                                               &it->second, ts.row_count});
                }
            } else {
                node.estimated_rows = static_cast<double>(FALLBACK_ROW_COUNT);
            }
            return ctx;
        }

        case LogicalNodeType::FILTER: {
            auto& f = static_cast<LogicalFilter&>(node);
            StatsContext ctx = estimateNode(*node.children[0], catalog);
            node.estimated_rows = node.children[0]->estimated_rows
                                * selectivity(f.predicate.get(), ctx);
            // independence assumption: column stats are not narrowed by the
            // filter; stacked predicates each see base-table distributions
            return ctx;
        }

        case LogicalNodeType::JOIN: {
            auto& join = static_cast<LogicalJoin&>(node);
            StatsContext left  = estimateNode(*node.children[0], catalog);
            StatsContext right = estimateNode(*node.children[1], catalog);
            double l = node.children[0]->estimated_rows;
            double r = node.children[1]->estimated_rows;

            // children[0]=FROM, children[1]=JOIN (fixed logical order, Week 16);
            // from_col/join_col were routed to their sides by extractJoinKeys
            const ColumnStatsEntry* lk = left.find(join.from_col, -1);
            const ColumnStatsEntry* rk = right.find(join.join_col, -1);
            int64_t ndv = std::max(lk ? lk->stats->distinct_count : int64_t(0),
                                   rk ? rk->stats->distinct_count : int64_t(0));

            // no usable key NDV: assume the FK-like case (max) rather than a
            // cross product, which would explode and mislead Week 22 costing
            node.estimated_rows = (ndv > 0) ? (l * r) / ndv : std::max(l, r);

            // merge contexts [FROM ++ JOIN], restamping join-side entries to
            // slot 1 — mirrors the merged-schema construction in
            // LogicalPlanBuilder::build
            StatsContext out = std::move(left);
            for (ColumnStatsEntry e : right.entries) {
                e.relation_slot = 1;
                out.entries.push_back(std::move(e));
            }
            return out;
        }

        case LogicalNodeType::AGGREGATE: {
            auto& agg = static_cast<LogicalAggregate&>(node);
            StatsContext child_ctx = estimateNode(*node.children[0], catalog);
            double child = node.children[0]->estimated_rows;

            if (agg.group_by.empty()) {
                node.estimated_rows = 1.0;  // global aggregate: exactly one row
            } else {
                double groups = 1.0;
                for (const auto& g : agg.group_by) {
                    // slot-first lookup mirrors execution: a qualified GROUP BY
                    // reads NDV from the named join side
                    const ColumnStatsEntry* e = child_ctx.find(g.column_name, g.relation_slot);
                    // unknown NDV contributes no reduction; the clamp bounds it
                    groups *= (e && e->stats->distinct_count > 0)
                            ? static_cast<double>(e->stats->distinct_count)
                            : child;
                }
                node.estimated_rows = std::min(groups, child);
            }

            // aggregate outputs are new columns (group keys + AVG(speed) etc.);
            // base-table stats no longer describe them
            return StatsContext{};
        }

        case LogicalNodeType::PROJECT:
        case LogicalNodeType::SORT:
        case LogicalNodeType::DISTINCT: {
            // row-preserving for estimation purposes (distinct: upper bound,
            // documented simplification); context flows through unchanged
            StatsContext ctx = estimateNode(*node.children[0], catalog);
            node.estimated_rows = node.children[0]->estimated_rows;
            return ctx;
        }

        case LogicalNodeType::LIMIT: {
            auto& lim = static_cast<LogicalLimit&>(node);
            StatsContext ctx = estimateNode(*node.children[0], catalog);
            node.estimated_rows = std::min(static_cast<double>(lim.limit),
                                           node.children[0]->estimated_rows);
            return ctx;
        }
    }
    return StatsContext{};
}
