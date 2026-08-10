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

// walk down children[0] to the leaf scan's table name.
//
// Week 34: NULLABLE, for the same reason vectorized_plan_builder.cc's twin is.
// A DERIVED relation has no catalog table and no TableStats, and walking through
// it returns the BODY's base table — so leafRowWidth would charge one table's
// per-column avg_width for another relation's columns, which is the attribution
// error Week 27 refused to make and which feeds the DP's cost directly.
const std::string* leafScanTableOfOrNull(const LogicalPlanNode* node) {
    while (node->type != LogicalNodeType::SCAN) {
        if (node->type == LogicalNodeType::DERIVED) return nullptr;
        if (node->children.empty()) return nullptr;
        node = node->children[0].get();
    }
    return &static_cast<const LogicalScan*>(node)->table_name;
}

// Number of RELATIONS of the block being planned — which is the size of its
// range table, and therefore the domain every join_slot and from_slot indexes.
//
// !! Week 34 corrected what this counts, and the correction is the one Weeks 28
// and 29 both wrote down in advance. It counted SCANS, recursively through every
// child, which equalled the range-table size only while every scan in the tree
// belonged to this block. Two constructs broke that, and in OPPOSITE directions:
//   - a DERIVED relation is ONE relation of this block whose body may hold any
//     number of scans, so counting scans OVER-counted and `slot >= n` stopped
//     meaning "outside the range table" — too permissive, letting through a tree
//     the pass cannot decompose;
//   - a SEMI/ANTI join's children[1] is a subquery BODY, not a relation of this
//     block at all, so its scans were counted as if they were.
// Counting the SPINE instead answers the question the callers actually ask.
int countRelations(const LogicalPlanNode* node) {
    if (node->type == LogicalNodeType::SCAN) return 1;
    // A derived relation is one relation of this block. Its body's scans are
    // relations of the BODY's range table, which this number does not describe.
    if (node->type == LogicalNodeType::DERIVED) return 1;
    if (node->type == LogicalNodeType::JOIN) {
        const auto* join = static_cast<const LogicalJoin*>(node);
        // children[1] of a semi/anti join is a body, not a range-table entry —
        // the same fact join_slot == -1 records. Count the left spine only.
        if (join->semantics != JoinSemantics::STANDARD)
            return countRelations(join->children[0].get());
        return countRelations(join->children[0].get())
             + countRelations(join->children[1].get());
    }
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
//
// !! WHAT IT MUST NOT LOOK AT — seam audit pass 2, B-2. It used to walk EVERY
// child, so it answered a question about a different query block. Its containment
// rule is now written the same way `countRelations` above writes it, because it
// is the SAME rule and having two spellings of it is how the two drifted:
//
//   * a DERIVED relation is an OPAQUE LEAF of this tree. `countRelations` counts
//     it as one relation, `decompose` moves it out whole and `rebuild` never
//     looks inside, so an outer join sealed in its BODY is never reordered by
//     this pass and cannot make an ordering illegal here. Recursing into it made
//     one `LEFT JOIN` inside a derived table switch ordering off for the
//     enclosing block's fully inner spine — the exact "an unrelated construct
//     costs the whole query its ordering" loss Week 29 added its decline string
//     for, in a shape the string then MISATTRIBUTED.
//   * a SEMI/ANTI join's children[1] is a subquery BODY, not a relation of this
//     block, for the same reason `countRelations` skips it. Walking into it let a
//     `LEFT JOIN` in the body report `join-ordering=skipped (outer join)` for a
//     tree whose real and stronger reason is `(semi/anti join)` — the decline
//     that `slotDeclineReason` would have named a few lines later. A decline
//     string that names the wrong cause is worse than none: it is the surface a
//     reader consults, and it sends them to the wrong construct.
//
// The node's OWN join_type is still decisive: a LEFT join anywhere on THIS
// spine is a tree this pass must not touch.
bool containsOuterJoin(const LogicalPlanNode* node) {
    if (node->type == LogicalNodeType::DERIVED) return false;
    if (node->type == LogicalNodeType::JOIN) {
        const auto* join = static_cast<const LogicalJoin*>(node);
        if (join->join_type != JoinType::INNER) return true;
        if (join->semantics != JoinSemantics::STANDARD)
            return containsOuterJoin(join->children[0].get());
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
// spurious "produced a cross product" throw if it did not. And a SEMI/ANTI join,
// whose join_slot is -1 because children[1] is a subquery BODY rather than a
// relation of this block (Week 32).
//
// !! DERIVED TABLES DO NOT REACH HERE, which four weeks predicted they would.
// Weeks 28-31 each recorded that "a scan that is not a range-table entry of the
// query being planned" would start firing this guard on legitimate plans from
// Week 34 on. What was actually wrong was countRelations, which counted SCANS
// and so over-counted a derived body's — making `slot >= n` too PERMISSIVE, not
// too strict. It counts the spine now, and a derived relation is an ordinary
// range-table entry with an in-range slot that the search reorders like any
// other. The prediction is recorded here rather than deleted because the FIX it
// asked for (re-derive n against the binder range table) was the right one; only
// its predicted symptom was wrong.
//
// DECLINE, do not throw. Volcano and the written-order vectorized path never
// consult from_slot for placement, so a throw here is the one place the
// optimized path can fail on input `--no-optimize` accepts, and that
// equivalence is the differential oracle compare_against_sqlite.py's fourth mode
// exists to be. Same shape as containsOuterJoin's decline.
//
// SEAM AUDIT (optimizer preservation, pass 1) MADE THE SEMI/ANTI CAUSE REPORTED.
// It used to be silent, on this file's own argument that "no ordering decision
// was available to report". That argument stopped being true once IN-lowering
// shipped: on `FROM a JOIN b JOIN c WHERE x IN (SELECT …)` the block below the
// semi join is a FULLY INNER three-relation tree the search could have reordered,
// and the whole query loses ordering because an unrelated subquery sits above it.
// Measured on the shipped catalog: the same spine without the IN gets
// `order=drivers@1,drivers@2,laps@0 cost=43104 (written=60637) method=dp`, and
// with the IN it gets nothing at all. That is exactly the loss Week 29 added its
// decline string for — "a supported query pays a real plan-quality cost".
//
// The reason is returned rather than a bool so the two causes stay
// distinguishable. The UNBOUND-KEY cause stays reported too but names itself
// differently; it is not reachable from the CLI (the Binder stamps every key),
// so it would be a decline string nobody can trigger if it shared the semi/anti
// wording — the dead-assertion failure this file records elsewhere.
//
// Returns nullptr when the tree is fine. The truth value is identical to the
// bool predicate it replaces, test for test.
const char* slotDeclineReason(const LogicalPlanNode* node, int n) {
    if (node->type != LogicalNodeType::JOIN) return nullptr;
    const auto* join = static_cast<const LogicalJoin*>(node);
    if (join->join_slot < 1 || join->join_slot >= n) {
        // A semi/anti join's join_slot is -1 BY CONTRACT (logical_plan.h), not by
        // accident, so this branch names the cause rather than the symptom.
        return join->semantics != JoinSemantics::STANDARD
             ? "semi/anti join" : "relation slot outside the range table";
    }
    for (const JoinKey& k : join->keys) {
        if (k.from_slot < 0 || k.from_slot >= n) {
            return "relation slot outside the range table";
        }
    }
    return slotDeclineReason(join->children[0].get(), n);
}

// Per-relation row width, the same rule VectorizedPlanBuilder's rowWidth uses on
// a single-relation input: real per-column avg_width, 8 bytes per column where
// statistics are absent.
double leafRowWidth(const LogicalPlanNode* leaf, const Catalog& catalog) {
    const std::string* table_p = leafScanTableOfOrNull(leaf);
    if (!table_p) return leaf->output_schema.size() * 8.0;   // Week 34: derived
    const std::string& table = *table_p;
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

// THE MERGE RULE for a left-deep join's output schema, in ONE place: the left
// input's columns verbatim, then the newly added relation's stamped with its
// binder slot. LogicalPlanBuilder::build writes it, `rebuild` below writes it
// generalized off the written order, and `refreshSchema` below has to write it a
// third time for a join whose left subtree this pass permuted. Two spellings of
// it are how they drift, and a schema that disagrees with the rows the operator
// actually emits is read as column IDENTITY by every consumer above it.
//
// By-value loop variable: a reference would mutate the right child's own schema
// and destroy its slot stamping. Copying whole ColumnDefs also preserves `hidden`.
void appendStamped(std::vector<ColumnDef>& merged, const Schema& right, int slot) {
    for (ColumnDef c : right.columns()) {
        c.relation_slot = slot;
        merged.push_back(c);
    }
}

// Column-sequence identity. Reordering PRESERVES a merged schema's
// (relation_slot, name) SET and PERMUTES its sequence, so this is exactly the
// property an ancestor's stored copy of a child schema can lose — and the test
// the refresh below is gated on.
bool sameColumnSequence(const Schema& a, const Schema& b) {
    if (a.size() != b.size()) return false;
    for (int i = 0; i < a.size(); ++i) {
        if (a.column(i).name != b.column(i).name) return false;
        if (a.column(i).relation_slot != b.column(i).relation_slot) return false;
    }
    return true;
}

// WEEK 38 — RE-DERIVE ONE NODE'S OUTPUT SCHEMA FROM ITS CHILDREN, for the node
// kinds that COPY a child's (FILTER / SORT / DISTINCT / LIMIT, and a SEMI/ANTI
// join, whose output schema is its PROBE input's by contract) or CONCATENATE
// over both (a STANDARD join). PROJECT / AGGREGATE / DERIVED / SCAN derive their
// own and are where the propagation stops.
//
// It exists because Week 38 let this pass reorder the spine BELOW a semi/anti
// join, and subquery_lowering.cc's decline comment named exactly this as the
// blocker: `rebuild` preserves the merged schema's (relation_slot, name) pair
// SET but PERMUTES its sequence, so every ancestor that stored a copy is stale
// afterwards — a wrong answer, not a lost optimization, since column POSITION is
// how the lowered operators above address their inputs.
//
// !! IT MUST NEVER RUN ON A JOIN `rebuild` PRODUCED. rebuild stamps the LEFTMOST
// LEAF's columns with order[0] in the merged schema while deliberately leaving
// the leaf's OWN schema stamping 0 (ChunkPruner reads slot < 1 as scan-local and
// leftKeyIndices resolves the first join's keys there), so re-deriving a rebuilt
// bottom join from its children hands back slot-0 columns where the merge put
// order[0]. The call sites guard that by refreshing ONLY when a child's schema
// actually changed, and nothing below a rebuilt spine can change:
// applyToSpineLeaves re-enters only leaves, and a leaf is a scan, a filter over
// one, or a derived relation whose body plan is topped by a PROJECT or AGGREGATE.
void refreshSchema(LogicalPlanNode* node) {
    switch (node->type) {
        case LogicalNodeType::JOIN: {
            auto* join = static_cast<LogicalJoin*>(node);
            if (join->semantics != JoinSemantics::STANDARD) {
                // THE CONTAINMENT (subquery_lowering.cc): a semi/anti join's
                // output schema is its left child's, never a merged one — which
                // is what keeps the body's slot numbering out of the outer plan.
                node->output_schema = join->children[0]->output_schema;
                return;
            }
            std::vector<ColumnDef> merged = join->children[0]->output_schema.columns();
            appendStamped(merged, join->children[1]->output_schema, join->join_slot);
            node->output_schema = Schema(merged);
            return;
        }
        case LogicalNodeType::FILTER:
        case LogicalNodeType::SORT:
        case LogicalNodeType::DISTINCT:
        case LogicalNodeType::LIMIT:
            node->output_schema = node->children[0]->output_schema;
            return;
        case LogicalNodeType::SCAN:
        case LogicalNodeType::DERIVED:
        case LogicalNodeType::AGGREGATE:
        case LogicalNodeType::PROJECT:
            return;   // self-derived: the propagation stops here
    }
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
        // slot-sorted "canonical" order is not available (invariant 1). The
        // append itself is `appendStamped` above, shared with refreshSchema so
        // the two cannot spell the merge differently.
        //
        // !! THIS IS THE LINE THE SORT TIE-BREAK USED TO READ AS DATA (seam audit
        // pass 3: engine E-9, join-chain B3-1, optimizer B3-1). The permutation
        // is deliberate and stays — it is the DP doing its job, and the two
        // output blocks genuinely must stay contiguous. What was wrong was a
        // CONSUMER that treated column POSITION as column IDENTITY:
        // `sort_comparator::rowLess` walked the row in schema index order when
        // the declared ORDER BY keys tied, so the two legs produced two different
        // total orders over the same rows and a LIMIT cut different ones.
        //
        // The distinction this comment did not draw, and now does: a canonical
        // MATERIALIZED order is not available, but a canonical COMPARISON order
        // is — the `c.relation_slot = r` stamp two lines below is the binder's
        // written-order slot, so `(relation_slot, name)` names a column
        // identically on every leg. `sort_comparator::tieBreakOrder` derives the
        // comparison order from exactly that. Any future consumer that needs a
        // plan-independent column ORDER must do the same; the schema's own
        // sequence is a physical detail of this fold.
        appendStamped(merged, rels[r].subtree->output_schema, r);
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
    // Week 30 — also BEFORE decompose(). REPORTED as of the phase 5 seam audit;
    // see the function for why the silence stopped being honest.
    //
    // !! WEEK 34 EXPECTED TO MAKE THIS FIRE FOR DERIVED TABLES AND IT DOES NOT,
    // which is worth recording because three prior weeks predicted otherwise
    // (Weeks 28, 29 and 30 all wrote the prediction down, and the README's Week
    // 34 notes name it as one of the three consumers that break silently). What
    // was actually wrong was `countRelations`, which counted SCANS and so
    // over-counted a derived body's — fix that and a derived relation is an
    // ordinary range-table entry with an in-range slot: `decompose` takes it as
    // a leaf, `rebuild` re-merges its schema, and the search reorders it like
    // any other relation. A reported decline was therefore NOT added: Week 30's
    // condition for earning one is "a supported query pays a real plan-quality
    // cost", and no query pays one here. A decline string that cannot fire is
    // the dead-assertion failure Week 33 recorded — it reads as a guarantee and
    // stops anyone looking.
    //
    // WHAT IS REAL: a derived relation has no TableStats, so it supplies no key
    // NDV of its own.
    //
    // !! THE PARAGRAPH THAT USED TO BE HERE WAS FALSE IN BOTH HALVES, and the
    // phase 5 seam audit measured both. It claimed `joinCardinality`'s
    // no-statistics branch `max(l, r)` runs on these queries, and that
    // `method=written-floor` was therefore reachable from the CLI. Neither
    // happens: `have_ndv` (cardinality_estimator.cc, inside joinCardinality) is
    // set when EITHER side supplies an NDV, and the non-derived side always does,
    // so the MULTIPLICATIVE branch runs and `max(l, r)` never does. And the DP
    // won outright on both queries tested (`method=dp`), because the derived
    // side's then-fabricated 0 rows made the reordered plan look strictly better
    // rather than merely path-dependent — the estimator had no DERIVED case, so
    // `rels[r].rows` below read `max(-1.0, 0.0)`.
    //
    // The DERIVED case now exists, so `rels[r].rows` is a real number here. The
    // remaining truth is narrower and worth keeping: a derived relation
    // contributes no NDV, so a subset containing one is priced from the other
    // side's statistics alone. `max(l, r)` still runs when NEITHER side has any —
    // it is not multiplicative, so such a subset has an order-dependent row count
    // and the DP's optimal substructure does not hold for it. The containment is
    // unchanged and is the written-order bound in reorder() below.
    if (const char* why = slotDeclineReason(node.get(), n)) {
        static_cast<LogicalJoin*>(node.get())->order_decision =
            std::string("join-ordering=skipped (") + why + ")";
        return node;
    }

    std::vector<std::unique_ptr<LogicalPlanNode>> leaves(n);
    std::vector<Edge> edges;
    decompose(std::move(node), leaves, edges);

    std::vector<Relation> rels(n);
    for (int r = 0; r < n; ++r) {
        if (!leaves[r]) throw std::runtime_error(
            "internal: join enumeration found no relation at range-table slot "
            + std::to_string(r));
        rels[r].slot = r;
        // Week 34: a DERIVED leaf names no catalog table. The empty string is
        // the honest answer and every consumer of `table` already tolerates a
        // name the catalog does not hold (hasStats returns false and the cost
        // falls back to the uniform proxy), which is Week 27's stance again.
        const std::string* leaf_table = leafScanTableOfOrNull(leaves[r].get());
        rels[r].table = leaf_table ? *leaf_table : std::string();
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

// Is `node` a link of the spine `reorder` just worked on — as opposed to a
// BOUNDARY that ends it? Week 38: a semi/anti join is a boundary. Its children[0]
// is a join tree of its own, still in WRITTEN order (nothing decomposed it), so
// handing it to `apply` is legal; its children[1] is a subquery body, which was
// always a separate tree.
bool isSpineLink(const LogicalPlanNode* node) {
    return node->type == LogicalNodeType::JOIN
        && static_cast<const LogicalJoin*>(node)->semantics == JoinSemantics::STANDARD;
}

// Re-enter every subtree hanging off a join spine that is NOT part of the spine:
// each relation leaf, a semi/anti join, and a semi/anti join's body. Called on a
// spine this pass has just finished with.
//
// It exists because `apply` must NOT recurse into its own result. `decompose`
// (above) is only decomposable on a WRITTEN-ORDER tree — its own comment says so:
// "a reordered tree's bottom join carries from_slot 0 for whatever relation sits
// there, and would not be decomposable this way". A plain `for (child) apply()`
// after `reorder` would hand the spine's inner joins straight back to
// `decompose`, so the descent has to step OVER the spine and into its leaves.
//
// WEEK 38 — IT STEPS OVER STANDARD JOINS ONLY. A semi/anti join reached down the
// children[0] chain (the `LogicalLeftJoin > LogicalSemiJoin > LogicalJoin` shape
// lowerCorrelatedScalars builds) is not part of this spine and was never
// decomposed, so it goes to `apply`, which enumerates the fully-inner tree
// underneath it. The refresh on the way back up is the other half; see
// refreshSchema for why it is gated on an actual change.
void applyToSpineLeaves(LogicalPlanNode* node, const Catalog& catalog) {
    auto* join = static_cast<LogicalJoin*>(node);
    const Schema before = join->children[0]->output_schema;
    if (isSpineLink(join->children[0].get()))
        applyToSpineLeaves(join->children[0].get(), catalog);
    else
        join->children[0] = JoinEnumeration::apply(std::move(join->children[0]), catalog);
    join->children[1] = JoinEnumeration::apply(std::move(join->children[1]), catalog);
    if (!sameColumnSequence(before, join->children[0]->output_schema))
        refreshSchema(node);
}

} // namespace

std::unique_ptr<LogicalPlanNode> JoinEnumeration::apply(std::unique_ptr<LogicalPlanNode> node,
                                                        const Catalog& catalog) {
    // !! SEAM AUDIT PASS 2, B-3. The comment that stood here read: "The topmost
    // JOIN is the root of the whole join tree — there is exactly one per statement
    // until subqueries arrive in Week 30 — so this replaces at most once and never
    // descends into a tree it has already reordered." Subqueries arrived in Week
    // 30, semi joins in Week 32, derived tables in Week 34. There are SEVERAL join
    // trees per statement now, and this reordered only the outermost: a derived or
    // subquery body's joins were enumerated only when the ENCLOSING block happened
    // to have no join of its own, because that is the only case in which the
    // generic child loop below was reached at all. Measured on
    // `data/tpch/sf0.01/catalog.json` before the fix, on a 3-relation body under a
    // joining outer block: 62729 vs 38417 (see the commit message), and NO decline
    // line was printed, so `--explain` showed a body that had simply not been
    // considered as one that had nothing to consider.
    //
    // SEQUENCING. This widens where the DP runs, and the DP permutes a join's
    // merged schema. That was safe to do only after the sort tie-break stopped
    // reading schema POSITION as column IDENTITY (`sort_comparator::tieBreakOrder`,
    // ae768d6/d085230/6ab8848) — before it, widening the DP would have widened
    // optimizer B3-1's `optimized != --no-optimize` to every sort inside a derived
    // body. Verified rather than assumed: see the derived-body ORDER BY entry in
    // tests/test_join_enumeration.cc.
    // WEEK 38 — A SEMI/ANTI JOIN IS A SPINE BOUNDARY, NOT A GLOBAL VETO.
    //
    // What stood here handed every JOIN to `reorder`, which declined the whole
    // tree the moment `slotDeclineReason` met a semi/anti node (its join_slot is
    // -1 by contract), and `applyToSpineLeaves` then stepped OVER every join on
    // the children[0] chain — so the FULLY INNER sub-spine underneath a lowered
    // IN / EXISTS was never enumerated. Measured on TPC-H q21, whose inner spine
    // is 4 relations: it kept the WRITTEN order `supplier JOIN l1 JOIN orders
    // JOIN nation`, so `n_name = ':NATION'` (1 of 25 rows, already pushed to the
    // nation scan) joined LAST and the intermediates ran ~3.0M then ~1.5M rows.
    //
    // WHAT DOES NOT MOVE, and each is load-bearing:
    //   * THIS NODE'S POSITION. Its probe input must remain the whole FROM/JOIN
    //     spine, because the lowered operand's binder slot resolves in the domain
    //     leftKeyIndices() uses (logical_plan.cc's lowering site). Only the
    //     ORDER of the relations inside that spine changes.
    //   * OUTER JOINS. `containsOuterJoin` is untouched and still declines the
    //     tree it is called on — R ⟕ S ≠ S ⟕ R.
    //   * `slotDeclineReason`'s semi/anti branch, which still names the cause for
    //     the case this function cannot route: a semi/anti join reached as
    //     children[0] of a STANDARD join that `reorder` was already called on.
    if (node->type == LogicalNodeType::JOIN) {
        auto* join = static_cast<LogicalJoin*>(node.get());
        if (join->semantics != JoinSemantics::STANDARD) {
            const Schema before = join->children[0]->output_schema;
            join->children[0] = apply(std::move(join->children[0]), catalog);
            join->children[1] = apply(std::move(join->children[1]), catalog);
            // The stored copy of the probe schema is stale exactly when the
            // subtree was permuted; see refreshSchema.
            if (!sameColumnSequence(before, join->children[0]->output_schema))
                refreshSchema(node.get());
            // THE CHECKPOINT SURFACE, and it deliberately keeps the `skipped`
            // token rather than growing an `order=`: no ordering decision was
            // made AT THIS NODE, and `order=` must keep meaning "the search ran
            // over these relations". What changed is that the reader is now told
            // where the search did run, so a partially-reordered tree does not
            // read as a fully declined one.
            join->order_decision =
                "join-ordering=skipped (semi/anti join; spine below enumerated separately)";
            return node;
        }
        node = reorder(std::move(node), catalog);
        // reorder() returns a JOIN in every path (rebuild folds a left-deep tree;
        // the declines return the node untouched).
        applyToSpineLeaves(node.get(), catalog);
        return node;
    }
    bool child_changed = false;
    for (auto& child : node->children) {
        const Schema before = child->output_schema;
        child = apply(std::move(child), catalog);
        child_changed = child_changed || !sameColumnSequence(before, child->output_schema);
    }
    if (child_changed) refreshSchema(node.get());
    return node;
}
