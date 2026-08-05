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


TypeId aggregateResultType(const std::string& function, TypeId arg_type) {
    if (function == "COUNT") return TypeId::INT;
    if (function == "MIN" || function == "MAX") return arg_type;
    return TypeId::DOUBLE;   // SUM, AVG — see the header for why SUM is not INT
}


// Argument type of an aggregate spec, for aggregateResultType. Prefers the
// general argument expression; falls back to the plain-ColumnRef fast path
// (spec.column), resolved slot-first like everywhere else. COUNT(*) and specs
// whose argument cannot be resolved report INT, which only COUNT consumes.
static TypeId specArgType(const AggregateSpec& spec, const Schema& table_schema) {
    if (spec.is_star) return TypeId::INT;
    if (spec.argument) return inferExprType(spec.argument, table_schema);
    if (spec.column.empty()) return TypeId::INT;
    int idx = spec.relation_slot >= 0
        ? table_schema.indexOf(spec.column, spec.relation_slot) : -1;
    if (idx < 0) idx = table_schema.indexOf(spec.column);
    return idx >= 0 ? table_schema.column(idx).type : TypeId::INT;
}


TypeId inferExprType(const Expr* expr, const Schema& schema) {
    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        return lit->value.type();
    }
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // slot-first with bare-name fallback — same contract as
        // resolveColumnIndex() in evaluator.cc
        int idx = col->relation_slot >= 0
            ? schema.indexOf(col->column_name, col->relation_slot) : -1;
        if (idx < 0) idx = schema.indexOf(col->column_name);
        if (idx < 0) throw std::runtime_error(
            "column not found: '" + col->column_name + "'");
        return schema.column(idx).type;
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        // Post-aggregate schemas already carry this aggregate's output column
        // with its resolved type (buildAggregateSchema is the single source of
        // truth). Prefer it so inference can never disagree with what the
        // aggregate node actually produced — this is the path taken by
        // expressions over aggregates, e.g. SUM(a) / SUM(b) in a projection.
        int idx = schema.indexOf(aggregateOutputName(agg));
        if (idx >= 0) return schema.column(idx).type;
        // pre-aggregate context (hand-built trees in tests): derive from the
        // argument against the input schema
        TypeId arg = (agg->is_star || !agg->argument)
            ? TypeId::INT : inferExprType(agg->argument.get(), schema);
        return aggregateResultType(agg->function_name, arg);
    }
    if (dynamic_cast<const IsNullExpr*>(expr)) {
        return TypeId::INT;   // boolean-as-INT convention
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        TypeId t = inferExprType(un->operand.get(), schema);
        if (t == TypeId::STRING)
            throw std::runtime_error("unary '-' requires a numeric operand");
        return t;
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        // recurse into both children first so ill-typed subtrees surface even
        // under comparisons ((team + 1) = 5 must be rejected)
        TypeId l = inferExprType(bin->left.get(), schema);
        TypeId r = inferExprType(bin->right.get(), schema);
        const std::string& op = bin->op;
        if (op == "+" || op == "-" || op == "*" || op == "/") {
            if (l == TypeId::STRING || r == TypeId::STRING)
                throw std::runtime_error("'" + op + "' requires numeric operands");
            // INT/INT stays INT (SQLite truncating division); any DOUBLE promotes
            return (l == TypeId::INT && r == TypeId::INT) ? TypeId::INT : TypeId::DOUBLE;
        }
        return TypeId::INT;   // comparison / AND / OR
    }
    throw std::runtime_error("inferExprType(): unknown Expr subtype");
}


Schema buildScanSchema(const SelectStatement& stmt, const Schema& full_schema) {
    if (stmt.select_star) return full_schema;
    std::unordered_set<std::string> required;
    for (const auto& expr : stmt.select_list) collectCols(expr.get(), required);
    collectCols(stmt.where.get(), required);
    for (const auto& g : stmt.group_by) {
        // expression keys reference columns only inside their tree — a miss
        // here would narrow them out of the scan
        if (g.expr) { collectCols(g.expr.get(), required); continue; }
        required.insert(g.column_name);
    }
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
            // the aggregate node below already emitted this column with its
            // resolved result type; reuse it so project and aggregate can never
            // disagree. table_schema here is the POST-aggregate schema, so
            // re-inferring from agg->argument would fail — the argument's base
            // column no longer exists at this level.
            std::string agg_name = aggregateOutputName(agg);
            int agg_idx = table_schema.indexOf(agg_name);
            TypeId result_type = agg_idx >= 0
                ? table_schema.column(agg_idx).type
                : aggregateResultType(agg->function_name, TypeId::DOUBLE);
            cols.push_back({agg_name, result_type});
        } else {
            // general expression (arithmetic, expr over aggregates): name from
            // exprToString, type from inference — never STRING-by-default
            cols.push_back({exprToString(expr.get()),
                            inferExprType(expr.get(), table_schema)});
        }
        // an alias renames only this projected output column; the aggregate
        // node's schema underneath keeps canonical aggregateOutputName names
        if (!expr->alias.empty()) cols.back().name = expr->alias;
    }

    return Schema(cols);
}


// Consumes the specs extractAggregates produced so schema and node can never
// disagree on column order or names.
Schema buildAggregateSchema(const std::vector<GroupByColumn>& group_by,
                            const std::vector<AggregateSpec>& aggregates,
                            const Schema& table_schema){
    // plan-time type check of expression arguments (SUM(speed * 2)): both
    // builders route through here, so ill-typed arguments fail before
    // execution. Plain-ColumnRef SUM/AVG args are already validator-checked.
    for (const auto& spec : aggregates) {
        if (spec.is_star || !spec.argument) continue;
        TypeId t = inferExprType(spec.argument, table_schema);
        if ((spec.function == "SUM" || spec.function == "AVG") && t == TypeId::STRING) {
            throw std::runtime_error(spec.function + "() requires a numeric argument");
        }
    }

    std::vector<ColumnDef> cols;

    // group-by columns in order, resolved slot-first so a qualified GROUP BY
    // picks the named join side even when both sides share the column name.
    // Expression keys become computed output columns named by exprToString —
    // the same string substituteGroupKeyRefs rewrites references into.
    for (const auto& g : group_by) {
        if (g.expr) {
            cols.push_back({exprToString(g.expr.get()),
                            inferExprType(g.expr.get(), table_schema)});
            continue;
        }
        int idx = g.relation_slot >= 0
            ? table_schema.indexOf(g.column_name, g.relation_slot)
            : -1;
        if (idx < 0) idx = table_schema.indexOf(g.column_name);
        if (idx < 0) {
            // Both executors push one key Value per group-by column, so silently
            // dropping the column here would leave the schema one column narrower
            // than every row — silent corruption. Unreachable today (Validator
            // and buildScanSchema guarantee resolution), so this is the shape of
            // a planner bug, not a user error.
            throw std::runtime_error(
                "internal: GROUP BY column '" + g.column_name
                + "' not present in the aggregate's input schema");
        }
        cols.push_back(table_schema.column(idx));
    }

    // one output column per aggregate, in spec order (SELECT-list aggregates
    // first, then hidden HAVING/ORDER-BY-only aggregates at the tail)
    for (const auto& spec : aggregates) {
        ColumnDef def{spec.output_name,
                      aggregateResultType(spec.function, specArgType(spec, table_schema))};
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
            spec.argument = agg->argument.get();  // non-owning; see AggregateSpec
            if (auto* col = dynamic_cast<const ColumnRef*>(agg->argument.get())) {
                spec.column = col->column_name;
                spec.relation_slot = col->relation_slot; // carry join side, e.g. AVG(l2.speed)
            }
        }
        return spec;
    };

    // Dedupe by output_name everywhere — the name is how evaluate() finds the
    // column, so name-identical references share one output column.
    auto known = [&specs](const std::string& name) {
        for (const auto& s : specs) {
            if (s.output_name == name) return true;
        }
        return false;
    };

    // recursive collection: SELECT AVG(speed) * 2 contains an aggregate
    // without being one at the top level
    std::vector<const AggregateExpr*> in_select;
    for (const auto& expr : stmt.select_list) collectAggregates(expr.get(), in_select);
    for (const AggregateExpr* agg : in_select) {
        if (!known(aggregateOutputName(agg))) specs.push_back(makeSpec(agg, /*hidden=*/false));
    }

    // aggregates referenced only in HAVING or ORDER BY are computed too, as
    // hidden tail columns the final projection drops
    std::vector<const AggregateExpr*> referenced;
    collectAggregates(stmt.having.get(), referenced);
    for (const auto& item : stmt.order_by) collectAggregates(item.expr.get(), referenced);
    for (const AggregateExpr* agg : referenced) {
        if (!known(aggregateOutputName(agg))) specs.push_back(makeSpec(agg, /*hidden=*/true));
    }

    return specs;
}


// depth-first replacement for substituteGroupKeyRefs; stops at AggregateExpr
// (arguments evaluate pre-aggregate) and at leaves (a plain ColumnRef can
// never match — expression keys are non-ColumnRef by construction)
static void substituteInto(std::unique_ptr<Expr>& expr, const std::vector<std::string>& keys) {
    if (!expr) return;
    if (dynamic_cast<AggregateExpr*>(expr.get())) return;
    if (dynamic_cast<ColumnRef*>(expr.get()) || dynamic_cast<Literal*>(expr.get())) return;

    std::string s = exprToString(expr.get());
    for (const auto& key : keys) {
        if (key == s) {
            auto ref = std::make_unique<ColumnRef>();
            ref->column_name = key;     // matches buildAggregateSchema's output name
            ref->alias = expr->alias;   // preserve a select-item alias
            expr = std::move(ref);
            return;
        }
    }

    if (auto* bin = dynamic_cast<BinaryExpr*>(expr.get())) {
        substituteInto(bin->left, keys);
        substituteInto(bin->right, keys);
        return;
    }
    if (auto* un = dynamic_cast<UnaryExpr*>(expr.get())) {
        substituteInto(un->operand, keys);
        return;
    }
    if (auto* isn = dynamic_cast<IsNullExpr*>(expr.get())) {
        substituteInto(isn->operand, keys);
    }
}

void substituteGroupKeyRefs(SelectStatement& stmt) {
    std::vector<std::string> keys;
    for (const auto& g : stmt.group_by) {
        if (g.expr) keys.push_back(exprToString(g.expr.get()));
    }
    if (keys.empty()) return;

    for (auto& e : stmt.select_list) substituteInto(e, keys);
    substituteInto(stmt.having, keys);
    for (auto& item : stmt.order_by) substituteInto(item.expr, keys);
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
            if (group_by[i].expr) {
                s += exprToString(group_by[i].expr.get());
                continue;
            }
            if (!group_by[i].table_name.empty()) s += group_by[i].table_name + ".";
            s += group_by[i].column_name;
        }
        s += " | ";
    }
    for (size_t i = 0; i < aggregates.size(); ++i) {
        if (i) s += ", ";
        s += aggregates[i].column.empty() && aggregates[i].argument && !aggregates[i].is_star
            ? aggregates[i].output_name
            : aggregates[i].function + "(" + (aggregates[i].is_star ? "*" : aggregates[i].column) + ")";
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

    // expression GROUP BY keys: rewrite post-aggregate references to the
    // aggregate's group-key output columns (no-op without expression keys)
    substituteGroupKeyRefs(stmt);

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
        // plan-time type check: reject STRING arithmetic here instead of
        // per-row "Type mismatch" throws during execution
        inferExprType(stmt.where.get(), node->output_schema);
        node = std::make_unique<LogicalFilter>(std::move(node), std::move(stmt.where));
    }

    // aggregate (GROUP BY + aggregates) — detection must recurse:
    // AVG(speed) * 2 is not a top-level AggregateExpr but still aggregates
    bool has_aggs = false;
    {
        std::vector<const AggregateExpr*> found;
        for (auto& expr : stmt.select_list) collectAggregates(expr.get(), found);
        has_aggs = !found.empty();
    }

    if (!stmt.group_by.empty() || has_aggs) {
        std::vector<AggregateSpec> agg_specs = extractAggregates(stmt);
        Schema agg_schema = buildAggregateSchema(stmt.group_by, agg_specs, node->output_schema);
        node = std::make_unique<LogicalAggregate>(std::move(node), stmt.group_by, std::move(agg_specs), agg_schema);
    }

    // HAVING — filter above the aggregate; no dedicated node
    if (stmt.having) {
        inferExprType(stmt.having.get(), node->output_schema);
        node = std::make_unique<LogicalFilter>(std::move(node), std::move(stmt.having));
    }

    // sort (ORDER BY) — must evaluate against pre-projection schema
    if (!stmt.order_by.empty()) {
        for (const auto& item : stmt.order_by) {
            inferExprType(item.expr.get(), node->output_schema);
        }
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