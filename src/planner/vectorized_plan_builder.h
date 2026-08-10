#pragma once

#include "planner/logical_plan.h"
#include "planner/vec_plan_node.h"
#include "storage/columnar_table.h"
#include <memory>
#include <string>
#include <unordered_map>

// lowers a logical plan into a vectorized physical operator tree
// consumes the logical argument: expressions are moved out of the logical nodes
// into the physical operators — the logical tree must not be reused after this
// call. The TABLES are SHARED with the scan nodes rather than moved into them,
// so a self-join lowers to two scans over one image of the table instead of
// copying it (see VecScanNode's constructor for the measurement). catalog is
// borrowed (read-only) for build-side cost estimation (per-column avg_width
// stats).
// physical-only decisions (build/probe side, pruning-hint routing) are made
// here, not in the logical plan
class VectorizedPlanBuilder {
    public:
        // `result_int_type_observable` — "somebody outside this plan is going to
        // DIVIDE with the value this plan returns, using an INTEGER as the other
        // operand". Default false, which is every top-level query: the divisions
        // a query makes with its own columns are found by the builder's own
        // walk. It is set only by the SUBQUERY RUNNER, because an uncorrelated
        // scalar body is a SEPARATE build and no walk can see across the two —
        // see the comment in build() and vec_types.h's IntNarrowing.
        static std::unique_ptr<VecPlanNode> build(
            std::unique_ptr<LogicalPlanNode> logical,
            std::unordered_map<std::string, std::shared_ptr<const ColumnarTable>> columnar_tables,
            const Catalog& catalog,
            bool result_int_type_observable = false);
};
