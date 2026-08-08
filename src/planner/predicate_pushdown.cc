#include "predicate_pushdown.h"
#include "cardinality_estimator.h"
#include "parser/ast.h"
#include "parser/expr_utils.h"     // conjoinAll
#include "parser/expr_totality.h"  // the totality screen: exprMayRaise / firstMayRaise
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

// Every ColumnRef of `expr` that belongs to THIS query block, in tree order.
//
// THE MUTABLE TWIN OF collectSlots, and it is ONE function on purpose: this file
// had two copies of the same dispatch (collectSlots reading, restampSlots
// writing) and the header already warns that they must stay in lockstep. Week 37
// needed a THIRD writer — remapping a conjunct's refs onto a derived body's
// schema — and a third open-coded copy is how a lockstep of two becomes a
// lockstep of three that nobody checks. restampSlots and both remappers below
// are now expressed in terms of this walker, so there is one place to add an
// Expr subtype on the writing side.
//
// The two SCOPE decisions are the ones collectSlots already documents and are
// repeated here because they are what makes "belongs to this block" true:
//   * a SubqueryExpr's OPERAND is this block's and is visited; its BODY is
//     another scope's range table and is NOT. Rewriting inside it would renumber
//     a different block.
//   * an AggregateExpr's argument is visited even though no conjunct containing
//     one survives to pushdown today, because collectSlots descends there and
//     the lockstep is only true if both do.
template <typename F>
void forEachLocalColumnRef(Expr* expr, F&& f) {
    if (!expr) return;
    if (auto* cr = dynamic_cast<ColumnRef*>(expr)) { f(*cr); return; }
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        forEachLocalColumnRef(bin->left.get(), f);
        forEachLocalColumnRef(bin->right.get(), f);
        return;
    }
    if (auto* isn = dynamic_cast<IsNullExpr*>(expr)) { forEachLocalColumnRef(isn->operand.get(), f); return; }
    if (auto* un = dynamic_cast<UnaryExpr*>(expr)) { forEachLocalColumnRef(un->operand.get(), f); return; }
    if (auto* in = dynamic_cast<InExpr*>(expr)) { forEachLocalColumnRef(in->operand.get(), f); return; }
    if (auto* lk = dynamic_cast<LikeExpr*>(expr)) { forEachLocalColumnRef(lk->operand.get(), f); return; }
    if (auto* c = dynamic_cast<CaseExpr*>(expr)) {
        for (auto& w : c->when_clauses) {
            forEachLocalColumnRef(w.condition.get(), f);
            forEachLocalColumnRef(w.result.get(), f);
        }
        forEachLocalColumnRef(c->else_expr.get(), f);
        return;
    }
    if (auto* sub = dynamic_cast<SubstringExpr*>(expr)) {
        forEachLocalColumnRef(sub->operand.get(), f);
        forEachLocalColumnRef(sub->start.get(), f);
        forEachLocalColumnRef(sub->length.get(), f);   // nullptr-safe
        return;
    }
    if (auto* agg = dynamic_cast<AggregateExpr*>(expr)) { forEachLocalColumnRef(agg->argument.get(), f); return; }
    if (auto* sq = dynamic_cast<SubqueryExpr*>(expr)) { forEachLocalColumnRef(sq->operand.get(), f); }
}

// Re-stamp every ColumnRef in a pushed conjunct to the given slot. A conjunct
// pushed below the join lives in a single-table subtree whose scan schema stamps
// all columns slot 0, so the refs must match — this is also what lets ChunkPruner
// act on join-side pruning hints (it ignores slot >= 1 refs; see chunk_pruner.h).
void restampSlots(Expr* expr, int slot) {
    forEachLocalColumnRef(expr, [slot](ColumnRef& cr) { cr.id = ColumnId::local(slot); });
}

// The one resolution rule, identical to evaluator.cc's resolveColumnIndex:
// slot-first, bare-name fallback. -1 when the schema does not hold the column.
int resolveInSchema(const ColumnRef& cr, const Schema& schema) {
    if (cr.id.isResolved() && cr.id.isLocal()) {
        int idx = schema.indexOf(cr.column_name, cr.id.localSlot("predicate pushdown"));
        if (idx >= 0) return idx;
    }
    return schema.indexOf(cr.column_name);
}

// Rewrite a conjunct written against a DERIVED relation's schema so it reads the
// BODY's schema instead, and report whether it could be.
//
// The mapping is POSITIONAL and that is the whole argument: derivedRelationSchema
// builds the relation's schema from the body plan's output schema column by
// column — it may RENAME (the `AS d (a, b)` alias list) and it stamps every slot
// 0, and it does nothing else. So column i of one IS column i of the other, and
// LogicalPlanBuilder::build's drift check already asserts the two agree in size
// and per-column name/type. Resolving by NAME against the derived schema and then
// taking the BODY's column at that index is therefore exact even when the body
// joins and its own schema carries several slots and a repeated name — which is
// the case a name-to-name mapping would get wrong.
//
// All-or-nothing: nothing is written until every ref has resolved, so a conjunct
// this declines is left byte-identical for the caller to keep above the node.
bool remapOntoDerivedBody(Expr* conjunct, const LogicalDerived& derived) {
    const Schema& from = derived.output_schema;
    const Schema& to = derived.children[0]->output_schema;
    if (from.size() != to.size()) return false;   // drift: decline rather than guess

    std::vector<ColumnRef*> refs;
    forEachLocalColumnRef(conjunct, [&refs](ColumnRef& cr) { refs.push_back(&cr); });
    std::vector<int> idx;
    idx.reserve(refs.size());
    for (const ColumnRef* cr : refs) {
        int i = resolveInSchema(*cr, from);
        if (i < 0) return false;
        idx.push_back(i);
    }
    for (size_t k = 0; k < refs.size(); ++k) {
        const ColumnDef& c = to.column(idx[k]);
        refs[k]->column_name = c.name;
        refs[k]->id = ColumnId::local(c.relation_slot);
        // The enclosing block's alias names nothing inside the body. Cleared
        // rather than kept: resolution is (slot, name) with a bare-name fallback
        // and never consults table_name (evaluator.cc's resolveColumnIndex), so
        // keeping it would change only what --explain prints, and it would print
        // a relation that does not exist at that depth.
        refs[k]->table_name.clear();
    }
    return true;
}

// Rewrite a conjunct written against a PROJECT's output so it reads the
// projection's INPUT instead, and report whether it could be.
//
// σ_p(π(R)) ≡ π(σ_p'(R)) for a row-wise π, with p' the conjunct rewritten onto
// π's inputs — but only where every column p names is a PLAIN PASSTHROUGH.
// A projected column that is COMPUTED (`speed * 2 AS fast`) cannot be rewritten
// by substitution here without duplicating the expression, and a projected
// AGGREGATE does not exist below the project at all. Both decline.
//
// THAT IS A SET EQUIVALENCE AND IT IS ONLY HALF THE OBLIGATION (seam audit pass
// 4, P4-B1). π is evaluated on FEWER ROWS after the descent, so the rule also
// needs an ERROR-BEHAVIOUR condition, and it is about the expressions this
// function never looks at: the SIBLINGS of the ones the conjunct names. That
// second condition is not checkable here — this function is handed one conjunct
// and answers for it — so it lives at the single call site in apply(), which
// screens `project.exprs` as a whole before any conjunct is offered. Do not
// read a `true` from here as permission to descend.
//
// All-or-nothing, like remapOntoDerivedBody, and for the same reason.
bool remapThroughProject(Expr* conjunct, const LogicalProject& project) {
    // A project whose select list and output schema are not 1:1 is a shape this
    // rewrite has no positional mapping for.
    if (static_cast<size_t>(project.output_schema.size()) != project.exprs.size()) return false;

    std::vector<ColumnRef*> refs;
    forEachLocalColumnRef(conjunct, [&refs](ColumnRef& cr) { refs.push_back(&cr); });
    std::vector<const ColumnRef*> sources;
    sources.reserve(refs.size());
    for (const ColumnRef* cr : refs) {
        int i = resolveInSchema(*cr, project.output_schema);
        if (i < 0) return false;
        const auto* src = dynamic_cast<const ColumnRef*>(project.exprs[i].get());
        if (!src) return false;   // computed or aggregate output: not a passthrough
        sources.push_back(src);
    }
    for (size_t k = 0; k < refs.size(); ++k) {
        refs[k]->column_name = sources[k]->column_name;
        refs[k]->table_name = sources[k]->table_name;
        refs[k]->id = sources[k]->id;
    }
    return true;
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

// ── THE TOTALITY SCREEN (seam audit pass 3, B3-2; pass 4, P4-1/P4-2/P4-B1/P4-B2)
//
// PER-ROW EVALUATION IS NOT TOTAL: `evaluate()` can THROW on a row. That makes
// every conjunct MOVE this pass performs — reordering inside one filter,
// pushing a conjunct below a join, pushing one into a derived body, and
// descending below that body's projection — a decision about whether a query
// ERRORS, not only about how fast it runs. Measured at HEAD before this screen
// existed, on the shipped `catalog.json`, in BOTH directions:
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
// SQL does not fix predicate evaluation order. SwiftQL DOES — see
// parser/expr_totality.h for the rule in full and for the screen itself. The
// half of it this file has to honour:
//
//   A conjunct is evaluated on the rows for which every conjunct WRITTEN BEFORE
//   IT evaluated TRUE. Both engines implement exactly that cascade
//   (evaluatePredicate in evaluator.cc, evalPredicate in columnar_eval.cc), so
//   the row set a conjunct sees is a property of the WRITTEN ORDER, and this
//   pass may not change it for a conjunct that can raise.
//
// That is strictly stronger than "the two legs agree", which is what pass 3
// aimed at; it is what makes the four movers below, the two engines and the
// chunk pruner obey ONE rule instead of three. `optimized == --no-optimize`
// (compare_against_sqlite.py's fourth mode, run_optimizer_invariant) then falls
// out of it rather than being asserted separately.
//
// THE PRECONDITION:
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
// WHAT THE LIST HAS TO ENUMERATE, and pass 5 changed the answer. Two versions
// of this paragraph have now been short by exactly one entry, and both times the
// missing entry WAS the divergence — so the axis the list is drawn on is worth
// more than the list:
//
//   THE OBLIGATION IS NOT PER MOVE. It is per EXPRESSION WHOSE ROW SET A MOVE
//   CHANGES, and that is a strictly larger set than the conjuncts the pass
//   touches. Enumerating moves finds the conjunct that travels; it does not find
//   the expression that stayed put while the rows underneath it changed.
//
// The four moves, and beside each the expressions whose row set it changes:
//
//   1. reordering conjuncts inside one filter   (orderByWork)
//      -> the conjuncts after the one that moved.
//   2. pushing a conjunct below a join          (distribute)
//      -> the conjunct itself (fewer rows: one relation, not the survivors),
//         AND the conjuncts it was written ahead of, AND — pass 5's P5-B1 —
//         a LEFT join's ON RESIDUAL, which this pass never touches and which
//         is not a conjunct of any list this file reads. See distribute.
//   3. pushing a conjunct into a derived body   (pushIntoDerived)
//      -> the conjunct, and everything that stays behind it.
//   4. descending below that body's projection  (apply's PROJECT arm)
//      -> the conjunct, AND — pass 4's P4-B1 — every SIBLING expression in
//         `project.exprs`, which the conjunct never names.
//
// Moves 2 and 4 are the same lesson twice, one round apart. Each rests on a SET
// equivalence — σ_p(R ⟕ S) ≡ σ_p(R) ⟕ S for 2, σ_p(π(R)) ≡ π(σ_p'(R)) for 4 —
// and a set equivalence about a node's OUTPUT says nothing about the row sets of
// the expressions evaluated INSIDE it. So each carries a second screen: the
// PROJECT arm screens `project.exprs`, and distribute screens `on_residual`.
//
// The screen itself is `exprMayRaise` / `conjunctMayRaise`
// (parser/expr_totality.h), shared with ChunkPruner and with the LIMIT rule in
// logical_plan.cc. It is SCHEMA-AWARE, and that is not a refinement: pass 4's
// P4-2 measured 87x on a three-conjunct WHERE whose only defect was a screen
// that judged arithmetic by OPERATOR (`l.speed * 2`, DOUBLE, cannot overflow)
// instead of by operand type.

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
// the first one that can raise onward keep their written positions, because BOTH
// engines evaluate a conjunct only over the rows every earlier conjunct kept
// (columnar_eval.cc's cascading AND, evaluator.cc's evaluatePredicate) and
// per-row evaluation can throw — so their order decides whether the query
// errors, not how fast it runs. See the screen above.
//
// The conjuncts are written against the CHILD's schema (this filter sits
// directly above it), which is what the screen needs to decide operand types.
void orderByWork(std::vector<std::unique_ptr<Expr>>& conjuncts,
                 const LogicalPlanNode* scan_child, const Catalog& catalog) {
    const size_t frozen = firstMayRaise(conjuncts, scan_child->output_schema);
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
        // The recursion into children[0] below is CONDITIONAL, and σ_p(R) ⟕ S ≡
        // σ_p(R ⟕ S) is only half of why. See the ON-residual screen below the
        // children[1] block: that identity is about the join's OUTPUT and says
        // nothing about its CANDIDATE PAIRS, which is what a LEFT join's ON
        // residual is evaluated on. Where there is no residual (every INNER
        // join, and every LEFT join whose ON is keys only) the identity IS the
        // whole obligation and the recursion is unconditional as before. The
        // test is re-applied at every join on the way down, so a conjunct owned
        // by a relation that some deeper outer join null-supplies stops at that
        // join.
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

        // SEAM AUDIT PASS 5, P5-B1. THE THIRD SCREEN, and the only one that is
        // not about a conjunct of any list.
        //
        // A LEFT join's non-key ON conjuncts are NOT folded into the WHERE the
        // way an INNER join's are (logical_plan.cc's join-type split, Week 29):
        // they hang on this node as `on_residual` and are evaluated ONCE PER
        // CANDIDATE PAIR inside the probe loop (plan_nodes.cc's HashJoinNode
        // and vec_hash_join_node.cc, both `passes()`). So the residual is a
        // per-row expression whose row set is the join's CANDIDATE PAIRS —
        // preserved-side rows × build side — and pushing a WHERE conjunct into
        // children[0] shrinks it.
        //
        // σ_p(R) ⟕ S ≡ σ_p(R ⟕ S) does not license that. It is a SET equivalence
        // about the join's OUTPUT; it says nothing about which pairs the match
        // test is run on, and the residual IS the match test. Measured on the
        // shipped catalog before this screen existed:
        //
        //   SELECT COUNT(*) FROM laps l LEFT JOIN drivers d
        //     ON l.driver_id = d.driver_id AND l.lap_id * 1000000000000000 > 0
        //   WHERE l.lap_id < 5
        //     optimized -> 4   --no-optimize -> Error: integer overflow in '*'
        //
        // and identically with SUBSTRING on a computed start, with the residual
        // reading the NULL-SUPPLYING side, with a derived preserved side, and
        // with the LEFT join at the bottom or in the middle of a 3-relation
        // spine. The three legs that agreed with `--no-optimize` were exactly
        // the three that never call PredicatePushdown.
        //
        // The INNER analogue is safe for the reason this one is not: its
        // residual becomes a conjunct of the WHERE list, AHEAD of the written
        // WHERE, so firstMayRaise freezes at index 0 in pushIntoJoin and no
        // conjunct moves at all. This screen rejoins the LEFT path to that one.
        //
        // conjunctMayRaise, not exprMayRaise: `passes()` does
        // `!v.isNull() && v.asInt() != 0`, so the residual is truth-tested
        // exactly as a filter conjunct is and a non-INT static type raises on
        // its own.
        //
        // DECLINE rather than repair: leaving the buckets in `by_slot` sends
        // them to pushIntoJoin's leftover loop, which lifts them above the whole
        // tree — the same "degrade instead of drop" path the LEFT/semi children[1]
        // decline above already uses. It costs pushdown only on a query that has
        // a raising ON residual at all; `on_residual` is assigned in exactly one
        // place (logical_plan.cc, under jc.type == JoinType::LEFT), which is what
        // bounds this screen to outer joins.
        //
        // PER JOIN, deliberately: the LEFT join carrying the residual need not
        // be the top of the spine, and this test is re-applied at every join on
        // the way down for the same reason the INNER/LEFT test above is.
        if (join->on_residual
            && conjunctMayRaise(join->on_residual.get(), join->output_schema))
            return node;

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
    // The WHERE was written against the join's output schema; the screen below
    // needs it to type the conjuncts' operands, and it must be read before the
    // tree is rewritten.
    const Schema join_schema = join->output_schema;

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
    const size_t frozen = firstMayRaise(conjuncts, join_schema);

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

// Push a WHERE filter INTO a derived relation's body (seam audit pass 3, B3-3).
//
// filterOnto used to WRAP a conjunct routed to a derived relation above the
// LogicalDerived and stop there, in every shape — including the one with no join
// anywhere. Measured on the simplest exhibiting query before this existed:
//
//   SELECT d.team, d.speed FROM (SELECT team, speed, season FROM laps) d
//   WHERE d.speed > 344 ORDER BY d.speed LIMIT 3
//
// 18465 us against 1969 us for the flat equivalent — 9.4x, and the cost is not
// the filter but the body's projection MATERIALIZING 10000 rows that the filter
// then discards to 174. The body's scan also printed no `chunks_skipped`, so the
// zone-map hint was lost as well.
//
// WHY ENTERING IS SAFE, stated so it can be checked: the filter is attached
// directly ABOVE the body's root, whose output rows ARE the derived relation's
// rows (derivedRelationSchema renames and re-stamps; it does not add, drop or
// reorder). So this step is σ applied at a point that produces the same
// relation, for ANY body shape — aggregate, DISTINCT, LIMIT and all. It is the
// NEXT step, below the body's projection, that has a precondition, and that one
// lives in remapThroughProject where apply() reaches it.
//
// The B3-2 freeze applies unchanged and for the same reason: entering the body
// moves a conjunct AHEAD of every conjunct that stays, so a raising conjunct
// must not enter and nothing may enter from behind one.
std::unique_ptr<LogicalPlanNode> pushIntoDerived(std::unique_ptr<LogicalFilter> filter,
                                                 const Catalog& catalog) {
    auto derived = std::unique_ptr<LogicalPlanNode>(filter->children[0].release());
    auto* d = static_cast<LogicalDerived*>(derived.get());

    std::vector<std::unique_ptr<Expr>> conjuncts;
    splitConjuncts(std::move(filter->predicate), conjuncts);
    // Written against the DERIVED RELATION's schema — remapOntoDerivedBody has
    // not run yet, so this is the schema the conjuncts still read.
    const size_t frozen = firstMayRaise(conjuncts, d->output_schema);

    std::vector<std::unique_ptr<Expr>> entering, staying;
    const char* decline = nullptr;
    for (size_t i = 0; i < conjuncts.size(); ++i) {
        if (i >= frozen) {
            decline = "predicate can raise";
            staying.push_back(std::move(conjuncts[i]));
        } else if (remapOntoDerivedBody(conjuncts[i].get(), *d)) {
            entering.push_back(std::move(conjuncts[i]));
        } else {
            decline = "column does not resolve against the body";
            staying.push_back(std::move(conjuncts[i]));
        }
    }

    // REPORTED, on the same argument `18af84f` added `join-ordering=skipped` on:
    // a decision was available here and was refused, and a reader of --explain
    // cannot otherwise tell "there was nothing to push" from "there was, and we
    // did not". Only a REFUSAL is stamped — a body that took every conjunct says
    // nothing, so every pre-existing --explain string is byte-identical.
    if (decline) d->pushdown_decision = std::string("pushdown=skipped (") + decline + ")";

    if (!entering.empty()) {
        d->children[0] = std::make_unique<LogicalFilter>(std::move(d->children[0]),
                                                         conjoinAll(std::move(entering)));
    }
    return filterOnto(std::move(derived), std::move(staying), catalog);
}

// Re-enter every subtree hanging off a join spine that is NOT part of the spine:
// each relation leaf, and a semi/anti join's body. Twin of the function of the
// same name in join_enumeration.cc, and for the same reason — see
// PredicatePushdown::apply below.
void applyToSpineLeaves(LogicalPlanNode* node, const Catalog& catalog) {
    auto* join = static_cast<LogicalJoin*>(node);
    if (join->children[0]->type == LogicalNodeType::JOIN)
        applyToSpineLeaves(join->children[0].get(), catalog);
    else
        join->children[0] = PredicatePushdown::apply(std::move(join->children[0]), catalog);
    join->children[1] = PredicatePushdown::apply(std::move(join->children[1]), catalog);
}

} // namespace

std::unique_ptr<LogicalPlanNode> PredicatePushdown::apply(std::unique_ptr<LogicalPlanNode> node,
                                                          const Catalog& catalog) {
    // Only a FILTER whose direct child is a JOIN is a pushable WHERE. A HAVING
    // filter sits above an AGGREGATE and is never rewritten.
    if (node->type == LogicalNodeType::FILTER &&
        node->children[0]->type == LogicalNodeType::JOIN) {
        auto* f = static_cast<LogicalFilter*>(node.release());
        node = pushIntoJoin(std::unique_ptr<LogicalFilter>(f), catalog);
        // SEAM AUDIT PASS 2, B-2. This used to `return` here, so a derived or
        // subquery body below a joining outer block was never visited and its own
        // FILTER-over-JOIN was never rewritten — the same silent decline
        // JoinEnumeration::apply had, in the same shape and found in the same
        // pass. Descending into the SPINE ITSELF would be wrong (and wasted):
        // pushIntoJoin has already routed every conjunct it can, and the
        // leftover/residual bookkeeping is written for one visit. The leaves are
        // what was never reached.
        LogicalPlanNode* spine = node->type == LogicalNodeType::FILTER
                               ? node->children[0].get() : node.get();
        applyToSpineLeaves(spine, catalog);
        return node;
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

    // SEAM AUDIT PASS 3, B3-3. A conjunct routed to a derived relation used to
    // stop above it in EVERY shape, silently. It now enters the body, and the
    // FILTER-over-PROJECT rule below carries it the rest of the way — which is
    // where the 9.4x lives, since the cost was the body's projection
    // materializing rows the filter immediately discards.
    if (node->type == LogicalNodeType::FILTER &&
        node->children[0]->type == LogicalNodeType::DERIVED) {
        auto* f = static_cast<LogicalFilter*>(node.release());
        node = pushIntoDerived(std::unique_ptr<LogicalFilter>(f), catalog);
        // fall through to the child loop: the FILTER just planted inside the body
        // is reached there, as is anything left above.
    }

    // A filter over a row-wise projection descends when every column it names is
    // a plain passthrough (remapThroughProject holds the argument and the
    // refusal). REACHED ONLY FROM A DERIVED BODY today: LogicalPlanBuilder puts
    // WHERE below the project and HAVING above the aggregate, so no top-level
    // plan has this shape — which bounds this rule's blast radius to exactly the
    // construct B3-3 is about.
    else if (node->type == LogicalNodeType::FILTER &&
             node->children[0]->type == LogicalNodeType::PROJECT) {
        auto* f = static_cast<LogicalFilter*>(node.get());
        auto* project = static_cast<LogicalProject*>(f->children[0].get());
        std::vector<std::unique_ptr<Expr>> conjuncts;
        splitConjuncts(std::move(f->predicate), conjuncts);
        const size_t frozen = firstMayRaise(conjuncts, project->output_schema);

        // SEAM AUDIT PASS 4, P4-B1. THE SECOND SCREEN, and the one the other
        // three moves do not need. Descending moves the filter BELOW the
        // projection, so every expression in the SELECT LIST is evaluated on
        // fewer rows — including the ones the conjunct never names, which is
        // exactly why screening the conjunct is not enough here. Measured before
        // this existed, on the shipped catalog:
        //
        //   SELECT COUNT(*) FROM (SELECT l.lap_id, l.lap_id * 1000000000000000
        //                         AS big FROM laps l) x WHERE x.lap_id < 100
        //     optimized -> 99      --no-optimize -> Error: integer overflow
        //
        // `lap_id` is a plain passthrough, so remapThroughProject's own
        // precondition was satisfied while `big` was the expression whose row
        // count changed. All-or-nothing: one raising select-list expression
        // stops the whole descent, because the filter lands in one place.
        //
        // Screened against the projection's INPUT schema — that is where its
        // expressions are evaluated. Type-aware, which is what makes this
        // affordable: a derived body computing `l.speed * 2` is the ordinary
        // case and DOUBLE arithmetic cannot raise, so it still descends. Under
        // the old operator-only screen this rule would have cost B3-3's whole
        // 9.4x on precisely the body shape it was written for.
        const Schema& project_input = project->children[0]->output_schema;
        bool projection_total = true;
        for (const auto& e : project->exprs) {
            if (exprMayRaise(e.get(), project_input)) { projection_total = false; break; }
        }

        std::vector<std::unique_ptr<Expr>> descending, staying;
        for (size_t i = 0; i < conjuncts.size(); ++i) {
            if (projection_total && i < frozen
                && remapThroughProject(conjuncts[i].get(), *project))
                descending.push_back(std::move(conjuncts[i]));
            else
                staying.push_back(std::move(conjuncts[i]));
        }
        if (!descending.empty()) {
            project->children[0] = std::make_unique<LogicalFilter>(
                std::move(project->children[0]), conjoinAll(std::move(descending)));
        }
        if (staying.empty()) {
            node = std::unique_ptr<LogicalPlanNode>(f->children[0].release());
        } else {
            f->predicate = conjoinAll(std::move(staying));
        }
        // A conjunct refused HERE is not stamped, and the argument first written
        // for that was FALSE (seam audit pass 4, P4-3): it claimed the refusal is
        // "readable from the plan" because --explain draws the filter above the
        // project's select list. It does not — LogicalProject::explain prints
        // OUTPUT COLUMN NAMES, never the expressions, so `LogicalProject [t, s2]`
        // is byte-identical whether `s2` is `speed * 2 AS s2` (refused) or a
        // passthrough (descended). What is true, and is all that is claimed now:
        // a LogicalFilter surviving directly above a LogicalProject in the
        // OPTIMIZED plan means at least one conjunct was refused, since a fully
        // descended filter is removed outright three lines above. WHICH reason
        // is not readable, and the projection-totality refusal added for P4-B1
        // is not readable at all. That is a real gap, left open deliberately
        // rather than by omission: a stamp needs somewhere to live, and
        // LogicalProject has no `pushdown_decision` field the way LogicalDerived
        // does. If a fifth move is added here, add the field first.
        //
        // fall through to the child loop, which reaches the new filter.
    }

    for (auto& child : node->children) child = apply(std::move(child), catalog);
    return node;
}
