#include "vectorized_plan_builder.h"
#include "execution/vec_scan_node.h"
#include "execution/vec_filter_node.h"
#include "execution/vec_project_node.h"
#include "execution/vec_limit_node.h"
#include "execution/vec_sort_node.h"
#include "execution/vec_distinct_node.h"
#include "execution/vec_hash_aggregate_node.h"
#include "execution/vec_hash_join_node.h"
#include "execution/vec_simd_loop_join_node.h"
#include "planner/cost_model.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace {

// per-build lowering state: the table map plus a remaining-use count per
// table, so a self-join (two LogicalScans, one map entry) copies the table
// for every scan except the last, which moves it
struct Lowering {
    std::unordered_map<std::string, ColumnarTable>& tables;
    std::unordered_map<std::string, int> scan_uses;
    const Catalog& catalog;   // borrowed, read-only: build-side width stats

    std::unique_ptr<VecPlanNode> lower(LogicalPlanNode* node, const Expr* pruning_where);
    std::unique_ptr<VecPlanNode> lowerNode(LogicalPlanNode* node, const Expr* pruning_where);
};

// count how many LogicalScans read each table (pre-pass over the whole tree)
void countScans(const LogicalPlanNode* node, std::unordered_map<std::string, int>& uses) {
    if (node->type == LogicalNodeType::SCAN) {
        ++uses[static_cast<const LogicalScan*>(node)->table_name];
    }
    for (const auto& child : node->children) {
        countScans(child.get(), uses);
    }
}

// walk down children[0] to the leaf scan's table name — used to read row
// counts for the build-side decision before lowering moves the tables
const std::string& leafScanTable(const LogicalPlanNode* node) {
    while (node->type != LogicalNodeType::SCAN) {
        node = node->children[0].get();
    }
    return static_cast<const LogicalScan*>(node)->table_name;
}

// True when this join input is exactly one relation: a scan, possibly under
// filters. From three relations on, children[0] can be a whole join subtree
// whose merged schema spans several tables — and then neither leafScanTable()
// nor any single TableStats describes it.
bool isSingleRelation(const LogicalPlanNode* node) {
    while (node->type != LogicalNodeType::SCAN) {
        if (node->type == LogicalNodeType::JOIN) return false;
        node = node->children[0].get();
    }
    return true;
}

// Resolve the LEFT input's key columns to physical column indices.
//
// A merged left schema can hold `team` at slot 0 AND slot 1 (laps.team,
// drivers.team), so a bare-name lookup here is a coin flip that returns
// plausible rows rather than an error. JoinKey::from_slot carries the binder
// slot of the left operand for exactly this, and honouring it only when it
// happens to hit would make the disambiguation advisory — so a miss throws
// instead of falling back to the bare-name overload, which is the bug this
// guards against.
std::vector<int> leftKeyIndices(const Schema& left_schema, const std::vector<JoinKey>& keys) {
    std::vector<int> idx;
    idx.reserve(keys.size());
    for (const JoinKey& k : keys) {
        // slot -1 = an unbound key from a validator-only caller, which has no
        // relation identity to be exact about; that path's documented fallback
        // is bare-name, and it is NOT a fallback for a miss on a bound key.
        int i = (k.from_slot >= 0) ? left_schema.indexOf(k.from_col, k.from_slot)
                                   : left_schema.indexOf(k.from_col);
        if (i < 0) {
            throw std::runtime_error(
                "join key '" + k.from_col + "' (relation slot "
                + std::to_string(k.from_slot) + ") not found on the left join input");
        }
        idx.push_back(i);
    }
    return idx;
}

// Resolve the RIGHT input's key columns. children[1] is always exactly one
// relation (left-deep; Week 28's DP keeps that shape), and a standalone scan's
// schema stamps every column slot 0 — the join_slot stamp lives only on the
// MERGED schema — so the bare-name overload is both unambiguous and the only
// one that resolves here.
std::vector<int> rightKeyIndices(const Schema& right_schema, const std::vector<JoinKey>& keys) {
    std::vector<int> idx;
    idx.reserve(keys.size());
    for (const JoinKey& k : keys) {
        int i = right_schema.indexOf(k.join_col);
        if (i < 0) {
            throw std::runtime_error(
                "join key '" + k.join_col + "' not found on the joined relation");
        }
        idx.push_back(i);
    }
    return idx;
}

// slot -> table for every relation in a join subtree. children[1] of each join
// IS relation join_slot; the leftmost block's slot is stamped on the merged
// schema's first column, which is the only place it is recorded once join
// enumeration (Week 28) may put a relation other than 0 at the bottom of the
// spine.
void collectSlotTables(const LogicalPlanNode* node,
                       std::unordered_map<int, std::string>& out) {
    if (node->type != LogicalNodeType::JOIN) return;
    const auto* join = static_cast<const LogicalJoin*>(node);
    out[join->join_slot] = leafScanTable(join->children[1].get());
    const LogicalPlanNode* left = join->children[0].get();
    if (isSingleRelation(left)) {
        // A join always carries at least its own key columns per side, so the
        // merged schema cannot be empty — but read defensively: an empty schema
        // here would index out of bounds rather than lose a width.
        if (join->output_schema.size() > 0)
            out[join->output_schema.column(0).relation_slot] = leafScanTable(left);
        return;
    }
    collectSlotTables(left, out);
}

// estimated bytes per row on one join input, for the hash-table memory cost.
// Sums the real per-column avg_width (Week 19 stats) over the input's output
// columns; a filter-over-scan child shares its scan's schema, all from one
// table. Falls back to 8 bytes/column when stats are absent (e.g. unit tests
// that don't seed them) — the same proxy the pre-Gap-3 code always used.
double rowWidth(const LogicalPlanNode* child, const Catalog& catalog) {
    if (isSingleRelation(child)) {
        const std::string& table = leafScanTable(child);
        if (!catalog.hasStats(table)) return child->output_schema.size() * 8.0;
        const TableStats& ts = catalog.getStats(table);
        double width = 0.0;
        for (const auto& col : child->output_schema.columns()) {
            auto it = ts.columns.find(col.name);
            width += (it != ts.columns.end()) ? it->second.avg_width : 8.0;
        }
        return width;
    }

    // Week 28: multi-relation input. Every column of a merged join schema
    // carries the binder slot of the relation it came from, so each avg_width
    // comes from ITS OWN table instead of relation 0's — the attribution error
    // Week 27 refused to make (leafScanTable() names relation 0 for a whole
    // subtree, so a shared column name like laps.team / drivers.team took the
    // wrong table's width) and the reason it fell back to columns * 8.0. That
    // fallback is now the thing being measured: differing intermediate widths
    // across orderings are what the data-volume term discriminates on, so the
    // real per-relation sum lands with the enumeration that consumes it.
    //
    // The fallback stays PER COLUMN. A subtree where only one relation has
    // stats must charge real widths for the columns it knows and 8.0 for the
    // rest, rather than throwing away what it has.
    std::unordered_map<int, std::string> slot_tables;
    collectSlotTables(child, slot_tables);
    double width = 0.0;
    for (const ColumnDef& col : child->output_schema.columns()) {
        auto t = slot_tables.find(col.relation_slot);
        if (t == slot_tables.end() || !catalog.hasStats(t->second)) {
            width += 8.0;
            continue;
        }
        const TableStats& ts = catalog.getStats(t->second);
        auto it = ts.columns.find(col.name);
        width += (it != ts.columns.end()) ? it->second.avg_width : 8.0;
    }
    return width;
}

// Week 23: every physical node inherits its logical counterpart's estimate so
// EXPLAIN ANALYZE can print est= next to rows_out. The wrapper stamps once for
// all eight cases (the JOIN case has two returns); -1 stays -1 under --no-optimize.
std::unique_ptr<VecPlanNode> Lowering::lower(LogicalPlanNode* node, const Expr* pruning_where) {
    std::unique_ptr<VecPlanNode> phys = lowerNode(node, pruning_where);
    phys->estimated_rows = node->estimated_rows;
    return phys;
}

std::unique_ptr<VecPlanNode> Lowering::lowerNode(LogicalPlanNode* node, const Expr* pruning_where) {
    switch (node->type) {
        case LogicalNodeType::SCAN: {
            auto* scan = static_cast<LogicalScan*>(node);
            // last scan of this table moves the data; earlier ones (self-join)
            // copy — same one-extra-copy tradeoff as Planner::plan
            ColumnarTable table = (--scan_uses.at(scan->table_name) > 0)
                ? tables.at(scan->table_name)
                : std::move(tables.at(scan->table_name));
            return std::make_unique<VecScanNode>(
                scan->table_name, std::move(table), scan->output_schema, pruning_where);
        }

        case LogicalNodeType::JOIN: {
            auto* join = static_cast<LogicalJoin*>(node);

            // Week 22: choose the build side from filtered cardinality estimates.
            // Post-pushdown, a child may be a LogicalFilter, so its estimated_rows
            // is the count *after* the WHERE — which can invert the raw table-size
            // ordering (a big filtered table can become the smaller input). Under
            // --no-optimize the estimator never ran (estimated_rows == -1), so fall
            // back to raw table sizes AND uniform per-row widths: the fallback
            // reproduces the pre-Week-22 pure row-count heuristic exactly (the
            // width term is a Week 22 cost-model input and must not leak into
            // the benchmark baseline; any equal constant cancels in the
            // comparison, so ties keep the deterministic join-side build).
            double from_est = join->children[0]->estimated_rows;
            double join_est = join->children[1]->estimated_rows;
            bool estimate_driven = from_est >= 0 && join_est >= 0;
            double from_w = 8.0, join_w = 8.0;
            if (estimate_driven) {
                // per-side row width from real avg_width stats (Gap 3); this
                // feeds the hash-table memory term, so with equal row counts
                // the narrower input becomes the cheaper build side
                from_w = rowWidth(join->children[0].get(), catalog);
                join_w = rowWidth(join->children[1].get(), catalog);
            } else {
                from_est = tables.at(leafScanTable(join->children[0].get())).num_rows;
                join_est = tables.at(leafScanTable(join->children[1].get())).num_rows;
            }

            // Pruning hint routes to the FROM side only — and only while that
            // side's leaf scan is relation 0.
            //
            // ChunkPruner treats a relation_slot < 1 ref in a scan hint as
            // scan-local (chunk_pruner.h). That is true of the leftmost scan only
            // while the leftmost relation IS slot 0 — which join enumeration
            // (Week 28) no longer guarantees. With relation 2 at the bottom of
            // the spine, a slot-0 ref in the hint would prune relation 2's chunks
            // on relation 0's value: rows vanish, no error.
            //
            // Unreachable today — post-pushdown the residual above a join holds
            // only multi-relation, OR or constant conjuncts, none of which
            // collectSimplePredicates accepts — but the reason it is unreachable
            // is exactly the invariant being deleted. Withhold the hint instead
            // of depending on it; it costs nothing, since the hint carries
            // nothing prunable in the case where it is withheld. The per-scan
            // hints pushdown created are unaffected: their refs were restamped to
            // 0 by distribute() and they sit directly above their own scan.
            //
            // Why testing the LEFTMOST relation is sufficient and not merely
            // necessary — the hint only ever descends children[0], so it reaches
            // exactly the leftmost leaf; every join down the spine re-evaluates
            // this same test against its own merged schema, so the decision is
            // consistent all the way down; and a pushed filter sitting above that
            // leaf discards the incoming hint and substitutes its own predicate
            // (the FILTER case below). So the only refs that can prune are those
            // in a hint that reached relation 0's own scan, which is where they
            // belong. Note this deliberately tests `== 0` while ChunkPruner
            // accepts `relation_slot < 1`: a -1 (unbound) ref is still safe under
            // this guard, because the hint only survives when the leftmost leaf IS
            // relation 0. Loosening the test to `<= 0` would let a hint through
            // for a leftmost stamp nobody can reason about — the wrong direction.
            //
            // Two preconditions this rests on, neither of them local:
            //   1. every ColumnRef reaching a hint carries a real binder slot
            //      (binder.cc stamps them; Validator rejects anything left at -1);
            //   2. a post-pushdown residual holds no `ColumnRef op Literal`
            //      conjunct, because soleSlot() routed every single-slot conjunct
            //      to its own relation (predicate_pushdown.cc).
            // If either stops holding, this guard is the thing that still has to
            // be true, which is why it is tested directly rather than trusted.
            const bool leftmost_is_slot0 =
                join->output_schema.size() > 0 &&
                join->output_schema.column(0).relation_slot == 0;
            auto from_child = lower(join->children[0].get(),
                                    leftmost_is_slot0 ? pruning_where : nullptr);
            auto join_child = lower(join->children[1].get(), nullptr);

            // Week 27: arbitrary key counts, and children[0] may itself be a
            // JOIN — the recursion above handles a left-deep tree of any depth
            // with no extra machinery. Resolve both sides' key columns to
            // physical indices ONCE, here, where the binder slots are still in
            // scope; the operators then index without ever resolving a name.
            //
            // Resolved against the PHYSICAL children, not the logical nodes.
            // They agree today — every lowering case forwards or copies its
            // logical schema — but these indices are what the operator will feed
            // to chunk->columns[i], which is unchecked, so they must come from
            // the schema the operator actually sees. The size check makes a
            // future divergence a plan-time error rather than a wrong column.
            const Schema& from_schema = from_child->outputSchema();
            const Schema& jn_schema   = join_child->outputSchema();
            if (from_schema.size() != join->children[0]->output_schema.size() ||
                jn_schema.size() != join->children[1]->output_schema.size()) {
                throw std::runtime_error(
                    "VectorizedPlanBuilder: lowered join input does not match its logical schema");
            }
            std::vector<int> left_idx  = leftKeyIndices(from_schema, join->keys);
            std::vector<int> right_idx = rightKeyIndices(jn_schema, join->keys);

            // Week 22 (build side) + Week 23.5 (algorithm): cost every legal
            // (side, algorithm) assignment and take the cheapest jointly. With
            // the current constants each algorithm prefers the same side — the
            // side-preference deltas coincide because CPU_HASH_BUILD -
            // CPU_HASH_PROBE == CPU_LOOP_BUILD (the SIMD quadratic term is
            // symmetric under side swap and cancels) — but that is a property
            // of the tuned weights, not a structural guarantee, so all four are
            // costed to keep future recalibration safe. SIMD eligibility is
            // hard (INT keys only: ColumnVector carries decoded strings, and
            // DOUBLE bitwise equality is a trap) and gated on estimate_driven
            // so --no-optimize keeps the pre-Week-22 hash-only lowering as the
            // unchanged baseline.
            //
            // Week 27 adds a third eligibility term: SIMD holds build keys in
            // ONE flat int64 buffer, which a composite key cannot occupy.
            // Decline multi-key rather than invent an encoding — an ineligible
            // algorithm is simply not costed, and the hash join is always
            // correct.
            bool int_keys =
                join->keys.size() == 1 &&
                from_schema.column(left_idx[0]).type == TypeId::INT &&
                jn_schema.column(right_idx[0]).type == TypeId::INT;

            double cost_hash_from = hashJoinCost(from_est, from_w, join_est);
            double cost_hash_join = hashJoinCost(join_est, join_w, from_est);
            double cost_simd_from = simdLoopJoinCost(from_est, from_w, join_est);
            double cost_simd_join = simdLoopJoinCost(join_est, join_w, from_est);

            double best_hash = std::min(cost_hash_from, cost_hash_join);
            double best_simd = std::min(cost_simd_from, cost_simd_join);
            bool use_simd = estimate_driven && int_keys && best_simd < best_hash;
            bool from_builds = use_simd ? cost_simd_from < cost_simd_join
                                        : cost_hash_from < cost_hash_join;

            // Week 23: hand the costed decision to the node for explain output —
            // but only when cardinality estimates drove it. The raw-table-size
            // fallback is the pre-Week-22 heuristic, and printing cost= under
            // --no-optimize would claim an optimizer decision that never happened.
            // Costs are unitless (cost_model.h) — never append a time unit.
            std::string decision;
            if (estimate_driven) {
                // first clause: side decision within the winning algorithm;
                // second clause (INT keys only): the algorithm decision, with
                // the rejected algorithm's best cost
                double side_cost = use_simd ? (from_builds ? cost_simd_from : cost_simd_join)
                                            : (from_builds ? cost_hash_from : cost_hash_join);
                double side_alt  = use_simd ? (from_builds ? cost_simd_join : cost_simd_from)
                                            : (from_builds ? cost_hash_join : cost_hash_from);
                // Naming relation 0 for a whole join subtree would claim a build
                // side that is not the one chosen, so a multi-relation input
                // reports what it is instead of a table it only starts with.
                const LogicalPlanNode* build_side = join->children[from_builds ? 0 : 1].get();
                std::ostringstream d;
                d << std::fixed << std::setprecision(0)
                  << "build=" << (isSingleRelation(build_side)
                                      ? leafScanTable(build_side) : "join-subtree")
                  << " cost=" << side_cost << " (alt=" << side_alt << ")";
                if (int_keys) {
                    d << " algo=" << (use_simd ? "simd" : "hash")
                      << " (" << (use_simd ? "hash=" : "simd=")
                      << (use_simd ? best_hash : best_simd) << ")";
                }
                decision = d.str();
            }

            // output schema stays in fixed logical order [FROM, JOIN] regardless
            // of which side physically builds; swapped tells the join to reorder
            // columns when assembling output.
            // The key index vectors swap with the children: passing left_idx as
            // the probe side while passing join_child as the probe child pairs
            // a.x with b.y — a silent wrong answer, not a crash, since the two
            // vectors are usually the same length.
            if (use_simd) {
                std::unique_ptr<VecSimdLoopJoinNode> join_node = from_builds
                    // FROM builds: JOIN side probes (swapped)
                    ? std::make_unique<VecSimdLoopJoinNode>(
                          std::move(join_child), std::move(from_child),
                          right_idx[0], left_idx[0], join->output_schema, /*swapped=*/true)
                    : std::make_unique<VecSimdLoopJoinNode>(
                          std::move(from_child), std::move(join_child),
                          left_idx[0], right_idx[0], join->output_schema, /*swapped=*/false);
                join_node->setCostDecision(std::move(decision));
                return join_node;
            }
            std::unique_ptr<VecHashJoinNode> join_node = from_builds
                // FROM builds: JOIN side probes (swapped)
                ? std::make_unique<VecHashJoinNode>(
                      std::move(join_child), std::move(from_child),
                      right_idx, left_idx, join->output_schema, /*swapped=*/true)
                : std::make_unique<VecHashJoinNode>(
                      std::move(from_child), std::move(join_child),
                      left_idx, right_idx, join->output_schema, /*swapped=*/false);
            join_node->setCostDecision(std::move(decision));
            return join_node;
        }

        case LogicalNodeType::FILTER: {
            auto* filter = static_cast<LogicalFilter*>(node);
            // a filter directly above the scan/join is the WHERE: hand its
            // predicate down as the zone-map pruning hint. Aliasing is safe:
            // moving the unique_ptr below never relocates the Expr, and
            // VecFilterNode (an ancestor of the scan) owns it for the plan's
            // lifetime. A filter above an aggregate is the HAVING — no hint.
            LogicalNodeType child_type = filter->children[0]->type;
            bool is_where = child_type == LogicalNodeType::SCAN
                         || child_type == LogicalNodeType::JOIN;
            auto child = lower(filter->children[0].get(),
                               is_where ? filter->predicate.get() : nullptr);
            return std::make_unique<VecFilterNode>(std::move(child), std::move(filter->predicate));
        }

        case LogicalNodeType::AGGREGATE: {
            auto* agg = static_cast<LogicalAggregate*>(node);
            auto child = lower(agg->children[0].get(), nullptr);
            return std::make_unique<VecHashAggregateNode>(
                std::move(child), std::move(agg->group_by),
                std::move(agg->aggregates), agg->output_schema);
        }

        case LogicalNodeType::PROJECT: {
            auto* proj = static_cast<LogicalProject*>(node);
            auto child = lower(proj->children[0].get(), nullptr);
            return std::make_unique<VecProjectNode>(
                std::move(child), std::move(proj->exprs), proj->output_schema);
        }

        case LogicalNodeType::SORT: {
            auto* sort = static_cast<LogicalSort*>(node);
            auto child = lower(sort->children[0].get(), nullptr);
            return std::make_unique<VecSortNode>(std::move(child), std::move(sort->order_by));
        }

        case LogicalNodeType::DISTINCT: {
            auto child = lower(node->children[0].get(), nullptr);
            return std::make_unique<VecDistinctNode>(std::move(child));
        }

        case LogicalNodeType::LIMIT: {
            auto* limit = static_cast<LogicalLimit*>(node);
            auto child = lower(limit->children[0].get(), nullptr);
            return std::make_unique<VecLimitNode>(std::move(child), limit->limit);
        }
    }
    throw std::runtime_error("VectorizedPlanBuilder: unknown logical node type");
}

} // namespace

std::unique_ptr<VecPlanNode> VectorizedPlanBuilder::build(
        std::unique_ptr<LogicalPlanNode> logical,
        std::unordered_map<std::string, ColumnarTable> columnar_tables,
        const Catalog& catalog) {
    Lowering lowering{columnar_tables, {}, catalog};
    countScans(logical.get(), lowering.scan_uses);
    return lowering.lower(logical.get(), nullptr);
}
