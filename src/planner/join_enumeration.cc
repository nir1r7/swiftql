#include "join_enumeration.h"
#include "cardinality_estimator.h"
#include "cost_model.h"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace {

// One relation of the join graph: its post-pushdown subtree plus the numbers the
// search runs on. `slot` is the binder range-table position, which is the
// relation's identity everywhere — never its position on the spine.
struct Relation {
    int slot = -1;
    std::unique_ptr<LogicalPlanNode> subtree;   // Scan, or Filter(s) over Scan
    StatsContext ctx;                            // entries stamped `slot`
    std::string table;                           // captured before rebuild moves the subtree
    double rows = 0.0;
    double width = 0.0;                          // bytes/row
};

// One equi-join edge, direction-free. Direction in the written tree is an
// artifact of the written order, so rebuild() re-orients each edge for the order
// it actually picked.
struct Edge {
    int slot_a; std::string col_a;
    int slot_b; std::string col_b;
};

// walk down children[0] to the leaf scan's table name
const std::string& leafScanTableOf(const LogicalPlanNode* node) {
    while (node->type != LogicalNodeType::SCAN) node = node->children[0].get();
    return static_cast<const LogicalScan*>(node)->table_name;
}

// Number of relations under a node, counted by its scans. Used only to size the
// range table before decomposing.
int countRelations(const LogicalPlanNode* node) {
    if (node->type == LogicalNodeType::SCAN) return 1;
    int n = 0;
    for (const auto& child : node->children) n += countRelations(child.get());
    return n;
}

// Week 29. An outer join is not commutative (R ⟕ S ≠ S ⟕ R) and not freely
// associative, so an ordering that is sound for inner joins is a WRONG ANSWER
// here, not a merely expensive one. The DP's premise — any relation may be added
// to any subset in any order — is a LEGALITY claim, and repairing it needs
// per-edge conflict/eligibility sets (Moerkotte TES/SES), which is a different
// algorithm and buys nothing until a supported query has an outer join inside a
// reorderable block. Decline the whole tree instead, in the same shape as the
// <3-relation and >32-relation declines: return it untouched and print no
// order= line, because there was no decision to report.
bool containsOuterJoin(const LogicalPlanNode* node) {
    if (node->type == LogicalNodeType::JOIN &&
        static_cast<const LogicalJoin*>(node)->join_type != JoinType::INNER) {
        return true;
    }
    for (const auto& child : node->children) {
        if (containsOuterJoin(child.get())) return true;
    }
    return false;
}

// Week 30. Every slot this pass will use, checked against the range table
// BEFORE decompose() moves any subtree out — a decline discovered afterwards
// would have nothing clean to return, which is the same reason containsOuterJoin
// runs where it does.
//
// Two ways in. An UNBOUND key (from_slot -1, join_condition.h's
// positional-routing path for callers that skip the Binder) can never satisfy
// keysBetween's placed-set test, so the key would vanish from the rebuilt tree:
// a missing conjunct and therefore MORE rows if the join had a second key, or a
// spurious "produced a cross product" throw if it did not. And, from Week 34
// on, a SCAN THAT IS NOT A RANGE-TABLE ENTRY of the query being planned, because
// it belongs to a subquery — at which point `slot >= countRelations()` stops
// meaning "unbound key" and starts firing on legitimate plans. (Week 31 was
// expected to make that live and did not: a materialized subquery's scans form
// their own plan, with their own range table.)
//
// DECLINE, do not throw. Volcano and the written-order vectorized path never
// consult from_slot for placement, so a throw here is the one place the
// optimized path can fail on input `--no-optimize` accepts, and that
// equivalence is the differential oracle compare_against_sqlite.py's fourth mode
// exists to be. Same shape as containsOuterJoin's decline, and silent like the
// <3-relation and >32-relation ones: no ordering decision was available to
// report, so an `order=` line would claim a search that never ran.
bool hasSlotOutsideRangeTable(const LogicalPlanNode* node, int n) {
    if (node->type != LogicalNodeType::JOIN) return false;
    const auto* join = static_cast<const LogicalJoin*>(node);
    if (join->join_slot < 1 || join->join_slot >= n) return true;
    for (const JoinKey& k : join->keys) {
        if (k.from_slot < 0 || k.from_slot >= n) return true;
    }
    return hasSlotOutsideRangeTable(join->children[0].get(), n);
}

// Per-relation row width, the same rule VectorizedPlanBuilder's rowWidth uses on
// a single-relation input: real per-column avg_width, 8 bytes per column where
// statistics are absent.
double leafRowWidth(const LogicalPlanNode* leaf, const Catalog& catalog) {
    const std::string& table = leafScanTableOf(leaf);
    if (!catalog.hasStats(table)) return leaf->output_schema.size() * 8.0;
    const TableStats& ts = catalog.getStats(table);
    double width = 0.0;
    for (const ColumnDef& col : leaf->output_schema.columns()) {
        auto it = ts.columns.find(col.name);
        width += (it != ts.columns.end()) ? it->second.avg_width : 8.0;
    }
    return width;
}

// Walk the left spine of a WRITTEN-ORDER tree, moving each relation's subtree out
// and recording every key as an edge.
//
// Only ever called on the tree LogicalPlanBuilder::build produced, so
// joins[i] -> slot i+1 still holds: the leftmost relation is slot 0 and every
// from_slot is a true binder slot. That is what makes the decomposition
// unambiguous — a reordered tree's bottom join carries from_slot 0 for whatever
// relation sits there, and would not be decomposable this way.
//
// `leaves` is indexed BY SLOT, not by spine position. That is the entire point.
void decompose(std::unique_ptr<LogicalPlanNode> node,
               std::vector<std::unique_ptr<LogicalPlanNode>>& leaves,
               std::vector<Edge>& edges) {
    if (node->type != LogicalNodeType::JOIN) {
        leaves[0] = std::move(node);   // bottom of the spine == relation 0
        return;
    }
    auto* join = static_cast<LogicalJoin*>(node.get());
    for (const JoinKey& k : join->keys)
        edges.push_back(Edge{k.from_slot, k.from_col, join->join_slot, k.join_col});
    if (join->join_slot < 1 || join->join_slot >= static_cast<int>(leaves.size()))
        throw std::runtime_error(
            "internal: join enumeration saw a relation slot outside the range table");
    leaves[join->join_slot] = std::move(join->children[1]);
    decompose(std::move(join->children[0]), leaves, edges);
}

// Fold `order` into a left-deep tree, re-deriving each join's keys and merged
// schema for the order actually chosen. Mirrors LogicalPlanBuilder::build's fold,
// generalized off the written order.
std::unique_ptr<LogicalPlanNode> rebuild(const std::vector<int>& order,
                                         std::vector<Relation>& rels,
                                         const std::vector<Edge>& edges) {
    std::unordered_set<int> placed{order[0]};
    std::unique_ptr<LogicalPlanNode> node = std::move(rels[order[0]].subtree);

    // The leftmost leaf's OWN schema stamps every column slot 0: a standalone
    // scan has one relation and nothing to disambiguate, its pushed filter's refs
    // were restamped to 0 by distribute(), and ChunkPruner reads slot < 1 as
    // scan-local. The MERGED schema is where a relation acquires its binder slot,
    // which is what lets references from above (SELECT, residual WHERE, GROUP BY,
    // later joins' keys) resolve slot-first. In written order the leftmost
    // relation IS slot 0, which is the only reason this loop has never existed in
    // LogicalPlanBuilder::build.
    std::vector<ColumnDef> merged = node->output_schema.columns();
    for (ColumnDef& c : merged) c.relation_slot = order[0];

    for (size_t k = 1; k < order.size(); ++k) {
        const int r = order[k];

        // Every edge incident to r whose other end is already placed — and
        // exactly those. Each edge is therefore consumed once, at the step its
        // later-in-order endpoint arrives, which is what makes the reordered tree
        // compute the same relation as the written one.
        std::vector<JoinKey> keys;
        for (const Edge& e : edges) {
            if (e.slot_b == r && placed.count(e.slot_a))
                keys.push_back(JoinKey{e.col_a, e.col_b, e.slot_a});
            else if (e.slot_a == r && placed.count(e.slot_b))
                keys.push_back(JoinKey{e.col_b, e.col_a, e.slot_b});
        }
        // Connectivity is the search's job; a keyless LogicalJoin is a cross
        // product, and SwiftQL has no operator to run one on.
        if (keys.empty())
            throw std::runtime_error(
                "internal: join enumeration produced a cross product at relation slot "
                + std::to_string(r));

        // FIRST join only: the left input is a LEAF, whose own schema stamps slot
        // 0. leftKeyIndices() resolves from_col against that schema and THROWS on
        // a miss (Week 27, deliberately — the bare-name fallback IS the bug).
        // Slot 0 is unambiguous there: exactly one relation is present. Every
        // later join's left input is the merged schema below, where binder slots
        // are real. Same rule LogicalJoin::explain() and joinCardinality() read;
        // see the JoinKey contract in join_condition.h.
        if (k == 1) {
            for (JoinKey& key : keys) key.from_slot = 0;
        }

        // merged schema: [left block] ++ [this relation's columns, stamped r].
        // Order matches VecHashJoinNode's two contiguous output blocks — a
        // slot-sorted "canonical" order is not available (invariant 1). By-value
        // loop var: a reference would mutate the join scan's own schema and
        // destroy its slot-0 stamping. Copying whole ColumnDefs also preserves
        // `hidden`.
        for (ColumnDef c : rels[r].subtree->output_schema.columns()) {
            c.relation_slot = r;
            merged.push_back(c);
        }
        node = std::make_unique<LogicalJoin>(std::move(node),
                                             std::move(rels[r].subtree),
                                             std::move(keys), r, Schema(merged));
        placed.insert(r);
    }
    return node;
}

// ── the search ───────────────────────────────────────────────────────────────

// best[S]: the cheapest left-deep plan joining exactly the relations in S
struct Sub {
    double cost = std::numeric_limits<double>::infinity();
    double rows = 0.0;
    double width = 0.0;
    int last = -1;          // relation added last
    uint32_t prev = 0;      // predecessor subset; walk back for the order
};

struct Step { double cost = 0.0; double rows = 0.0; double width = 0.0; };

// Statistics visible above a subset: the concatenation of its relations' leaf
// contexts, each already stamped with its own binder slot. Cheap enough to
// rebuild per transition at N <= MAX_DP_RELATIONS; caching it would be a
// micro-optimization with no measurement behind it.
StatsContext contextFor(uint32_t s, const std::vector<Relation>& rels) {
    StatsContext ctx;
    for (const Relation& rel : rels) {
        if (!(s & (1u << rel.slot))) continue;
        ctx.entries.insert(ctx.entries.end(), rel.ctx.entries.begin(), rel.ctx.entries.end());
    }
    return ctx;
}

// Edges between relation r and the placed set, oriented left = placed. BINDER
// slots throughout: the search is pure arithmetic and never touches a physical
// schema, so rebuild()'s first-join from_slot = 0 rewrite is its concern alone.
std::vector<JoinKey> keysBetween(uint32_t placed, int r, const std::vector<Edge>& edges) {
    std::vector<JoinKey> keys;
    for (const Edge& e : edges) {
        if (e.slot_b == r && (placed & (1u << e.slot_a)))
            keys.push_back(JoinKey{e.col_a, e.col_b, e.slot_a});
        else if (e.slot_a == r && (placed & (1u << e.slot_b)))
            keys.push_back(JoinKey{e.col_b, e.col_a, e.slot_b});
    }
    return keys;
}

// One transition's cost and output shape. The build side is chosen by min(),
// which is what VectorizedPlanBuilder does at lowering — so an ordering is not
// penalised for a build-side choice lowering would never make. The SIMD loop
// join is deliberately not costed; see the header's approximation note.
Step stepCost(const Sub& s, uint32_t placed, const Relation& rel,
              const std::vector<Edge>& edges, const std::vector<Relation>& rels) {
    std::vector<JoinKey> keys = keysBetween(placed, rel.slot, edges);
    StatsContext left = contextFor(placed, rels);
    double rows  = joinCardinality(s.rows, rel.rows, keys, left, rel.ctx);
    double width = s.width + rel.width;
    double join_cost = std::min(hashJoinCost(s.rows, s.width, rel.rows),
                                hashJoinCost(rel.rows, rel.width, s.rows));
    return Step{s.cost + join_cost + joinOutputCost(rows, width), rows, width};
}

// Left-deep DP over subsets. Only CONNECTED extensions are legal: SwiftQL has no
// cross-product operator, so a disconnected step is unbuildable, not merely
// expensive. The original graph is connected by construction — every ON clause
// yields at least one key to a preceding relation — so an order always exists.
std::vector<int> enumerateDP(const std::vector<Relation>& rels,
                             const std::vector<Edge>& edges,
                             const std::vector<uint32_t>& adj) {
    const int n = static_cast<int>(rels.size());
    std::vector<Sub> best(static_cast<size_t>(1u) << n);
    for (int r = 0; r < n; ++r) {
        Sub& b = best[1u << r];
        b.cost = 0.0;                    // the scan happens under every ordering
        b.rows = rels[r].rows;
        b.width = rels[r].width;
        b.last = r;
    }

    for (uint32_t s = 1; s < (1u << n); ++s) {
        if (best[s].last < 0) continue;                // unreachable (disconnected) subset
        for (int r = 0; r < n; ++r) {                  // ascending: deterministic ties
            if (s & (1u << r)) continue;
            if (!(adj[r] & s)) continue;               // would be a cross product
            Step step = stepCost(best[s], s, rels[r], edges, rels);
            Sub& target = best[s | (1u << r)];
            if (step.cost < target.cost) {             // strict: first found wins ties
                target = Sub{step.cost, step.rows, step.width, r, s};
            }
        }
    }

    std::vector<int> order;
    for (uint32_t s = (1u << n) - 1; ;) {
        order.push_back(best[s].last);
        uint32_t p = best[s].prev;
        if (p == 0) break;                             // singleton reached
        s = p;
    }
    std::reverse(order.begin(), order.end());
    return order;
}

// O(N^2) fallback above MAX_DP_RELATIONS. Never optimal, always connected,
// always legal — which is the only guarantee rebuild() needs. Starting from the
// smallest relation is a heuristic, not a proof: a small relation on the
// periphery of a chain can force a poor second step. Acceptable for a fallback
// no TPC-H query reaches; do not "improve" it without a measurement.
std::vector<int> enumerateGreedy(const std::vector<Relation>& rels,
                                 const std::vector<Edge>& edges,
                                 const std::vector<uint32_t>& adj) {
    const int n = static_cast<int>(rels.size());
    int start = 0;
    for (int r = 1; r < n; ++r) {
        if (rels[r].rows < rels[start].rows) start = r;
    }

    std::vector<int> order{start};
    uint32_t placed = 1u << start;
    Sub cur{0.0, rels[start].rows, rels[start].width, start, 0};

    while (static_cast<int>(order.size()) < n) {
        int pick = -1;
        Step best_step{};
        for (int r = 0; r < n; ++r) {
            if (placed & (1u << r)) continue;
            if (!(adj[r] & placed)) continue;
            Step step = stepCost(cur, placed, rels[r], edges, rels);
            if (pick < 0 || step.cost < best_step.cost) { pick = r; best_step = step; }
        }
        if (pick < 0) break;   // disconnected graph: impossible by construction
        order.push_back(pick);
        placed |= 1u << pick;
        cur = Sub{best_step.cost, best_step.rows, best_step.width, pick, placed};
    }
    return order;
}

// Cumulative cost of one complete left-deep order, through the same transition
// function the search uses. Costing the WRITTEN order too is what makes the
// printed decision evidence rather than decoration.
double orderCost(const std::vector<int>& order, const std::vector<Relation>& rels,
                 const std::vector<Edge>& edges) {
    uint32_t placed = 1u << order[0];
    Sub cur{0.0, rels[order[0]].rows, rels[order[0]].width, order[0], placed};
    for (size_t k = 1; k < order.size(); ++k) {
        Step step = stepCost(cur, placed, rels[order[k]], edges, rels);
        placed |= 1u << order[k];
        cur = Sub{step.cost, step.rows, step.width, order[k], placed};
    }
    return cur.cost;
}

// `table@slot` — the same disambiguation idiom qualifyIfAmbiguous uses for
// columns. Two relations can share a table name (a self-join), and an order that
// cannot name its relations apart is not an auditable decision.
std::string renderOrder(const std::vector<int>& order, const std::vector<Relation>& rels) {
    std::string s;
    for (size_t k = 0; k < order.size(); ++k) {
        if (k) s += ",";
        s += rels[order[k]].table + "@" + std::to_string(order[k]);
    }
    return s;
}

// Reorder one join tree. Returns it unchanged below MIN_ENUMERATED_RELATIONS.
std::unique_ptr<LogicalPlanNode> reorder(std::unique_ptr<LogicalPlanNode> node,
                                         const Catalog& catalog) {
    const int n = countRelations(node.get());
    if (n < MIN_ENUMERATED_RELATIONS) return node;
    if (n > 32) return node;   // uint32_t subset masks; unreachable in practice
    // Week 29 — BEFORE decompose(), which moves subtrees out of the tree: a
    // decline discovered afterwards would have nothing clean to return.
    //
    // Unlike the two declines above, this one is REPORTED. Below three relations
    // there is no decision to make and above 32 the pass does not exist, so
    // silence is honest there; here a decision was available and was refused, and
    // the cost is real — one outer join switches ordering off for the query's
    // fully inner block too, which the README records as deliberate. A declined
    // tree printing nothing at all made that loss invisible on the one surface a
    // reader consults. It is deliberately NOT spelled `order=`: no order was
    // chosen, and the token `--explain` readers and the harness grep for must keep
    // meaning "the search ran".
    if (containsOuterJoin(node.get())) {
        static_cast<LogicalJoin*>(node.get())->order_decision =
            "join-ordering=skipped (outer join)";
        return node;
    }
    // Week 30 — also BEFORE decompose(), and silent. See the function.
    if (hasSlotOutsideRangeTable(node.get(), n)) return node;

    std::vector<std::unique_ptr<LogicalPlanNode>> leaves(n);
    std::vector<Edge> edges;
    decompose(std::move(node), leaves, edges);

    std::vector<Relation> rels(n);
    for (int r = 0; r < n; ++r) {
        if (!leaves[r]) throw std::runtime_error(
            "internal: join enumeration found no relation at range-table slot "
            + std::to_string(r));
        rels[r].slot = r;
        rels[r].table = leafScanTableOf(leaves[r].get());
        // Estimate each leaf in isolation, through the same function the final
        // whole-tree pass uses, so a leaf costed here and the same leaf stamped
        // later cannot disagree. Post-pushdown a leaf carries its own filters, so
        // `rows` is the FILTERED cardinality — which is the number ordering has
        // to use.
        rels[r].ctx = CardinalityEstimator::estimateSubtree(*leaves[r], catalog);
        // The leaf's entries stamp slot 0 (a standalone scan's schema does); the
        // search works in binder numbering throughout, so restamp here.
        for (ColumnStatsEntry& e : rels[r].ctx.entries) e.relation_slot = r;
        rels[r].rows = std::max(leaves[r]->estimated_rows, 0.0);
        rels[r].width = leafRowWidth(leaves[r].get(), catalog);
        rels[r].subtree = std::move(leaves[r]);
    }

    std::vector<uint32_t> adj(n, 0u);
    for (const Edge& e : edges) {
        // DEAD since Week 30: hasSlotOutsideRangeTable declines the whole tree
        // before decompose() runs, which is the only point at which returning
        // it untouched is still possible. Kept as the invariant it always was —
        // an endpoint outside the range table means the rebuilt tree would drop
        // a key, and a dropped key is MORE rows, not an error — so that
        // reordering these two passes fails loudly instead of silently.
        if (e.slot_a < 0 || e.slot_a >= n || e.slot_b < 0 || e.slot_b >= n) {
            throw std::runtime_error(
                "internal: join enumeration cannot reorder a tree with an unbound "
                "join key (relation slot out of range)");
        }
        adj[e.slot_a] |= 1u << e.slot_b;
        adj[e.slot_b] |= 1u << e.slot_a;
    }

    const bool use_dp = n <= MAX_DP_RELATIONS;
    std::vector<int> order = use_dp ? enumerateDP(rels, edges, adj)
                                    : enumerateGreedy(rels, edges, adj);

    // The written order is always legal and always in the search space, so it is
    // both the fallback and the bound.
    std::vector<int> written(n);
    for (int r = 0; r < n; ++r) written[r] = r;

    // A search that could not cover every relation (only reachable from a
    // disconnected graph, which classifyJoinCondition makes impossible) falls
    // back to the written order rather than dropping a relation.
    bool searched = static_cast<int>(order.size()) == n;
    if (!searched) order = written;

    double chosen_cost = orderCost(order, rels, edges);
    const double written_cost = orderCost(written, rels, edges);

    // Never install an order this cost model scores WORSE than the one the user
    // wrote. The written order is inside the search space, so a sound search
    // could not return something strictly worse — but soundness rests on
    // rows(S) being path-independent, which joinCardinality's no-statistics
    // max() branch breaks (and which the ≥1 floor broke until it moved to the
    // stamping sites). Rather than trust every future estimate to keep that
    // property, bound the outcome here: the search may only improve on the
    // written order. This also makes the printed `cost=… (written=…)` pair
    // self-consistent by construction, which is what makes it evidence rather
    // than decoration.
    bool kept_written = false;
    if (written_cost < chosen_cost) {
        order = written;
        chosen_cost = written_cost;
        kept_written = true;
    }

    std::unique_ptr<LogicalPlanNode> root = rebuild(order, rels, edges);

    // The checkpoint surface. Costs are unitless (cost_model.h) — never append a
    // time unit. `written=` is the comparand that makes this a decision rather
    // than a number, and `method=` must name what actually produced the printed
    // order — reporting `dp` for an order the DP did not choose would make the
    // one field a reader uses to trust the decision the one field that lies.
    const char* method = !searched ? "written-fallback"
                       : kept_written ? "written-floor"
                       : use_dp ? "dp" : "greedy";
    std::ostringstream d;
    d << std::fixed << std::setprecision(0)
      << "order=" << renderOrder(order, rels)
      << " cost=" << chosen_cost
      << " (written=" << written_cost << ")"
      << " method=" << method;
    static_cast<LogicalJoin*>(root.get())->order_decision = d.str();
    return root;
}

} // namespace

std::unique_ptr<LogicalPlanNode> JoinEnumeration::apply(std::unique_ptr<LogicalPlanNode> node,
                                                        const Catalog& catalog) {
    // The topmost JOIN is the root of the whole join tree — there is exactly one
    // per statement until subqueries arrive in Week 30 — so this replaces at most
    // once and never descends into a tree it has already reordered.
    if (node->type == LogicalNodeType::JOIN) return reorder(std::move(node), catalog);
    for (auto& child : node->children) child = apply(std::move(child), catalog);
    return node;
}
