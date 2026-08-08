#pragma once

#include "common/schema.h"
#include "common/value.h"
#include "parser/ast.h"
#include "execution/evaluator.h"
#include <algorithm>
#include <numeric>
#include <vector>

// THE SORT COMPARATOR, SHARED BY BOTH ENGINES.
//
// `SortNode` (Volcano, planner/plan_nodes.cc) and `VecSortNode`
// (execution/vec_sort_node.cc) call this and nothing else. They used to hold two
// byte-identical lambdas; the seam audit (pass 2, E-1) named that as the thing a
// future edit would drift, and the rule below is only worth anything if BOTH
// engines obey it — a tie-break one engine applies and the other does not is not
// a fix, it is the same divergence with a new cause.
//
// ---------------------------------------------------------------------------
// WHY THERE IS A TIE-BREAK BELOW THE DECLARED KEYS
//
// SQL does not say which row survives a `LIMIT` cut when the `ORDER BY` is not a
// total order. Both engines therefore used to answer with whatever their INPUT
// order happened to be — `std::stable_sort` propagates it — and input order is a
// function of the PLAN, not of the query:
//
//   * a hash join emits probe-major, so the build-side choice reverses it, and
//     the two engines pick the build side by different rules (Volcano from raw
//     table row counts, planner.cc; the vectorized builder from post-pushdown
//     cardinality ESTIMATES and real row widths, vectorized_plan_builder.cc);
//   * `HashAggregateNode` / `VecHashAggregateNode` emit groups in first-encounter
//     order, which is the join's output order;
//   * `JoinEnumeration` reorders the join spine under the optimizer and not
//     under `--no-optimize`.
//
// Measured consequence before this rule existed (audit A1-repro / A8): one query
// returned `{AlphaTauri, Alpine, McLaren}` on Volcano and `{RedBull, AlphaTauri,
// McLaren}` on the optimized vectorized path, and a scalar subquery of the same
// shape — materialized to a `Literal` — turned that into `COUNT(*)` = 977 versus
// 1536. The project asserts `optimized == --no-optimize`, so that is a defect
// even though every one of those answers is legal SQL.
//
// The fix is NOT to constrain the optimizer's build-side choice. It is to make
// the surviving row independent of it.
//
// ---------------------------------------------------------------------------
// THE RULE, AND WHAT IT DOES AND DOES NOT GUARANTEE
//
// When every declared key ties, compare the whole row, column by column, in the
// CANONICAL column order `tieBreakOrder` computes, ascending. Properties:
//
//   * It is a function of the row's VALUES and of each column's QUERY IDENTITY.
//     It cannot consult arrival order, hash bucket layout, chunk boundaries,
//     which side built the hash table, or the POSITION a column happens to
//     occupy in the plan's row — precisely the things that differ between the
//     legs.
//   * It is a total order on DISTINGUISHABLE rows. Rows that remain tied after it
//     are equal in every column, so whichever survives the cut, the result is the
//     same. (This is the audit's "material tie" notion: an immaterial tie cannot
//     change an answer, so it does not need breaking.)
//   * Always ascending, ignoring `desc`. The tie-break is not a key the user
//     wrote, so its direction is arbitrary; picking one and stating it is what
//     makes it reproducible.
//
// COST: only where the declared keys tie. A distinct-key sort pays one extra
// `compareForSort` on the first column of the tie-break — no evaluation, no
// allocation — and stops. Rows that do tie pay up to one `compareForSort` per
// column, against the `evaluate()` call per key per comparison the declared keys
// already cost, which is far more expensive. `tieBreakOrder` is computed ONCE
// per sort by the caller, never per comparison.
//
// !! WHY THE COMPARISON ORDER IS NOT THE SCHEMA'S ORDER (seam audit pass 3:
// engine E-9, join-chain B3-1, optimizer B3-1 — one defect, three routes).
// The paragraph that stood here stated the precondition as "the sort's INPUT row
// must be the same in every mode", and that is FALSE the moment a multi-way join
// sits under the sort. `JoinEnumeration::rebuild` builds the merged schema in
// the DP's CHOSEN order — its own comment concedes that a slot-sorted canonical
// order "is not available (invariant 1)", because `VecHashJoinNode`'s two output
// blocks must stay contiguous. So the two legs hand this comparator the same row
// VALUES as a PERMUTED SEQUENCE, and a lexicographic order over a permuted tuple
// is a DIFFERENT TOTAL ORDER. Measured: a 3-relation TPC-H join with
// `ORDER BY n_regionkey LIMIT 5` returned five ALGERIA suppliers optimized and
// ALGERIA/MOZAMBIQUE/MOROCCO/MOROCCO/ALGERIA under `--no-optimize`. Both legs
// ran the tie-break; both were deterministic; they disagreed.
//
// The precondition is therefore restated as the thing the code now enforces:
// THE COMPARISON ORDER MUST BE DERIVED FROM COLUMN IDENTITY, NEVER FROM COLUMN
// POSITION. `tieBreakOrder` derives it from `(relation_slot, name)`:
//   * `relation_slot` is the BINDER's range-table position — 0 for FROM, i+1 for
//     the i-th JOIN in WRITTEN order. `rebuild` re-stamps it as `order[k]`, i.e.
//     with that same written-order slot, and copies whole `ColumnDef`s, so the
//     SET of `(slot, name)` pairs is identical on both legs and only the
//     SEQUENCE differs. Sorting by the pair recovers the sequence.
//   * the pair is unique on any schema a sort can see: two relations cannot
//     share a slot, and one relation cannot repeat a column name (the catalog
//     refuses duplicate names, and so does `derivedRelationSchema`). Where it is
//     NOT unique — a projected schema with two identically named output columns —
//     the sort is stable, so those columns keep their schema order, and a
//     PROJECTED schema's order is a function of the SELECT list rather than of
//     the plan.
//
// !! THE OTHER PLACE THE PRECONDITION FAILED, AND WHY THAT IS NOW A FACT RATHER
// THAN AN ARGUMENT. Row storage under Volcano used to hand `SeqScanNode` the FULL
// catalog schema while every columnar mode handed it the narrowed
// `buildScanSchema`, so the two legs tie-broke over different column sets. The
// paragraph that stood here argued the difference was immaterial (the extra
// columns are ones the query names nowhere, so rows the narrow leg leaves tied
// project identically) and called that "the weakest thing here", pinning it
// behaviourally with the DISTINCT entry in `ENGINE_AGREEMENT_QUERIES` — which
// agreed only because the first discriminating column happened to be
// `driver_id` in both legs.
//
// Seam audit pass 2 CLOSED the asymmetry instead of continuing to argue about
// it: `Planner::plan` now narrows the ROWS as well as the schema (`narrowRows`,
// planner.cc), because the row path returns `&rows_[cursor_]` verbatim and a
// narrowed schema over a wide row would mis-index every column. Both legs hand
// `SeqScanNode` the same schema, and this comparator sees one column set in
// every mode. The DISTINCT entry stays — it is still the only entry whose sort
// input is a raw join row, so it is still where a future re-divergence would
// show up — but it is now a regression test rather than a standing hazard.
//
// !! AND THE CUT THIS COMPARATOR CANNOT REACH AT ALL. Nothing requires a sort
// beneath a `LIMIT`. `LIMIT n` with no `ORDER BY` cuts a join's raw probe order,
// which is plan-dependent for exactly the reasons listed above, and no edit to
// this file can see it. That half is closed where the cut is — `LogicalPlanBuilder`
// and `Planner::plan` insert a sort with NO declared keys (so `rowLess` falls
// straight through to this tie-break) directly beneath a `LIMIT` whose input
// order is not already plan-independent. See `orderIsPlanStable` in
// planner/logical_plan.cc.
namespace sort_comparator {

// compareForSort, widened so it can never throw. compareForSort refuses STRING
// against a number (as every comparison in the engine does), which is correct for
// a user-written key — a comparator that throws mid-`stable_sort` is not. A
// declared ORDER BY key is type-checked by `inferExprType` at plan time; a
// tie-break column is not chosen by anyone, so it gets the total, total-in-all-
// cases version: any number sorts before any string.
inline int compareForTieBreak(const Value& a, const Value& b) {
    if (a.isNull() || b.isNull()) return compareForSort(a, b);
    const bool a_str = a.type() == TypeId::STRING;
    const bool b_str = b.type() == TypeId::STRING;
    if (a_str != b_str) return a_str ? 1 : -1;
    return compareForSort(a, b);
}

// The CANONICAL order in which the tie-break visits a schema's columns: column
// INDICES sorted by `(relation_slot, name)`, i.e. by what the column IS in the
// query rather than by where the plan happened to put it. See the header for why
// schema order is not usable and why the pair is both stable and (where it
// matters) unique. Compute once per sort; it allocates.
inline std::vector<int> tieBreakOrder(const Schema& schema) {
    std::vector<int> order(static_cast<size_t>(schema.size()));
    std::iota(order.begin(), order.end(), 0);
    // stable: identical (slot, name) pairs keep schema order, which is a
    // function of the SELECT list wherever such pairs can occur.
    std::stable_sort(order.begin(), order.end(), [&schema](int x, int y) {
        const ColumnDef& a = schema.column(x);
        const ColumnDef& b = schema.column(y);
        if (a.relation_slot != b.relation_slot) return a.relation_slot < b.relation_slot;
        return a.name < b.name;
    });
    return order;
}

// The strict weak ordering handed to std::stable_sort by both engines.
//
// compareForSort, not Value::operator< — the SQL operators return false for every
// comparison against NULL, which makes NULL equivalent to every value and the
// equivalence non-transitive. That is not a strict weak ordering, so
// stable_sort's behaviour is undefined: it inverted non-NULL keys and dropped
// rows under LIMIT. See the comment on compareForSort in value.h.
//
// `tie_order` must be `tieBreakOrder(schema)`. It is a parameter rather than
// something this function derives so that the O(n log n) it costs is paid once
// per sort instead of once per comparison — and so that the caller cannot
// silently fall back to schema order, which is the defect this closes.
inline bool rowLess(const std::vector<OrderByItem>& order_by,
                    const Schema& schema,
                    const std::vector<int>& tie_order,
                    const Row& a,
                    const Row& b) {
    for (const auto& item : order_by) {
        int c = compareForSort(evaluate(item.expr.get(), a, schema),
                               evaluate(item.expr.get(), b, schema));
        if (c != 0) return item.desc ? c > 0 : c < 0;
    }
    // every declared key ties — see the header comment. `tie_order`'s entries are
    // schema indices by construction; the bound is on the ROWS, defensively, for
    // the same reason the schema-order loop carried one.
    const int n = static_cast<int>(std::min(a.size(), b.size()));
    for (int i : tie_order) {
        if (i >= n) continue;
        int c = compareForTieBreak(a[i], b[i]);
        if (c != 0) return c < 0;
    }
    return false;
}

// One-shot form, for a single comparison (and for the unit tests, which compare
// two rows at a time). Never call it from inside a sort — it rebuilds the
// canonical order on every comparison.
inline bool rowLess(const std::vector<OrderByItem>& order_by,
                    const Schema& schema,
                    const Row& a,
                    const Row& b) {
    return rowLess(order_by, schema, tieBreakOrder(schema), a, b);
}

}  // namespace sort_comparator
