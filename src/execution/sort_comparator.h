#pragma once

#include "common/schema.h"
#include "common/value.h"
#include "parser/ast.h"
#include "execution/evaluator.h"
#include <algorithm>
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
// When every declared key ties, compare the whole row, column by column, in
// schema order, ascending. Properties:
//
//   * It is a function of the row's VALUES alone. It cannot consult arrival
//     order, hash bucket layout, chunk boundaries, or which side built the hash
//     table — precisely the things that differ between the legs.
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
// already cost, which is far more expensive.
//
// THE PRECONDITION IT RESTS ON, STATED SO IT CAN BE CHECKED: the sort's INPUT
// row must be the same in every mode. It is, for every shape this can decide,
// because ORDER BY is planned directly above the aggregate/filter/join and below
// the projection in both builders, and an aggregate's output schema
// (`buildAggregateSchema`) is the same in all four modes. Where it is NOT the
// same is row storage under Volcano, which hands `SeqScanNode` the FULL table
// schema while every columnar mode hands it the narrowed `buildScanSchema`
// (planner.cc). Columns present only in the wide leg are, by construction,
// columns the query references nowhere — so two rows that the narrow leg leaves
// tied project to identical output rows and the difference is immaterial.
//
// That last argument is the weakest thing here, so it is pinned BEHAVIOURALLY
// rather than trusted: the DISTINCT entry in `ENGINE_AGREEMENT_QUERIES`
// (compare_against_sqlite.py) is the one entry whose sort input is a raw join
// row rather than an aggregate's output, so it is the one that compares the two
// legs over genuinely different column sets. It agrees today because the first
// discriminating column is `driver_id` in both. If a future change makes the
// wide leg's extra columns decide a cut, that entry goes red. `tests/
// test_sort_tiebreak.cc` pins the comparator's own rules; it cannot see this
// one, because it builds one schema.
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

// The strict weak ordering handed to std::stable_sort by both engines.
//
// compareForSort, not Value::operator< — the SQL operators return false for every
// comparison against NULL, which makes NULL equivalent to every value and the
// equivalence non-transitive. That is not a strict weak ordering, so
// stable_sort's behaviour is undefined: it inverted non-NULL keys and dropped
// rows under LIMIT. See the comment on compareForSort in value.h.
inline bool rowLess(const std::vector<OrderByItem>& order_by,
                    const Schema& schema,
                    const Row& a,
                    const Row& b) {
    for (const auto& item : order_by) {
        int c = compareForSort(evaluate(item.expr.get(), a, schema),
                               evaluate(item.expr.get(), b, schema));
        if (c != 0) return item.desc ? c > 0 : c < 0;
    }
    // every declared key ties — see the header comment
    const int n = std::min(schema.size(), static_cast<int>(std::min(a.size(), b.size())));
    for (int i = 0; i < n; ++i) {
        int c = compareForTieBreak(a[i], b[i]);
        if (c != 0) return c < 0;
    }
    return false;
}

}  // namespace sort_comparator
