#pragma once

#include "catalog/catalog.h"
#include "planner/logical_plan.h"
#include <string>
#include <vector>

// documented fallback selectivities, applied when statistics cannot answer
// a predicate (missing stats, aggregate outputs, unrecognized expression
// shapes). Values follow the System R defaults.
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
    // Schema::indexOf(name, slot) resolution order
    const ColumnStatsEntry* find(const std::string& name, int slot) const;
};

// annotates every logical node's estimated_rows in place, bottom-up.
// runs after LogicalPlanBuilder::build and before VectorizedPlanBuilder::build
// (lowering consumes the logical tree, so estimation must precede it)
class CardinalityEstimator {
    public:
        static void estimate(LogicalPlanNode& root, const Catalog& catalog);

        // fraction of input rows a predicate keeps, in [0, 1]. Public so the
        // Week 21 pushdown pass can order scan-local conjuncts by expected work.
        static double selectivity(const Expr* pred, const StatsContext& ctx);

    private:
        // recursive worker: stamps node.estimated_rows, returns the column
        // stats visible above this node
        static StatsContext estimateNode(LogicalPlanNode& node, const Catalog& catalog);
};
