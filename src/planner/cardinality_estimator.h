#pragma once

#include "catalog/catalog.h"
#include "planner/logical_plan.h"
#include <string>
#include <vector>

// documented fallback selectivities, applied when statistics cannot answer
// a predicate (missing stats, aggregate outputs, unrecognized expression
// shapes). Values follow the System R defaults.
// FALLBACK_EQ_SELECTIVITY also serves as the assumed null fraction for
// IS [NOT] NULL without stats (IS NULL -> 0.1, IS NOT NULL -> 0.9) and as the
// equality mass for guaranteed-match range edges when NDV is unknown.
constexpr double FALLBACK_EQ_SELECTIVITY    = 0.1;       // equality, no usable NDV
constexpr double FALLBACK_RANGE_SELECTIVITY = 1.0 / 3.0; // range, no numeric min/max
constexpr double FALLBACK_SELECTIVITY       = 0.5;       // any other predicate
constexpr int64_t FALLBACK_ROW_COUNT        = 1000;      // scan with no stats

// statistics for one column visible at a plan node; table_rows is the base
// table's row count, needed to turn null_count into a null fraction
struct ColumnStatsEntry {
    std::string name;
    int relation_slot = 0;
    const ColumnStats* stats = nullptr; // owned by Catalog; valid for the pass
    int64_t table_rows = 0;
};

// the set of columns whose base-table statistics are visible at a plan node.
// built by scans, merged by joins, emptied by aggregates.
struct StatsContext {
    std::vector<ColumnStatsEntry> entries;

    // slot-first lookup with bare-name fallback, mirroring
    // Schema::indexOf(name, slot) resolution order. The fallback is what makes
    // unbound refs (slot -1) and hand-built contexts resolvable, so it stays —
    // but it means a slot MISS silently returns some other relation's column of
    // the same name. Use findExact wherever the slot is a real relation
    // identity rather than a hint.
    const ColumnStatsEntry* find(const std::string& name, int slot) const;

    // Slot-exact lookup: no bare-name fallback, so a miss means "this relation
    // has no statistic for this column" instead of "here is a different
    // relation's". Returns nullptr for slot < 0, which has no relation to be
    // exact about.
    const ColumnStatsEntry* findExact(const std::string& name, int slot) const;

    // Lookup for a reference that carries a binder-assigned relation identity:
    // exact when a slot is present, bare-name only when it is not. Every caller
    // holding a real slot wants this — a merged context can hold one column
    // name at several slots, and the slot MISSES whenever the owning relation
    // has no TableStats (a stats-less scan contributes no entries at all), at
    // which point find() would hand back a different relation's column with no
    // signal. Every consumer already models "no statistic" with a fallback.
    const ColumnStatsEntry* findForRef(const std::string& name, int slot) const;
};

// Estimated output rows of an inner equi-join.
//
// `keys` are ORIENTED: from_col/from_slot address `left`, join_col addresses
// `right`. `left` may be a merged context holding one column name at several
// slots, which is why the left lookup is slot-EXACT (findForRef) — honouring the
// slot only when it happens to hit would make the disambiguation advisory.
// `right` is always exactly one relation (left-deep), so a bare-name match there
// is unambiguous and -1 asks for it deliberately.
//
// Shared by CardinalityEstimator's JOIN case and Week 28's join enumeration.
// The two MUST NOT hold separate copies: the search would then rank orderings
// under one model while --explain prints another, and the NDV rule this encodes
// was already corrected twice in Week 26 (the slot-exact left lookup, and
// have_ndv tracked separately from the product so an NDV of 1 stays a usable
// statistic instead of falling through to the no-stats branch).
//
// Returns the RAW expected value, which for keys with statistics is the pure
// product (∏rows / ∏ndv) and therefore a function of the joined SET alone,
// independent of the order the relations were added in. That is exactly what the
// join search's dynamic program needs: keeping one subplan per subset is only
// sound when the cost of FINISHING a subset depends on the subset alone. Apply
// flooredJoinCardinality on top wherever the value is STAMPED on a plan node.
double joinCardinality(double left_rows, double right_rows,
                       const std::vector<JoinKey>& keys,
                       const StatsContext& left, const StatsContext& right);

// The ≥1-row floor, as a separate step. Stamping sites apply it; the join search
// must not, because a per-step clamp makes a subset's row count depend on the
// path that reached it and destroys the DP's optimal substructure. See the .cc
// for the measured plans that motivated splitting the two apart.
double flooredJoinCardinality(double left_rows, double right_rows, double rows);

// annotates every logical node's estimated_rows in place, bottom-up.
// runs after LogicalPlanBuilder::build and before VectorizedPlanBuilder::build
// (lowering consumes the logical tree, so estimation must precede it).
// Stamped FILTER/JOIN estimates carry a ≥1-row floor (selectivities do not:
// a true 0 is still the best conjunct-ordering signal for pushdown)
class CardinalityEstimator {
    public:
        static void estimate(LogicalPlanNode& root, const Catalog& catalog);

        // fraction of input rows a predicate keeps, in [0, 1]. Public so the
        // Week 21 pushdown pass can order scan-local conjuncts by expected work.
        //
        // !! WHAT THIS NUMBER IS ALLOWED TO DECIDE, stated because the seam audit
        // (pass 3, B.1) found it was the one estimator entry point with NO stated
        // precondition at all, and B3-2 is what that cost. `estimate` above feeds
        // plan-SHAPE choices, where a wrong number is only a slow plan. THIS
        // function decides conjunct ORDER, and the columnar cascade evaluates the
        // right conjunct only over the left's survivors — so with per-row
        // evaluation not being total, a wrong number here used to decide whether
        // the query ERRORS. The rule is therefore not "this may be inaccurate":
        // it is that a conjunct that CAN RAISE must never be ranked at all.
        // PredicatePushdown enforces it (firstMayRaise), not this function, and
        // any new caller of selectivity() that reorders evaluation owes the same
        // screen.
        static double selectivity(const Expr* pred, const StatsContext& ctx);

        // Estimate ONE subtree in isolation: stamps estimated_rows bottom-up and
        // returns the column statistics visible above it. Week 28's join
        // enumeration needs both for every leaf of the join graph before it can
        // cost an ordering. Same function `estimate` drives, so a leaf costed
        // here and the same leaf stamped by the final whole-tree pass cannot
        // disagree.
        static StatsContext estimateSubtree(LogicalPlanNode& node, const Catalog& catalog);

    private:
        // recursive worker: stamps node.estimated_rows, returns the column
        // stats visible above this node
        static StatsContext estimateNode(LogicalPlanNode& node, const Catalog& catalog);
};
