#include "logical_plan.h"
#include "join_condition.h"
#include "validator.h"
#include "parser/expr_utils.h"
#include <unordered_set>


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
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)){
        collectCols(un->operand.get(), out);
        return;
    }
}

// build schema using only required columns
// copies whole ColumnDefs so order and relation_slot stamps are preserved
static Schema narrowSchema(const Schema& full, const std::unordered_set<std::string>& required) {
    std::vector<ColumnDef> cols;
    for (int i = 0; i < full.size(); ++i) {
        if (required.count(full.column(i).name))
            cols.push_back(full.column(i));
    }
    return Schema(cols);
}


Schema buildScanSchema(const SelectStatement& stmt, const Schema& full_schema) {
    if (stmt.select_star) return full_schema;
    std::unordered_set<std::string> required;
    for (const auto& expr : stmt.select_list) collectCols(expr.get(), required);
    collectCols(stmt.where.get(), required);
    for (const auto& g : stmt.group_by) required.insert(g.column_name);
    collectCols(stmt.having.get(), required);
    for (const auto& item : stmt.order_by) collectCols(item.expr.get(), required);
    if (stmt.join.has_value()) collectCols(stmt.join->condition.get(), required);
    return narrowSchema(full_schema, required);
}


Schema buildProjectSchema(const SelectStatement& stmt, const Schema& table_schema){
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
            cols.push_back({aggregateOutputName(agg), result_type});
        }
    }

    return Schema(cols);
}


// Consumes the specs extractAggregates produced so schema and node can never
// disagree on column order or names.
Schema buildAggregateSchema(const std::vector<GroupByColumn>& group_by,
                            const std::vector<AggregateSpec>& aggregates,
                            const Schema& table_schema){
    std::vector<ColumnDef> cols;

    // group-by columns in order, resolved slot-first so a qualified GROUP BY
    // picks the named join side even when both sides share the column name
    for (const auto& g : group_by) {
        int idx = g.relation_slot >= 0
            ? table_schema.indexOf(g.column_name, g.relation_slot)
            : -1;
        if (idx < 0) idx = table_schema.indexOf(g.column_name);
        if (idx >= 0) {
            cols.push_back(table_schema.column(idx));
        }
    }

    // one output column per aggregate, in spec order (SELECT-list aggregates
    // first, then hidden HAVING/ORDER-BY-only aggregates at the tail)
    for (const auto& spec : aggregates) {
        TypeId result_type = (spec.function == "COUNT") ? TypeId::INT : TypeId::DOUBLE;
        ColumnDef def{spec.output_name, result_type};
        def.hidden = spec.hidden;
        cols.push_back(def);
    }

    return Schema(cols);
}


std::vector<AggregateSpec> extractAggregates(const SelectStatement& stmt){
    std::vector<AggregateSpec> specs;

    auto makeSpec = [](const AggregateExpr* agg, bool hidden) {
        AggregateSpec spec;
        spec.function = agg->function_name;
        spec.is_star = agg->is_star;
        spec.output_name = aggregateOutputName(agg);
        spec.hidden = hidden;
        if (!agg->is_star && agg->argument) {
            if (auto* col = dynamic_cast<const ColumnRef*>(agg->argument.get())) {
                spec.column = col->column_name;
                spec.relation_slot = col->relation_slot; // carry join side, e.g. AVG(l2.speed)
            }
        }
        return spec;
    };

    for (const auto& expr : stmt.select_list) {
        if (auto* agg = dynamic_cast<AggregateExpr*>(expr.get())) {
            specs.push_back(makeSpec(agg, /*hidden=*/false));
        }
    }

    // aggregates referenced only in HAVING or ORDER BY are computed too, as
    // hidden tail columns the final projection drops. Dedupe by output_name —
    // the name is how evaluate() finds the column, so name-identical
    // references share one output column.
    std::vector<const AggregateExpr*> referenced;
    collectAggregates(stmt.having.get(), referenced);
    for (const auto& item : stmt.order_by) collectAggregates(item.expr.get(), referenced);
    for (const AggregateExpr* agg : referenced) {
        std::string name = aggregateOutputName(agg);
        bool known = false;
        for (const auto& s : specs) {
            if (s.output_name == name) { known = true; break; }
        }
        if (!known) specs.push_back(makeSpec(agg, /*hidden=*/true));
    }

    return specs;
}


// LogicalScan
std::string LogicalScan::explain() const {
    return "LogicalScan [" + table_name + ", " + std::to_string(output_schema.columns().size()) + " columns]";
}


// LogicalJoin
std::string LogicalJoin::explain() const {
    return "LogicalJoin [" + from_col + " = " + join_col + "]";
}


// LogicalFilter
std::string LogicalFilter::explain() const {
    return "LogicalFilter [" + exprToString(predicate.get()) + "]";
}


// LogicalAggregate
std::string LogicalAggregate::explain() const {
    std::string s = "LogicalAggregate [";
    if (!group_by.empty()) {
        for (size_t i = 0; i < group_by.size(); ++i) {
            if (i) s += ", ";
            if (!group_by[i].table_name.empty()) s += group_by[i].table_name + ".";
            s += group_by[i].column_name;
        }
        s += " | ";
    }
    for (size_t i = 0; i < aggregates.size(); ++i) {
        if (i) s += ", ";
        s += aggregates[i].function + "(" + (aggregates[i].is_star ? "*" : aggregates[i].column) + ")";
    }
    return s + "]";
}


// LogicalProject
std::string LogicalProject::explain() const {
    std::string s = "LogicalProject [";
    for (int i = 0; i < output_schema.size(); ++i) {
        if (i) s += ", ";
        s += output_schema.column(i).name;
    }
    return s + "]";
}


// LogicalSort
std::string LogicalSort::explain() const {
    std::string s = "LogicalSort [";
    for (size_t i = 0; i < order_by.size(); ++i) {
        if (i) s += ", ";
        s += exprToString(order_by[i].expr.get());
        if (order_by[i].desc) s += " DESC";
    }
    return s + "]";
}


// LogicalDistinct
std::string LogicalDistinct::explain() const {
    return "LogicalDistinct";
}


// LogicalLimit
std::string LogicalLimit::explain() const {
    return "LogicalLimit [" + std::to_string(limit) + "]";
}


// LogicalPlanBuilder
std::unique_ptr<LogicalPlanNode> LogicalPlanBuilder::build(SelectStatement stmt, const Catalog& catalog) {
    Validator::validate(stmt, catalog);

    // FROM scan, narrowed to only the columns the query actually needs
    std::unique_ptr<LogicalPlanNode> node = std::make_unique<LogicalScan>(
        stmt.from_table,
        buildScanSchema(stmt, catalog.getTable(stmt.from_table).schema));

    // join
    if (stmt.join.has_value()) {
        const TableMetadata& join_meta = catalog.getTable(stmt.join->join_table);
        auto join_scan = std::make_unique<LogicalScan>(
            stmt.join->join_table,
            buildScanSchema(stmt, join_meta.schema));

        // classifyJoinCondition routes keys by binder-assigned slot — the only
        // way to disambiguate a self-join's two occurrences of the same table
        JoinConditionKeys keys = classifyJoinCondition(stmt.join->condition.get());
        std::string from_col = keys.from_col;
        std::string join_col = keys.join_col;

        // Output schema order is always [FROM columns, JOIN columns] — fixed
        // logical order. JOIN-side columns are stamped slot 1 so qualified
        // references resolve to the correct side even when both sides share
        // a column name. By-value loop var: a reference would mutate the
        // join scan's own schema.
        std::vector<ColumnDef> merged_cols = node->output_schema.columns();
        for (ColumnDef col : join_scan->output_schema.columns()) {
            col.relation_slot = 1;
            merged_cols.push_back(col);
        }

        // no build/probe swap decision here — that's a physical concern
        // (Week 18/22), not part of the logical plan.
        node = std::make_unique<LogicalJoin>(std::move(node), std::move(join_scan), from_col, join_col, Schema(merged_cols));
    }

    // filter (WHERE)
    if (stmt.where) {
        node = std::make_unique<LogicalFilter>(std::move(node), std::move(stmt.where));
    }

    // aggregate (GROUP BY + aggregates)
    bool has_aggs = false;
    for (auto& expr : stmt.select_list) {
        if (dynamic_cast<AggregateExpr*>(expr.get())) {
            has_aggs = true;
            break;
        }
    }

    if (!stmt.group_by.empty() || has_aggs) {
        std::vector<AggregateSpec> agg_specs = extractAggregates(stmt);
        Schema agg_schema = buildAggregateSchema(stmt.group_by, agg_specs, node->output_schema);
        node = std::make_unique<LogicalAggregate>(std::move(node), stmt.group_by, std::move(agg_specs), agg_schema);
    }

    // HAVING — filter above the aggregate; no dedicated node
    if (stmt.having) {
        node = std::make_unique<LogicalFilter>(std::move(node), std::move(stmt.having));
    }

    // sort (ORDER BY) — must evaluate against pre-projection schema
    if (!stmt.order_by.empty()) {
        node = std::make_unique<LogicalSort>(std::move(node), std::move(stmt.order_by));
    }

    // project; SELECT list — placed after Sort so sort expressions resolve against full schema
    if (stmt.select_star) {
        const Schema& child_schema = node->output_schema;
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
        node = std::make_unique<LogicalProject>(std::move(node), std::move(star_exprs), Schema(star_cols));
    } else {
        Schema proj_schema = buildProjectSchema(stmt, node->output_schema);
        node = std::make_unique<LogicalProject>(std::move(node), std::move(stmt.select_list), proj_schema);
    }

    // DISTINCT — runs on projected rows
    if (stmt.distinct) {
        node = std::make_unique<LogicalDistinct>(std::move(node));
    }

    // limit
    if (stmt.limit.has_value()) {
        node = std::make_unique<LogicalLimit>(std::move(node), stmt.limit.value());
    }

    return node;
}