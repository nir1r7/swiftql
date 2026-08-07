#include "vectorized_plan_builder.h"
#include "execution/vec_derived_node.h"
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
#include "planner/predicate_pushdown.h"   // pruningHintForPreservedSide — shared with Planner::plan
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

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
// Week 34: NULLABLE. A DERIVED relation has no catalog table, so there is no
// per-column avg_width to look up and no TableStats to consult — and walking
// THROUGH it returns the BODY's base table, attributing that table's widths to
// the derived relation's columns. That is the identical defect Week 27 found in
// rowWidth for a join subtree and closed by refusing to guess: the uniform proxy
// and `build=join-subtree` beat a plausible wrong number, because this feeds the
// decision Week 28's enumeration is built on.
const std::string* leafScanTableOrNull(const LogicalPlanNode* node) {
    while (node->type != LogicalNodeType::SCAN) {
        if (node->type == LogicalNodeType::DERIVED) return nullptr;
        if (node->children.empty()) return nullptr;
        node = node->children[0].get();
    }
    return &static_cast<const LogicalScan*>(node)->table_name;
}

// True when this join input is exactly one relation: a scan, possibly under
// filters. From three relations on, children[0] can be a whole join subtree
// whose merged schema spans several tables — and then neither leafScanTable()
// nor any single TableStats describes it.
bool isSingleRelation(const LogicalPlanNode* node) {
    while (node->type != LogicalNodeType::SCAN) {
        if (node->type == LogicalNodeType::JOIN) return false;
        // Week 34: a DERIVED relation IS one relation of this block — that is
        // the whole point of the node — but it is not one a TableStats describes,
        // and every caller of this predicate follows it with leafScanTable. So
        // it answers false, which routes them to the no-statistics path rather
        // than to the body's base table. Same stance as the row above.
        if (node->type == LogicalNodeType::DERIVED) return false;
        if (node->children.empty()) return false;
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

// Resolve the RIGHT input's key columns.
//
// For a STANDARD join, children[1] is always exactly one relation (left-deep;
// Week 28's DP keeps that shape), and a standalone scan's schema stamps every
// column slot 0 — the join_slot stamp lives only on the MERGED schema — so the
// bare-name overload is both unambiguous and the only one that resolves here.
//
// For a SEMI/ANTI join that rationale does NOT hold and Week 33 round 1 found
// two silent wrong answers behind it: children[1] is a whole subquery body,
// which may be a JOIN (merged schema, duplicate names legal by invariant 3) and
// whose column names come from its SELECT ALIASES. So the name being looked up
// was resolved in one schema and matched in another. `positional` says the
// lowering has already arranged the body's output to BE the key tuple, in key
// order — subquery_decorrelation.cc projects the body to its key columns, and
// Week 32's IN body has exactly one output column by the Validator's arity
// rule. Nothing is looked up by name, so nothing can shadow it.
std::vector<int> rightKeyIndices(const Schema& right_schema, const std::vector<JoinKey>& keys,
                                bool positional = false) {
    std::vector<int> idx;
    idx.reserve(keys.size());
    if (positional) {
        // Loud rather than latent: the lowering owns this arrangement, so a
        // mismatch is a planner bug and says so like every other check of its
        // kind.
        if (right_schema.size() != static_cast<int>(keys.size())) {
            throw std::runtime_error(
                "internal: a semi/anti join's build input must output exactly its "
                "key columns (got " + std::to_string(right_schema.size())
                + " for " + std::to_string(keys.size()) + " keys)");
        }
        for (size_t i = 0; i < keys.size(); ++i) idx.push_back(static_cast<int>(i));
        return idx;
    }
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
    // Week 32 — this is a READER of join_slot, and logical_plan.h's contract
    // says every one of them either declines on semantics != STANDARD or is
    // provably unreachable for such a node. It is reachable: two IN conjuncts
    // stack two semi joins, and rowWidth() on the outer one's left child runs
    // this walk over the inner one. Without the decline it stamps the BODY's
    // table at key -1 — a slot that names no relation of this block — and the
    // body contributes nothing to a semi join's output schema, so there is no
    // entry to make.
    //
    // Why it was latent and never a wrong width — the reason is the map, not
    // the cost block. The ONLY read of this map is
    // slot_tables.find(col.relation_slot) over child->output_schema.columns()
    // (rowWidth, below). When child is a semi/anti join that schema IS its left
    // child's — asserted, not observed, at subquery_lowering.cc's
    // "output schema must be its left child's" check — so every column in it
    // comes from the outer spine and carries a real binder slot; ColumnDef::
    // relation_slot defaults to 0 and the binder stamps the rest, so no column
    // anywhere can carry -1. The out[-1] entry was therefore written and never
    // read, and the width rowWidth returns is bit-identical before and after
    // this decline. That is why no behavioural test can tell the two apart:
    // this is a contract repair, not a corrected cost decision.
    //
    // An earlier rationale said instead "the widths computed here are discarded
    // before setCostDecision". Do not rely on that one — and as of Week 34 it is
    // not merely unreliable, it is FALSE. It rested on a build-order fact stated
    // in another file and enforced nowhere: that no STANDARD join is ever built
    // above a semi join. Week 34's correlated-scalar rewrite
    // (lowerCorrelatedScalars) runs AFTER lowerInSubqueries and
    // lowerExistsSubqueries on the same spine, so a STANDARD LEFT join over a
    // LogicalDerived is now routinely built above a semi join, and the
    // discarded-widths argument is dissolved exactly as that comment predicted.
    //
    // Nothing to fix, because the guard was never resting on it: the argument
    // above — every column of a semi/anti join's output schema comes from the
    // outer spine and carries a real binder slot, so nothing ever looks up -1 —
    // is independent and still holds. This is what the earlier comment meant by
    // "the map argument above does not depend on it", now that the dependent
    // argument has actually died. Fourth time this block has had to be corrected;
    // stating which argument is load-bearing is the only thing that has helped.
    if (join->semantics == JoinSemantics::STANDARD) {
        // Week 34: a DERIVED children[1] names no catalog table, so the entry is
        // SKIPPED rather than filled with the body's base table. rowWidth's
        // slot_tables lookup then misses and falls back to 8 bytes/column, which
        // is Week 27's refuse-to-guess rather than a plausible wrong width.
        if (const std::string* t = leafScanTableOrNull(join->children[1].get()))
            out[join->join_slot] = *t;
    }
    // The left side is walked either way: a semi join's output schema IS its
    // left child's, so every column this map is consulted for comes from there.
    const LogicalPlanNode* left = join->children[0].get();
    if (isSingleRelation(left)) {
        // A join always carries at least its own key columns per side, so the
        // merged schema cannot be empty — but read defensively: an empty schema
        // here would index out of bounds rather than lose a width.
        if (join->output_schema.size() > 0) {
            if (const std::string* t = leafScanTableOrNull(left))
                out[join->output_schema.column(0).relation_slot] = *t;
        }
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
        const std::string* table_p = leafScanTableOrNull(child);
        if (!table_p) return child->output_schema.size() * 8.0;
        const std::string& table = *table_p;
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

        // Week 34 — a derived relation lowers to its BODY's operator tree and
        // nothing else. There is deliberately NO physical derived operator: a
        // derived table is a naming and slot-normalization artifact, not a
        // computation. What does have to reach the physical side is the column
        // RENAME from `AS d (a, b)`, because resolveColumnIndex (evaluator.cc)
        // and every indexOf above this graft look the new names up.
        case LogicalNodeType::DERIVED: {
            auto* derived = static_cast<LogicalDerived*>(node);
            auto child = lowerNode(derived->children[0].get(), nullptr);
            if (derived->output_schema.size() != child->outputSchema().size()) {
                throw std::runtime_error(
                    "internal: derived relation '" + derived->alias
                    + "' lowered to a child of a different width");
            }
            return std::make_unique<VecDerivedNode>(
                std::move(child), derived->alias, derived->output_schema);
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
            // Week 33, Task 7(3) — setCostDecision's consumption of rowWidth,
            // TRACED END TO END, because three audit rounds left it unconfirmed
            // and half of collectSlotTables' rationale rested on it.
            //
            // The trace: rowWidth() runs HERE for every join, including a
            // SEMI/ANTI one, because this block sits above the semantics switch
            // below. Its results reach the cost model, and therefore
            // setCostDecision, ONLY through the STANDARD branch — the SEMI/ANTI
            // branch returns before from_w/join_w are read. So for a semi or
            // anti join the widths are computed and DISCARDED.
            //
            // What that means for Week 33: a decorrelated EXISTS lowers to a
            // SEMI/ANTI join, so it can never be mis-costed by a width that
            // counts the body's columns — the cost decision it would feed is
            // never taken. The asymmetry a merged-schema join would create
            // (output is the left child's, so a width including children[1]
            // would over-count) is therefore unreachable, not merely unlikely.
            //
            // !! This is a statement about the CURRENT shape, and the file's own
            // collectSlotTables comment already warns that the "discarded before
            // setCostDecision" argument dissolves the moment a STANDARD join is
            // planned ABOVE a semi join. Week 34's derived tables are that week.
            // The map argument in collectSlotTables does not depend on this one,
            // which is why it is the load-bearing rationale and this is a trace.
            if (estimate_driven) {
                // per-side row width from real avg_width stats (Gap 3); this
                // feeds the hash-table memory term, so with equal row counts
                // the narrower input becomes the cheaper build side
                from_w = rowWidth(join->children[0].get(), catalog);
                join_w = rowWidth(join->children[1].get(), catalog);
            } else {
                // Week 34: with a DERIVED input there is no loaded table to
                // read a row count from. Fall back to the logical estimate (or
                // to 1 when there is none, which is what --no-optimize leaves),
                // rather than throwing out of a map lookup on a legal query.
                const std::string* fp = leafScanTableOrNull(join->children[0].get());
                const std::string* jp = leafScanTableOrNull(join->children[1].get());
                from_est = fp ? tables.at(*fp).num_rows
                              : std::max(1.0, join->children[0]->estimated_rows);
                join_est = jp ? tables.at(*jp).num_rows
                              : std::max(1.0, join->children[1]->estimated_rows);
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
            // One precondition this rests on, and it is not local: every
            // ColumnRef reaching a hint carries a real binder slot (binder.cc
            // stamps them; Validator rejects anything left at -1).
            //
            // !! A SECOND precondition was stated here through Week 28 and is now
            // FALSE BY DESIGN: "a post-pushdown residual holds no
            // `ColumnRef op Literal` conjunct, because soleSlot() routed every
            // single-slot conjunct to its own relation". Week 29's outer join
            // deletes it — distribute() declines to push a conjunct onto an outer
            // join's null-supplying side, and pushIntoJoin's leftover loop then
            // leaves exactly such a conjunct in the residual above the join, from
            // where it descends to the PRESERVED side's scan as this hint.
            // (`... d LEFT JOIN l ON k WHERE l.season = 2024` is the shape.)
            //
            // With that precondition gone, the only thing standing between a
            // slot-1 predicate and the slot-0 relation's zone maps is
            // chunk_pruner.h's `relation_slot < 1` test in ANOTHER file — and the
            // failure mode is silent row loss on the preserved side, the one side
            // this week exists to protect. So do not delegate it: withhold the
            // hint when this join does not preserve every relation the hint
            // mentions. The rule itself lives in predicate_pushdown.h, because
            // Planner::plan routes a hint to its FROM scan for exactly the same
            // reason and a second copy is how the two engines drift apart.
            //
            // `preserved` is read off the LEFT INPUT's schema rather than assumed
            // to be {0}: in (A ⋈ B) ⟕ C, relation B is preserved too, and a hint
            // over B belongs to a scan that is entitled to it.
            const bool leftmost_is_slot0 =
                join->output_schema.size() > 0 &&
                join->output_schema.column(0).relation_slot == 0;
            std::unordered_set<int> preserved;
            for (const ColumnDef& c : join->children[0]->output_schema.columns()) {
                preserved.insert(c.relation_slot);
            }
            const Expr* from_hint = leftmost_is_slot0
                ? pruningHintForPreservedSide(pruning_where, join->join_type, preserved)
                : nullptr;
            auto from_child = lower(join->children[0].get(), from_hint);
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
            // POSITIONAL for a semi/anti join: both lowerings arrange the body's
            // output to BE the key tuple, so nothing is matched by name against a
            // schema that name was not resolved in (round 1 H-1, H-2, M-3).
            std::vector<int> right_idx = rightKeyIndices(
                jn_schema, join->keys, join->semantics != JoinSemantics::STANDARD);

            // Week 32 — SEMI/ANTI. The side is FORCED, not costed: a hash
            // semi-join emits PROBE-side rows, so the outer spine must be the
            // probe input and the subquery body the build input. Same stance as
            // Week 29's left_outer, and the same enforcement — the operator's
            // constructor throws on every other combination.
            //
            // The key indices resolve in DIFFERENT schemas and that is the
            // point: the probe side by slot against the outer spine's schema
            // (leftKeyIndices), the build side against the BODY's own schema.
            // The two numbering domains never meet, because output_schema below
            // is the probe schema, unmerged (docs/week-32-plan.md 0).
            //
            // No setCostDecision() here: estimates did not drive this choice,
            // and printing one would make --explain claim an optimizer decision
            // that never happened (the discipline at LogicalJoin::order_decision).
            if (join->semantics != JoinSemantics::STANDARD) {
                return std::make_unique<VecHashJoinNode>(
                    std::move(from_child), std::move(join_child),
                    std::move(left_idx), std::move(right_idx),
                    join->output_schema, /*swapped=*/false, /*left_outer=*/false,
                    /*on_residual=*/nullptr, join->semantics);
            }

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
            // Week 29 adds a fourth eligibility term for the same reason: the
            // SIMD loop join is an INNER equi-join, and its probe loop has no
            // unmatched path at all.
            const bool outer = join->join_type == JoinType::LEFT;

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
            bool use_simd = estimate_driven && int_keys && !outer && best_simd < best_hash;
            // Week 29: for an outer join the build side stops being a free cost
            // choice. With the preserved side BUILDING, the operator would need a
            // matched flag per build row plus an end-of-probe drain; with it
            // PROBING it needs one branch. Force the side. The cost — when the
            // null-supplying side is the larger input we hash the larger input —
            // is stated in README Limitations and belongs to Week 37 measurement,
            // not to a hunch here.
            bool from_builds = outer ? false
                             : (use_simd ? cost_simd_from < cost_simd_join
                                         : cost_hash_from < cost_hash_join);

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
                  << "build=" << (leafScanTableOrNull(build_side) && isSingleRelation(build_side)
                                      ? *leafScanTableOrNull(build_side)
                                      : (build_side->type == LogicalNodeType::DERIVED
                                             ? "derived" : "join-subtree"))
                  << " cost=" << side_cost << " (alt=" << side_alt << ")";
                if (int_keys && !outer) {
                    d << " algo=" << (use_simd ? "simd" : "hash")
                      << " (" << (use_simd ? "hash=" : "simd=")
                      << (use_simd ? best_hash : best_simd) << ")";
                }
                decision = d.str();
                if (outer) {
                    // Never print an (alt=) that was not an option: the side was
                    // forced, not costed against its alternative. On INT keys the
                    // suffix also has to say that the ALGORITHM was not a choice
                    // either — the `algo=` clause is suppressed above, so without
                    // this a reader of a plan whose keys are both INT cannot tell
                    // whether the SIMD loop join was ineligible or merely lost on
                    // cost, and "SIMD is never costed for an outer join" is one of
                    // the properties this week claims. Only on INT keys, because
                    // that is exactly when SIMD would otherwise have been in the
                    // running.
                    decision = decision.substr(0, decision.find(" (alt="))
                             + (int_keys ? " (outer: the preserved side must probe, hash only)"
                                         : " (outer: the preserved side must probe)");
                }
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
            // Week 29: an outer join always takes the second branch — the
            // preserved (FROM) side probes — and carries its ON residual into the
            // operator, where it filters the match test rather than the result.
            std::unique_ptr<VecHashJoinNode> join_node = from_builds
                // FROM builds: JOIN side probes (swapped)
                ? std::make_unique<VecHashJoinNode>(
                      std::move(join_child), std::move(from_child),
                      right_idx, left_idx, join->output_schema, /*swapped=*/true)
                : std::make_unique<VecHashJoinNode>(
                      std::move(from_child), std::move(join_child),
                      left_idx, right_idx, join->output_schema, /*swapped=*/false,
                      outer, std::move(join->on_residual));
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
