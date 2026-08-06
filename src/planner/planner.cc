#include "planner.h"
#include "join_condition.h"
#include "parser/expr_utils.h"

std::unique_ptr<PlanNode> Planner::plan(SelectStatement stmt, const Catalog& catalog, std::unordered_map<std::string, std::vector<Row>> table_rows, std::unordered_map<std::string, ColumnarTable> columnar_tables){
    // validate
    Validator::validate(stmt, catalog);

    // Volcano builds exactly ONE join and is not planned to gain more: it is the
    // correctness baseline, not the feature-complete path. Multi-way execution
    // is deliberately vectorized-only from Week 27 on, so name the path rather
    // than describing a temporary limitation. Deleting this and letting the
    // single HashJoinNode below run would build one join out of N clauses and
    // silently drop a relation — the wrong answer every refusal here prevents.
    //
    // This refusal cannot be deferred past the plan-time type checks the way the
    // multi-key one was (Week 26): this function builds one join, so with three
    // relations the merged schema is missing a relation's columns entirely and a
    // deferred check would report a misleading "column not found".
    if (stmt.joins.size() > 1) {
        throw std::runtime_error(
            "multi-way joins are not supported on the Volcano path; "
            "use --execution vectorized");
    }

    // expression GROUP BY keys: rewrite post-aggregate references, mirroring
    // LogicalPlanBuilder::build (no-op without expression keys)
    substituteGroupKeyRefs(stmt);

    // ON decomposition runs BEFORE the FROM scan is built, because the scan is
    // handed `stmt.where.get()` as its zone-map pruning hint: folding the
    // residuals in afterwards would leave the hint pointing at the pre-fold
    // predicate, so a relation-0 residual (`ON k = k AND l.season = 2024`) would
    // prune chunks on the vectorized path and none here. Same conjuncts, same
    // rows, silently different work — a per-mode difference with nothing to
    // catch it, since results agree either way.
    //
    // For an inner join ON and WHERE are interchangeable, so the residuals join
    // the WHERE conjunction; see logical_plan.cc for the same fold and
    // join_condition.h for why Week 29 must revisit it. conjoinAll MOVES the old
    // predicate rather than cloning it, so any raw Expr* taken afterwards is of
    // the merged tree and stays valid.
    // Week 29: the fold is INNER-only. For a LEFT join, ON and WHERE are
    // different queries (a left row whose only candidate fails the residual is
    // null-extended, not deleted), so an outer join's residuals stay on the join
    // and become part of its match test.
    const bool outer = !stmt.joins.empty() && stmt.joins[0].type == JoinType::LEFT;
    std::vector<JoinKey> join_keys;
    std::unique_ptr<Expr> on_residual;
    if (!stmt.joins.empty()) {
        JoinCondition on = classifyJoinCondition(stmt.joins[0].condition.get(), 1);
        join_keys = std::move(on.keys);
        if (outer) {
            std::vector<std::unique_ptr<Expr>> parts;
            for (const Expr* r : on.residuals) parts.push_back(cloneExpr(r));
            if (!parts.empty()) on_residual = conjoinAll(std::move(parts));
        } else if (!on.residuals.empty()) {
            std::vector<std::unique_ptr<Expr>> parts;
            for (const Expr* r : on.residuals) parts.push_back(cloneExpr(r));
            if (stmt.where) parts.push_back(std::move(stmt.where));
            stmt.where = conjoinAll(std::move(parts));
        }
    }

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
    bool self_join = !stmt.joins.empty() && stmt.joins[0].join_table == stmt.from_table;
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

    // hash join (exactly one, guarded above)
    if (!stmt.joins.empty()){
        const auto& join_clause = stmt.joins[0];
        const TableMetadata& join_meta = catalog.getTable(join_clause.join_table);

        Schema right_scan_schema = buildScanSchema(stmt, join_meta.schema);

        // capture before std::move transfers ownership into SeqScanNode
        int join_row_count = self_join
            ? from_row_count
            : (columnar_tables.count(join_clause.join_table) > 0
                ? columnar_tables.at(join_clause.join_table).num_rows
                : (int)table_rows.at(join_clause.join_table).size());

        std::unique_ptr<PlanNode> right;
        if (self_join) {
            // read from the copy preserved before the FROM scan moved the data
            if (self_join_columnar.has_value())
                right = std::make_unique<SeqScanNode>(join_clause.join_table, std::move(*self_join_columnar), right_scan_schema, nullptr);
            else
                right = std::make_unique<SeqScanNode>(join_clause.join_table, std::move(*self_join_rows), join_meta.schema);
        } else if (columnar_tables.count(join_clause.join_table) > 0) {
            right = std::make_unique<SeqScanNode>(join_clause.join_table, std::move(columnar_tables.at(join_clause.join_table)), right_scan_schema, nullptr);
        } else {
            right = std::make_unique<SeqScanNode>(join_clause.join_table, std::move(table_rows.at(join_clause.join_table)), join_meta.schema);
        }

        // Keys were routed by binder-assigned slot above — the only way to
        // disambiguate a self-join's two occurrences of the same table.
        //
        // Both children are single-relation scans here (one join), so their
        // schemas cannot repeat a column name and the join node resolves keys by
        // bare name. The vectorized builder cannot: its left input may be a
        // merged join schema, so it resolves by slot (see leftKeyIndices).
        std::vector<std::string> from_cols, join_cols;
        for (const JoinKey& k : join_keys) {
            from_cols.push_back(k.from_col);
            join_cols.push_back(k.join_col);
        }

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

        // Week 29: an outer join never swaps — the preserved (FROM) side must be
        // the probe input, because a build-side-preserved outer join needs a
        // matched flag per build row and an end-of-probe drain. The side is
        // forced, not costed.
        bool swap = !outer && from_row_count < join_row_count;
        if (swap) {
            node = std::make_unique<HashJoinNode>(std::move(right), std::move(node), join_cols, from_cols, merged_schema, /*swapped=*/true);
        } else {
            // plan-time type check of the ON residual against the merged schema,
            // mirroring LogicalPlanBuilder::build — an ill-typed residual fails
            // here, not per row inside the probe loop
            if (on_residual) inferExprType(on_residual.get(), merged_schema);
            node = std::make_unique<HashJoinNode>(std::move(node), std::move(right), from_cols, join_cols, merged_schema, /*swapped=*/false, outer, std::move(on_residual));
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


