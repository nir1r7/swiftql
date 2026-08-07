#include "cardinality_estimator.h"
#include "parser/ast.h"
#include <algorithm>

#include <stdexcept>
const ColumnStatsEntry* StatsContext::findExact(const std::string& name, int slot) const {
    if (slot < 0) return nullptr;
    for (const auto& e : entries)
        if (e.relation_slot == slot && e.name == name) return &e;
    return nullptr;
}

const ColumnStatsEntry* StatsContext::findForRef(const std::string& name, int slot) const {
    return slot >= 0 ? findExact(name, slot) : find(name, -1);
}

const ColumnStatsEntry* StatsContext::find(const std::string& name, int slot) const {
    if (const ColumnStatsEntry* e = findExact(name, slot)) return e;
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
        const ColumnStatsEntry* e = col ? ctx.findForRef(col->column_name, col->relation_slot) : nullptr;
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
        const ColumnStatsEntry* e = col ? ctx.findForRef(col->column_name, col->relation_slot) : nullptr;

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

    // Week 31. A NULL literal became possible for the first time: a materialized
    // scalar subquery that returned zero rows (or one NULL row) substitutes one.
    // Every comparison against NULL is UNKNOWN, so the predicate matches nothing
    // — 0.0 is the exact answer, not a fallback, and it is the same conclusion
    // the eq branch below already draws for a literal outside [min,max].
    //
    // It is also a correctness guard, not only an accuracy one: both branches
    // below call lit->value.type(), which THROWS on a null Value, so before this
    // the optimized vectorized path died with "Cannot get type of null Value" on
    // a query --no-optimize answered correctly.
    if (lit->value.isNull()) return 0.0;

    const ColumnStatsEntry* e = ctx.findForRef(col->column_name, col->relation_slot);

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

double joinCardinality(double left_rows, double right_rows,
                       const std::vector<JoinKey>& keys,
                       const StatsContext& left, const StatsContext& right) {
    // Lifted VERBATIM out of estimateNode's JOIN case in Week 28 so the join
    // search and the stamped plan cannot hold different cardinality models.
    //
    // One NDV per key, divided out together — the independent-key generalization
    // of the single-key formula (Week 26 multi-key equi-joins). The left lookup
    // goes by slot: the merged left context can hold the same column name at
    // several slots.
    //
    // `have_ndv` is tracked separately from the product on purpose: an NDV of 1
    // leaves divisor == 1.0 while being a perfectly usable statistic — every left
    // row matches every right row, so l*r/1 is the exact answer. Testing
    // `divisor > 1.0` instead sent that case to the no-statistics fallback and
    // underestimated a constant-key join by the table size.
    double divisor = 1.0;
    bool have_ndv = false;
    for (const JoinKey& k : keys) {
        // Left side: slot-EXACT. from_slot exists precisely because the merged
        // left context can hold one column name at several slots, so honouring
        // it only when it happens to hit would make the disambiguation advisory.
        // It misses whenever the key's own relation has no TableStats (a
        // stats-less scan contributes no entries at all), and the bare-name
        // fallback would then hand back a different relation's column with no
        // signal. A miss is "no statistic" — which have_ndv below already models.
        // Right side: one relation, so a bare-name match is unambiguous and -1
        // asks for it deliberately.
        // An unbound key (from_slot -1, positional routing) has no relation
        // identity to be exact about, so findForRef keeps the documented
        // bare-name behaviour for it.
        const ColumnStatsEntry* lk = left.findForRef(k.from_col, k.from_slot);
        const ColumnStatsEntry* rk = right.find(k.join_col, -1);
        int64_t ndv = std::max(lk ? lk->stats->distinct_count : int64_t(0),
                               rk ? rk->stats->distinct_count : int64_t(0));
        if (ndv > 0) {
            divisor *= static_cast<double>(ndv);
            have_ndv = true;
        }
    }

    // no usable key NDV: assume the FK-like case (max) rather than a cross
    // product, which would explode and mislead Week 22 costing.
    //
    // !! max() is not multiplicative, so a subset containing a stats-less
    // relation has an order-dependent row estimate and the DP's optimal
    // substructure does not hold for it. Unfixable inside this rule — there is no
    // path-independent estimate to fall back to — so the containment is the
    // written-order guard in JoinEnumeration::reorder, which bounds the search to
    // "never worse than the written order" whatever the estimates do.
    return have_ndv ? (left_rows * right_rows) / divisor
                    : std::max(left_rows, right_rows);
}

double flooredJoinCardinality(double left_rows, double right_rows, double rows) {
    // ≥1-row floor, matching the FILTER case: a zero estimate poisons join
    // costing downstream, and a stamped 0 reads as "this returns nothing".
    //
    // Applied at the STAMPING sites only, never inside joinCardinality, because
    // the join search must not see it. The clamp is per join step, so a candidate
    // order passing through a sub-1-row intermediate has every later estimate
    // inflated by 1/true_rows while an order that never dips below 1 does not —
    // which makes rows(S) depend on the path that reached S, destroys the DP's
    // optimal substructure, and lets it lock onto a cheap prefix whose floored
    // count poisons every later transition. Measured before this split: a
    // 4-relation shape where the DP returned cost=666 against the written order's
    // 629, and a 5-relation one 4.81x worse, every violation carrying a floored
    // est=1 intermediate. The search now chains the raw product, which is a pure
    // function of the subset; the stamped tree is unchanged, because a floored
    // child feeds the next stamp exactly as before.
    if (left_rows >= 1.0 && right_rows >= 1.0) return std::max(rows, 1.0);
    return rows;
}

void CardinalityEstimator::estimate(LogicalPlanNode& root, const Catalog& catalog) {
    estimateNode(root, catalog);
}

StatsContext CardinalityEstimator::estimateSubtree(LogicalPlanNode& node, const Catalog& catalog) {
    return estimateNode(node, catalog);
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

            // children[0]=left input, children[1]=the relation this join adds
            // (fixed logical order, Week 16); keys were routed to their sides by
            // classifyJoinCondition. The formula itself lives in
            // joinCardinality() since Week 28, shared with join enumeration so
            // the search and the stamp cannot disagree.
            //
            // The key lookups happen HERE, before the merge restamp below: at the
            // bottom join the left context is a leaf's, stamped slot 0, and the
            // keys' from_slot is 0 to match (see JoinEnumeration::rebuild and the
            // JoinKey contract in join_condition.h). Hoisting the restamp above
            // this call makes every bottom-join key lookup miss — nothing fails,
            // the estimates just quietly degrade.
            // the floor belongs to the STAMP, not to the rule — see
            // flooredJoinCardinality for why the search must chain the raw product
            const double l_rows = node.children[0]->estimated_rows;
            const double r_rows = node.children[1]->estimated_rows;
            double rows = flooredJoinCardinality(
                l_rows, r_rows,
                joinCardinality(l_rows, r_rows, join.keys, left, right));
            // merge contexts [left ++ added relation], restamping each block to
            // the slot the MERGED SCHEMA gives it — must stay in lockstep with
            // the merged-schema construction in LogicalPlanBuilder::build and in
            // JoinEnumeration::rebuild.
            StatsContext out = std::move(left);
            // Week 28: a leaf's own context stamps slot 0, because a standalone
            // scan has one relation and nothing to disambiguate. That is the
            // leftmost relation's real identity only while the leftmost relation
            // IS slot 0 — which join enumeration no longer guarantees. Read the
            // truth off the merged schema's first column. No-op in written order.
            if (node.children[0]->type != LogicalNodeType::JOIN &&
                node.output_schema.size() > 0) {
                const int left_slot = node.output_schema.column(0).relation_slot;
                for (ColumnStatsEntry& e : out.entries) e.relation_slot = left_slot;
            }
            for (ColumnStatsEntry e : right.entries) {
                e.relation_slot = join.join_slot;
                out.entries.push_back(std::move(e));
            }

            // Week 29: a LEFT join emits every preserved-side row at least once,
            // so its output is |R ⋈ S| + |unmatched R|. There is no statistic for
            // the second term; max() is the standard containment.
            //
            // The ON residual narrows the MATCH term and nothing else, so it is
            // applied INSIDE the max rather than ignored. Ignoring it is unsafe in
            // exactly the direction the floor exposes: a highly selective residual
            // drives the true answer down TO the floor, while the unadjusted
            // estimate stays at the ceiling — measured est=10000 against
            // rows_out=20 on `d LEFT JOIN l ON k AND l.speed > 349`, a value the
            // max() clamp already knows as l_rows. It is not cosmetic: the parent
            // join of a mixed query reads this number as `from_est` for its
            // build-side and algorithm choice. Scored against the MERGED context,
            // which is why this sits below the merge — the residual may reference
            // either side, and the merged context is the one where both resolve.
            //
            // All of it stays at the STAMP and out of joinCardinality: neither
            // max() nor a residual selectivity is multiplicative, so a subset's row
            // count would depend on the path that reached it and the DP's optimal
            // substructure would be gone — the same reason the ≥1-row floor moved
            // out in Week 28. JoinEnumeration declines outer-join trees entirely,
            // so the search never meets either; both facts have to stay true.
            // Week 32 — SEMI/ANTI, AT THE STAMP and never inside
            // joinCardinality: both rules are NON-MULTIPLICATIVE (a clamp
            // against l_rows, and a subtraction), so a subset's estimate would
            // depend on the path that reached it and the DP's optimal
            // substructure would be gone. Identical argument to the >=1-row
            // floor (Week 28) and the outer-join max() below (Week 29).
            // JoinEnumeration also declines these trees — hasSlotOutsideRangeTable
            // fires on join_slot == -1 — and both facts must hold INDEPENDENTLY.
            //
            // Semi-join selectivity is a property of the LEFT side: the fraction
            // of left rows whose key value also occurs on the right. The right
            // side contributes only its NDV, never its row count, which is
            // exactly why the product form joinCardinality computed above is the
            // wrong shape here and is overwritten rather than adjusted.
            if (join.semantics != JoinSemantics::STANDARD) {
                if (join.on_residual) {
                    throw std::runtime_error(
                        "internal: a semi/anti join carries no ON residual");
                }
                double frac = 1.0;
                if (!join.keys.empty()) {
                    // The SAME lookups joinCardinality makes — slot-exact on the
                    // left (the merged context can hold one name at several
                    // slots), bare-name on the right (a body plan is one
                    // relation) — but read SEPARATELY rather than max()'d,
                    // because the semi rule needs the ratio. `have_ndv` stays
                    // tracked apart from the value: an NDV of 1 is a usable
                    // statistic, a rule this codebase has corrected twice.
                    const ColumnStatsEntry* lk =
                        out.findForRef(join.keys[0].from_col, join.keys[0].from_slot);
                    const ColumnStatsEntry* rk = right.find(join.keys[0].join_col, -1);
                    const bool have_l = lk && lk->stats->distinct_count > 0;
                    const bool have_r = rk && rk->stats->distinct_count > 0;
                    if (have_l && have_r) {
                        frac = std::min(1.0,
                            static_cast<double>(rk->stats->distinct_count)
                          / static_cast<double>(lk->stats->distinct_count));
                    }
                }
                double semi = l_rows * frac;
                rows = (join.semantics == JoinSemantics::SEMI) ? semi : (l_rows - semi);
                rows = std::max(0.0, std::min(rows, l_rows));   // never exceeds the left side
                node.estimated_rows = rows;
                return out;
            }

            if (join.join_type == JoinType::LEFT) {
                if (join.on_residual) {
                    rows *= selectivity(join.on_residual.get(), out);
                }
                rows = std::max(rows, l_rows);
            }
            node.estimated_rows = rows;
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
                    const ColumnStatsEntry* e = child_ctx.findForRef(g.column_name, g.relation_slot);
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
