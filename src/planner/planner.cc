#include "planner.h"

std::unique_ptr<PlanNode> Planner::plan(const SelectStatement& stmt, const Catalog& catalog){
    // validate
    Validator::validate(stmt, catalog);

    const TableMetadata& meta = catalog.getTable(stmt.from_table);

    // load rows and build seqScan (bottom of tree)
    auto rows = CSVLoader::load(meta.filepath, meta.schema);
    std::unique_ptr<PlanNode> node = std::make_unique<SeqScanNode>(std::move(rows), meta.schema);

    // hash join
    if (stmt.join.has_value()){
        const TableMetadata& join_meta = catalog.getTable(stmt.join->join_table);
        auto join_rows = CSVLoader::load(join_meta.filepath, join_meta.schema);
        auto right = std::make_unique<SeqScanNode>(std::move(join_rows), join_meta.schema);

        // build merged schema for join output
        std::vector<ColumnDef> merged_cols = meta.schema.columns();
        for (const auto& col : join_meta.schema.columns()) {
            merged_cols.push_back(col);
        }
        Schema merged_schema(merged_cols);

        // extract join column names from ON condition
        // for phase 1 assume ON is always col = col
        std::string left_col, right_col;
        if (auto* bin = dynamic_cast<BinaryExpr*>(stmt.join->condition.get())) {
            if (auto* lc = dynamic_cast<ColumnRef*>(bin->left.get()))
                left_col = lc->column_name;
            if (auto* rc = dynamic_cast<ColumnRef*>(bin->right.get()))
                right_col = rc->column_name;
        }

        node = std::make_unique<HashJoinNode>(std::move(node), std::move(right), left_col, right_col, merged_schema);
    }

    // fliter (WHERE)
    if (stmt.where) {
        // clone predicate
        node = std::make_unique<FilterNode>(std::move(node), std::move(const_cast<SelectStatement&>(stmt).where));
    }

    // HashAgrgegate (GROUP BY + aggregates)
    bool has_aggregates = false;
    for (auto& expr : stmt.select_list) {
        if (dynamic_cast<AggregateExpr*>(expr.get())) {
            has_aggregates = true;
            break;
        }
    }

    if (!stmt.group_by.empty() || has_aggregates) {
        auto agg_schema = buildAggregateSchema(stmt, node->outputSchema());
        node = std::make_unique<HashAggregateNode>(std::move(node), stmt.group_by, extractAggregates(stmt), agg_schema);
    }

    // HAVING
    if (stmt.having) {
        node = std::make_unique<HavingNode>(std::move(node), std::move(const_cast<SelectStatement&>(stmt).having));
    }

    // DISTINCT
    if (stmt.distinct) {
        node = std::make_unique<DistinctNode>(std::move(node));
    }

    // sort (ORDER BY)
    if (!stmt.order_by.empty()) {
        node = std::make_unique<SortNode>(std::move(node), stmt.order_by);
    }

    // limit
    if (stmt.limit.has_value()) {
        node = std::make_unique<LimitNode>(std::move(node), stmt.limit.value());
    }

    // project; SELECT list (top)
    auto project_schema = buildProjectSchema(stmt, meta.schema);
    node = std::make_unique<ProjectNode>(
        std::move(node),
        std::move(const_cast<SelectStatement&>(stmt).select_list),
        project_schema);

    return node;
}


Schema Planner::buildProjectSchema(const SelectStatement& stmt, const Schema& table_schema){
    std::vector<ColumnDef> cols;

    for (const auto& expr : stmt.select_list) {
        if (auto* col = dynamic_cast<ColumnRef*>(expr.get())) {
            int idx = table_schema.indexOf(col->column_name);
            if (idx >= 0) {
                cols.push_back(table_schema.column(idx));
            } else {
                cols.push_back({col->column_name, TypeId::STRING});
            }
        } else if (auto* agg = dynamic_cast<AggregateExpr*>(expr.get())) {
            // aggregates on numeric columns produce DOUBLE
            // COUNT always produces INT
            TypeId result_type = (agg->function_name == "COUNT") ? TypeId::INT : TypeId::DOUBLE;
            
            std::string col_name = agg->function_name + "(";
            col_name += agg->is_star ? "*" : dynamic_cast<ColumnRef*>(agg->argument.get())->column_name;
            col_name += ")";
            cols.push_back({col_name, result_type});
        }
    }

    return Schema(cols);
}


std::vector<AggregateSpec> Planner::extractAggregates(const SelectStatement& stmt){
    std::vector<AggregateSpec> specs;

    for (const auto& expr : stmt.select_list) {
        if (auto* agg = dynamic_cast<AggregateExpr*>(expr.get())) {
            AggregateSpec spec;
            spec.function = agg->function_name;
            spec.is_star = agg->is_star;

            if (!agg->is_star && agg->argument) {
                if (auto* col = dynamic_cast<ColumnRef*>(agg->argument.get())) {
                    spec.column = col->column_name;
                }
            }

            specs.push_back(spec);
        }
    }

    return specs;
}


Schema Planner::buildAggregateSchema(const SelectStatement& stmt, const Schema& table_schema){
    std::vector<ColumnDef> cols;

    // group-by colums in order
    for (const auto& col_name : stmt.group_by) {
        int idx = table_schema.indexOf(col_name);
        if (idx >= 0) {
            cols.push_back(table_schema.column(idx));
        }
    }

    // one output column per aggregate in the SELECT list
    for (const auto& expr : stmt.select_list) {
        if (auto* agg = dynamic_cast<AggregateExpr*>(expr.get())) {
            TypeId result_type = (agg->function_name == "COUNT") ? TypeId::INT : TypeId::DOUBLE;

            std::string col_name = agg->function_name + "(";
            if (agg->is_star) {
                col_name += "*";
            } else if (auto* col = dynamic_cast<ColumnRef*>(agg->argument.get())) {
                col_name += col->column_name;
            }
            col_name += ")";

            cols.push_back({col_name, result_type});
        }
    }

    return Schema(cols);
}

