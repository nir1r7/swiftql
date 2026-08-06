#include "predicate_pushdown.h"
#include "cardinality_estimator.h"
#include "parser/ast.h"
#include "parser/expr_utils.h"   // conjoinAll
#include <algorithm>
#include <map>
#include <unordered_set>
#include <vector>

// Collect the set of relation slots a predicate's columns reference. Same
// dispatch as collectCols(), reading relation_slot instead of name.
//
// Declared in the header since Week 27: classifyJoinCondition uses it to spot a
// forward reference inside a residual ON conjunct of any shape. Two callers, one
// walker — a private copy would be an eleventh silent dispatch site.
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
    // An aggregate cannot appear in WHERE (Validator forbids it), but it CAN
    // appear inside an ON conjunct, where classifyJoinCondition uses this walker
    // to spot a forward reference. `SUM(c.val) > 1` on a join that has not
    // reached relation c must be refused for naming a later relation, not
    // accepted as a residual — and validateJoinCondition's own refusal a line
    // later masks the difference, so it cannot be relied on to cover this.
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        collectSlots(agg->argument.get(), out);   // nullptr-safe: COUNT(*)
        return;
    }
    // Literal / IntervalLiteral: no slot.
}

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

// The single relation slot a conjunct references, or -1 when it references none
// (constant), several (a cross-relation residual), or an unresolved ref.
//
// Week 26: this returns a SLOT, not a side. The old three-way FROM/JOIN/RESIDUAL
// enum collapsed "not slot 0" into JOIN, and pushIntoJoin attached those
// conjuncts to join->children[1]. With two relations that was exactly right;
// with three or more in a left-deep tree children[1] is one specific scan, so a
// conjunct belonging to a different relation was filtered against the wrong
// table — a wrong answer, not just lost pushdown.
int soleSlot(const Expr* conjunct) {
    std::unordered_set<int> slots;
    collectSlots(conjunct, slots);
    if (slots.size() == 1 && !slots.count(-1)) return *slots.begin();
    return -1;
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
        return;
    }
    // Unreachable today — no conjunct containing an aggregate survives to
    // pushdown (forbidden in WHERE by the Validator, refused inside ON by
    // validateJoinCondition) — but collectSlots now descends here, and the
    // lockstep above is only true if both do. A ref collected but not restamped
    // keeps its own relation's slot below a join whose scan schema stamps 0.
    if (auto* agg = dynamic_cast<AggregateExpr*>(expr)) {
        restampSlots(agg->argument.get(), slot);
    }
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
    return std::make_unique<LogicalFilter>(std::move(child), conjoinAll(std::move(conjuncts)));
}

// Attach each bucket to the subtree that owns its relation. The tree is
// left-deep: at a JOIN, children[1] is exactly relation `join_slot` and every
// lower slot lives in children[0]; the bottom of the spine is relation 0's scan.
std::unique_ptr<LogicalPlanNode> distribute(std::unique_ptr<LogicalPlanNode> node,
                                            std::map<int, std::vector<std::unique_ptr<Expr>>>& by_slot,
                                            const Catalog& catalog) {
    if (node->type == LogicalNodeType::JOIN) {
        auto* join = static_cast<LogicalJoin*>(node.get());

        // Week 29: children[1] of a LEFT join is the NULL-SUPPLYING relation, and
        // σ_p(R ⟕ S) is NOT σ_p(R) ⟕ σ_p(S). Filtering S first makes left rows
        // that HAD matches lose them, and the outer join then null-extends exactly
        // the rows the WHERE existed to remove: MORE rows, no error, and both
        // plans look reasonable in --explain. Leaving the bucket in by_slot is not
        // a leak — pushIntoJoin's leftover loop lifts it above the whole tree,
        // which is where WHERE semantics put it anyway ("degrade instead of drop",
        // as that loop already documents).
        //
        // The recursion into children[0] below stays UNCONDITIONAL: the preserved
        // side is safe (σ_p(R) ⟕ S ≡ σ_p(R ⟕ S)), and the test is re-applied at
        // every join on the way down, so a conjunct owned by a relation that some
        // deeper outer join null-supplies stops at that join.
        //
        // Written as `== INNER` rather than `!= LEFT` so a future RIGHT/FULL join
        // is refused by default rather than pushed through by omission.
        auto it = join->join_type == JoinType::INNER
                      ? by_slot.find(join->join_slot) : by_slot.end();
        if (it != by_slot.end()) {
            // Below the join these execute against the standalone scan, whose
            // schema stamps every column slot 0. Re-stamp so slot lookups hit
            // directly and ChunkPruner can act on the hint (it must keep
            // ignoring slot >= 1 refs — the FROM-side hint path routes
            // residual/un-pushed predicates containing join-side conjuncts to
            // the FROM scan, where shared column names would make name-based
            // pruning wrong). Residuals keep their slots.
            for (auto& c : it->second) restampSlots(c.get(), 0);
            join->children[1] = filterOnto(std::move(join->children[1]), std::move(it->second), catalog);
            by_slot.erase(it);
        }
        join->children[0] = distribute(std::move(join->children[0]), by_slot, catalog);
        return node;
    }

    // bottom of the left spine: relation 0's scan (already slot 0, no re-stamp)
    auto it = by_slot.find(0);
    if (it == by_slot.end()) return node;
    auto conjuncts = std::move(it->second);
    by_slot.erase(it);
    return filterOnto(std::move(node), std::move(conjuncts), catalog);
}

// Rewrite a WHERE filter sitting directly above a join tree.
std::unique_ptr<LogicalPlanNode> pushIntoJoin(std::unique_ptr<LogicalFilter> filter,
                                              const Catalog& catalog) {
    // steal the join tree out from under the filter
    auto join = std::unique_ptr<LogicalPlanNode>(filter->children[0].release());

    std::vector<std::unique_ptr<Expr>> conjuncts;
    splitConjuncts(std::move(filter->predicate), conjuncts);   // filter is now empty

    // std::map, not unordered_map: the leftover loop below must be deterministic
    std::map<int, std::vector<std::unique_ptr<Expr>>> by_slot;
    std::vector<std::unique_ptr<Expr>> residual;
    for (auto& c : conjuncts) {
        int slot = soleSlot(c.get());
        if (slot < 0) residual.push_back(std::move(c));
        else          by_slot[slot].push_back(std::move(c));
    }

    join = distribute(std::move(join), by_slot, catalog);

    // Nothing should be left: slots come from the same range table the tree was
    // built from. Anything unclaimed stays above the join — correct, just
    // slower — so a future tree shape degrades instead of dropping a predicate.
    for (auto& entry : by_slot)
        for (auto& c : entry.second) residual.push_back(std::move(c));

    // residuals (mixed / unresolved) stay above the join
    return filterOnto(std::move(join), std::move(residual), catalog);
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
        f->predicate = conjoinAll(std::move(parts));
        return node;
    }

    for (auto& child : node->children) child = apply(std::move(child), catalog);
    return node;
}
