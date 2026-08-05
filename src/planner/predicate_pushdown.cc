#include "predicate_pushdown.h"
#include "cardinality_estimator.h"
#include "parser/ast.h"
#include <algorithm>
#include <unordered_set>
#include <vector>

namespace {

// Flatten an AND-chain into its atomic conjuncts, moving ownership out of the
// tree. OR / comparison / IS NULL are indivisible — each becomes one leaf.
// Mirrors the recursion shape of collectCols() in logical_plan.cc.
void splitConjuncts(std::unique_ptr<Expr> pred, std::vector<std::unique_ptr<Expr>>& out) {
    auto* bin = dynamic_cast<BinaryExpr*>(pred.get());
    if (bin && bin->op == "AND") {
        // move both operands out before the AND node dies at end of scope
        splitConjuncts(std::move(bin->left), out);
        splitConjuncts(std::move(bin->right), out);
        return;
    }
    out.push_back(std::move(pred));
}

// Collect the set of relation slots a predicate's columns reference. Same
// dispatch as collectCols(), reading relation_slot instead of name.
void collectSlots(const Expr* expr, std::unordered_set<int>& out) {
    if (!expr) return;
    if (auto* cr = dynamic_cast<const ColumnRef*>(expr)) { out.insert(cr->relation_slot); return; }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        collectSlots(bin->left.get(), out);
        collectSlots(bin->right.get(), out);
        return;
    }
    if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) { collectSlots(isn->operand.get(), out); return; }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) { collectSlots(un->operand.get(), out); return; }
    // Week 25 nodes. Missing one here returns an EMPTY slot set, so classify()
    // falls through to RESIDUAL and the conjunct is never pushed below the join
    // — correct answers, silently lost pushdown. TPC-H Q12/Q14/Q16/Q19 all
    // depend on a LIKE or IN predicate reaching its own scan.
    if (auto* in = dynamic_cast<const InExpr*>(expr)) { collectSlots(in->operand.get(), out); return; }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) { collectSlots(lk->operand.get(), out); return; }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        for (const auto& w : c->when_clauses) {
            collectSlots(w.condition.get(), out);
            collectSlots(w.result.get(), out);
        }
        collectSlots(c->else_expr.get(), out);
        return;
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        collectSlots(sub->operand.get(), out);
        collectSlots(sub->start.get(), out);
        collectSlots(sub->length.get(), out);   // nullptr-safe
        return;
    }
    // Literal / IntervalLiteral: no slot. AggregateExpr cannot appear in WHERE
    // (Validator forbids it).
}

enum class PushTarget { FROM, JOIN, RESIDUAL };

// A conjunct is pushable only if every column it references resolves to one
// side. Mixed (both slots), unresolved (-1), or literal-only (empty) stay above
// the join as a residual.
PushTarget classify(const Expr* conjunct) {
    std::unordered_set<int> slots;
    collectSlots(conjunct, slots);
    if (slots.size() == 1 && !slots.count(-1))
        return slots.count(0) ? PushTarget::FROM : PushTarget::JOIN;
    return PushTarget::RESIDUAL;
}

// Re-stamp every ColumnRef in a pushed conjunct to the given slot. Same
// dispatch as collectSlots(). A conjunct pushed below the join lives in a
// single-table subtree whose scan schema stamps all columns slot 0, so the
// refs must match — this is also what lets ChunkPruner act on join-side
// pruning hints (it ignores slot >= 1 refs; see chunk_pruner.h).
void restampSlots(Expr* expr, int slot) {
    if (!expr) return;
    if (auto* cr = dynamic_cast<ColumnRef*>(expr)) { cr->relation_slot = slot; return; }
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        restampSlots(bin->left.get(), slot);
        restampSlots(bin->right.get(), slot);
        return;
    }
    if (auto* isn = dynamic_cast<IsNullExpr*>(expr)) { restampSlots(isn->operand.get(), slot); return; }
    if (auto* un = dynamic_cast<UnaryExpr*>(expr)) { restampSlots(un->operand.get(), slot); return; }
    // Must stay in lockstep with collectSlots above: a conjunct classified as
    // pushable there but not re-stamped here keeps slot 1 below the join, where
    // ChunkPruner ignores it and the zone-map hint is lost.
    if (auto* in = dynamic_cast<InExpr*>(expr)) { restampSlots(in->operand.get(), slot); return; }
    if (auto* lk = dynamic_cast<LikeExpr*>(expr)) { restampSlots(lk->operand.get(), slot); return; }
    if (auto* c = dynamic_cast<CaseExpr*>(expr)) {
        for (auto& w : c->when_clauses) {
            restampSlots(w.condition.get(), slot);
            restampSlots(w.result.get(), slot);
        }
        restampSlots(c->else_expr.get(), slot);
        return;
    }
    if (auto* sub = dynamic_cast<SubstringExpr*>(expr)) {
        restampSlots(sub->operand.get(), slot);
        restampSlots(sub->start.get(), slot);
        restampSlots(sub->length.get(), slot);   // nullptr-safe
    }
}

// Rebuild a left-deep AND-chain from conjuncts, or nullptr if empty.
std::unique_ptr<Expr> conjoin(std::vector<std::unique_ptr<Expr>> parts) {
    if (parts.empty()) return nullptr;
    std::unique_ptr<Expr> acc = std::move(parts[0]);
    for (size_t i = 1; i < parts.size(); ++i) {
        auto conj = std::make_unique<BinaryExpr>();
        conj->op = "AND";
        conj->left = std::move(acc);
        conj->right = std::move(parts[i]);
        acc = std::move(conj);
    }
    return acc;
}

// Build a one-table StatsContext for a scan-local filter's child, exactly as
// CardinalityEstimator's SCAN case does, so selectivity() can score conjuncts.
StatsContext scanStats(const LogicalPlanNode* scan_child, const Catalog& catalog) {
    StatsContext ctx;
    if (scan_child->type != LogicalNodeType::SCAN) return ctx;
    const auto* scan = static_cast<const LogicalScan*>(scan_child);
    if (!catalog.hasStats(scan->table_name)) return ctx;
    const TableStats& ts = catalog.getStats(scan->table_name);
    for (const auto& col : scan->output_schema.columns()) {
        auto it = ts.columns.find(col.name);
        if (it != ts.columns.end())
            ctx.entries.push_back({col.name, col.relation_slot, &it->second, ts.row_count});
    }
    return ctx;
}

// Order conjuncts most-selective-first (smallest keep-fraction). Stable so ties
// and stat-less predicates (fallback selectivity) keep their original order.
// "Expected work" is modeled as selectivity only — optimal when per-predicate
// eval cost is uniform (true for fast-path col-op-literal comparisons). A
// selectivity x eval-cost ranking (which would demote fallback-path predicates
// like IS NULL or col-vs-col) is deferred to Week 28, where join enumeration
// gives the cost model per-predicate costs; Week 22 wired costs into join
// selection only.
void orderByWork(std::vector<std::unique_ptr<Expr>>& conjuncts,
                 const LogicalPlanNode* scan_child, const Catalog& catalog) {
    StatsContext ctx = scanStats(scan_child, catalog);
    std::stable_sort(conjuncts.begin(), conjuncts.end(),
        [&](const std::unique_ptr<Expr>& a, const std::unique_ptr<Expr>& b) {
            return CardinalityEstimator::selectivity(a.get(), ctx)
                 < CardinalityEstimator::selectivity(b.get(), ctx);
        });
}

// Attach conjuncts directly above `child` as one ordered LogicalFilter, or
// return child untouched when there are none.
std::unique_ptr<LogicalPlanNode> filterOnto(std::unique_ptr<LogicalPlanNode> child,
                                            std::vector<std::unique_ptr<Expr>> conjuncts,
                                            const Catalog& catalog) {
    if (conjuncts.empty()) return child;
    orderByWork(conjuncts, child.get(), catalog);
    return std::make_unique<LogicalFilter>(std::move(child), conjoin(std::move(conjuncts)));
}

// Rewrite a WHERE filter sitting directly above a join.
std::unique_ptr<LogicalPlanNode> pushIntoJoin(std::unique_ptr<LogicalFilter> filter,
                                              const Catalog& catalog) {
    // steal the join out from under the filter
    auto join = std::unique_ptr<LogicalJoin>(static_cast<LogicalJoin*>(filter->children[0].release()));

    std::vector<std::unique_ptr<Expr>> conjuncts;
    splitConjuncts(std::move(filter->predicate), conjuncts);   // filter is now empty

    std::vector<std::unique_ptr<Expr>> from_p, join_p, residual_p;
    for (auto& c : conjuncts) {
        switch (classify(c.get())) {
            case PushTarget::FROM:     from_p.push_back(std::move(c));     break;
            case PushTarget::JOIN:     join_p.push_back(std::move(c));     break;
            case PushTarget::RESIDUAL: residual_p.push_back(std::move(c)); break;
        }
    }

    // Pushed JOIN-side conjuncts were classified as referencing only slot 1;
    // below the join they execute against the standalone scan, whose schema
    // stamps every column slot 0. Re-stamp so slot lookups hit directly and
    // ChunkPruner can act on the hint (it must keep ignoring slot >= 1 refs —
    // the FROM-side hint path routes residual/un-pushed predicates containing
    // join-side conjuncts to the FROM scan, where shared column names would
    // make name-based pruning wrong). Residuals keep their slots.
    for (auto& c : join_p) restampSlots(c.get(), 0);
    join->children[0] = filterOnto(std::move(join->children[0]), std::move(from_p), catalog);
    join->children[1] = filterOnto(std::move(join->children[1]), std::move(join_p), catalog);

    // residuals (mixed / unresolved) stay above the join
    return filterOnto(std::move(join), std::move(residual_p), catalog);
}

} // namespace

std::unique_ptr<LogicalPlanNode> PredicatePushdown::apply(std::unique_ptr<LogicalPlanNode> node,
                                                          const Catalog& catalog) {
    // Only a FILTER whose direct child is a JOIN is a pushable WHERE. A HAVING
    // filter sits above an AGGREGATE and is never rewritten.
    if (node->type == LogicalNodeType::FILTER &&
        node->children[0]->type == LogicalNodeType::JOIN) {
        auto* f = static_cast<LogicalFilter*>(node.release());
        return pushIntoJoin(std::unique_ptr<LogicalFilter>(f), catalog);
    }

    // A WHERE directly above a single scan is already at the lowest node, but
    // its conjuncts still get ordered most-selective-first for the cascade.
    if (node->type == LogicalNodeType::FILTER &&
        node->children[0]->type == LogicalNodeType::SCAN) {
        auto* f = static_cast<LogicalFilter*>(node.get());
        std::vector<std::unique_ptr<Expr>> parts;
        splitConjuncts(std::move(f->predicate), parts);
        orderByWork(parts, f->children[0].get(), catalog);
        f->predicate = conjoin(std::move(parts));
        return node;
    }

    for (auto& child : node->children) child = apply(std::move(child), catalog);
    return node;
}
