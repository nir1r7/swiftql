#include <gtest/gtest.h>
#include "planner/logical_plan.h"
#include "planner/binder.h"
#include "parser/parser.h"
#include "parser/expr_utils.h"
#include "catalog/catalog.h"
#include "common/schema.h"
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
    EXPECT_EQ(join->from_col, "driver_id");
    EXPECT_EQ(join->join_col, "driver_id");
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
    EXPECT_EQ(j1->from_col, "id");
    EXPECT_EQ(j1->join_col, "grp");

    // reversed operand order in the ON clause must route identically
    auto plan2 = buildLogical("SELECT l1.id FROM sj l1 JOIN sj l2 ON l2.grp = l1.id", cat);
    const LogicalPlanNode* join2 = findNode(plan2.get(), LogicalNodeType::JOIN);
    ASSERT_NE(join2, nullptr);
    ASSERT_EQ(join2->children.size(), 2u);
    EXPECT_EQ(static_cast<const LogicalScan*>(join2->children[0].get())->table_name, "sj");
    EXPECT_EQ(static_cast<const LogicalScan*>(join2->children[1].get())->table_name, "sj");
    const auto* j2 = static_cast<const LogicalJoin*>(join2);
    EXPECT_EQ(j2->from_col, "id");
    EXPECT_EQ(j2->join_col, "grp");
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
            if (col->relation_slot == 1) {
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
