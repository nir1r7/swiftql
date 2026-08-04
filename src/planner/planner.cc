#include "planner.h"
#include "join_condition.h"
#include "parser/expr_utils.h"

std::unique_ptr<PlanNode> Planner::plan(SelectStatement stmt, const Catalog& catalog, std::unordered_map<std::string, std::vector<Row>> table_rows, std::unordered_map<std::string, ColumnarTable> columnar_tables){
    // validate
    Validator::validate(stmt, catalog);

    // expression GROUP BY keys: rewrite post-aggregate references, mirroring
    // LogicalPlanBuilder::build (no-op without expression keys)
    substituteGroupKeyRefs(stmt);

    const TableMetadata& meta = catalog.getTable(stmt.from_table);

    // narrowed scan schema via the shared logical-layer helper — this also
    // fixes SELECT * + JOIN under columnar storage, which previously narrowed
    // the join table by the FROM table's column names
    Schema scan_schema = buildScanSchema(stmt, meta.schema);

    // capture before std::move transfers ownership into SeqScanNode
    int from_row_count = columnar_tables.count(stmt.from_table) > 0 ? columnar_tables.at(stmt.from_table).num_rows : (int)table_rows.at(stmt.from_table).size();

    // Self-join: both scans read the same catalog table, keyed once in the
    // map. The FROM scan below moves that data out, so preserve a copy for the
    // JOIN scan. (A copy — not shared ownership — keeps this a minimal change;
    // it costs one extra table copy, acceptable at this project's scale.)
    bool self_join = stmt.join.has_value() && stmt.join->join_table == stmt.from_table;
    std::optional<ColumnarTable> self_join_columnar;
    std::optional<std::vector<Row>> self_join_rows;
    if (self_join) {
        if (columnar_tables.count(stmt.from_table) > 0)
            self_join_columnar = columnar_tables.at(stmt.from_table);
        else
            self_join_rows = table_rows.at(stmt.from_table);
    }

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

        Schema right_scan_schema = buildScanSchema(stmt, join_meta.schema);

        // capture before std::move transfers ownership into SeqScanNode
        int join_row_count = self_join
            ? from_row_count
            : (columnar_tables.count(stmt.join->join_table) > 0
                ? columnar_tables.at(stmt.join->join_table).num_rows
                : (int)table_rows.at(stmt.join->join_table).size());

        std::unique_ptr<PlanNode> right;
        if (self_join) {
            // read from the copy preserved before the FROM scan moved the data
            if (self_join_columnar.has_value())
                right = std::make_unique<SeqScanNode>(stmt.join->join_table, std::move(*self_join_columnar), right_scan_schema, nullptr);
            else
                right = std::make_unique<SeqScanNode>(stmt.join->join_table, std::move(*self_join_rows), join_meta.schema);
        } else if (columnar_tables.count(stmt.join->join_table) > 0) {
            right = std::make_unique<SeqScanNode>(stmt.join->join_table, std::move(columnar_tables.at(stmt.join->join_table)), right_scan_schema, nullptr);
        } else {
            right = std::make_unique<SeqScanNode>(stmt.join->join_table, std::move(table_rows.at(stmt.join->join_table)), join_meta.schema);
        }

        // classifyJoinCondition routes keys by binder-assigned slot — the only
        // way to disambiguate a self-join's two occurrences of the same table
        JoinConditionKeys keys = classifyJoinCondition(stmt.join->condition.get());
        std::string from_col = keys.from_col;
        std::string join_col = keys.join_col;

        // put the smaller table on the build side (right_) to minimise hash table memory.
        // Output schema order is always [FROM columns, JOIN columns] — fixed
        // logical order, independent of which side is physically build.
        // JOIN-side columns are stamped slot 1 so qualified references resolve
        // to the correct side even when both sides share a column name.
        std::vector<ColumnDef> merged_cols = node->outputSchema().columns();
        for (ColumnDef col : right->outputSchema().columns()) {
            col.relation_slot = 1;
            merged_cols.push_back(col);
        }
        Schema merged_schema(merged_cols);

        bool swap = from_row_count < join_row_count;
        if (swap) {
            node = std::make_unique<HashJoinNode>(std::move(right), std::move(node), join_col, from_col, merged_schema, /*swapped=*/true);
        } else {
            node = std::make_unique<HashJoinNode>(std::move(node), std::move(right), from_col, join_col, merged_schema, /*swapped=*/false);
        }
    }

    // fliter (WHERE)
    if (stmt.where) {
        // plan-time type check, mirroring LogicalPlanBuilder::build
        inferExprType(stmt.where.get(), node->outputSchema());
        node = std::make_unique<FilterNode>(std::move(node), std::move(stmt.where));
    }

    // HashAgrgegate (GROUP BY + aggregates) — recursive detection, mirroring
    // LogicalPlanBuilder::build
    bool has_aggregates = false;
    {
        std::vector<const AggregateExpr*> found;
        for (auto& expr : stmt.select_list) collectAggregates(expr.get(), found);
        has_aggregates = !found.empty();
    }

    if (!stmt.group_by.empty() || has_aggregates) {
        std::vector<AggregateSpec> agg_specs = extractAggregates(stmt);
        auto agg_schema = buildAggregateSchema(stmt.group_by, agg_specs, node->outputSchema());
        node = std::make_unique<HashAggregateNode>(std::move(node), stmt.group_by, std::move(agg_specs), agg_schema);
    }

    // HAVING
    if (stmt.having) {
        inferExprType(stmt.having.get(), node->outputSchema());
        node = std::make_unique<HavingNode>(std::move(node), std::move(stmt.having));
    }

    // sort (ORDER BY) — must evaluate against pre-projection schema
    if (!stmt.order_by.empty()) {
        for (const auto& item : stmt.order_by) {
            inferExprType(item.expr.get(), node->outputSchema());
        }
        node = std::make_unique<SortNode>(std::move(node), std::move(stmt.order_by));
    }

    // project; SELECT list — placed after Sort so sort expressions resolve against full schema
    if (stmt.select_star) {
        const Schema& child_schema = node->outputSchema();
        std::vector<std::unique_ptr<Expr>> star_exprs;
        std::vector<ColumnDef> star_cols;
        for (const auto& col : child_schema.columns()) {
            if (col.hidden) continue; // HAVING/ORDER-BY-only aggregates never reach output
            auto ref = std::make_unique<ColumnRef>();
            ref->column_name = col.name;
            ref->relation_slot = col.relation_slot; // preserve side so SELECT * on a self-join emits both sides
            star_exprs.push_back(std::move(ref));
            star_cols.push_back(col);
        }
        node = std::make_unique<ProjectNode>(std::move(node), std::move(star_exprs), Schema(star_cols));
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


