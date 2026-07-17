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

        for (const auto& item : stmt.order_by){
            collectCols(item.expr.get(), required);
        }
        if (stmt.join.has_value()){
            collectCols(stmt.join->condition.get(), required);
        }
    }

    // narrow down schema
    Schema scan_schema = narrowSchema(meta.schema, required);

    // capture before std::move transfers ownership into SeqScanNode
    int from_row_count = columnar_tables.count(stmt.from_table) > 0 ? columnar_tables.at(stmt.from_table).num_rows : (int)table_rows.at(stmt.from_table).size();

    // build seqScan (bottom of tree) using narrowed schema
    std::unique_ptr<PlanNode> node;
    if (columnar_tables.count(stmt.from_table) > 0) {
        node = std::make_unique<SeqScanNode>(stmt.from_table, std::move(columnar_tables.at(stmt.from_table)), scan_schema, stmt.where.get());
    } else {
        node = std::make_unique<SeqScanNode>(stmt.from_table, std::move(table_rows.at(stmt.from_table)), meta.schema);
    }

    // hash join
    if (stmt.join.has_value()){
        const TableMetadata& join_meta = catalog.getTable(stmt.join->join_table);

        Schema right_scan_schema = narrowSchema(join_meta.schema, required);

        // capture before std::move transfers ownership into SeqScanNode
        int join_row_count = columnar_tables.count(stmt.join->join_table) > 0
            ? columnar_tables.at(stmt.join->join_table).num_rows
            : (int)table_rows.at(stmt.join->join_table).size();

        std::unique_ptr<PlanNode> right;
        if (columnar_tables.count(stmt.join->join_table) > 0) {
            right = std::make_unique<SeqScanNode>(stmt.join->join_table, std::move(columnar_tables.at(stmt.join->join_table)), right_scan_schema, nullptr);
        } else {
            right = std::make_unique<SeqScanNode>(stmt.join->join_table, std::move(table_rows.at(stmt.join->join_table)), join_meta.schema);
        }

        // extract join column names from ON condition
        std::string left_col, right_col, left_table, right_table;
        if (auto* bin = dynamic_cast<BinaryExpr*>(stmt.join->condition.get())) {
            if (auto* lc = dynamic_cast<ColumnRef*>(bin->left.get())) {
                left_col   = lc->column_name;
                left_table = lc->table_name;
            }
            if (auto* rc = dynamic_cast<ColumnRef*>(bin->right.get())) {
                right_col   = rc->column_name;
                right_table = rc->table_name;
            }
        }
        // If table qualifiers are present, route by table name so that
        // ON join_table.col = from_table.col is handled correctly.
        // Without qualifiers, fall back to positional assumption (left=FROM, right=JOIN).
        std::string from_col = left_col, join_col = right_col;
        if (!left_table.empty() && !right_table.empty()) {
            if (left_table == stmt.join->join_table)
                std::swap(from_col, join_col);
        }

        // put the smaller table on the build side (right_) to minimise hash table memory
        // Option A: output schema order follows probe side (see memory: hash-join-schema-ordering-debt)
        bool swap = from_row_count < join_row_count;
        if (swap) {
            // FROM table is smaller — FROM becomes build (right_), JOIN becomes probe (left_)
            std::vector<ColumnDef> merged_cols = right->outputSchema().columns();
            for (const auto& col : node->outputSchema().columns())
                merged_cols.push_back(col);
            Schema merged_schema(merged_cols);
            node = std::make_unique<HashJoinNode>(std::move(right), std::move(node), join_col, from_col, merged_schema);
        } else {
            // JOIN table is smaller (or equal) — JOIN stays build (right_), FROM stays probe (left_)
            std::vector<ColumnDef> merged_cols = node->outputSchema().columns();
            for (const auto& col : right->outputSchema().columns())
                merged_cols.push_back(col);
            Schema merged_schema(merged_cols);
            node = std::make_unique<HashJoinNode>(std::move(node), std::move(right), from_col, join_col, merged_schema);
        }
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

    // sort (ORDER BY) — must evaluate against pre-projection schema
    if (!stmt.order_by.empty()) {
        node = std::make_unique<SortNode>(std::move(node), std::move(stmt.order_by));
    }

    // project; SELECT list — placed after Sort so sort expressions resolve against full schema
    if (stmt.select_star) {
        const Schema& child_schema = node->outputSchema();
        std::vector<std::unique_ptr<Expr>> star_exprs;
        for (const auto& col : child_schema.columns()) {
            auto ref = std::make_unique<ColumnRef>();
            ref->column_name = col.name;
            ref->relation_slot = col.relation_slot; // preserve side so SELECT * on a self-join emits both sides
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
            // resolve slot-first (correct side on a join with shared names),
            // falling back to bare name against post-aggregate schemas
            int idx = col->relation_slot >= 0
                ? table_schema.indexOf(col->column_name, col->relation_slot)
                : -1;
            if (idx < 0) idx = table_schema.indexOf(col->column_name);
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
                    spec.relation_slot = col->relation_slot; // carry join side, e.g. AVG(l2.speed)
                }
            }

            specs.push_back(spec);
        }
    }

    return specs;
}


Schema Planner::buildScanSchema(const SelectStatement& stmt, const Schema& full_schema) {
    if (stmt.select_star) return full_schema;
    std::unordered_set<std::string> required;
    for (const auto& expr : stmt.select_list) collectCols(expr.get(), required);
    collectCols(stmt.where.get(), required);
    for (const auto& col_name : stmt.group_by) required.insert(col_name);
    collectCols(stmt.having.get(), required);
    for (const auto& item : stmt.order_by) collectCols(item.expr.get(), required);
    if (stmt.join.has_value()) collectCols(stmt.join->condition.get(), required);
    return narrowSchema(full_schema, required);
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