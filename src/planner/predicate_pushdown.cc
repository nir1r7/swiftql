#include "predicate_pushdown.h"
#include "cardinality_estimator.h"
#include "parser/ast.h"
#include "parser/expr_utils.h"   // conjoinAll
#include <algorithm>
#include <map>
#include <unordered_set>
#include <vector>

// Collect the set of relation slots a predicate's columns reference, IN THIS
// QUERY BLOCK. Same dispatch as collectCols(), reading relation_slot instead of
// name.
//
// Declared in the header since Week 27: classifyJoinCondition uses it to spot a
// forward reference inside a residual ON conjunct of any shape. THREE callers,
// one walker — soleSlot (here), classifyJoinCondition (join_condition.cc,
// Week 27) and pruningHintForPreservedSide (below, Week 29). A private copy
// would be an eleventh silent dispatch site. The three do NOT agree on what an
// EMPTY set means; the header says which one is which and why only one of them
// can fail closed on its own account.
void collectSlots(const Expr* expr, std::unordered_set<int>& out) {
    if (!expr) return;
    if (auto* cr = dynamic_cast<const ColumnRef*>(expr)) {
        // Week 30: a slot is a position in the range table of the scope
        // query_level steps out, so a ref belonging to an ENCLOSING block is
        // not one of this block's relations. It is not nothing either — a
        // correlated conjunct cannot be routed to one relation of this block —
        // so contribute -1, this walker's existing "unresolved, be
        // conservative" value.
        //
        // A TOP-LEVEL ref reaches this with query_level > 0, and that is why the
        // branch is here rather than only inside the SubqueryExpr case below:
        // classifyJoinCondition calls this walker directly on a NESTED query's
        // ON conjuncts (via Validator::validateQuery), where a correlated ref is
        // an ordinary top-level ref of that expression. An earlier revision of
        // this comment claimed otherwise — the same class of
        // false-justification-that-stops-anyone-checking that this file's header
        // was corrected for four lines below.
        // Week 33, Task 8: the README asked this week to make the answer for a
        // correlated subquery PRECISE (its refs' slots, decremented one level)
        // rather than conservative. It stays conservative, and the argument
        // first written here for why that is moot was FALSE — it claimed "no
        // correlated ref survives into any predicate this walker runs over",
        // which the comment twenty lines above contradicts in the same file
        // (classifyJoinCondition calls this walker directly on a nested query's
        // ON conjuncts, where a correlated ref is exactly what it sees) and
        // which round 1's C-2 disproved with a wrong answer.
        //
        // The containment that actually holds is CHECKABLE, which is the whole
        // difference: soleSlot (below) returns -1 for any conjunct whose slot
        // set contains -1, so a conjunct holding a correlated or unresolved ref
        // is never pushed, and restampSlots is never called on one. That matters
        // because restampSlots does `cr->id = ColumnId::local(slot)` — it would
        // overwrite a correlated id's LEVEL with 0, the exact collapse ColumnId
        // exists to prevent. Record it as "soleSlot rejects any conjunct
        // containing -1", which a reader can verify in ten lines, not as "no
        // correlated ref arrives", which depends on every upstream pass.
        out.insert(cr->id.isLocal() ? cr->id.localSlot("collectSlots") : -1);
        return;
    }
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
    // Week 30 — the tenth Expr subtype, and the one readme.md's site-8 note
    // names. A subquery's OWN refs (query_level 0 inside it) are positions in a
    // DIFFERENT scope's range table; contributing them would name this block's
    // relations by another block's numbering. Its CORRELATED refs DO reference
    // this block — that is what correlation means — so they must contribute
    // something, and the conservative form is -1:
    //
    //   soleSlot                    -> -1, so the conjunct is never pushed. Right:
    //                                  a correlated conjunct owns no single relation.
    //   pruningHintForPreservedSide -> -1 is not in preserved_slots, so the hint
    //                                  is withheld. Right: fail closed.
    //   classifyJoinCondition       -> -1 <= right_slot, so no forward-reference
    //                                  throw. Moot: validateJoinCondition
    //                                  (site 18) refuses a subquery in ON.
    //
    // An UNCORRELATED subquery contributes nothing, which is also right: it is
    // a constant with respect to this block, so `WHERE r1.x = (SELECT ...)`
    // keeps soleSlot == 1 and still pushes onto relation 1's scan.
    //
    // The exact set (correlated refs' slots, level-decremented) would buy
    // pushdown for a correlated conjunct. Week 33 did not add it, and the
    // reason recorded here for the branch being unreachable — "Validator
    // refuses a bound subquery before any logical plan exists" — is the refusal
    // Week 33 DELETED, so it justifies nothing now. What is true, and is
    // checkable rather than historical: -1 is the conservative "cannot name it
    // here" sentinel, soleSlot rejects any conjunct whose slot set contains it,
    // and the effect is therefore only WITHHELD PUSHDOWN. Safe in either
    // reachability state, which is why nothing else had to change when the
    // refusal came down.
    if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        collectSlots(sq->operand.get(), out);   // IN's operand is THIS block's
        if (sq->correlated) out.insert(-1);
        return;
    }
    // Literal / IntervalLiteral: no slot.
}

// Week 29 — the pruning-hint rule, in ONE place for both planners. See the
// header for why it exists and why each branch is spelled the way it is.
const Expr* pruningHintForPreservedSide(const Expr* hint, JoinType join_type,
                                        const std::unordered_set<int>& preserved_slots) {
    if (!hint || join_type == JoinType::INNER) return hint;
    std::unordered_set<int> slots;
    collectSlots(hint, slots);
    if (slots.empty()) return nullptr;          // fail closed: see the header
    for (int s : slots) {
        if (!preserved_slots.count(s)) return nullptr;
    }
    return hint;
}

namespace {

// splitConjuncts moved to parser/expr_utils.h in Week 32 — the set-membership
// lowering needs the SAME notion of "one conjunct" this pass uses.

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
    if (auto* cr = dynamic_cast<ColumnRef*>(expr)) { cr->id = ColumnId::local(slot); return; }
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
        return;
    }
    // Week 30, in lockstep with collectSlots: the IN operand is this block's
    // and is restamped; the BODY is another scope and must not be touched —
    // rewriting its slots would renumber a different range table. Provably
    // never reached with a CORRELATED subquery: that contributes -1 above, so
    // soleSlot is -1 and the conjunct is never pushed.
    if (auto* sq = dynamic_cast<SubqueryExpr*>(expr)) {
        restampSlots(sq->operand.get(), slot);
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

// ── THE TOTALITY SCREEN (seam audit pass 3, B3-2) ────────────────────────────
//
// PER-ROW EVALUATION IS NOT TOTAL: `evaluate()` can THROW on a row. That makes
// every conjunct MOVE this pass performs — reordering inside one filter, and
// pushing a conjunct below a join — a decision about whether a query ERRORS,
// not only about how fast it runs. Measured at HEAD before this screen existed,
// on the shipped `catalog.json`, in BOTH directions:
//
//   MASKED    WHERE SUBSTRING(team, lap_id - lap_id, 2) = 'x' AND speed = 333.3333
//             optimized -> 0 rows,  --no-optimize -> Error
//   INTRODUCED WHERE team LIKE 'zzz%' AND SUBSTRING(team, lap_id - lap_id, 2) = 'x'
//             optimized -> Error,   --no-optimize -> 0 rows
//   INTRODUCED (by distribute, NOT by orderByWork — the same defect with a
//             second cause, so a fix confined to the sort would have closed half
//             of it) laps l JOIN drivers d ON …
//             WHERE d.nationality = 'Zzz' AND SUBSTRING(l.team, l.lap_id - l.lap_id, 2) = 'x'
//             optimized -> Error,   --no-optimize -> 0 rows
//
// SQL does not fix predicate evaluation order, so each of those answers is legal
// in isolation. This project nevertheless asserts `optimized == --no-optimize`
// (compare_against_sqlite.py's fourth mode, run_optimizer_invariant), so the two
// legs DISAGREEING is the defect. THE PROPERTY BEING RESTORED IS THEREFORE
// NARROW AND STATABLE: every conjunct is evaluated on exactly the ROW SET that
// written order gives it, so it raises in the optimized leg if and only if it
// raises in the `--no-optimize` leg.
//
// THE PRECONDITION, which was written down nowhere before:
//
//   Permuting conjuncts C1..Cn preserves both the result set and the error
//   behaviour IFF every conjunct that can RAISE is evaluated on the same row set
//   in both arrangements. AND is commutative over TOTAL conjuncts under SQL's
//   three-valued logic (a filter keeps only TRUE), so the survivor set entering
//   position k depends only on the SET {C1..C_{k-1}}, not on its order. Hence:
//   freely permuting a prefix of TOTAL conjuncts is sound, and so is pushing any
//   of them below an inner join (σ_p(R⋈S) ≡ σ_p(R)⋈S makes the join output set
//   identical). Both stop being sound at the FIRST conjunct that can raise:
//     - it must not move, or it sees rows written order never gave it;
//     - nothing after it may move ahead of it or be pushed below the join, or it
//       sees FEWER rows and a raise it owed is masked.
//
// So: `firstMayRaise` splits the conjunct list. Everything before that index is
// optimized exactly as before. Everything from it on is FROZEN — kept in written
// order and kept above the join. A query with no raising conjunct (every TPC-H
// query, every query in both harnesses) is unaffected, which is why this costs
// nothing measurable.
//
// mayRaise is a CONSERVATIVE over-approximation of "evaluate() can throw on some
// row". Over-approximating costs plan quality on the queries it hits and nothing
// else; under-approximating is a divergence, so the two errors are not
// symmetric. What it does NOT have to cover, because `inferExprType`
// (logical_plan.cc) decides it at PLAN time — in both legs, before any pass in
// this file runs — is every TYPE error: STRING arithmetic, a non-STRING LIKE or
// SUBSTRING operand, a mixed IN list, a CASE with mixed branches. Those raise
// identically in both legs no matter how the conjuncts are arranged. What is
// left is exactly the DATA-dependent raises:
//
//   * substringOf  — start < 1 or length < 0 (evaluator.cc). Only reachable with
//     a COMPUTED position: inferExprType refuses a constant one at plan time, so
//     a SUBSTRING whose start and length are both Literals cannot raise here.
//     That carve-out is what keeps TPC-H Q22's `SUBSTRING(c_phone, 1, 2)` total.
//   * checkedAdd/Sub/Mul/Div/Negate — INT overflow (checked_arith.h). Screened by
//     OPERATOR rather than by inferred type: DOUBLE arithmetic cannot overflow,
//     but deciding that needs a schema this function does not have, and post-
//     folding an arithmetic conjunct in a WHERE is rare enough that the extra
//     conservatism is free (no TPC-H query has one — Q19's `:QTY1 + 10` is two
//     literals and folds).
//   * integer division by zero is NOT in the list: it yields NULL (verified —
//     `SELECT 100 / (lap_id - lap_id) FROM laps LIMIT 1` prints NULL). Only
//     INT64_MIN / -1 raises, and that is covered by the operator screen anyway.
//
// IntervalLiteral and SubqueryExpr both throw unconditionally in evaluate();
// neither can reach a built plan (foldConstants removes the first,
// materializeSubqueries the second), and both are screened here rather than
// argued about, since the cost of screening them is zero.
//
// Same dispatch as collectSlots/restampSlots. A MISSED subtype here answers
// "total" and is therefore the unsafe direction — unlike dispatch site 8, whose
// miss only costs pushdown. Keep the three in lockstep.
bool mayRaise(const Expr* expr) {
    if (!expr) return false;
    if (dynamic_cast<const Literal*>(expr)) return false;
    if (dynamic_cast<const ColumnRef*>(expr)) return false;
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        const std::string& op = bin->op;
        if (op == "+" || op == "-" || op == "*" || op == "/") return true;
        return mayRaise(bin->left.get()) || mayRaise(bin->right.get());
    }
    if (dynamic_cast<const UnaryExpr*>(expr)) return true;   // checkedNegate
    if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) return mayRaise(isn->operand.get());
    if (auto* in = dynamic_cast<const InExpr*>(expr)) return mayRaise(in->operand.get());
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) return mayRaise(lk->operand.get());
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        // evaluate() short-circuits an untaken branch, but ExpressionExecutor
        // declines to compile CaseExpr at all for exactly that reason, so which
        // branches run is an ENGINE detail. Screen every arm.
        for (const auto& w : c->when_clauses) {
            if (mayRaise(w.condition.get()) || mayRaise(w.result.get())) return true;
        }
        return mayRaise(c->else_expr.get());
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        const bool const_start = dynamic_cast<const Literal*>(sub->start.get()) != nullptr;
        const bool const_len = !sub->length
                             || dynamic_cast<const Literal*>(sub->length.get()) != nullptr;
        if (!const_start || !const_len) return true;
        return mayRaise(sub->operand.get());
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) return mayRaise(agg->argument.get());
    // IntervalLiteral, SubqueryExpr, and any Expr subtype added later.
    return true;
}

// Index of the first conjunct that can raise, or conjuncts.size() when none can.
// Everything from here on is frozen: see the screen's comment above.
size_t firstMayRaise(const std::vector<std::unique_ptr<Expr>>& conjuncts) {
    for (size_t i = 0; i < conjuncts.size(); ++i) {
        if (mayRaise(conjuncts[i].get())) return i;
    }
    return conjuncts.size();
}

// Order conjuncts most-selective-first (smallest keep-fraction). Stable so ties
// and stat-less predicates (fallback selectivity) keep their original order.
// "Expected work" is modeled as selectivity only — optimal when per-predicate
// eval cost is uniform (true for fast-path col-op-literal comparisons). A
// selectivity x eval-cost ranking (which would demote fallback-path predicates
// like IS NULL or col-vs-col) is deferred to Week 28, where join enumeration
// gives the cost model per-predicate costs; Week 22 wired costs into join
// selection only.
//
// Only the TOTAL PREFIX is sorted (seam audit pass 3, B3-2). The conjuncts from
// the first one that can raise onward keep their written positions, because the
// columnar predicate cascade evaluates the right conjunct only over the left's
// survivors (columnar_eval.cc's AND) and per-row evaluation can throw — so their
// order decides whether the query errors, not how fast it runs. See mayRaise.
void orderByWork(std::vector<std::unique_ptr<Expr>>& conjuncts,
                 const LogicalPlanNode* scan_child, const Catalog& catalog) {
    const size_t frozen = firstMayRaise(conjuncts);
    if (frozen < 2) return;   // nothing sortable ahead of the frozen tail
    StatsContext ctx = scanStats(scan_child, catalog);
    std::stable_sort(conjuncts.begin(), conjuncts.begin() + static_cast<long>(frozen),
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
        //
        // Week 32: a SEMI/ANTI join is declined for a STRONGER reason than an
        // outer join's. children[1] is a subquery BODY, not a relation of this
        // block's range table — join_slot is -1 to say so — and none of its
        // columns is in this node's output schema at all, so a conjunct pushed
        // there is unresolvable rather than merely mis-scoped. The recursion
        // into children[0] stays unconditional and is what keeps a WHERE
        // conjunct pushing to the spine's scans exactly as it did before the
        // semi-join was interposed between the filter and the spine.
        auto it = (join->join_type == JoinType::INNER
                   && join->semantics == JoinSemantics::STANDARD)
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

    // Seam audit pass 3, B3-2. The SECOND half of the totality screen, and the
    // one a fix confined to orderByWork would have missed: pushing a conjunct
    // below the join moves it to a point where it sees a DIFFERENT ROW SET
    // (every row of one relation, rather than the join's survivors), so it can
    // introduce a raise the written order never reached — and a conjunct pushed
    // out from AHEAD of a raising one leaves that one with fewer rows, masking a
    // raise it was owed. Both were reproduced on the shipped catalog; see
    // mayRaise. Freezing the suffix in `residual` costs pushdown only on queries
    // that contain a raising conjunct at all.
    const size_t frozen = firstMayRaise(conjuncts);

    // std::map, not unordered_map: the leftover loop below must be deterministic
    std::map<int, std::vector<std::unique_ptr<Expr>>> by_slot;
    // Each residual carries its WRITTEN index. See the restore below: the
    // leftover path can reorder conjuncts relative to what the user wrote, and
    // since B3-2 that order is load-bearing rather than cosmetic.
    std::map<int, std::vector<size_t>> by_slot_written;
    std::vector<std::pair<size_t, std::unique_ptr<Expr>>> residual;
    for (size_t i = 0; i < conjuncts.size(); ++i) {
        auto& c = conjuncts[i];
        int slot = (i >= frozen) ? -1 : soleSlot(c.get());
        if (slot < 0) residual.emplace_back(i, std::move(c));
        else { by_slot[slot].push_back(std::move(c)); by_slot_written[slot].push_back(i); }
    }

    join = distribute(std::move(join), by_slot, catalog);

    // Anything unclaimed stays above the join — correct, just slower — so a
    // future tree shape degrades instead of dropping a predicate. This is NOT
    // unreachable: distribute deliberately leaves a bucket behind when the join
    // at that slot is LEFT (Week 29) or semi/anti (Week 32), which is a shape the
    // CLI can type.
    //
    // Seam audit pass 3, B3-2: it is therefore also not free. Appending a
    // leftover after a FROZEN conjunct would put a written-earlier predicate
    // after a written-later one, and the frozen conjunct would then see rows
    // written order never gave it — reintroducing exactly the divergence the
    // screen closes, on a LEFT JOIN. distribute either consumes a whole bucket
    // (and erases it) or leaves it whole, so the parallel index vector still
    // lines up, and re-sorting by written index makes "the residual is in
    // written order" true by construction rather than by argument.
    for (auto& entry : by_slot) {
        const std::vector<size_t>& written = by_slot_written[entry.first];
        for (size_t k = 0; k < entry.second.size(); ++k)
            residual.emplace_back(written[k], std::move(entry.second[k]));
    }
    std::sort(residual.begin(), residual.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::unique_ptr<Expr>> residual_exprs;
    residual_exprs.reserve(residual.size());
    for (auto& r : residual) residual_exprs.push_back(std::move(r.second));

    // residuals (mixed / unresolved / frozen) stay above the join
    return filterOnto(std::move(join), std::move(residual_exprs), catalog);
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
