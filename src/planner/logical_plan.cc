#include "logical_plan.h"
// planner.h is included only for the four static schema helpers
// (buildScanSchema/buildAggregateSchema/buildProjectSchema/extractAggregates).
// Week 18 (once main.cc stops calling them) relocates those helpers into this
// layer and drops this include — keeping the logical layer free of the
// physical operator headers at the TU level too.
#include "planner.h"
#include "validator.h"
#include "parser/expr_utils.h"


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
            s += group_by[i];
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


// extract join column names + binder-assigned slots from ON condition
static void extractJoinKeys(const Expr* condition, std::string& from_col, std::string& join_col) {
    std::string left_col, right_col;
    int left_slot = -1, right_slot = -1;
    if (auto* bin = dynamic_cast<const BinaryExpr*>(condition)) {
        if (auto* lc = dynamic_cast<const ColumnRef*>(bin->left.get())) {
            left_col  = lc->column_name;
            left_slot = lc->relation_slot;
        }
        if (auto* rc = dynamic_cast<const ColumnRef*>(bin->right.get())) {
            right_col  = rc->column_name;
            right_slot = rc->relation_slot;
        }
    }
    // Route each ON column to its side by binder-assigned slot (0=FROM,
    // 1=JOIN). This is the only way to disambiguate a self-join's two
    // occurrences (both share the canonical table name). Falls back to
    // positional (left=FROM, right=JOIN) when slots are unset.
    if (left_slot >= 0 && right_slot >= 0 && left_slot != right_slot) {
        from_col = (left_slot == 0) ? left_col : right_col;
        join_col = (left_slot == 0) ? right_col : left_col;
    } else {
        from_col = left_col;
        join_col = right_col;
    }
}


// LogicalPlanBuilder
std::unique_ptr<LogicalPlanNode> LogicalPlanBuilder::build(SelectStatement stmt, const Catalog& catalog) {
    Validator::validate(stmt, catalog);

    // FROM scan, narrowed to only the columns the query actually needs
    std::unique_ptr<LogicalPlanNode> node = std::make_unique<LogicalScan>(
        stmt.from_table,
        Planner::buildScanSchema(stmt, catalog.getTable(stmt.from_table).schema));

    // join
    if (stmt.join.has_value()) {
        const TableMetadata& join_meta = catalog.getTable(stmt.join->join_table);
        auto join_scan = std::make_unique<LogicalScan>(
            stmt.join->join_table,
            Planner::buildScanSchema(stmt, join_meta.schema));

        std::string from_col, join_col;
        extractJoinKeys(stmt.join->condition.get(), from_col, join_col);

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
        Schema agg_schema = Planner::buildAggregateSchema(stmt, node->output_schema);
        node = std::make_unique<LogicalAggregate>(std::move(node), stmt.group_by, Planner::extractAggregates(stmt), agg_schema);
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
        for (const auto& col : child_schema.columns()) {
            auto ref = std::make_unique<ColumnRef>();
            ref->column_name = col.name;
            ref->relation_slot = col.relation_slot; // preserve side so SELECT * on a self-join emits both sides
            star_exprs.push_back(std::move(ref));
        }
        node = std::make_unique<LogicalProject>(std::move(node), std::move(star_exprs), child_schema);
    } else {
        Schema proj_schema = Planner::buildProjectSchema(stmt, node->output_schema);
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