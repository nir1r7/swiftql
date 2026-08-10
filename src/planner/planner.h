#pragma once

#include "plan_nodes.h"
#include "validator.h"
#include "parser/ast.h"
#include "catalog/catalog.h"
#include <memory>
#include <unordered_map>
#include <string>

class Planner {
    public:
        // validates and builds a plan tree from a parsed statement
        // table_rows must contain pre-loaded rows for every table the query touches
        // main entry point: returns owning pointer to root node
        static std::unique_ptr<PlanNode> plan(
            SelectStatement stmt,
            const Catalog& catalog,
            std::unordered_map<std::string, std::vector<Row>> table_rows,
            // SHARED with the scan nodes, not moved into them: a self-join reads
            // one image of the table twice rather than copying it, and a
            // subquery body naming a table the outer query also reads shares it
            // too. See SeqScanNode's constructor.
            std::unordered_map<std::string, std::shared_ptr<const ColumnarTable>> columnar_tables = {}
        );
        // schema helpers (buildScanSchema/buildProjectSchema/buildAggregateSchema/
        // extractAggregates) live in the logical layer — see logical_plan.h
};