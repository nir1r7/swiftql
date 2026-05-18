#include "planner.h"

// collecting column names from expressions
static void collectCols(const Expr* expr, std::unordered_set<std::string>& out){
    if (!expr) return;
    if (auto* cr = dynamic_cast<const ColumnRef*>(expr)){
        out.insert(cr->column_name);
        return;
    }
    
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)){
        collectCols(bin->left.get(), out);
        collectCols(bin->right.get(), out);
        return;
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)){
        collectCols(agg->argument.get(), out);
        return;
    }
    if (auto* isnull = dynamic_cast<const IsNullExpr*>(expr)){
        collectCols(isnull->operand.get(), out);
        return;
    }
}

// build schema using only required columns
static Schema narrowSchema(const Schema& full, const std::unordered_set<std::string>& required) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < full.size(); ++i) {
        if (required.count(full.column(i).name))
            cols.push_back(full.column(i));
    }
    return Schema(cols);
}

std::unique_ptr<PlanNode> Planner::plan(SelectStatement stmt, const Catalog& catalog, std::unordered_map<std::string, std::vector<Row>> table_rows, std::unordered_map<std::string, ColumnarTable> columnar_tables){
    // validate
    Validator::validate(stmt, catalog);

    const TableMetadata& meta = catalog.getTable(stmt.from_table);

    // get required columns only
    std::unordered_set<std::string> required;
    if (stmt.select_star) {
        for (const auto& col : meta.schema.columns()){
            required.insert(col.name);
        }
    } else {
        for (const auto& expr : stmt.select_list){
            collectCols(expr.get(), required);
        }
        collectCols(stmt.where.get(), required);

        for (const auto& col_name : stmt.group_by){
            required.insert(col_name);
        }
        collectCols(stmt.having.get(), required);

        for (const auto& expr : stmt.order_by){
            collectCols(expr.get(), required);
        }
        if (stmt.join.has_value()){
            collectCols(stmt.join->condition.get(), required);
        }
    }

    // narrow down schema
    Schema scan_schema = narrowSchema(meta.schema, required);

    // build seqScan (bottom of tree) using narrowed schema
    std::unique_ptr<PlanNode> node;
    if (columnar_tables.count(stmt.from_table) > 0) {
        node = std::make_unique<SeqScanNode>(stmt.from_table, std::move(columnar_tables.at(stmt.from_table)), scan_schema);
    } else {
        node = std::make_unique<SeqScanNode>(stmt.from_table, std::move(table_rows.at(stmt.from_table)), meta.schema);
    }

    // hash join
    if (stmt.join.has_value()){
        const TableMetadata& join_meta = catalog.getTable(stmt.join->join_table);

        Schema right_scan_schema = narrowSchema(join_meta.schema, required);

        std::unique_ptr<PlanNode> right;
        if (columnar_tables.count(stmt.join->join_table) > 0) {
            right = std::make_unique<SeqScanNode>(stmt.join->join_table, std::move(columnar_tables.at(stmt.join->join_table)), right_scan_schema);
        } else {
            right = std::make_unique<SeqScanNode>(stmt.join->join_table, std::move(table_rows.at(stmt.join->join_table)), join_meta.schema);
        }

        // build merged schema for join output
        std::vector<ColumnDef> merged_cols = node->outputSchema().columns();
        for (const auto& col : right->outputSchema().columns()) {
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
        node = std::make_unique<FilterNode>(std::move(node), std::move(stmt.where));
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
        node = std::make_unique<HavingNode>(std::move(node), std::move(stmt.having));
    }

    // project; SELECT list — placed before Distinct/Sort/Limit so DISTINCT deduplicates projected rows
    if (stmt.select_star) {
        const Schema& child_schema = node->outputSchema();
        std::vector<std::unique_ptr<Expr>> star_exprs;
        for (const auto& col : child_schema.columns()) {
            auto ref = std::make_unique<ColumnRef>();
            ref->column_name = col.name;
            star_exprs.push_back(std::move(ref));
        }
        node = std::make_unique<ProjectNode>(std::move(node), std::move(star_exprs), child_schema);
    } else {
        auto project_schema = buildProjectSchema(stmt, node->outputSchema());
        node = std::make_unique<ProjectNode>(
            std::move(node),
            std::move(stmt.select_list),
            project_schema);
    }

    // DISTINCT — runs on projected rows
    if (stmt.distinct) {
        node = std::make_unique<DistinctNode>(std::move(node));
    }

    // sort (ORDER BY)
    if (!stmt.order_by.empty()) {
        node = std::make_unique<SortNode>(std::move(node), std::move(stmt.order_by));
    }

    // limit
    if (stmt.limit.has_value()) {
        node = std::make_unique<LimitNode>(std::move(node), stmt.limit.value());
    }

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