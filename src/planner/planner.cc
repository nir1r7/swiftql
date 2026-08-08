#include "planner.h"
#include "join_condition.h"
#include "predicate_pushdown.h"   // pruningHintForPreservedSide — shared with the vectorized builder
#include "subquery_materialization.h"   // forEachSubqueryConst
#include "parser/expr_utils.h"
#include <unordered_set>

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

    // Week 32 — set-membership lowering is vectorized-only, and the refusal
    // lives HERE rather than in Validator for the reason Week 26 established:
    // the two engines genuinely differ in capability, and a shared-validator
    // refusal would deny the shape to the engine that can execute it.
    //
    // Why Volcano does not get a semi-join yet. This function builds exactly ONE
    // HashJoinNode, from stmt.joins; a semi-join is a SECOND join for any query
    // whose FROM already joins, and there is no plan-tree shape here to hold it
    // (the logical layer's LogicalPlanBuilder is where the lowering grafts the
    // body's subtree, and this path does not go through it). Faking one would be
    // the silent dropped-relation failure the refusal above exists to prevent.
    //
    // !! WHAT THIS COSTS, stated rather than discovered later: Volcano is the
    // correctness baseline and compare_against_sqlite.py runs it as a separate
    // oracle leg, so every IN-subquery query is now diffed against SQLite in the
    // two VECTORIZED modes only. The refusal is pinned by message in that file's
    // Volcano rejection suite — the diffed suite cannot hold a query that
    // errors — which is what makes this boundary visible instead of implicit.
    if (stmt.has_subquery) {
        // WHERE and HAVING are the only legal subquery positions
        // (validateExpr's allow_subqueries flag), so those two trees are the
        // whole search space.
        bool has_in = false;
        // Week 33. A CORRELATED subquery decorrelates into the same semi/anti
        // join, and this path does not run LogicalPlanBuilder, where the rewrite
        // grafts the body's subtree — so it is the same capability difference
        // with the same containment, and it is refused here rather than in the
        // shared Validator (Week 26's rule). What it costs is stated rather than
        // hidden: every correlated query is diffed against SQLite in the two
        // VECTORIZED modes only, with the refusal pinned by message in the two
        // Volcano ones. Closing it means JoinSemantics in hash_join_node.cc —
        // Task 6 of docs/week-33-plan.md, and the honest end state.
        bool has_correlated = false;
        auto scan = [&](const Expr* e) {
            forEachSubqueryConst(e, [&](const SubqueryExpr& sq) {
                if (sq.kind == SubqueryExpr::Kind::IN) has_in = true;
                if (sq.correlated) has_correlated = true;
            });
        };
        scan(stmt.where.get());
        scan(stmt.having.get());
        if (has_in) {
            throw std::runtime_error(
                "IN subqueries are lowered to a semi-join and are not supported "
                "on the Volcano path; use --execution vectorized");
        }
        if (has_correlated) {
            throw std::runtime_error(
                "correlated subqueries are decorrelated to a semi-join and are "
                "not supported on the Volcano path; use --execution vectorized");
        }
    }

    // Week 34 — DERIVED TABLES. Third refusal in this block, same shape and same
    // reason as the two above: this function builds its scan directly from a
    // catalog table name and exactly one HashJoinNode out of stmt.joins, so
    // there is no plan shape here that can hold a relation which is itself a
    // PLAN, and this path does not run LogicalPlanBuilder, where the graft
    // happens. A capability difference, so it lives here and not in the shared
    // Validator (Week 26's rule).
    //
    // !! WHAT IT COSTS, extending the paragraph above rather than restating it:
    // every derived-table query joins the IN-subquery and correlated ones in
    // being diffed against SQLite in the two VECTORIZED modes only, with the
    // refusal pinned by message in the two Volcano ones. That is now three
    // families of query the four-mode oracle does not cover, and the count is
    // stated in README Limitations so the week it tips is visible.
    //
    // WEEK 36 PRICED IT ON TPC-H, because "which of these guards actually costs
    // the coverage" had been an assumption. Of the 34 Volcano refusal cells in
    // the 22x4 matrix, classified by the message each cell RECORDED:
    //
    //     multi-way joins   14 cells  (q2 q3 q5 q10 q11 q18 q21)
    //     derived tables    12 cells  (q7 q8 q9 q13 q15 q22)
    //     IN subqueries      4 cells  (q16 q20)
    //     correlated         4 cells  (q4 q17)
    //
    // So the standing expectation -- that Volcano semi/anti parity would move
    // many queries from two modes to four -- is WRONG by measurement: the
    // semi/anti family is 8 of 34, and SIX of those 8 additionally need a query
    // shape this function cannot express (q16, q20 and q17 each join in their
    // FROM as well, so the semi/anti join is a SECOND join). The only reachable
    // case is q4, whose FROM is a single relation: 2 cells, at the price of
    // JoinSemantics in HashJoinNode plus a second decorrelation production on a
    // path with no logical layer -- the two-paths drift Weeks 26/28/30 undid.
    // The two guards that actually dominate are the two that are DELIBERATE.
    {
        bool derived = stmt.from.isDerived();
        for (const auto& j : stmt.joins) derived = derived || j.relation.isDerived();
        if (derived) {
            throw std::runtime_error(
                "derived tables (FROM (subquery)) are not supported on the "
                "Volcano path; use --execution vectorized");
        }
    }

    // Every read below is of a NAMED relation, which the refusal above is what
    // guarantees. Named once so the dependency is local rather than restated at
    // sixteen sites — the discipline the `jc` clause pointer established after
    // Week 29 found `preserved_slots{0}` coupled to a refusal 110 lines away.
    const std::string& from_table = stmt.from.tableName("Planner::plan FROM");

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
    //
    // ONE clause pointer, used by every decision below — the residual split here,
    // the scans, the swap, and the operator. `stmt.joins[0]` read at each of those
    // sites would be correct only because of the `joins.size() > 1` throw at the
    // top of this function, an undocumented coupling across 110 lines: relax that
    // refusal and `FROM a JOIN b ON k1 LEFT JOIN c ON k2` would decide `outer`
    // from the wrong clause and build an inner join over a/b. Naming the clause
    // once makes the dependency local, and the assertion below the swap makes the
    // remaining one loud.
    const SelectStatement::JoinClause* jc =
        stmt.joins.empty() ? nullptr : &stmt.joins.front();
    const bool outer = jc && jc->type == JoinType::LEFT;
    std::vector<JoinKey> join_keys;
    std::unique_ptr<Expr> on_residual;
    if (jc) {
        JoinCondition on = classifyJoinCondition(jc->condition.get(), 1);
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

    const TableMetadata& meta = catalog.getTable(from_table);

    // narrowed scan schema via the shared logical-layer helper — this also
    // fixes SELECT * + JOIN under columnar storage, which previously narrowed
    // the join table by the FROM table's column names
    Schema scan_schema = buildScanSchema(stmt, meta.schema);

    // SEAM AUDIT pass 2 (E-1's leftover, handed over by the sort-tiebreak
    // fixer). ROW STORAGE USED TO HAND SeqScanNode THE FULL CATALOG SCHEMA while
    // every columnar mode handed it `scan_schema`. The scan node's row path
    // returns `&rows_[cursor_]` verbatim, so a narrowed schema over a wide row
    // would have mis-indexed every column — which is why the wide schema was
    // there, and why closing the asymmetry means narrowing the ROWS, not just
    // the schema.
    //
    // WHY IT MATTERS, and it is not projection pushdown. sort_comparator's
    // tie-break compares the WHOLE ROW when every declared ORDER BY key ties,
    // over `min(schema.size(), row.size())` columns. Two legs holding different
    // column sets therefore tie-break over different data, so a LIMIT cut could
    // survive different rows per mode — and the project asserts
    // `optimized == --no-optimize == Volcano`, so that is a defect even though
    // every such answer is legal SQL. It was benign only because the first
    // discriminating column happened to be `driver_id` in both legs; that is
    // luck, and it changes with the data. The comparator's header names this
    // as the weakest premise it rests on, and it is now simply not a premise.
    //
    // Nothing is lost by narrowing: buildScanSchema collects from the select
    // list, WHERE, GROUP BY, HAVING, ORDER BY and every ON condition, and
    // returns the FULL schema for `SELECT *` or any subquery-bearing statement —
    // so a column it drops is one no clause names. The columnar legs have run on
    // exactly this schema since Week 30, which is the evidence that it suffices.
    //
    // Cost: one pass over the table's rows at plan time, moving Values rather
    // than copying them, and only when the schema actually narrows. Skipped
    // entirely when it does not (SELECT *, subqueries), which is where the wide
    // tables are.
    auto narrowRows = [](std::vector<Row> rows, const Schema& full,
                         const Schema& narrowed) {
        if (narrowed.size() == full.size()) return rows;   // nothing dropped
        std::vector<int> keep;
        keep.reserve(narrowed.size());
        for (const ColumnDef& c : narrowed.columns()) {
            const int i = full.indexOf(c.name);
            if (i < 0)
                throw std::runtime_error(
                    "internal: Planner::plan narrowed a row-storage scan to a "
                    "column '" + c.name + "' the catalog schema does not hold");
            keep.push_back(i);
        }
        for (Row& r : rows) {
            Row narrow;
            narrow.reserve(keep.size());
            for (int i : keep) narrow.push_back(std::move(r[i]));
            r = std::move(narrow);
        }
        return rows;
    };

    // capture before std::move transfers ownership into SeqScanNode
    int from_row_count = columnar_tables.count(from_table) > 0 ? columnar_tables.at(from_table).num_rows : (int)table_rows.at(from_table).size();

    // Self-join: both scans read the same catalog table, keyed once in the
    // map. The FROM scan below moves that data out, so preserve a copy for the
    // JOIN scan. (A copy — not shared ownership — keeps this a minimal change;
    // it costs one extra table copy, acceptable at this project's scale.)
    bool self_join = jc && jc->relation.tableName("Planner::plan JOIN") == from_table;
    std::optional<ColumnarTable> self_join_columnar;
    std::optional<std::vector<Row>> self_join_rows;
    if (self_join) {
        if (columnar_tables.count(from_table) > 0)
            self_join_columnar = columnar_tables.at(from_table);
        else
            self_join_rows = table_rows.at(from_table);
    }

    // build seqScan (bottom of tree) using narrowed schema
    //
    // Week 29: the FROM scan is the PRESERVED side of an outer join, and this is
    // the second of the engine's two pruning-hint routes. For an inner join
    // `stmt.where` already absorbed the ON residuals and every conjunct is a legal
    // filter on the join output; for a LEFT join the fold was deliberately dropped
    // above, so `stmt.where` is exactly where the null-supplying side's conjuncts
    // live — and handing it to this scan is the same delegation to
    // chunk_pruner.h's slot test that the vectorized builder stopped making. Same
    // rule, one implementation (predicate_pushdown.h): Volcano is the correctness
    // baseline, so it is the last path whose latent guard should be the weaker one.
    //
    // Week 30: DERIVED from the FROM scan's own schema, not asserted as {0}.
    // The constant was correct only because of the `stmt.joins.size() > 1`
    // refusal 110 lines above — the identical undocumented coupling the `jc`
    // pointer above exists to remove, re-introduced at a new site with the
    // comment stating the conclusion ("Volcano builds one join") rather than
    // the refusal that guarantees it. Relax that refusal and
    // `FROM a JOIN b ON k1 LEFT JOIN c ON k2 WHERE b.x = 5` would give the two
    // engines different preserved sets: same rows, different work per mode,
    // with nothing to catch it.
    //
    // Mirrors VectorizedPlanBuilder's JOIN case, which reads its set off
    // join->children[0]->output_schema for the same reason. Byte-identical
    // today — a catalog schema stamps slot 0 — but no longer something a future
    // change can invalidate silently.
    std::unordered_set<int> preserved_slots;
    for (const ColumnDef& c : scan_schema.columns()) preserved_slots.insert(c.relation_slot);
    const Expr* prune_hint = pruningHintForPreservedSide(
        stmt.where.get(), outer ? JoinType::LEFT : JoinType::INNER, preserved_slots);

    // The schema `prune_hint` was WRITTEN against, which is where the FilterNode
    // 100 lines below type-checks and evaluates it: the merged join schema when
    // there is a join, the scan schema otherwise. It is NOT the scan's schema,
    // and passing that was not a conservative approximation but a wrong answer —
    // ChunkPruner screens each conjunct for "can raise" and staticTypeOf's
    // bare-name fallback resolved a JOIN-side ref against the FROM table's
    // same-named column, calling a raiser total. Three seam audits found it
    // independently (engine pass 5 E-20, storage S-13, optimizer P5-2), and the
    // scope note that matters here is theirs: this path hands the RAW
    // `stmt.where` to the FROM scan whether or not the optimizer ran, so both of
    // Volcano's legs were affected, in correctness AND in the pruning the
    // previous round's screen lost. See chunk_pruner.h.
    //
    // Built the same way the merged schema below is (FROM columns, then the JOIN
    // relation's stamped slot 1) rather than reusing it, because the scan is
    // constructed before the join is.
    Schema hint_schema = scan_schema;
    if (jc) {
        const TableMetadata& hint_join_meta =
            catalog.getTable(jc->relation.tableName("Planner::plan JOIN"));
        // NAMED, not a temporary in the range-for: until C++23 a range-for binds
        // its `__range` to `.columns()`'s reference and lets the Schema it points
        // into die at the end of that initializer, which reads freed memory.
        const Schema join_side = buildScanSchema(stmt, hint_join_meta.schema);
        std::vector<ColumnDef> hint_cols = scan_schema.columns();
        for (ColumnDef c : join_side.columns()) {
            c.relation_slot = 1;
            hint_cols.push_back(c);
        }
        hint_schema = Schema(hint_cols);
    }

    std::unique_ptr<PlanNode> node;
    if (columnar_tables.count(from_table) > 0) {
        node = std::make_unique<SeqScanNode>(from_table, std::move(columnar_tables.at(from_table)), scan_schema, prune_hint, &hint_schema);
    } else {
        node = std::make_unique<SeqScanNode>(
            from_table,
            narrowRows(std::move(table_rows.at(from_table)), meta.schema, scan_schema),
            scan_schema);
    }

    // hash join (exactly one, guarded above)
    if (jc){
        const auto& join_clause = *jc;
        const TableMetadata& join_meta = catalog.getTable(join_clause.relation.tableName("Planner::plan JOIN"));

        Schema right_scan_schema = buildScanSchema(stmt, join_meta.schema);

        // capture before std::move transfers ownership into SeqScanNode
        int join_row_count = self_join
            ? from_row_count
            : (columnar_tables.count(join_clause.relation.tableName("Planner::plan JOIN")) > 0
                ? columnar_tables.at(join_clause.relation.tableName("Planner::plan JOIN")).num_rows
                : (int)table_rows.at(join_clause.relation.tableName("Planner::plan JOIN")).size());

        std::unique_ptr<PlanNode> right;
        if (self_join) {
            // read from the copy preserved before the FROM scan moved the data
            if (self_join_columnar.has_value())
                right = std::make_unique<SeqScanNode>(join_clause.relation.tableName("Planner::plan JOIN"), std::move(*self_join_columnar), right_scan_schema, nullptr);
            else
                right = std::make_unique<SeqScanNode>(
                    join_clause.relation.tableName("Planner::plan JOIN"),
                    narrowRows(std::move(*self_join_rows), join_meta.schema, right_scan_schema),
                    right_scan_schema);
        } else if (columnar_tables.count(join_clause.relation.tableName("Planner::plan JOIN")) > 0) {
            right = std::make_unique<SeqScanNode>(join_clause.relation.tableName("Planner::plan JOIN"), std::move(columnar_tables.at(join_clause.relation.tableName("Planner::plan JOIN"))), right_scan_schema, nullptr);
        } else {
            right = std::make_unique<SeqScanNode>(
                join_clause.relation.tableName("Planner::plan JOIN"),
                narrowRows(std::move(table_rows.at(join_clause.relation.tableName("Planner::plan JOIN"))),
                           join_meta.schema, right_scan_schema),
                right_scan_schema);
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
        // The preserved side must be the probe input, and the residual is moved
        // into the operator only on the unswapped branch — so a swapped outer
        // join would not merely be the wrong algorithm, it would DROP the ON
        // residual silently. The operator's own constructor refuses the
        // combination too; this one names the reason at the place that decides it.
        if (outer && swap) {
            throw std::runtime_error(
                "internal: a left outer join must not swap its build side");
        }
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

    // Is the row order reaching a LIMIT a function of the query alone? See the
    // deterministic-cut block at the bottom of this function. Captured HERE
    // because the ORDER BY test below moves `stmt.order_by` out, and a
    // moved-from vector is not a question worth asking.
    bool order_is_plan_stable = (jc == nullptr) || !stmt.order_by.empty();

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
            // HAVING/ORDER-BY-only aggregates never reach output. The OTHER
            // producer of `hidden` — a lowered correlated scalar's synthetic
            // relation (seam audit pass 2, B-1) — cannot occur on this path:
            // every correlated subquery is refused above, and this path runs no
            // LogicalPlanBuilder. The test is kept identical to build()'s
            // anyway, because "the two star expansions differ" is the shape a
            // Volcano-only wrong answer would take.
            if (col.hidden) continue;
            auto ref = std::make_unique<ColumnRef>();
            ref->column_name = col.name;
            ref->id = ColumnId::local(col.relation_slot);  // schema slot -> local id:
            // preserve side so SELECT * on a self-join emits both sides
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
        // A CUT over a plan-dependent order is a plan-dependent ANSWER. The
        // logical builder answers this with `orderIsPlanStable` over its tree
        // (logical_plan.cc, and the reasoning is there); this path is a straight
        // line whose only unstable node is the join, so the same question is a
        // local fact. Everything else Volcano can build — the scan, the filter,
        // the aggregate's first-encounter group order, HAVING, the projection,
        // DISTINCT — is order-preserving over its input, and multi-way joins,
        // derived tables and subqueries are all refused above.
        //
        // `stmt.order_by` non-empty already put a SortNode below the projection,
        // and the shared comparator makes THAT order plan-independent, so the
        // extra sort is for the no-ORDER-BY case only. The two engines must
        // agree here or the fix is the same divergence with a new cause: a hash
        // join's output is probe-major, and Volcano picks its build side from
        // raw row counts while the vectorized builder picks it from post-pushdown
        // estimates.
        if (!order_is_plan_stable) {
            node = std::make_unique<SortNode>(std::move(node), std::vector<OrderByItem>{},
                                              /*row_cap=*/stmt.limit.value());
        }
        node = std::make_unique<LimitNode>(std::move(node), stmt.limit.value());
    }

    return node;
}


