#pragma once

#include "planner/logical_plan.h"
#include "planner/vec_plan_node.h"
#include "storage/columnar_table.h"
#include <memory>
#include <string>
#include <unordered_map>

// lowers a logical plan into a vectorized physical operator tree
// consumes both arguments: expressions are moved out of the logical nodes
// into the physical operators, and tables are moved into the scan nodes —
// the logical tree must not be reused after this call
// physical-only decisions (build/probe side, pruning-hint routing) are made
// here, not in the logical plan
class VectorizedPlanBuilder {
    public:
        static std::unique_ptr<VecPlanNode> build(
            std::unique_ptr<LogicalPlanNode> logical,
            std::unordered_map<std::string, ColumnarTable> columnar_tables);
};
