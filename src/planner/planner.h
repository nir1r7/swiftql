#pragma once

#include "plan_nodes.h"
#include "validator.h"
#include "parser/ast.h"
#include "catalog/catalog.h"
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
            std::unordered_map<std::string, ColumnarTable> columnar_tables = {}
        );
        // schema helpers (buildScanSchema/buildProjectSchema/buildAggregateSchema/
        // extractAggregates) live in the logical layer — see logical_plan.h
};