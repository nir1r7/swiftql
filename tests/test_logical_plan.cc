#include <gtest/gtest.h>
#include "planner/logical_plan.h"
#include "planner/binder.h"
#include "planner/validator.h"
#include "planner/subquery_materialization.h"
#include "execution/evaluator.h"
#include "parser/parser.h"
#include "parser/expr_utils.h"
#include "catalog/catalog.h"
#include "common/schema.h"
#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// The test catalog lives one level up from the build dir (tests run from build/).
static const char* CATALOG = "../tests/data/test_catalog.json";

// Parse + bind + build the logical plan for a query against the test catalog.
static std::unique_ptr<LogicalPlanNode> buildLogical(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    return LogicalPlanBuilder::build(std::move(stmt), cat);
}

// Parse + bind + validate only. Week 31 narrowed the subquery refusal to
// CORRELATED subqueries, so an uncorrelated one now VALIDATES and is
// materialized by the CLI before planning — a test that only wants to know what
// the Validator decides must stop at the Validator rather than plan a statement
// whose subqueries nothing has substituted.
static void validateOnly(const std::string& sql, const Catalog& cat) {
    Parser parser(sql);
    auto stmt = parser.parse();
    Binder::bind(stmt, cat);
    Validator::validate(stmt, cat);
}

// Top-down node-type spine following children[0] (the single-input chain).
static std::vector<LogicalNodeType> spine(const LogicalPlanNode* root) {
    std::vector<LogicalNodeType> types;
    const LogicalPlanNode* n = root;
    while (n) {
        types.push_back(n->type);
        n = n->children.empty() ? nullptr : n->children[0].get();
    }
    return types;
}

// Walk down children[0] to the first node of a given type, or nullptr.
static const LogicalPlanNode* findNode(const LogicalPlanNode* root, LogicalNodeType t) {
    const LogicalPlanNode* n = root;
    while (n) {
        if (n->type == t) return n;
        n = n->children.empty() ? nullptr : n->children[0].get();
    }
    return nullptr;
}

// Recursively collect every node in the tree (all children, not just [0]) —
// used by the explain() smoke test since JOIN branches into two subtrees.
static void collectAllNodes(const LogicalPlanNode* node, std::vector<const LogicalPlanNode*>& out) {
    if (!node) return;
    out.push_back(node);
    for (const auto& child : node->children) collectAllNodes(child.get(), out);
}

// ===== Scan / Project =====

TEST(LogicalPlan, SimpleSelectProducesProjectOverScan) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT team FROM laps", cat);

    EXPECT_EQ(spine(plan.get()), (std::vector<LogicalNodeType>{LogicalNodeType::PROJECT, LogicalNodeType::SCAN}));

    const LogicalPlanNode* scan_node = findNode(plan.get(), LogicalNodeType::SCAN);
    ASSERT_NE(scan_node, nullptr);
    const auto* scan = static_cast<const LogicalScan*>(scan_node);
    EXPECT_EQ(scan->table_name, "laps");
    ASSERT_EQ(scan->output_schema.size(), 1);
    EXPECT_EQ(scan->output_schema.column(0).name, "team");
}

TEST(LogicalPlan, WhereProducesFilterBetweenProjectAndScan) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT team FROM laps WHERE speed > 200", cat);

    EXPECT_EQ(spine(plan.get()), (std::vector<LogicalNodeType>{
        LogicalNodeType::PROJECT, LogicalNodeType::FILTER, LogicalNodeType::SCAN}));

    const LogicalPlanNode* filter_node = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter_node, nullptr);
    const auto* filter = static_cast<const LogicalFilter*>(filter_node);
    EXPECT_NE(exprToString(filter->predicate.get()).find("speed"), std::string::npos);

    const LogicalPlanNode* scan_node = findNode(plan.get(), LogicalNodeType::SCAN);
    ASSERT_NE(scan_node, nullptr);
    const auto* scan = static_cast<const LogicalScan*>(scan_node);
    // "speed" must be present in the scan schema even though it isn't selected.
    EXPECT_TRUE(scan->output_schema.hasColumn("speed"));
    EXPECT_TRUE(scan->output_schema.hasColumn("team"));
}

TEST(LogicalPlan, SelectStarSynthesizesAllColumns) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT * FROM laps", cat);

    ASSERT_EQ(plan->type, LogicalNodeType::PROJECT);
    const auto* project = static_cast<const LogicalProject*>(plan.get());
    EXPECT_EQ(project->exprs.size(), 9u);  // laps has 9 columns in the test catalog

    const LogicalPlanNode* scan_node = findNode(plan.get(), LogicalNodeType::SCAN);
    ASSERT_NE(scan_node, nullptr);
    EXPECT_EQ(project->output_schema.size(), scan_node->output_schema.size());
}

// ===== Aggregation =====

TEST(LogicalPlan, GlobalAggregateWithoutGroupBy) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT AVG(speed) FROM laps", cat);

    EXPECT_EQ(spine(plan.get()), (std::vector<LogicalNodeType>{
        LogicalNodeType::PROJECT, LogicalNodeType::AGGREGATE, LogicalNodeType::SCAN}));

    const LogicalPlanNode* agg_node = findNode(plan.get(), LogicalNodeType::AGGREGATE);
    ASSERT_NE(agg_node, nullptr);
    const auto* agg = static_cast<const LogicalAggregate*>(agg_node);
    EXPECT_TRUE(agg->group_by.empty());
    ASSERT_EQ(agg->aggregates.size(), 1u);
    EXPECT_EQ(agg->aggregates[0].function, "AVG");
    EXPECT_EQ(agg->aggregates[0].column, "speed");
    EXPECT_FALSE(agg->aggregates[0].is_star);

    ASSERT_EQ(agg->output_schema.size(), 1);
    EXPECT_EQ(agg->output_schema.column(0).type, TypeId::DOUBLE);
}

TEST(LogicalPlan, FullClauseQueryProducesExactSpine) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT team, COUNT(*) FROM laps WHERE speed > 200 GROUP BY team "
        "HAVING COUNT(*) > 5 ORDER BY team DESC LIMIT 3", cat);

    // Sort executes above the aggregate/having-filter but below the
    // projection in this engine's spine — see LogicalPlanBuilder::build.
    EXPECT_EQ(spine(plan.get()), (std::vector<LogicalNodeType>{
        LogicalNodeType::LIMIT, LogicalNodeType::PROJECT, LogicalNodeType::SORT,
        LogicalNodeType::FILTER, LogicalNodeType::AGGREGATE, LogicalNodeType::FILTER,
        LogicalNodeType::SCAN}));

    ASSERT_EQ(plan->type, LogicalNodeType::LIMIT);
    const auto* limit = static_cast<const LogicalLimit*>(plan.get());
    EXPECT_EQ(limit->limit, 3);

    const LogicalPlanNode* sort_node = findNode(plan.get(), LogicalNodeType::SORT);
    ASSERT_NE(sort_node, nullptr);
    const auto* sort = static_cast<const LogicalSort*>(sort_node);
    ASSERT_FALSE(sort->order_by.empty());
    EXPECT_TRUE(sort->order_by[0].desc);
}

// ===== Join =====

TEST(LogicalPlan, JoinHasTwoChildrenAndStampedMergedSchema) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id", cat);

    const LogicalPlanNode* join_node = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join_node, nullptr);
    ASSERT_EQ(join_node->children.size(), 2u);

    const LogicalPlanNode* from_child = join_node->children[0].get();
    const LogicalPlanNode* join_child = join_node->children[1].get();
    ASSERT_EQ(from_child->type, LogicalNodeType::SCAN);
    ASSERT_EQ(join_child->type, LogicalNodeType::SCAN);
    EXPECT_EQ(static_cast<const LogicalScan*>(from_child)->table_name, "laps");
    EXPECT_EQ(static_cast<const LogicalScan*>(join_child)->table_name, "drivers");

    // merged schema = FROM columns (slot 0) then JOIN columns (slot 1)
    const Schema& merged = join_node->output_schema;
    int from_size = from_child->output_schema.size();
    int join_size = join_child->output_schema.size();
    ASSERT_EQ(merged.size(), from_size + join_size);
    for (int i = 0; i < from_size; ++i) {
        EXPECT_EQ(merged.column(i).relation_slot, 0);
    }
    for (int i = from_size; i < from_size + join_size; ++i) {
        EXPECT_EQ(merged.column(i).relation_slot, 1);
    }

    const auto* join = static_cast<const LogicalJoin*>(join_node);
    ASSERT_EQ(join->keys.size(), 1u);
    EXPECT_EQ(join->keys[0].from_col, "driver_id");
    EXPECT_EQ(join->keys[0].join_col, "driver_id");
    EXPECT_EQ(join->join_slot, 1);
}

TEST(LogicalPlan, SelfJoinKeysRouteBySlotNotPosition) {
    Catalog cat(CATALOG);

    auto plan1 = buildLogical("SELECT l1.id FROM sj l1 JOIN sj l2 ON l1.id = l2.grp", cat);
    const LogicalPlanNode* join1 = findNode(plan1.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join1, nullptr);
    ASSERT_EQ(join1->children.size(), 2u);
    EXPECT_EQ(static_cast<const LogicalScan*>(join1->children[0].get())->table_name, "sj");
    EXPECT_EQ(static_cast<const LogicalScan*>(join1->children[1].get())->table_name, "sj");
    const auto* j1 = static_cast<const LogicalJoin*>(join1);
    ASSERT_EQ(j1->keys.size(), 1u);
    EXPECT_EQ(j1->keys[0].from_col, "id");
    EXPECT_EQ(j1->keys[0].join_col, "grp");

    // reversed operand order in the ON clause must route identically
    auto plan2 = buildLogical("SELECT l1.id FROM sj l1 JOIN sj l2 ON l2.grp = l1.id", cat);
    const LogicalPlanNode* join2 = findNode(plan2.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join2, nullptr);
    ASSERT_EQ(join2->children.size(), 2u);
    EXPECT_EQ(static_cast<const LogicalScan*>(join2->children[0].get())->table_name, "sj");
    EXPECT_EQ(static_cast<const LogicalScan*>(join2->children[1].get())->table_name, "sj");
    const auto* j2 = static_cast<const LogicalJoin*>(join2);
    ASSERT_EQ(j2->keys.size(), 1u);
    EXPECT_EQ(j2->keys[0].from_col, "id");
    EXPECT_EQ(j2->keys[0].join_col, "grp");
}

// ===== Week 26: multi-way + multi-key logical join trees =====

// The checkpoint: a multi-table query produces a qualified logical join tree.
// "Qualified" = every relation owns a distinct slot in the merged schema, which
// is what lets Schema::indexOf(name, slot) tell same-named columns apart.
TEST(LogicalPlan, ThreeWayJoinIsLeftDeepWithAscendingSlots) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT a.id, c.val FROM sj a JOIN sj b ON a.grp = b.id "
        "JOIN sj c ON b.grp = c.id", cat);

    const LogicalPlanNode* top = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(static_cast<const LogicalJoin*>(top)->join_slot, 2);

    // left-deep: the second join's left input is the first join
    ASSERT_EQ(top->children[0]->type, LogicalNodeType::JOIN);
    EXPECT_EQ(top->children[1]->type, LogicalNodeType::SCAN);
    const auto* inner = static_cast<const LogicalJoin*>(top->children[0].get());
    EXPECT_EQ(inner->join_slot, 1);
    EXPECT_EQ(inner->children[0]->type, LogicalNodeType::SCAN);
    EXPECT_EQ(inner->children[1]->type, LogicalNodeType::SCAN);

    // the merged schema carries all three relations, in relation order
    std::vector<int> slots;
    for (const auto& col : top->output_schema.columns()) slots.push_back(col.relation_slot);
    ASSERT_FALSE(slots.empty());
    EXPECT_EQ(*std::max_element(slots.begin(), slots.end()), 2);
    EXPECT_TRUE(std::is_sorted(slots.begin(), slots.end()));
    for (int s : {0, 1, 2}) {
        EXPECT_NE(std::find(slots.begin(), slots.end(), s), slots.end())
            << "relation " << s << " missing from the merged schema";
    }
}

TEST(LogicalPlan, MultiKeyJoinKeepsBothKeys) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT a.val FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp", cat);

    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    const auto* lj = static_cast<const LogicalJoin*>(join);
    ASSERT_EQ(lj->keys.size(), 2u);
    EXPECT_EQ(lj->keys[0].from_col, "id");
    EXPECT_EQ(lj->keys[1].from_col, "grp");
    EXPECT_EQ(lj->explain(), "LogicalJoin [id = id AND grp = grp]");
}

// A single-key join must render exactly as it did before Week 26 — existing
// --explain assertions depend on it.
TEST(LogicalPlan, SingleKeyJoinExplainUnchanged) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT a.id FROM sj a JOIN sj b ON a.id = b.grp", cat);
    const LogicalPlanNode* join = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->explain(), "LogicalJoin [id = grp]");
}

// A join key must survive scan narrowing even when no other clause names it.
TEST(LogicalPlan, ThreeWayJoinKeepsEveryJoinKeyInScanSchemas) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id JOIN sj c ON b.val = c.val", cat);
    const LogicalPlanNode* top = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(top, nullptr);
    // relation 1's scan needs both `id` (first join) and `val` (second)
    const LogicalPlanNode* inner_right = top->children[0]->children[1].get();
    EXPECT_TRUE(inner_right->output_schema.hasColumn("id"));
    EXPECT_TRUE(inner_right->output_schema.hasColumn("val"));
}

TEST(LogicalPlan, SelectStarOnSelfJoinPreservesSlots) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT * FROM sj s1 JOIN sj s2 ON s1.id = s2.id", cat);

    ASSERT_EQ(plan->type, LogicalNodeType::PROJECT);
    const auto* project = static_cast<const LogicalProject*>(plan.get());
    EXPECT_EQ(project->exprs.size(), 6u);  // sj has 3 columns; 2 sides = 6

    bool found_join_side = false;
    for (const auto& e : project->exprs) {
        if (auto* col = dynamic_cast<const ColumnRef*>(e.get())) {
            if (col->id.slotInOwnScope("test") == 1) {
                found_join_side = true;
                break;
            }
        }
    }
    EXPECT_TRUE(found_join_side);
}

// ===== DISTINCT / LIMIT =====

TEST(LogicalPlan, DistinctAndLimitStackAboveProject) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT DISTINCT team FROM laps LIMIT 2", cat);

    EXPECT_EQ(spine(plan.get()), (std::vector<LogicalNodeType>{
        LogicalNodeType::LIMIT, LogicalNodeType::DISTINCT, LogicalNodeType::PROJECT, LogicalNodeType::SCAN}));
}

// ===== Error propagation =====

TEST(LogicalPlan, UnknownColumnThrows) {
    Catalog cat(CATALOG);
    EXPECT_THROW(buildLogical("SELECT nope FROM laps", cat), std::runtime_error);
}

// ===== Week 17 checkpoint: existing vectorized queries =====

// The five README benchmark queries — the canonical query set every phase
// re-measures on the vectorized path — must each produce a complete logical
// plan: a PROJECT is present, every leaf is a SCAN, and every node explains.
TEST(LogicalPlan, ExistingVectorizedQueriesProduceCompleteLogicalPlans) {
    Catalog cat(CATALOG);
    const std::vector<std::string> corpus = {
        "SELECT AVG(speed) FROM laps",
        "SELECT COUNT(*) FROM laps WHERE season = 2025",
        "SELECT team, speed FROM laps WHERE speed > 300",
        "SELECT team, COUNT(*) FROM laps GROUP BY team",
        "SELECT l.team, AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team",
    };

    for (const auto& sql : corpus) {
        auto plan = buildLogical(sql, cat);
        ASSERT_NE(plan, nullptr) << sql;

        std::vector<const LogicalPlanNode*> all_nodes;
        collectAllNodes(plan.get(), all_nodes);

        bool has_project = false;
        for (const auto* n : all_nodes) {
            if (n->type == LogicalNodeType::PROJECT) has_project = true;
            // leaves must all be scans; scans must all be leaves
            EXPECT_EQ(n->children.empty(), n->type == LogicalNodeType::SCAN) << sql;
            EXPECT_FALSE(n->explain().empty()) << sql;
        }
        EXPECT_TRUE(has_project) << sql;
    }
}

// ===== explain() smoke test =====

TEST(LogicalPlan, ExplainSmokeTest) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT team, COUNT(*) FROM laps WHERE speed > 200 GROUP BY team "
        "HAVING COUNT(*) > 5 ORDER BY team DESC LIMIT 3", cat);

    std::vector<const LogicalPlanNode*> all_nodes;
    collectAllNodes(plan.get(), all_nodes);
    ASSERT_FALSE(all_nodes.empty());
    for (const auto* n : all_nodes) {
        EXPECT_FALSE(n->explain().empty());
    }

    const LogicalPlanNode* scan_node = findNode(plan.get(), LogicalNodeType::SCAN);
    ASSERT_NE(scan_node, nullptr);
    EXPECT_NE(scan_node->explain().find("laps"), std::string::npos);

    // spot-check the WHERE filter (predicate over "speed"), not the HAVING
    // filter above the aggregate.
    auto plan_where = buildLogical("SELECT team FROM laps WHERE speed > 200", cat);
    const LogicalPlanNode* filter_node = findNode(plan_where.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter_node, nullptr);
    EXPECT_NE(filter_node->explain().find("speed"), std::string::npos);
}


// ===== Week 24: expression type inference =====

TEST(InferExprType, ArithmeticTyping) {
    Catalog cat(CATALOG);
    const Schema& laps = cat.getTable("laps").schema;

    auto typeOf = [&](const std::string& sql) {
        Parser parser("SELECT " + sql + " FROM laps");
        auto stmt = parser.parse();
        return inferExprType(stmt.select_list[0].get(), laps);
    };

    EXPECT_EQ(typeOf("season + 1"), TypeId::INT);      // INT op INT stays INT
    EXPECT_EQ(typeOf("season / 4"), TypeId::INT);      // SQLite truncating division
    EXPECT_EQ(typeOf("speed * 2"), TypeId::DOUBLE);    // DOUBLE operand promotes
    EXPECT_EQ(typeOf("season / 2.0"), TypeId::DOUBLE);
    EXPECT_EQ(typeOf("-speed"), TypeId::DOUBLE);       // unary minus passthrough
    EXPECT_EQ(typeOf("-season"), TypeId::INT);
    EXPECT_EQ(typeOf("speed > 300"), TypeId::INT);     // boolean-as-INT convention
    EXPECT_EQ(typeOf("AVG(speed)"), TypeId::DOUBLE);
    EXPECT_EQ(typeOf("COUNT(*)"), TypeId::INT);
    EXPECT_EQ(typeOf("AVG(speed) * 2"), TypeId::DOUBLE);
}

TEST(InferExprType, StringArithmeticThrows) {
    Catalog cat(CATALOG);
    const Schema& laps = cat.getTable("laps").schema;

    auto infer = [&](const std::string& sql) {
        Parser parser("SELECT " + sql + " FROM laps");
        auto stmt = parser.parse();
        return inferExprType(stmt.select_list[0].get(), laps);
    };

    EXPECT_THROW(infer("team + 1"), std::runtime_error);
    EXPECT_THROW(infer("-team"), std::runtime_error);
    EXPECT_THROW(infer("(team + 1) = 5"), std::runtime_error);  // children checked before boolean short-circuit
}

TEST(LogicalPlan, ProjectSchemaTypesExpressions) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT speed * 2, season + 1 FROM laps", cat);

    const Schema& out = plan->output_schema;
    ASSERT_EQ(out.size(), 2);
    EXPECT_EQ(out.column(0).type, TypeId::DOUBLE);   // no STRING fallback
    EXPECT_EQ(out.column(1).type, TypeId::INT);
}

TEST(LogicalPlan, StringArithmeticRejectedAtPlanTime) {
    Catalog cat(CATALOG);
    EXPECT_THROW(buildLogical("SELECT team FROM laps WHERE team + 1 = 5", cat), std::runtime_error);
    EXPECT_THROW(buildLogical("SELECT team + 1 FROM laps", cat), std::runtime_error);
}


// ===== Week 24: expressions in aggregation =====

TEST(LogicalPlan, AggregateOverExpressionExtracted) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT SUM(speed * 2) FROM laps", cat);

    auto* agg = static_cast<const LogicalAggregate*>(findNode(plan.get(), LogicalNodeType::AGGREGATE));
    ASSERT_NE(agg, nullptr);
    ASSERT_EQ(agg->aggregates.size(), 1u);
    EXPECT_TRUE(agg->aggregates[0].column.empty());     // no plain-column fast path
    ASSERT_NE(agg->aggregates[0].argument, nullptr);    // expression argument carried
    EXPECT_EQ(agg->aggregates[0].output_name, "SUM((speed * 2))");
    EXPECT_EQ(agg->output_schema.column(0).name, "SUM((speed * 2))");
}

TEST(LogicalPlan, ExpressionOverAggregatePlansAggregate) {
    // AVG(speed) * 2 is not a top-level AggregateExpr but must still force
    // an aggregation node (recursive detection)
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT AVG(speed) * 2 FROM laps", cat);

    EXPECT_NE(findNode(plan.get(), LogicalNodeType::AGGREGATE), nullptr);
    EXPECT_EQ(plan->output_schema.column(0).type, TypeId::DOUBLE);
}

TEST(LogicalPlan, DuplicateAggregatesShareOneSpec) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT SUM(speed) / COUNT(*), SUM(speed) FROM laps", cat);

    auto* agg = static_cast<const LogicalAggregate*>(findNode(plan.get(), LogicalNodeType::AGGREGATE));
    ASSERT_NE(agg, nullptr);
    // SUM(speed) appears twice, COUNT(*) once: 2 deduped specs
    EXPECT_EQ(agg->aggregates.size(), 2u);
}


// ===== Week 24: expression GROUP BY keys =====

TEST(LogicalPlan, ExpressionGroupBySubstitutesProjectRef) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT season - 2000, COUNT(*) FROM laps GROUP BY season - 2000", cat);

    auto* agg = static_cast<const LogicalAggregate*>(findNode(plan.get(), LogicalNodeType::AGGREGATE));
    ASSERT_NE(agg, nullptr);
    EXPECT_EQ(agg->output_schema.column(0).name, "(season - 2000)");
    EXPECT_EQ(agg->output_schema.column(0).type, TypeId::INT);

    // the select item matching the group key was rewritten into a ColumnRef
    // over the aggregate's group-key output column
    auto* proj = static_cast<const LogicalProject*>(findNode(plan.get(), LogicalNodeType::PROJECT));
    ASSERT_NE(proj, nullptr);
    auto* ref = dynamic_cast<const ColumnRef*>(proj->exprs[0].get());
    ASSERT_NE(ref, nullptr);
    EXPECT_EQ(ref->column_name, "(season - 2000)");
}

TEST(LogicalPlan, ExpressionGroupByScanKeepsInputColumns) {
    // season is referenced only inside the group expression — the narrowed
    // scan schema must still carry it (collectCols over g.expr)
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT COUNT(*) FROM laps GROUP BY season - 2000", cat);

    const LogicalPlanNode* scan = findNode(plan.get(), LogicalNodeType::SCAN);
    ASSERT_NE(scan, nullptr);
    EXPECT_TRUE(scan->output_schema.hasColumn("season"));
}


// ============================================================
// Audit fixes: canonical expression-group-key identity, ordinal
// rejection, nested aggregates, constant folding
// ============================================================

// Expression group keys are matched by exprKey (slot-based), not exprToString
// (as-typed qualifier). All four combinations of qualifying/not-qualifying in
// SELECT, GROUP BY, HAVING, and ORDER BY must therefore agree, as they do in
// SQLite. Matching on the rendered text rejected every mismatched pair, and
// qualifying in SELECT but not GROUP BY is routine in TPC-H.
TEST(GroupKeyIdentity, QualifierMismatchStillMatches) {
    Catalog cat(CATALOG);
    EXPECT_NO_THROW(buildLogical(
        "SELECT laps.season - 1 AS s, COUNT(*) FROM laps GROUP BY season - 1", cat));
    EXPECT_NO_THROW(buildLogical(
        "SELECT season - 1 AS s, COUNT(*) FROM laps GROUP BY laps.season - 1", cat));
    EXPECT_NO_THROW(buildLogical(
        "SELECT season - 1 AS s, COUNT(*) FROM laps GROUP BY season - 1 "
        "HAVING laps.season - 1 > 2021", cat));
    EXPECT_NO_THROW(buildLogical(
        "SELECT season - 1 AS s, COUNT(*) FROM laps GROUP BY season - 1 "
        "ORDER BY laps.season - 1", cat));
}

// The output column is named from the GROUP BY expression regardless of how the
// SELECT list wrote it, so the aggregate schema and the substituted reference
// can never disagree.
TEST(GroupKeyIdentity, OutputColumnNamedFromTheGroupKey) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT laps.season - 1 AS s, COUNT(*) FROM laps GROUP BY season - 1", cat);
    const LogicalPlanNode* agg = findNode(plan.get(), LogicalNodeType::AGGREGATE);
    ASSERT_NE(agg, nullptr);
    // named from the GROUP BY expression, which was written unqualified
    EXPECT_EQ(agg->output_schema.column(0).name, "(season - 1)");
}

// exprKey renders a ColumnRef by relation slot, so a self-join's two
// occurrences stay distinct: SELECT l1.season - 1 with GROUP BY l2.season - 1 is
// a different key and must still be rejected. This is the non-regression that
// stops the fix from over-matching.
TEST(GroupKeyIdentity, SelfJoinSidesStayDistinct) {
    Catalog cat(CATALOG);
    EXPECT_NO_THROW(buildLogical(
        "SELECT l1.grp - 1 AS a, COUNT(*) FROM sj l1 JOIN sj l2 ON l1.id = l2.id "
        "GROUP BY l1.grp - 1", cat));
    EXPECT_THROW(buildLogical(
        "SELECT l1.grp - 1 AS a, COUNT(*) FROM sj l1 JOIN sj l2 ON l1.id = l2.id "
        "GROUP BY l2.grp - 1", cat), std::runtime_error);
}

// A bare integer in ORDER BY parses as a Literal, so ORDER BY 1 used to sort
// every row by the same constant and return them unsorted with no error at all.
// Rejecting is the fix; SQLite would treat it as output column 1.
TEST(ColumnOrdinals, RejectedInOrderByAndGroupBy) {
    Catalog cat(CATALOG);
    EXPECT_THROW(buildLogical("SELECT team, speed FROM laps ORDER BY 1", cat),
                 std::runtime_error);
    EXPECT_THROW(buildLogical("SELECT team, COUNT(*) FROM laps GROUP BY 1", cat),
                 std::runtime_error);
    // a non-ordinal expression that merely contains an integer is unaffected
    EXPECT_NO_THROW(buildLogical("SELECT team, speed FROM laps ORDER BY speed + 1", cat));
    EXPECT_NO_THROW(buildLogical(
        "SELECT season - 1 AS s, COUNT(*) FROM laps GROUP BY season - 1", cat));
}

// collectAggregates() stops walking at an AggregateExpr, so a nested aggregate
// was never collected as a spec and SUM(AVG(speed)) reached execution before
// dying with "Column not found in schema: AVG(speed)".
TEST(NestedAggregates, RejectedAtValidation) {
    Catalog cat(CATALOG);
    EXPECT_THROW(buildLogical("SELECT SUM(AVG(speed)) FROM laps", cat), std::runtime_error);
    EXPECT_THROW(buildLogical("SELECT MAX(COUNT(speed)) FROM laps", cat), std::runtime_error);
    EXPECT_THROW(buildLogical("SELECT SUM(speed + AVG(speed)) FROM laps", cat),
                 std::runtime_error);
    // an aggregate over an ordinary expression is still fine
    EXPECT_NO_THROW(buildLogical("SELECT SUM(speed * (1 - sector_1)) FROM laps", cat));
    // and an expression OVER aggregates is not nesting
    EXPECT_NO_THROW(buildLogical("SELECT SUM(speed) / COUNT(*) FROM laps", cat));
}

// Constant folding rewrites `season = 2020 + 4` to `season = 2024` before
// validation, which restores three separate fast paths at once: zone-map chunk
// pruning, the tight comparison loop in scanColumn, and equality selectivity in
// the cardinality estimator. Measured: 203ms -> 0.35ms on 1M rows.
TEST(ConstantFolding, ArithmeticInAPredicateIsFolded) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT COUNT(*) FROM laps WHERE season = 2020 + 4", cat);
    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->explain(), "LogicalFilter [(season = 2024)]");
}

TEST(ConstantFolding, FoldsNestedAndUnaryConstants) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT COUNT(*) FROM laps WHERE season > 2 * (1000 + 10)", cat);
    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->explain(), "LogicalFilter [(season > 2020)]");

    auto neg = buildLogical("SELECT COUNT(*) FROM laps WHERE season > -(-2024)", cat);
    const LogicalPlanNode* neg_filter = findNode(neg.get(), LogicalNodeType::FILTER);
    ASSERT_NE(neg_filter, nullptr);
    EXPECT_EQ(neg_filter->explain(), "LogicalFilter [(season > 2024)]");
}

// A column reference makes a subtree non-constant, so nothing is folded away.
TEST(ConstantFolding, LeavesColumnReferencesAlone) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT COUNT(*) FROM laps WHERE season = round + 4", cat);
    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->explain(), "LogicalFilter [(season = (round + 4))]");
}

// Folding must not manufacture a NULL literal: the grammar has none, and a
// Literal holding a null Value has no type() for inferExprType to report.
TEST(ConstantFolding, DoesNotFoldToNull) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT COUNT(*) FROM laps WHERE season = 1 / 0", cat);
    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->explain(), "LogicalFilter [(season = (1 / 0))]");
}

// An overflowing constant is left unfolded so the error still surfaces from the
// evaluator with its usual message, rather than from the fold pass.
TEST(ConstantFolding, LeavesOverflowingConstantsUnfolded) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT COUNT(*) FROM laps WHERE season = 9223372036854775807 + 1", cat);
    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->explain(), "LogicalFilter [(season = (9223372036854775807 + 1))]");
}

// An aliased constant keeps its alias through the fold.
TEST(ConstantFolding, PreservesSelectListAliases) {
    Catalog cat(CATALOG);
    auto plan = buildLogical("SELECT 2020 + 4 AS yr FROM laps", cat);
    ASSERT_EQ(plan->output_schema.size(), 1);
    EXPECT_EQ(plan->output_schema.column(0).name, "yr");
}

// Folding is canonicalization, so an expression group key folds too and the two
// sides still match.
TEST(ConstantFolding, AppliesToExpressionGroupKeys) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT season - (2 - 1) AS s, COUNT(*) FROM laps GROUP BY season - 1", cat);
    const LogicalPlanNode* agg = findNode(plan.get(), LogicalNodeType::AGGREGATE);
    ASSERT_NE(agg, nullptr);
    EXPECT_EQ(agg->output_schema.column(0).name, "(season - 1)");
}


// exprKey tags a literal with its type. Value::toString() renders the DOUBLE 1.0
// as "1", so an untagged key made `GROUP BY season - 1` match
// `SELECT season - 1.0`: the projection then read the INT group-key column and
// `(season - 1.0) / 2` truncated to 1010 instead of 1010.5. Silently wrong
// arithmetic, not a display difference — so the mismatch is now an error that
// names the actual cause.
TEST(GroupKeyIdentity, LiteralTypeIsPartOfTheKey) {
    Catalog cat(CATALOG);
    // matched types on both sides: fine, either way round
    EXPECT_NO_THROW(buildLogical(
        "SELECT season - 1 AS s, COUNT(*) FROM laps GROUP BY season - 1", cat));
    EXPECT_NO_THROW(buildLogical(
        "SELECT season - 1.0 AS s, COUNT(*) FROM laps GROUP BY season - 1.0", cat));

    // mismatched literal types are a different expression
    EXPECT_THROW(buildLogical(
        "SELECT season - 1.0 AS s, COUNT(*) FROM laps GROUP BY season - 1", cat),
        std::runtime_error);
    EXPECT_THROW(buildLogical(
        "SELECT season - 1 AS s, COUNT(*) FROM laps GROUP BY season - 1.0", cat),
        std::runtime_error);
}

// The near-miss message has to name the literal type, not report a missing
// GROUP BY column — that was the confusing symptom.
TEST(GroupKeyIdentity, LiteralTypeMismatchExplainsItself) {
    Catalog cat(CATALOG);
    try {
        buildLogical("SELECT season - 1.0 AS s, COUNT(*) FROM laps GROUP BY season - 1", cat);
        FAIL() << "expected a runtime_error";
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("literal's type"), std::string::npos) << msg;
        EXPECT_NE(msg.find("(season - 1)"), std::string::npos) << msg;
    }
}

// A DOUBLE group key keeps its own type through to the output column, so
// downstream arithmetic stays DOUBLE.
TEST(GroupKeyIdentity, DoubleGroupKeyStaysDouble) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT season - 1.0 AS s, COUNT(*) FROM laps GROUP BY season - 1.0", cat);
    const LogicalPlanNode* agg = findNode(plan.get(), LogicalNodeType::AGGREGATE);
    ASSERT_NE(agg, nullptr);
    EXPECT_EQ(agg->output_schema.column(0).type, TypeId::DOUBLE);
}

// ===== Week 25: type inference for the new expression nodes =====
//
// inferExprType is a hard contract: the vectorized path pre-allocates output
// columns from it and ExpressionExecutor::compile asserts its own result type
// matches. A node whose runtime type can differ produces bad_variant_access,
// not a SQL error — so every new node is pinned down here.

TEST(InferExprType, Week25NodeTypes) {
    Catalog cat(CATALOG);
    const Schema& laps = cat.getTable("laps").schema;

    auto typeOf = [&](const std::string& sql) {
        Parser parser("SELECT " + sql + " FROM laps");
        auto stmt = parser.parse();
        return inferExprType(stmt.select_list[0].get(), laps);
    };

    EXPECT_EQ(typeOf("season IN (1, 2)"), TypeId::INT);        // boolean-as-INT
    EXPECT_EQ(typeOf("team IN ('a')"), TypeId::INT);
    EXPECT_EQ(typeOf("team LIKE 'a%'"), TypeId::INT);
    EXPECT_EQ(typeOf("SUBSTRING(team, 1, 2)"), TypeId::STRING);
    EXPECT_EQ(typeOf("SUBSTRING(team, 1)"), TypeId::STRING);
}

TEST(InferExprType, CaseUnifiesResultBranches) {
    Catalog cat(CATALOG);
    const Schema& laps = cat.getTable("laps").schema;

    auto typeOf = [&](const std::string& sql) {
        Parser parser("SELECT " + sql + " FROM laps");
        auto stmt = parser.parse();
        return inferExprType(stmt.select_list[0].get(), laps);
    };
    auto infer = typeOf;

    // same promotion rule as arithmetic: INT+INT stays INT, any DOUBLE promotes
    EXPECT_EQ(typeOf("CASE WHEN season = 1 THEN 1 ELSE 2 END"), TypeId::INT);
    EXPECT_EQ(typeOf("CASE WHEN season = 1 THEN 1 ELSE 2.5 END"), TypeId::DOUBLE);
    EXPECT_EQ(typeOf("CASE WHEN season = 1 THEN speed ELSE 0 END"), TypeId::DOUBLE);
    EXPECT_EQ(typeOf("CASE WHEN season = 1 THEN 'a' ELSE 'b' END"), TypeId::STRING);
    // a missing ELSE contributes NULL, which is typeless and cannot promote
    EXPECT_EQ(typeOf("CASE WHEN season = 1 THEN 1 END"), TypeId::INT);
    // multi-branch unification looks at every THEN
    EXPECT_EQ(typeOf("CASE WHEN season = 1 THEN 1 WHEN season = 2 THEN 2.5 ELSE 0 END"),
              TypeId::DOUBLE);

    EXPECT_THROW(infer("CASE WHEN season = 1 THEN 'a' ELSE 2 END"), std::runtime_error);
    EXPECT_THROW(infer("CASE WHEN season = 1 THEN 1 ELSE team END"), std::runtime_error);
    EXPECT_THROW(infer("CASE WHEN speed THEN 1 ELSE 2 END"), std::runtime_error);
}

TEST(InferExprType, Week25IllTypedNodesThrow) {
    Catalog cat(CATALOG);
    const Schema& laps = cat.getTable("laps").schema;

    auto infer = [&](const std::string& sql) {
        Parser parser("SELECT " + sql + " FROM laps");
        auto stmt = parser.parse();
        return inferExprType(stmt.select_list[0].get(), laps);
    };

    // Value's comparison operators throw on STRING vs numeric; reject at plan
    // time so the failure is a SQL error, not a per-row throw inside the scan
    EXPECT_THROW(infer("season IN ('a', 1)"), std::runtime_error);
    EXPECT_THROW(infer("team IN (1)"), std::runtime_error);
    EXPECT_THROW(infer("speed LIKE 'a%'"), std::runtime_error);
    EXPECT_THROW(infer("SUBSTRING(season, 1, 2)"), std::runtime_error);
    EXPECT_THROW(infer("SUBSTRING(team, 1.5, 2)"), std::runtime_error);
    EXPECT_THROW(infer("SUBSTRING(team, 1, speed)"), std::runtime_error);
}

// An interval that survives folding means the query was not constant date
// arithmetic. Loud at plan time is the design; see ast.h.
TEST(InferExprType, UnfoldedIntervalThrows) {
    Catalog cat(CATALOG);
    const Schema& laps = cat.getTable("laps").schema;
    Parser parser("SELECT team FROM laps WHERE season < interval '90' day");
    auto stmt = parser.parse();
    EXPECT_THROW(inferExprType(stmt.where.get(), laps), std::runtime_error);
}

// ===== Week 25: interval folding =====
//
// `date '1998-12-01' - interval '90' day` must reach the planner as a plain
// STRING Literal. This is the whole reason IntervalLiteral exists: a runtime
// date subtraction would leave a computed expression on the right of the
// comparison, which defeats ChunkPruner, scanColumn's typed loop and
// selectivity() simultaneously — the same regression `season = 2020 + 4` had.
TEST(ConstantFolding, DateMinusIntervalFoldsToALiteral) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT COUNT(*) FROM laps WHERE team <= date '1998-12-01' - interval '90' day", cat);
    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->explain(), "LogicalFilter [(team <= 1998-09-02)]");
}

TEST(ConstantFolding, DatePlusIntervalHandlesEveryUnitAndOperandOrder) {
    Catalog cat(CATALOG);
    auto folded = [&](const std::string& expr) {
        auto plan = buildLogical("SELECT COUNT(*) FROM laps WHERE team >= " + expr, cat);
        const LogicalPlanNode* f = findNode(plan.get(), LogicalNodeType::FILTER);
        return f ? f->explain() : std::string("<no filter>");
    };

    EXPECT_EQ(folded("date '1994-01-01' + interval '1' year"),
              "LogicalFilter [(team >= 1995-01-01)]");
    EXPECT_EQ(folded("date '1994-01-01' + interval '3' month"),
              "LogicalFilter [(team >= 1994-04-01)]");
    EXPECT_EQ(folded("date '1994-01-01' + interval '31' day"),
              "LogicalFilter [(team >= 1994-02-01)]");
    // `interval + date` is legal SQL too; only '+' may commute
    EXPECT_EQ(folded("interval '3' month + date '2024-01-31'"),
              "LogicalFilter [(team >= 2024-04-30)]");
    // month arithmetic clamps the day to the target month
    EXPECT_EQ(folded("date '2024-01-31' + interval '1' month"),
              "LogicalFilter [(team >= 2024-02-29)]");
    // and chains fold left to right
    EXPECT_EQ(folded("date '1994-01-01' + interval '1' year + interval '1' month"),
              "LogicalFilter [(team >= 1995-02-01)]");
}

// The fold is what makes BETWEEN on dates a pair of plain literal comparisons,
// which is the shape TPC-H Q6/Q7/Q8 need for pruning.
TEST(ConstantFolding, BetweenOnDatesFoldsBothBounds) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT COUNT(*) FROM laps WHERE team BETWEEN date '1994-01-01' "
        "AND date '1994-01-01' + interval '1' year", cat);
    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->explain(),
              "LogicalFilter [((team >= 1994-01-01) AND (team <= 1995-01-01))]");
}

// TPC-H Q6 writes its bounds as `[D] - 0.01` / `[D] + 0.01`. Both engines fold
// with the same IEEE doubles, so SwiftQL and SQLite agree bit for bit.
TEST(ConstantFolding, BetweenFoldsArithmeticBounds) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT COUNT(*) FROM laps WHERE speed BETWEEN 300 - 10 AND 300 + 10", cat);
    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->explain(),
              "LogicalFilter [((speed >= 290) AND (speed <= 310))]");
}

// checkGroupedRefs (validator.cc) is a dispatch site SEPARATE from
// Validator::validateExpr, and development.md's checklist does not list it. It
// fails silently: an ungrouped column hidden inside a Week 25 node passes
// validation and then dies at execution with "Column not found in schema"
// against the post-aggregate schema, far from the cause.
TEST(Validation, UngroupedColumnInsideWeek25NodesIsRejected) {
    Catalog cat(CATALOG);

    // Assert on the MESSAGE, not just that something threw. Without the fix
    // these queries still fail — but with "column not found: 'season'" from
    // inferExprType against the post-aggregate schema, which is the confusing
    // far-from-the-cause failure this site exists to prevent. Only the
    // validator's own message proves checkGroupedRefs recursed.
    auto expectGroupByError = [&](const std::string& sql) {
        try {
            buildLogical(sql, cat);
            ADD_FAILURE() << "expected a GROUP BY error for: " << sql;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find("must appear in GROUP BY"), std::string::npos)
                << "wrong error for: " << sql << "\n  got: " << e.what();
        }
    };

    expectGroupByError(
        "SELECT team, CASE WHEN season > 2020 THEN 1 ELSE 0 END FROM laps GROUP BY team");
    expectGroupByError(
        "SELECT team, CASE WHEN 1 = 1 THEN season ELSE 0 END FROM laps GROUP BY team");
    // group by season so `team` is the ungrouped ref, and keep every earlier
    // select item grouped so the error can only come from inside the new node
    expectGroupByError("SELECT season, SUBSTRING(team, 1, 2) FROM laps GROUP BY season");
    expectGroupByError("SELECT team, season IN (2020) FROM laps GROUP BY team");
    expectGroupByError("SELECT season, team LIKE 'a%' FROM laps GROUP BY season");

    // the same shapes are fine when the column IS grouped, or is inside an
    // aggregate (whose argument is evaluated pre-grouping)
    EXPECT_NO_THROW(buildLogical(
        "SELECT team, SUBSTRING(team, 1, 2) FROM laps GROUP BY team", cat));
    EXPECT_NO_THROW(buildLogical(
        "SELECT team, SUM(CASE WHEN season > 2020 THEN 1 ELSE 0 END) FROM laps GROUP BY team", cat));
}

// The fold must surface an out-of-range date as an error, not as a silently
// wrapped literal. `+ interval '100000' year` used to plan as the INPUT date.
TEST(ConstantFolding, OutOfRangeDateArithmeticIsAnError) {
    Catalog cat(CATALOG);
    auto plan = [&](const std::string& expr) {
        return buildLogical("SELECT COUNT(*) FROM laps WHERE team >= " + expr, cat);
    };
    EXPECT_THROW(plan("date '1994-01-01' + interval '100000' year"), std::runtime_error);
    EXPECT_THROW(plan("date '1994-01-01' - interval '2000' year"), std::runtime_error);
    EXPECT_THROW(plan("date '1994-01-01' + interval '9223372036854775807' day"),
                 std::runtime_error);
    // INT64_MIN: `-count` would be UB, so the fold routes through checkedNegate
    EXPECT_THROW(plan("date '1994-01-01' - interval '-9223372036854775808' day"),
                 std::runtime_error);

    // in-range arithmetic is untouched
    EXPECT_NO_THROW(plan("date '1994-01-01' + interval '1' year"));
    EXPECT_NO_THROW(plan("date '1994-01-01' + interval '8000' year"));
}

// ===== Week 27: residual ON conjuncts =====

// A non-key ON conjunct becomes a predicate over the join's output. It is folded
// into the WHERE conjunction rather than given its own LogicalFilter, because
// PredicatePushdown only rewrites a FILTER whose DIRECT child is a JOIN — a
// stacked pair would leave the WHERE unpushed.
TEST(LogicalPlan, ResidualOnConjunctBecomesTheFilterAboveTheJoin) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT a.val FROM sj a JOIN sj b ON a.id = b.id AND a.grp < b.grp", cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->children[0]->type, LogicalNodeType::JOIN);
    EXPECT_EQ(filter->explain(), "LogicalFilter [(a.grp < b.grp)]");

    // and the join kept exactly the one real key
    const auto* lj = static_cast<const LogicalJoin*>(filter->children[0].get());
    ASSERT_EQ(lj->keys.size(), 1u);
    EXPECT_EQ(lj->keys[0].from_col, "id");
}

// One filter, not two: a written WHERE and a residual ON conjunct share it.
TEST(LogicalPlan, ResidualOnConjunctAndWhereShareOneFilter) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT a.val FROM sj a JOIN sj b ON a.id = b.id AND a.grp < b.grp "
        "WHERE a.val > 1", cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    EXPECT_EQ(filter->children[0]->type, LogicalNodeType::JOIN)
        << "a second stacked filter would break pushdown's FILTER-over-JOIN match";
}

// The residual is CLONED out of the statement's ON tree: the statement dies with
// build(), so a borrowed pointer would dangle. Cloning must preserve the binder
// slots, or the conjunct resolves against the wrong relation after the fold.
TEST(LogicalPlan, ResidualOnConjunctKeepsItsRelationSlots) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT a.val FROM sj a JOIN sj b ON a.id = b.id AND a.grp < b.grp", cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    const auto* bin = dynamic_cast<const BinaryExpr*>(
        static_cast<const LogicalFilter*>(filter)->predicate.get());
    ASSERT_NE(bin, nullptr);
    const auto* lhs = dynamic_cast<const ColumnRef*>(bin->left.get());
    const auto* rhs = dynamic_cast<const ColumnRef*>(bin->right.get());
    ASSERT_NE(lhs, nullptr);
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(lhs->id.slotInOwnScope("test"), 0);
    EXPECT_EQ(rhs->id.slotInOwnScope("test"), 1);
}

// A residual column must survive scan narrowing: buildScanSchema collects from
// every ON condition, so a column referenced ONLY by a residual still reaches
// its scan. Without that the plan fails later with "column not found".
TEST(LogicalPlan, ResidualOnlyColumnSurvivesScanNarrowing) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND b.val > 1", cat);

    const LogicalPlanNode* filter = findNode(plan.get(), LogicalNodeType::FILTER);
    ASSERT_NE(filter, nullptr);
    const LogicalPlanNode* join = filter->children[0].get();
    EXPECT_GE(join->children[1]->output_schema.indexOf("val"), 0);
}

// The logical plan has the same problem and the same fix: a key name that
// appears at several relation slots on the left input is qualified with the slot
// the key actually resolved to, so a plan built against the wrong relation no
// longer renders identically to the right one.
TEST(LogicalPlan, ExplainQualifiesAJoinKeyThatIsAmbiguousOnTheLeftInput) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT a.val FROM sj a JOIN sj b ON a.id = b.id "
        "JOIN sj c ON b.grp = c.grp", cat);

    const LogicalPlanNode* top = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(top, nullptr);
    // `grp` exists at slot 0 (a) and slot 1 (b) on the merged left schema
    EXPECT_EQ(top->explain(), "LogicalJoin [grp@1 = grp]");
}

// ===== Week 30: the subquery node's dispatch sites =====

static const SubqueryExpr* firstSubquery(const Expr* e) {
    if (!e) return nullptr;
    if (auto* s = dynamic_cast<const SubqueryExpr*>(e)) return s;
    if (auto* b = dynamic_cast<const BinaryExpr*>(e)) {
        if (auto* l = firstSubquery(b->left.get())) return l;
        return firstSubquery(b->right.get());
    }
    return nullptr;
}

static SelectStatement bindOnly(const std::string& sql, const Catalog& cat) {
    Parser p(sql);
    auto stmt = p.parse();
    Binder::bind(stmt, cat);
    return stmt;
}

// DISPATCH SITE 11. cloneExpr THROWS on an unknown subtype, which is why it is
// done first. The statement is SHARED rather than deep-copied: SelectStatement
// is move-only, so a deep copy would need a clone-a-statement walker whose
// omissions (a dropped HAVING) are silent.
TEST(SubqueryDispatch, CloneSharesTheStatementAndCopiesTheOperand) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT name FROM drivers WHERE driver_id IN "
                         "(SELECT driver_id FROM laps)", cat);
    const SubqueryExpr* sq = firstSubquery(stmt.where.get());
    ASSERT_NE(sq, nullptr);

    auto copy = cloneExpr(sq);
    auto* c = dynamic_cast<SubqueryExpr*>(copy.get());
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->subquery.get(), sq->subquery.get()) << "the statement is shared";
    EXPECT_NE(c->operand.get(), sq->operand.get()) << "the operand is a real copy";
    EXPECT_EQ(dynamic_cast<ColumnRef*>(c->operand.get())->id.slotInOwnScope("test"),
              dynamic_cast<const ColumnRef*>(sq->operand.get())->id.slotInOwnScope("test"));
    EXPECT_EQ(c->kind, sq->kind);
    EXPECT_EQ(c->negated, sq->negated);
    EXPECT_EQ(c->correlated, sq->correlated);
}

// DISPATCH SITE 1. Falling through to "?" would give two different subqueries
// ONE identity, and substituteInto() rewrites any subtree whose exprKey matches
// a GROUP BY key. The address is stable across a clone precisely because the
// statement is shared.
TEST(SubqueryDispatch, ExprKeyIsDistinctPerSubqueryAndStableAcrossAClone) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps) "
                         "AND season > (SELECT MIN(season) FROM laps)", cat);
    auto* top = dynamic_cast<const BinaryExpr*>(stmt.where.get());
    ASSERT_NE(top, nullptr);
    const SubqueryExpr* a = firstSubquery(top->left.get());
    const SubqueryExpr* b = firstSubquery(top->right.get());
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_NE(exprKey(a), exprKey(b));
    EXPECT_NE(exprKey(a), "?");

    auto copy = cloneExpr(a);
    EXPECT_EQ(exprKey(copy.get()), exprKey(a));
}

// DISPATCH SITE 10. Visible: --explain prints predicates.
TEST(SubqueryDispatch, ExprToStringRendersEachForm) {
    Catalog cat(CATALOG);
    auto s1 = bindOnly("SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)", cat);
    EXPECT_EQ(exprToString(firstSubquery(s1.where.get())), "(SELECT ...)");

    auto s2 = bindOnly("SELECT name FROM drivers WHERE NOT EXISTS (SELECT * FROM laps)", cat);
    EXPECT_EQ(exprToString(firstSubquery(s2.where.get())), "NOT EXISTS (SELECT ...)");

    auto s3 = bindOnly("SELECT name FROM drivers WHERE driver_id IN "
                       "(SELECT driver_id FROM laps)", cat);
    EXPECT_EQ(exprToString(firstSubquery(s3.where.get())), "driver_id IN (SELECT ...)");
}

// DISPATCH SITE 7, the sharpest silent one. Collecting the INNER aggregate as an
// outer AggregateSpec would compute AVG(speed) over the OUTER relation and emit
// a column for it.
TEST(SubqueryDispatch, CollectAggregatesStopsAtTheSubqueryBoundary) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team, SUM(speed) FROM laps GROUP BY team "
                         "HAVING SUM(speed) > (SELECT AVG(speed) FROM drivers)", cat);
    std::vector<const AggregateExpr*> aggs;
    collectAggregates(stmt.having.get(), aggs);
    ASSERT_EQ(aggs.size(), 1u) << "the subquery's own AVG must not be collected here";
    EXPECT_EQ(aggs[0]->function_name, "SUM");
}

// DISPATCH SITES 12 and 13 both THROW, which is what makes every later omission
// loud. Week 31 closed them as INTERNAL invariants rather than as features: an
// uncorrelated subquery is replaced by a constant before planning and a
// correlated one is refused, so a SubqueryExpr reaching either site means the
// materialization walker (site 19) missed an Expr subtype.
//
// That is precisely why these two throws are the backstop that lets site 19 be a
// LOUD dispatch site instead of the eleventh silent one. Deleting them would
// turn a missed subtype into a wrong answer.
TEST(SubqueryDispatch, TypeInferenceAndEvaluationAreLoudAndNamed) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)", cat);
    const SubqueryExpr* sq = firstSubquery(stmt.where.get());
    ASSERT_NE(sq, nullptr);
    const Schema& s = cat.getTable("laps").schema;
    try {
        inferExprType(sq, s);
        ADD_FAILURE() << "inferExprType must throw for a subquery";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("without being materialized"), std::string::npos)
            << e.what();
    }
    // site 13, in the same test as site 12 because they must close together:
    // evaluate() is the semantic reference the vectorized kernels are checked
    // against, and the two must never disagree about what a node means.
    try {
        evaluate(sq, Row{}, s);
        ADD_FAILURE() << "evaluate must throw for a subquery";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("without being materialized"), std::string::npos)
            << e.what();
    }
}

// ===== Week 30: what Validator refuses, and in which order =====

TEST(SubqueryValidation, PositionIsRestrictedToWhereAndHaving) {
    Catalog cat(CATALOG);
    auto expectMessage = [&](const std::string& sql, const std::string& needle) {
        try {
            buildLogical(sql, cat);
            ADD_FAILURE() << "expected a rejection for: " << sql;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
                << "for: " << sql << "\n  actual: " << e.what();
        }
    };
    expectMessage("SELECT (SELECT AVG(speed) FROM laps) FROM drivers",
                  "SELECT: subqueries are supported in WHERE and HAVING only");
    expectMessage("SELECT team FROM laps GROUP BY (SELECT AVG(speed) FROM laps)",
                  "GROUP BY: subqueries are supported in WHERE and HAVING only");
    expectMessage("SELECT team FROM laps ORDER BY (SELECT AVG(speed) FROM laps)",
                  "ORDER BY: subqueries are supported in WHERE and HAVING only");
    // DISPATCH SITE 18, in the shape that file already uses for AggregateExpr
    expectMessage("SELECT l.team FROM laps l JOIN drivers d "
                  "ON l.driver_id = d.driver_id AND EXISTS (SELECT * FROM laps)",
                  "JOIN ON: subqueries are not supported in a join condition");
}

// Arity is decidable at bind time from the select list alone; CARDINALITY
// ("returned more than one row") is Week 31's runtime check. EXISTS has no arity
// rule at all — TPC-H Q4 and Q21 both write `select *`.
TEST(SubqueryValidation, ScalarAndInRequireExactlyOneOutputColumn) {
    Catalog cat(CATALOG);
    auto expectMessage = [&](const std::string& sql, const std::string& needle) {
        try {
            buildLogical(sql, cat);
            ADD_FAILURE() << "expected a rejection for: " << sql;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
                << "for: " << sql << "\n  actual: " << e.what();
        }
    };
    expectMessage("SELECT team FROM laps WHERE speed > (SELECT speed, team FROM laps)",
                  "scalar subquery must return exactly one column");
    expectMessage("SELECT team FROM laps WHERE speed > (SELECT * FROM laps)",
                  "scalar subquery must return exactly one column");
    expectMessage("SELECT team FROM laps WHERE season IN (SELECT * FROM drivers)",
                  "IN subquery must return exactly one column");
    // ...and EXISTS (SELECT *) has no arity rule at all, so since Week 31 it is
    // simply a legal, executable query: the check must not have grown into one
    // that rejects what EXISTS is entitled to write.
    EXPECT_NO_THROW(validateOnly("SELECT team FROM laps WHERE EXISTS (SELECT * FROM drivers)", cat));
}

// The refusal is LAST: every parse, bind and validate error a query is entitled
// to fires first. Proving it needs faults INSIDE the nested query, which is also
// what proves validateExpr handed the body to a fresh validation against its own
// schema rather than descending with the outer one.
//
// Week 31 narrowed the refusal to correlated subqueries and left the ordering
// alone, which is what these cases pin: every message below is unchanged, and
// the correlated case at the end shows the discipline still holds for the
// refusal that replaced it.
TEST(SubqueryValidation, RealQueryDefectsOutrankTheNotExecutableRefusal) {
    Catalog cat(CATALOG);
    auto expectMessage = [&](const std::string& sql, const std::string& needle) {
        try {
            buildLogical(sql, cat);
            ADD_FAILURE() << "expected a rejection for: " << sql;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find(needle), std::string::npos)
                << "for: " << sql << "\n  actual: " << e.what();
        }
    };
    expectMessage("SELECT team FROM laps WHERE EXISTS (SELECT * FROM nosuchtable)",
                  "Table not found: 'nosuchtable'");
    expectMessage("SELECT team FROM laps WHERE EXISTS "
                  "(SELECT * FROM drivers WHERE nosuchcol = 1)",
                  "column not found: 'nosuchcol'");
    expectMessage("SELECT team FROM laps WHERE EXISTS "
                  "(SELECT name FROM drivers GROUP BY age)",
                  "must appear in GROUP BY");
    expectMessage("SELECT team FROM laps WHERE EXISTS "
                  "(SELECT COUNT(*) FROM drivers HAVING COUNT(*) > 1)",
                  "HAVING requires GROUP BY");
    // ...and the outer query's own faults still come first too
    expectMessage("SELECT nosuchcol FROM laps WHERE EXISTS (SELECT * FROM drivers)",
                  "column not found: 'nosuchcol'");
    // the same discipline for Week 31's narrowed refusal: a defect inside a
    // CORRELATED subquery must outrank "correlated ... (Week 33)", or that
    // refusal becomes the catch-all the old one was kept from becoming
    expectMessage("SELECT l.team FROM laps l WHERE EXISTS "
                  "(SELECT * FROM drivers d WHERE d.nosuchcol = l.team)",
                  "'nosuchcol' not found");
}

// A subquery's own aggregate is legal even inside a WHERE, because the body is a
// different scope with its own aggregate rule. Descending into it from the outer
// walk (which carries allow_aggregates=false for a WHERE) would reject TPC-H
// Q17, Q11 and Q22 outright.
TEST(SubqueryValidation, AnAggregateInsideASubqueryInWhereIsLegal) {
    Catalog cat(CATALOG);
    // Since Week 31 this validates cleanly rather than reaching a refusal, which
    // is stronger evidence for the same property: if the outer WHERE's
    // aggregate rule reached into the body, this would throw
    // "aggregate functions are not allowed in WHERE clause".
    EXPECT_NO_THROW(
        validateOnly("SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)", cat));
}

// A correlated reference is supplied by an enclosing query, so it is constant
// within every group of THIS one and needs no GROUP BY entry (site 5), and it
// must not be checked against this scope's schema (site 4).
TEST(SubqueryValidation, CorrelatedRefsAreNotThisScopesToCheckOrGroup) {
    Catalog cat(CATALOG);
    try {
        buildLogical("SELECT l.team FROM laps l WHERE EXISTS "
                     "(SELECT d.name FROM drivers d WHERE d.driver_id = l.driver_id "
                     " GROUP BY d.name)", cat);
        ADD_FAILURE() << "expected a correlated-subquery refusal";
    } catch (const std::runtime_error& e) {
        // Week 33 deleted the blanket Validator refusal this used to name and
        // replaced it with per-shape refusals from decorrelation. The subject of
        // the test is unchanged and is what the arrival of ANY refusal proves:
        // neither site 4 nor site 5 threw first, so the correlated ref was
        // neither checked against this scope's schema nor demanded as a group
        // key. What is pinned now is the shape-specific message that arrives.
        EXPECT_NE(std::string(e.what()).find("a body with GROUP BY cannot be decorrelated"),
                  std::string::npos) << e.what();
    }
}

// ===== Week 30 round 1: a correlated ref is not this block's to route =====

// Inside a subquery a correlated ref is an ordinary top-level ref of that ON
// expression, carrying a slot that indexes the ENCLOSING block's range table.
// validateJoinCondition's `relations` is the INNER one, so indexing it compared
// two numbering domains and reported a column against a relation the query never
// named — an error it is not entitled to, arriving instead of the refusal.
TEST(SubqueryValidation, ACorrelatedRefInANestedOnClauseIsNotCheckedHere) {
    Catalog cat(CATALOG);
    try {
        buildLogical("SELECT lap_id FROM laps l WHERE EXISTS "
                     "(SELECT 1 FROM drivers d JOIN drivers d2 "
                     " ON d.driver_id = d2.driver_id AND d.age = l.lap_id)", cat);
        ADD_FAILURE() << "expected a correlated-subquery refusal";
    } catch (const std::runtime_error& e) {
        // The correlated ref is in the body's ON clause, which splitCorrelation
        // does not read, so no join key is produced and decorrelation refuses
        // the whole shape. That refusal is what now stands in for the deleted
        // Validator one; the negative assertion below is the actual subject and
        // is untouched.
        EXPECT_NE(std::string(e.what()).find("no equality links the subquery to the "
                                             "enclosing query"), std::string::npos)
            << e.what();
        EXPECT_EQ(std::string(e.what()).find("not found in table 'd'"), std::string::npos)
            << "an inner-scope schema must not be indexed by an outer-scope slot";
    }
}

// The other half, and the one that is a WRONG ANSWER rather than a wrong message:
// a correlated ref must not become a JoinKey. `l` is the OUTER relation, so this
// inner join has no equality between d and p at all — a cartesian product, which
// classifyJoinCondition exists to refuse. Treating l.driver_id as the left
// operand fabricated JoinKey{driver_id, driver_id, from_slot=0}, joining the
// inner `d` to `p` on a predicate the user never wrote; keys was non-empty, so
// the refusal never fired. Week 31 would plan from that invented key.
TEST(SubqueryValidation, ANestedKeylessJoinStillHitsTheCrossProductRefusal) {
    Catalog cat(CATALOG);
    try {
        buildLogical("SELECT lap_id FROM laps l WHERE EXISTS "
                     "(SELECT 1 FROM drivers d JOIN laps p ON p.driver_id = l.driver_id)", cat);
        ADD_FAILURE() << "expected the cross-product refusal";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("at least one equality"), std::string::npos)
            << e.what();
    }

    // control: a real inner key beside a correlated residual is still legal, and
    // the residual must not have been mistaken for the key that saves it
    try {
        buildLogical("SELECT lap_id FROM laps l WHERE EXISTS "
                     "(SELECT 1 FROM drivers d JOIN laps p "
                     " ON d.driver_id = p.driver_id AND p.speed > l.speed)", cat);
        ADD_FAILURE() << "expected a correlated-subquery refusal";
    } catch (const std::runtime_error& e) {
        // Still refused, and the point survives: the correlated residual
        // `p.speed > l.speed` was not mistaken for the inner key. The message is
        // now decorrelation's, because the body's WHERE holds no correlated
        // equality to key on.
        EXPECT_NE(std::string(e.what()).find("no equality links the subquery to the "
                                             "enclosing query"), std::string::npos)
            << e.what();
    }
}

// A GROUP BY item resolves through resolveColumnRef, which walks OUT, so a
// correlated group key is legal SQL (it is constant within every group). The
// skip used to be keyed on `!g.table_name.empty()`, and the binder only writes a
// qualifier back for a block holding two or more relations — so the SAME
// subquery was refused under a one-relation outer query and accepted under a
// two-relation one.
TEST(SubqueryValidation, ACorrelatedGroupKeyIsAcceptedWhateverTheOuterBlockHolds) {
    Catalog cat(CATALOG);
    auto expectRefusal = [&](const std::string& sql) {
        try {
            buildLogical(sql, cat);
            ADD_FAILURE() << "expected a correlated-subquery refusal for: " << sql;
        } catch (const std::runtime_error& e) {
            // Reaching decorrelation's GROUP BY refusal is the proof the test
            // wants: the correlated group key was ACCEPTED by the validator in
            // both outer shapes (a one-relation and a two-relation enclosing
            // block), so the skip no longer depends on the qualifier the binder
            // writes back. A refusal from the group-key rule itself would name
            // `season`, not the body's GROUP BY.
            EXPECT_NE(std::string(e.what()).find("a body with GROUP BY cannot be decorrelated"),
                      std::string::npos) << "for: " << sql << "\n  actual: " << e.what();
        }
    };
    // `season` exists only in laps, so it resolves outward in both
    expectRefusal("SELECT lap_id FROM laps l WHERE EXISTS "
                  "(SELECT COUNT(*) FROM drivers d GROUP BY season)");
    expectRefusal("SELECT l.lap_id FROM laps l JOIN drivers dd "
                  "ON l.driver_id = dd.driver_id WHERE EXISTS "
                  "(SELECT COUNT(*) FROM drivers d GROUP BY season)");

    // ...and a genuinely missing group key is still refused, in both shapes
    for (const char* sql : {"SELECT lap_id FROM laps l WHERE EXISTS "
                            "(SELECT COUNT(*) FROM drivers d GROUP BY nosuchcol)",
                            "SELECT lap_id FROM laps GROUP BY nosuchcol"}) {
        try {
            buildLogical(sql, cat);
            ADD_FAILURE() << "expected a GROUP BY error for: " << sql;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find("GROUP BY column not found"),
                      std::string::npos) << "for: " << sql << "\n  actual: " << e.what();
        }
    }
}

// The position rule was a one-line test on the ROOT of the ORDER BY expression,
// so a subquery one level down was invisible. SELECT and GROUP BY have no such
// hole because they route through validateExpr, whose allow_subqueries=false
// default is checked at every node; ORDER BY now does too.
TEST(SubqueryValidation, TheOrderByPositionRuleIsRecursive) {
    Catalog cat(CATALOG);
    auto expectPositionError = [&](const std::string& sql) {
        try {
            buildLogical(sql, cat);
            ADD_FAILURE() << "expected a position error for: " << sql;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find(
                          "ORDER BY: subqueries are supported in WHERE and HAVING only"),
                      std::string::npos) << "for: " << sql << "\n  actual: " << e.what();
        }
    };
    expectPositionError("SELECT lap_id FROM laps ORDER BY (SELECT MAX(age) FROM drivers)");
    expectPositionError("SELECT lap_id FROM laps ORDER BY lap_id + (SELECT MAX(age) FROM drivers)");
    expectPositionError("SELECT lap_id FROM laps ORDER BY "
                        "CASE WHEN lap_id > (SELECT MAX(age) FROM drivers) THEN 1 ELSE 0 END");
    // the bare-ColumnRef check keeps owning its own message
    try {
        buildLogical("SELECT lap_id FROM laps ORDER BY nosuchcol", cat);
        ADD_FAILURE() << "expected an ORDER BY column error";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("ORDER BY column not found: 'nosuchcol'"),
                  std::string::npos) << e.what();
    }
}

// DISPATCH SITE 14. Week 30's own rule — descend into what is written in THIS
// block, never into the body — was applied at sites 2, 5, 6, 7, 8 and 9 and not
// here. Week 32's semi-join probes on exactly this operand, and three fast paths
// pattern-match on the ColumnRef-op-Literal shape folding restores.
TEST(SubqueryDispatch, ConstantFoldingReachesTheInOperandAndNotTheBody) {
    Catalog cat(CATALOG);
    auto stmt = bindOnly("SELECT team FROM laps WHERE season + 4 IN "
                         "(SELECT driver_id + 1 FROM drivers)", cat);
    auto* sq = dynamic_cast<const SubqueryExpr*>(stmt.where.get());
    ASSERT_NE(sq, nullptr);
    // `season + 4` has a ColumnRef on the left, so it does not fold to a literal —
    // what must fold is a constant SUBexpression inside it
    auto stmt2 = bindOnly("SELECT team FROM laps WHERE season IN "
                          "(SELECT driver_id FROM drivers) AND round = 2 + 3", cat);
    auto* conj = dynamic_cast<const BinaryExpr*>(stmt2.where.get());
    ASSERT_NE(conj, nullptr);

    auto folded = bindOnly("SELECT team FROM laps WHERE season * (2 + 3) IN "
                           "(SELECT driver_id FROM drivers)", cat);
    auto* fsq = dynamic_cast<const SubqueryExpr*>(folded.where.get());
    ASSERT_NE(fsq, nullptr);
    auto* mul = dynamic_cast<const BinaryExpr*>(fsq->operand.get());
    ASSERT_NE(mul, nullptr) << "the operand must still be the arithmetic tree";
    EXPECT_NE(dynamic_cast<const Literal*>(mul->right.get()), nullptr)
        << "(2 + 3) inside the IN operand must have folded to a literal: "
        << exprToString(fsq->operand.get());
}

// ===== Week 30 round 2: two more (level, slot) collapses =====

// The SUM/AVG argument type check indexes `stmt.joins` — THIS statement's join
// list — by the argument's slot, and validateQuery recurses into every nested
// statement. A correlated argument therefore indexed the INNER list with an
// OUTER slot, so the same illegal aggregate was caught or silently skipped
// depending on the order of the inner query's own joins. Both orders, plus the
// no-inner-join case, must now agree.
TEST(SubqueryValidation, ACorrelatedAggregateArgumentIsTypedWhereItResolved) {
    Catalog cat(CATALOG);
    auto expectStringRefusal = [&](const std::string& sql) {
        try {
            buildLogical(sql, cat);
            ADD_FAILURE() << "expected the SUM type error for: " << sql;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find(
                          "SUM() requires a numeric column, but 'name' is of type STRING"),
                      std::string::npos) << "for: " << sql << "\n  actual: " << e.what();
        }
    };
    const std::string outer =
        "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id WHERE EXISTS ";
    // inner joins[0] is `drivers`, which happens to hold `name` — the case that
    // reported the right answer for the wrong reason
    expectStringRefusal(outer + "(SELECT SUM(d.name) FROM laps y JOIN drivers x "
                                "ON x.driver_id = y.driver_id)");
    // inner joins[0] is `laps`, which does not — the case that was skipped
    expectStringRefusal(outer + "(SELECT SUM(d.name) FROM drivers x JOIN laps y "
                                "ON x.driver_id = y.driver_id)");
    // no inner join at all: the slot fell past joins.size() and the qualifier was
    // an outer alias matching nothing inner
    expectStringRefusal(outer + "(SELECT SUM(d.name) FROM drivers x)");
    // control: the local rule is untouched
    expectStringRefusal("SELECT SUM(d.name) FROM laps l JOIN drivers d "
                        "ON l.driver_id = d.driver_id");

    // ...and a LEGAL correlated numeric argument must still bind
    try {
        buildLogical(outer + "(SELECT SUM(d.age) FROM drivers x)", cat);
        ADD_FAILURE() << "expected a correlated-subquery refusal";
    } catch (const std::runtime_error& e) {
        // The legal numeric argument BOUND — no type error — and the refusal
        // that arrives is about the body's aggregate, not about d.age. That is
        // the property this control asserts; only the message changed.
        EXPECT_NE(std::string(e.what()).find("a body with an aggregate cannot be decorrelated"),
                  std::string::npos) << e.what();
    }
}

// exprKey encoded a ColumnRef as `slot#name` with no level, so two refs
// differing only in level hashed identically. checkGroupedRefs matches
// EXPRESSION group keys through exprKey before its own query_level guard, so a
// CORRELATED group key satisfied an ungrouped LOCAL reference. Round 1 fixed the
// plain-column path and stopped exactly there — adding `+ 1` to both sides
// routed the identical pair through exprKey and the refusal disappeared.
TEST(SubqueryValidation, ACorrelatedExpressionGroupKeyDoesNotSatisfyALocalColumn) {
    Catalog cat(CATALOG);
    auto expectUngrouped = [&](const std::string& sql) {
        try {
            buildLogical(sql, cat);
            ADD_FAILURE() << "expected an ungrouped-column error for: " << sql;
        } catch (const std::runtime_error& e) {
            EXPECT_NE(std::string(e.what()).find(
                          "SELECT column 'driver_id' must appear in GROUP BY"),
                      std::string::npos) << "for: " << sql << "\n  actual: " << e.what();
        }
    };
    // the expression path — the finding
    expectUngrouped("SELECT lap_id FROM laps l WHERE EXISTS "
                    "(SELECT driver_id + 1 FROM drivers d GROUP BY l.driver_id + 1)");
    // the plain-column path — round 1's fix, kept as the contrast that shows the
    // two halves of one rule
    expectUngrouped("SELECT lap_id FROM laps l WHERE EXISTS "
                    "(SELECT driver_id FROM drivers d GROUP BY l.driver_id)");

    // control: a LOCAL expression group key must still satisfy the same
    // reference, or the fix has simply broken expression grouping. Uncorrelated,
    // so since Week 31 it validates cleanly instead of reaching a refusal.
    EXPECT_NO_THROW(
        validateOnly("SELECT lap_id FROM laps l WHERE EXISTS "
                     "(SELECT driver_id + 1 FROM drivers d GROUP BY driver_id + 1)", cat));
}

// exprKey is prefixed only above level 0, so every key in a query with no
// subquery is byte-identical to what it was — which is what keeps aggregate-spec
// dedupe and group-key matching unchanged for the whole pre-Week-30 surface.
TEST(SubqueryDispatch, ExprKeyIsUnchangedAtLevelZeroAndDistinctAbove) {
    Catalog cat(CATALOG);
    auto local = bindOnly("SELECT driver_id FROM drivers d GROUP BY driver_id", cat);
    auto* lref = dynamic_cast<const ColumnRef*>(local.select_list[0].get());
    ASSERT_NE(lref, nullptr);
    EXPECT_EQ(exprKey(lref), "0#driver_id") << "level 0 keys must not move";

    auto corr = bindOnly("SELECT lap_id FROM laps l WHERE EXISTS "
                         "(SELECT COUNT(*) FROM drivers d WHERE d.age > l.driver_id)", cat);
    auto* sq = dynamic_cast<const SubqueryExpr*>(corr.where.get());
    ASSERT_NE(sq, nullptr);
    auto* cmp = dynamic_cast<const BinaryExpr*>(sq->subquery->where.get());
    ASSERT_NE(cmp, nullptr);
    auto* outer_ref = dynamic_cast<const ColumnRef*>(cmp->right.get());
    ASSERT_NE(outer_ref, nullptr);
    ASSERT_EQ(outer_ref->id.level(), 1);
    EXPECT_NE(exprKey(outer_ref), "0#driver_id")
        << "a correlated ref must not key the same as a local one at the same slot";
}

// Week 30 round 3. GroupByColumn is the one struct that CARRIES a query_level and
// whose every consumer ignored it — buildAggregateSchema, HashAggregateNode,
// VecHashAggregateNode and CardinalityEstimator all read g.relation_slot bare.
//
// The failure is quieter than a miss: for `EXISTS (SELECT COUNT(*) FROM drivers d
// GROUP BY l.team)` the key is (level 1, slot 0, "team"), and the aggregate's
// child schema is `drivers`, whose slot 0 DOES hold a column named `team`. So
// indexOf("team", 0) is a clean HIT on the wrong relation — neither the bare-name
// fallback nor the `idx < 0` throw fires, and the subquery groups by
// drivers.team instead of the correlated laps.team.
//
// Grouping is not an optimization and a correlated key has no correct local
// fallback, so this throws rather than declining. One guard covers the whole
// consumer set: the other three run on a plan whose schema was built here.
TEST(SubqueryValidation, ACorrelatedGroupKeyCannotReachPlanConstruction) {
    Schema child({{"driver_id", TypeId::INT, 0}, {"team", TypeId::STRING, 0}});
    std::vector<AggregateSpec> specs{{"COUNT", "", true}};

    // control: the identical key at level 0 builds the schema it always did
    GroupByColumn local;
    local.table_name = "drivers";
    local.column_name = "team";
    local.id = ColumnId::local(0);
    Schema ok = buildAggregateSchema({local}, specs, child);
    EXPECT_EQ(ok.column(0).name, "team");

    // the finding: one block out, the same slot names a different relation
    GroupByColumn correlated = local;
    correlated.id = ColumnId::outer(1, 0);
    try {
        buildAggregateSchema({correlated}, specs, child);
        ADD_FAILURE() << "a correlated GROUP BY key must not resolve against this "
                         "block's schema — it hits the wrong relation silently";
    } catch (const std::runtime_error& e) {
        EXPECT_NE(std::string(e.what()).find("correlated GROUP BY key"), std::string::npos)
            << e.what();
    }
}

// Week 33. The ColumnId migration must not move one byte of exprKey's output:
// the string is the identity every aggregate-spec dedupe, every GROUP BY key
// match and every substituteInto rewrite compares on, so a changed encoding is
// a semantic change wearing a refactor's clothes. Pinned against the literal
// text the pre-migration encoder produced.
TEST(SubqueryDispatch, ExprKeyEncodingIsUnchangedByColumnId) {
    ColumnRef local;
    local.column_name = "team";
    local.id = ColumnId::local(1);
    EXPECT_EQ(exprKey(&local), "1#team");

    ColumnRef correlated;
    correlated.column_name = "team";
    correlated.id = ColumnId::outer(2, 0);
    EXPECT_EQ(exprKey(&correlated), "^2:0#team");

    ColumnRef unbound;
    unbound.column_name = "team";
    EXPECT_EQ(exprKey(&unbound), "team");
}

// The level prefix has to be prefix-free, or it reintroduces the collision it was
// added to remove: level 1 / slot 23 and level 12 / slot 3 both rendered
// "^123#team". Both halves are legal SwiftQL — a 24-relation block plans, and
// nesting depth is unbounded.
TEST(SubqueryDispatch, ExprKeyLevelAndSlotCannotRunTogether) {
    ColumnRef a;
    a.column_name = "team";
    a.id = ColumnId::outer(1, 23);

    ColumnRef b;
    b.column_name = "team";
    b.id = ColumnId::outer(12, 3);

    EXPECT_NE(exprKey(&a), exprKey(&b))
        << "concatenated decimals are not prefix-free: " << exprKey(&a);
}

// ---------------------------------------------------------------------------
// Week 34 — derived tables. These pin DECISIONS the SQLite oracle structurally
// cannot see: it compares rows, so a plan that produces the right rows for the
// wrong reason passes it. Each test below names the reason.
// ---------------------------------------------------------------------------

namespace {

// Walk to the LogicalDerived under a plan, wherever it sits on the spine.
const LogicalDerived* findDerived(const LogicalPlanNode* node) {
    if (!node) return nullptr;
    if (node->type == LogicalNodeType::DERIVED)
        return static_cast<const LogicalDerived*>(node);
    for (const auto& child : node->children) {
        if (const LogicalDerived* d = findDerived(child.get())) return d;
    }
    return nullptr;
}

const LogicalJoin* findJoin(const LogicalPlanNode* node) {
    if (!node) return nullptr;
    if (node->type == LogicalNodeType::JOIN)
        return static_cast<const LogicalJoin*>(node);
    for (const auto& child : node->children) {
        if (const LogicalJoin* j = findJoin(child.get())) return j;
    }
    return nullptr;
}

} // namespace

// THE WEEK'S CENTRAL INVARIANT, and nothing else asserts it directly. A derived
// relation's OWN schema stamps slot 0, exactly as a leaf scan's does; the outer
// slot is applied only by the merged join schema. Stamp the outer slot here
// instead and the rows are still right for a one-relation query, while
// PredicatePushdown's restampSlots(c, 0) before a push and ChunkPruner's
// relation_slot < 1 scan-local test both silently stop matching.
TEST(DerivedTable, RelationColumnsAreStampedSlotZero) {
    Catalog cat(CATALOG);
    // The BODY joins, so its own output schema carries slots 0 AND 1 before
    // normalization — the case that would put two numbering domains in one schema.
    auto plan = buildLogical(
        "SELECT x.t FROM (SELECT l.team AS t, d.name AS nm FROM laps l "
        "JOIN drivers d ON l.driver_id = d.driver_id) AS x", cat);
    const LogicalDerived* derived = findDerived(plan.get());
    ASSERT_NE(derived, nullptr);
    ASSERT_EQ(derived->output_schema.size(), 2);
    for (const auto& col : derived->output_schema.columns()) {
        EXPECT_EQ(col.relation_slot, 0) << "column " << col.name;
    }
    // The body's own plan still carries the body's numbering — the normalization
    // is applied at the boundary, not inside.
    const Schema& body = derived->children[0]->output_schema;
    ASSERT_EQ(body.size(), 2);
}

// The merged join schema is where the OUTER slot appears, applied by the same
// loop that stamps a base relation. If it did not, a derived relation joined to
// a base one would be unreferenceable by slot from above.
TEST(DerivedTable, MergedJoinSchemaStampsTheOuterSlot) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT dr.name, d.s FROM (SELECT driver_id, AVG(speed) AS s FROM laps "
        "GROUP BY driver_id) AS d JOIN drivers dr ON d.driver_id = dr.driver_id", cat);
    const LogicalJoin* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->join_slot, 1);
    bool saw_slot_zero = false, saw_slot_one = false;
    for (const auto& col : join->output_schema.columns()) {
        if (col.relation_slot == 0) saw_slot_zero = true;
        if (col.relation_slot == 1) saw_slot_one = true;
    }
    EXPECT_TRUE(saw_slot_zero) << "the derived relation's columns";
    EXPECT_TRUE(saw_slot_one)  << "the joined base relation's columns";
}

// The column-alias list RENAMES positionally and its arity is checked. The
// rename has to survive into the plan schema, because resolveColumnIndex and
// every indexOf above the graft look the new names up.
TEST(DerivedTable, ColumnAliasListRenamesPositionally) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT d.a FROM (SELECT team, speed FROM laps) AS d (a, b)", cat);
    const LogicalDerived* derived = findDerived(plan.get());
    ASSERT_NE(derived, nullptr);
    ASSERT_EQ(derived->output_schema.size(), 2);
    EXPECT_EQ(derived->output_schema.column(0).name, "a");
    EXPECT_EQ(derived->output_schema.column(1).name, "b");
}

TEST(DerivedTable, ColumnAliasArityMismatchIsRefused) {
    Catalog cat(CATALOG);
    EXPECT_THROW(buildLogical("SELECT * FROM (SELECT team FROM laps) AS d (a, b)", cat),
                 std::runtime_error);
}

// A base table cannot produce two columns of one name; a derived table can, and
// after the slot-0 normalization BOTH indexOf overloads become a coin flip.
// Refused rather than disambiguated.
TEST(DerivedTable, DuplicateOutputColumnNameIsRefused) {
    Catalog cat(CATALOG);
    EXPECT_THROW(buildLogical(
        "SELECT * FROM (SELECT l.team, d.team FROM laps l "
        "JOIN drivers d ON l.driver_id = d.driver_id) AS x", cat),
        std::runtime_error);
}

// Week 34's correlated-scalar rewrite MUST produce a LEFT join. An INNER join
// returns the same rows for most shapes — SQL's NULL comparison excludes the
// unmatched rows anyway — and a different answer as soon as the predicate sits
// under an OR. The harness carries that query; this pins the plan property
// directly, so a regression is localized to the rewrite rather than to a diff.
TEST(CorrelatedScalar, DecorrelatesToALeftJoinOverADerivedRelation) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT COUNT(*) FROM laps l WHERE l.speed > "
        "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)", cat);
    const LogicalJoin* join = findJoin(plan.get());
    ASSERT_NE(join, nullptr);
    EXPECT_EQ(join->join_type, JoinType::LEFT)
        << "an INNER join drops the outer row where SQL says the scalar is NULL";
    EXPECT_EQ(join->semantics, JoinSemantics::STANDARD);
    // The right child is the SAME node FROM (subquery) produces, not a special
    // case — if it stops being one, four walkers and the range-table size each
    // need a second argument and the two will drift.
    ASSERT_EQ(join->children[1]->type, LogicalNodeType::DERIVED);
    // ONE aggregate under the join, not one per outer row.
    const LogicalPlanNode* right = join->children[1].get();
    int aggregates = 0;
    std::function<void(const LogicalPlanNode*)> count = [&](const LogicalPlanNode* n) {
        if (n->type == LogicalNodeType::AGGREGATE) ++aggregates;
        for (const auto& c : n->children) count(c.get());
    };
    count(right);
    EXPECT_EQ(aggregates, 1);
}

// The guard that keeps Week 31's runtime cardinality divergence honest: after
// the rewrite the GROUP BY gives one row per key by construction, so a
// non-aggregate body has no `returned more than one row` check left anywhere.
TEST(CorrelatedScalar, NonAggregateBodyIsRefusedRatherThanSilentlyAnswered) {
    Catalog cat(CATALOG);
    EXPECT_THROW(buildLogical(
        "SELECT COUNT(*) FROM laps l WHERE l.speed > "
        "(SELECT l2.speed FROM laps l2 WHERE l2.team = l.team)", cat),
        std::runtime_error);
}
