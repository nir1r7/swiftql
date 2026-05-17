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
    private:
        // build the schema of the project node's output based on the select statement and input table schema
        static Schema buildProjectSchema(const SelectStatement& stmt, const Schema& table_schema);

        // extract aggregate specifications from the select statement (for building the HashAggregateNode)
        static std::vector<AggregateSpec> extractAggregates(const SelectStatement& stmt);

        // build the schema of the aggregate node's output based on the select statement and input table schema
        static Schema buildAggregateSchema(const SelectStatement& stmt, const Schema& table_schema);
};