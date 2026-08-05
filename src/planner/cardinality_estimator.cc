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
        if (!e || e->table_rows == 0) {
            // fallback is the assumed null fraction, so it must respect polarity:
            // IS NOT NULL keeps ~everything, not ~nothing
            return isnull->is_not_null ? 1.0 - FALLBACK_EQ_SELECTIVITY
                                       : FALLBACK_EQ_SELECTIVITY;
        }
        double null_frac = static_cast<double>(e->stats->null_count) / e->table_rows;
        return isnull->is_not_null ? 1.0 - null_frac : null_frac;
    }

    // IN over k constants: the equality case (1/ndv) generalised to a list,
    // capped at 1. Material for TPC-H Q12/Q16/Q19/Q22, where an IN list is the
    // most selective conjunct and therefore has to sort first in the cascade.
    // Safe to promote: a compiled IN_SET probe measures the same as a native
    // comparison (2.6ms vs 2.4ms per 1M rows), so ranking it by selectivity
    // alone does not misprice it.
    if (auto* in = dynamic_cast<const InExpr*>(pred)) {
        auto* col = dynamic_cast<const ColumnRef*>(in->operand.get());
        const ColumnStatsEntry* e = col ? ctx.find(col->column_name, col->relation_slot) : nullptr;

        // Distinct values only. `x IN (2022, 2022, 2022)` matches exactly the
        // rows `x = 2022` does; counting the duplicates inflated k past NDV,
        // clamped selectivity to 1.0, and drove the negated case to 0. Lists are
        // a handful of elements, so the quadratic scan is free — and it avoids
        // needing a hash for Value. inferExprType has already rejected mixed
        // STRING/numeric lists, so operator== cannot throw here.
        auto isDuplicate = [&in](size_t i) {
            for (size_t j = 0; j < i; ++j) {
                if (in->values[j] == in->values[i]) return true;
            }
            return false;
        };

        double s;
        if (e && e->stats->distinct_count > 0) {
            // Count only the values the column could actually hold — the same
            // [min,max] containment the equality branch below applies, one list
            // element at a time. Counting all k instead is not merely imprecise,
            // it inverts the negated case: `season NOT IN (1..10)` against
            // season [2022,2025] scored k=10 over ndv=4, clamped to selectivity
            // 1.0, and returned 0 for NOT — a 1-row estimate for a filter that
            // keeps every row. That flipped the join onto the 10000-row build
            // side (measured 24.0ms against 15.6ms for the equivalent positive
            // form). An estimate that is low and wrong is exactly what the LIKE
            // note below refuses to ship; the same guard belongs here.
            double k = 0.0;
            for (size_t i = 0; i < in->values.size(); ++i) {
                const Value& v = in->values[i];
                if (isDuplicate(i)) continue;
                if (comparable(v, e->stats->min_val) &&
                    (v < e->stats->min_val || v > e->stats->max_val)) {
                    continue;   // cannot match any row
                }
                k += 1.0;
            }
            s = std::min(1.0, k / static_cast<double>(e->stats->distinct_count));
        } else {
            double k = 0.0;
            for (size_t i = 0; i < in->values.size(); ++i) {
                if (!isDuplicate(i)) k += 1.0;
            }
            s = std::min(1.0, k * FALLBACK_EQ_SELECTIVITY);
        }
        if (!in->negated) return s;

        // NOT IN never estimates that nothing survives. Reaching s == 1.0 means
        // the list *appears* to cover every distinct value, but that rests on an
        // NDV estimate (or, with no stats at all, on a flat 0.1 guess that any
        // list of ten values saturates). A 0 selectivity floors to a 1-row
        // estimate for a filter that keeps nearly everything, which put the
        // 10000-row input on the join's build side — measured 18.3ms against
        // 15.8ms for an equivalent-cardinality control.
        //
        // The positive sense is deliberately NOT floored: there, s == 0 comes
        // from a proof (every listed value lies outside [min,max], so no row can
        // match), not from a guess. Same asymmetry the equality branch below
        // relies on when it zeroes `eq` for an out-of-range literal.
        const double single_value_mass = (e && e->stats->distinct_count > 0)
            ? 1.0 / static_cast<double>(e->stats->distinct_count)
            : FALLBACK_EQ_SELECTIVITY;
        return std::max(1.0 - s, single_value_mass);
    }

    // LIKE deliberately has NO rule: it falls through to FALLBACK_SELECTIVITY.
    //
    // A histogram-free engine cannot estimate pattern matching, and guessing low
    // actively hurts. Postgres' 0.005 was tried and measured 1.4-1.7x SLOWER on
    // `WHERE team LIKE 'Fer%' AND lap_id BETWEEN 900000 AND 900100` (1M rows,
    // Release): orderByWork ranks conjuncts on selectivity alone, so the tiny
    // estimate promoted the LIKE ahead of two cheap integer comparisons and the
    // expensive predicate then ran on every surviving row instead of on the ~100
    // the ranges would have left. The guess was also simply wrong — that pattern
    // keeps 25% of this table, not 0.5%.
    //
    // An accurate LIKE selectivity is only safe once conjunct ordering is
    // cost-aware, which predicate_pushdown.cc already defers to Week 28. Land
    // the two together there, not the estimate alone.

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
        double v  = lit->value.toNumeric();
        if (hi == lo) {
            // single-value column: decidable from stats — all rows or none
            bool match = (op == "<")  ? lo <  v
                       : (op == "<=") ? lo <= v
                       : (op == ">")  ? lo >  v
                       :                lo >= v;
            return match ? 1.0 : 0.0;
        }
        if (hi < lo) return FALLBACK_RANGE_SELECTIVITY;  // corrupt stats only
        double below = std::clamp((v - lo) / (hi - lo), 0.0, 1.0);
        // min/max come from real rows, so a closed comparison at the domain
        // edge is guaranteed to match: floor it at the equality mass 1/ndv
        double eq = e->stats->distinct_count > 0
            ? 1.0 / e->stats->distinct_count
            : FALLBACK_EQ_SELECTIVITY;
        if (op == "<")  return below;
        if (op == ">")  return 1.0 - below;
        if (op == "<=") return v < lo ? 0.0 : std::max(below, eq);
        return              v > hi ? 0.0 : std::max(1.0 - below, eq);   // ">="
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
            double child = node.children[0]->estimated_rows;
            double est = child * selectivity(f.predicate.get(), ctx);
            // ≥1-row floor (Postgres-style): a zero estimate poisons join
            // costing downstream. Applied only when the child itself has rows,
            // so FilterNeverExceedsChild monotonicity holds.
            node.estimated_rows = (child >= 1.0) ? std::max(est, 1.0) : est;
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
            // ≥1-row floor, matching the FILTER case
            if (l >= 1.0 && r >= 1.0) {
                node.estimated_rows = std::max(node.estimated_rows, 1.0);
            }

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
