#include <gtest/gtest.h>
#include "planner/plan_nodes.h"
#include "execution/evaluator.h"
#include "parser/ast.h"
#include "common/value.h"
#include "common/schema.h"
#include <memory>
#include <vector>
#include <stdexcept>

// helper functions

static Schema makeSchema(
        std::initializer_list<std::pair<std::string, TypeId>> cols) {
    std::vector<ColumnDef> defs;
    for (auto& [name, type] : cols) defs.push_back({name, type});
    return Schema(defs);
}

static std::unique_ptr<BinaryExpr> makeBinary(
        std::string op, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r) {
    auto b = std::make_unique<BinaryExpr>();
    b->op = std::move(op);
    b->left  = std::move(l);
    b->right = std::move(r);
    return b;
}

static std::unique_ptr<Literal> lit(Value v) {
    return std::make_unique<Literal>(std::move(v));
}

static std::unique_ptr<ColumnRef> colRef(std::string name) {
    auto c = std::make_unique<ColumnRef>();
    c->column_name = std::move(name);
    return c;
}

static std::unique_ptr<AggregateExpr> aggExpr(std::string fn, std::string col) {
    auto a = std::make_unique<AggregateExpr>();
    a->function_name = std::move(fn);
    a->is_star  = false;
    a->argument = colRef(std::move(col));
    return a;
}

static std::unique_ptr<AggregateExpr> aggExprStar(std::string fn) {
    auto a = std::make_unique<AggregateExpr>();
    a->function_name = std::move(fn);
    a->is_star = true;
    return a;
}

static std::unique_ptr<SeqScanNode> makeScan(std::vector<Row> rows, Schema schema) {
    return std::make_unique<SeqScanNode>("test", std::move(rows), std::move(schema));
}

// open a node, collect all rows into a vector (copying each), then close.
static std::vector<Row> drainAll(PlanNode* node) {
    node->open();
    std::vector<Row> result;
    while (Row* r = node->next()) result.push_back(*r);
    node->close();
    return result;
}


// ===== evaluator tests: Literal, ColumnRef, IsNullExpr =====

TEST(Evaluate, LiteralColumnRefIsNull) {
    Schema schema = makeSchema({{"x", TypeId::INT}, {"y", TypeId::DOUBLE}});
    Row row = {Value(int64_t(7)), Value(3.5)};

    // literal returns stored value directly
    EXPECT_EQ(evaluate(lit(Value(int64_t(99))).get(), row, schema).asInt(), 99);

    // ColumnRef resolves by name into the row
    EXPECT_EQ(evaluate(colRef("x").get(), row, schema).asInt(), 7);
    EXPECT_DOUBLE_EQ(evaluate(colRef("y").get(), row, schema).asDouble(), 3.5);

    // IS NULL on null → true (1)
    IsNullExpr isn;
    isn.operand    = lit(Value::null());
    isn.is_not_null = false;
    EXPECT_EQ(evaluate(&isn, row, schema).asInt(), 1);

    // IS NULL on non-null → false (0)
    isn.operand = lit(Value(int64_t(5)));
    EXPECT_EQ(evaluate(&isn, row, schema).asInt(), 0);

    // IS NOT NULL on non-null → true (1)
    isn.is_not_null = true;
    EXPECT_EQ(evaluate(&isn, row, schema).asInt(), 1);

    // IS NOT NULL on null → false (0)
    isn.operand = lit(Value::null());
    EXPECT_EQ(evaluate(&isn, row, schema).asInt(), 0);
}

// ===== evaluator tests: BinaryExpr =====

TEST(Evaluate, BinaryExprAllOps) {
    Schema schema = makeSchema({{"a", TypeId::INT}});
    Row row = {Value(int64_t(0))};  // row unused, all literals

    auto i10 = []{ return lit(Value(int64_t(10))); };
    auto i20 = []{ return lit(Value(int64_t(20))); };
    auto t   = []{ return lit(Value(int64_t(1)));  };
    auto f   = []{ return lit(Value(int64_t(0)));  };

    // equality and comparisons
    EXPECT_EQ(evaluate(makeBinary("=",  i10(), i10()).get(), row, schema).asInt(), 1);
    EXPECT_EQ(evaluate(makeBinary("=",  i10(), i20()).get(), row, schema).asInt(), 0);
    EXPECT_EQ(evaluate(makeBinary("!=", i10(), i20()).get(), row, schema).asInt(), 1);
    EXPECT_EQ(evaluate(makeBinary("!=", i10(), i10()).get(), row, schema).asInt(), 0);
    EXPECT_EQ(evaluate(makeBinary("<",  i10(), i20()).get(), row, schema).asInt(), 1);
    EXPECT_EQ(evaluate(makeBinary("<",  i20(), i10()).get(), row, schema).asInt(), 0);
    EXPECT_EQ(evaluate(makeBinary(">",  i20(), i10()).get(), row, schema).asInt(), 1);
    EXPECT_EQ(evaluate(makeBinary(">",  i10(), i20()).get(), row, schema).asInt(), 0);
    EXPECT_EQ(evaluate(makeBinary("<=", i10(), i10()).get(), row, schema).asInt(), 1);
    EXPECT_EQ(evaluate(makeBinary("<=", i20(), i10()).get(), row, schema).asInt(), 0);
    EXPECT_EQ(evaluate(makeBinary(">=", i10(), i10()).get(), row, schema).asInt(), 1);
    EXPECT_EQ(evaluate(makeBinary(">=", i10(), i20()).get(), row, schema).asInt(), 0);

    // AND and OR
    EXPECT_EQ(evaluate(makeBinary("AND", t(), t()).get(), row, schema).asInt(), 1);
    EXPECT_EQ(evaluate(makeBinary("AND", t(), f()).get(), row, schema).asInt(), 0);
    EXPECT_EQ(evaluate(makeBinary("OR",  f(), t()).get(), row, schema).asInt(), 1);
    EXPECT_EQ(evaluate(makeBinary("OR",  f(), f()).get(), row, schema).asInt(), 0);

    // null propagation: any null operand leads to null result
    EXPECT_TRUE(evaluate(makeBinary("=",  lit(Value::null()), i10()).get(), row, schema).isNull());
    EXPECT_TRUE(evaluate(makeBinary(">",  i10(), lit(Value::null())).get(), row, schema).isNull());

    // unknown operator leads to throws
    EXPECT_THROW(
        evaluate(makeBinary("XOR", i10(), i10()).get(), row, schema),
        std::runtime_error);
}

// ===== evaluator tests: AggregateExpr =====

TEST(Evaluate, AggregateExpr) {
    // schema mimics HashAggregateNode output; columns are named "FUNC(col)"
    Schema schema = makeSchema({
        {"team",      TypeId::STRING},
        {"AVG(speed)", TypeId::DOUBLE},
        {"COUNT(*)",  TypeId::INT}
    });
    Row row = {Value(std::string("Ferrari")), Value(350.0), Value(int64_t(5))};

    // non star
    EXPECT_DOUBLE_EQ(evaluate(aggExpr("AVG", "speed").get(), row, schema).asDouble(), 350.0);

    // star
    EXPECT_EQ(evaluate(aggExprStar("COUNT").get(), row, schema).asInt(), 5);
}

// ===== SeqScanNode =====

TEST(SeqScanNode, ScanAndReset) {
    Schema schema = makeSchema({{"n", TypeId::INT}});
    std::vector<Row> rows = {
        {Value(int64_t(1))}, {Value(int64_t(2))}, {Value(int64_t(3))}
    };

    SeqScanNode scan("test", rows, schema);

    // first pass
    scan.open();
    EXPECT_EQ(scan.next()->at(0).asInt(), 1);
    EXPECT_EQ(scan.next()->at(0).asInt(), 2);
    EXPECT_EQ(scan.next()->at(0).asInt(), 3);
    EXPECT_EQ(scan.next(), nullptr);
    scan.close();

    // open() resets cursor, full pass again
    scan.open();
    int count = 0;
    while (scan.next()) ++count;
    scan.close();
    EXPECT_EQ(count, 3);

    // Empty table is immediately nullptr
    SeqScanNode empty("test", {}, schema);
    empty.open();
    EXPECT_EQ(empty.next(), nullptr);
    empty.close();
}

// ===== FilterNode =====

TEST(FilterNode, FiltersRows) {
    Schema schema = makeSchema({{"v", TypeId::INT}});

    // rows: 5 (fails), 15 (passes), 25 (passes)
    // predicate: v > 10
    std::vector<Row> rows = {{Value(int64_t(5))}, {Value(int64_t(15))}, {Value(int64_t(25))}};
    auto pred = makeBinary(">", colRef("v"), lit(Value(int64_t(10))));
    FilterNode filter(makeScan(rows, schema), std::move(pred));
    auto result = drainAll(&filter);

    // bug check
    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0][0].asInt(), 15);
    EXPECT_EQ(result[1][0].asInt(), 25);

    // null predicate: null IS NULL passes, non null IS NULL fails
    std::vector<Row> null_rows = {{Value::null()}, {Value(int64_t(1))}};
    auto isn = std::make_unique<IsNullExpr>();
    isn->is_not_null = false;
    isn->operand = colRef("v");
    FilterNode null_filter(makeScan(null_rows, schema), std::move(isn));
    auto null_result = drainAll(&null_filter);
    ASSERT_EQ(null_result.size(), 1u);
    EXPECT_TRUE(null_result[0][0].isNull());
}

// ===== ProjectNode =====

TEST(ProjectNode, ProjectsExpressions) {
    // input looks like HashAggregateNode output
    Schema in_schema = makeSchema({{"team", TypeId::STRING}, {"AVG(speed)", TypeId::DOUBLE}});
    std::vector<Row> rows = {
        {Value(std::string("Ferrari")), Value(355.0)},
        {Value(std::string("RedBull")), Value(320.0)},
    };

    std::vector<std::unique_ptr<Expr>> exprs;
    exprs.push_back(colRef("team"));
    exprs.push_back(aggExpr("AVG", "speed"));  // resolves "AVG(speed)" via schema lookup

    Schema out_schema = makeSchema({{"team", TypeId::STRING}, {"AVG(speed)", TypeId::DOUBLE}});
    ProjectNode proj(makeScan(rows, in_schema), std::move(exprs), out_schema);
    auto result = drainAll(&proj);

    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0][0].asString(), "Ferrari");
    EXPECT_DOUBLE_EQ(result[0][1].asDouble(), 355.0);
    EXPECT_EQ(result[1][0].asString(), "RedBull");
    EXPECT_DOUBLE_EQ(result[1][1].asDouble(), 320.0);
}

// ===== HashAggregateNode =====

TEST(HashAggregateNode, AllFunctionsAndNullHandling) {
    Schema schema = makeSchema({{"team", TypeId::STRING}, {"speed", TypeId::DOUBLE}, {"laps", TypeId::INT}});
    // 4 rows: group A (two non-null speeds), group B (one null + one non-null speed)
    std::vector<Row> rows = {
        {Value(std::string("A")), Value(100.0), Value(int64_t(2))},
        {Value(std::string("A")), Value(200.0), Value(int64_t(3))},
        {Value(std::string("B")), Value::null(), Value(int64_t(1))}, // null speed
        {Value(std::string("B")), Value(300.0), Value(int64_t(4))},
    };

    std::vector<AggregateSpec> specs = {
        {"COUNT", "", true}, // COUNT(*); counts all rows
        {"COUNT", "speed", false}, // COUNT(speed); skips nulls
        {"SUM", "speed", false},
        {"AVG", "speed", false},
        {"MIN", "speed", false},
        {"MAX", "speed", false},
    };

    Schema out_schema = makeSchema({
        {"team", TypeId::STRING},
        {"COUNT(*)", TypeId::INT},
        {"COUNT(speed)", TypeId::INT},
        {"SUM(speed)", TypeId::DOUBLE},
        {"AVG(speed)", TypeId::DOUBLE},
        {"MIN(speed)", TypeId::DOUBLE},
        {"MAX(speed)", TypeId::DOUBLE},
    });

    HashAggregateNode agg(makeScan(rows, schema), {{"", "team"}}, specs, out_schema);
    auto result = drainAll(&agg);
    ASSERT_EQ(result.size(), 2u);

    // order is hash map dependent
    Row* ra = nullptr;
    Row* rb = nullptr;
    for (auto& r : result) {
        if (r[0].asString() == "A") ra = &r;
        if (r[0].asString() == "B") rb = &r;
    }
    ASSERT_NE(ra, nullptr);
    ASSERT_NE(rb, nullptr);

    // Group A: 2 rows, speed 100 and 200
    EXPECT_EQ((*ra)[1].asInt(), 2); // COUNT(*)
    EXPECT_EQ((*ra)[2].asInt(), 2); // COUNT(speed)
    EXPECT_DOUBLE_EQ((*ra)[3].asDouble(), 300.0); // SUM
    EXPECT_DOUBLE_EQ((*ra)[4].asDouble(), 150.0); // AVG
    EXPECT_DOUBLE_EQ((*ra)[5].asDouble(), 100.0); // MIN
    EXPECT_DOUBLE_EQ((*ra)[6].asDouble(), 200.0); // MAX

    // Group B: 2 rows, one null speed — COUNT(*) counts both, others skip the null
    EXPECT_EQ((*rb)[1].asInt(), 2); // COUNT(*) counts the null row
    EXPECT_EQ((*rb)[2].asInt(), 1); // COUNT(speed) skips null
    EXPECT_DOUBLE_EQ((*rb)[3].asDouble(), 300.0); // SUM of non-nulls only
    EXPECT_DOUBLE_EQ((*rb)[4].asDouble(), 300.0); // AVG = 300/1
    EXPECT_DOUBLE_EQ((*rb)[5].asDouble(), 300.0); // MIN
    EXPECT_DOUBLE_EQ((*rb)[6].asDouble(), 300.0); // MAX
}

TEST(HashAggregateNode, ScalarAggregateOverEmptyInputEmitsOneRow) {
    // SQL: a scalar aggregate (no GROUP BY) over empty input still emits one row —
    // COUNT -> 0, SUM/AVG/MIN/MAX -> NULL. (GROUP BY over empty input emits zero rows.)
    Schema schema = makeSchema({{"team", TypeId::STRING}, {"speed", TypeId::DOUBLE}});
    std::vector<AggregateSpec> specs = {
        {"COUNT", "", true},
        {"SUM", "speed", false},
        {"AVG", "speed", false},
        {"MIN", "speed", false},
        {"MAX", "speed", false},
    };
    Schema out_schema = makeSchema({
        {"COUNT(*)", TypeId::INT},
        {"SUM(speed)", TypeId::DOUBLE},
        {"AVG(speed)", TypeId::DOUBLE},
        {"MIN(speed)", TypeId::DOUBLE},
        {"MAX(speed)", TypeId::DOUBLE},
    });

    HashAggregateNode agg(makeScan({}, schema), {}, specs, out_schema);
    auto result = drainAll(&agg);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result[0][0].asInt(), 0);   // COUNT(*)
    EXPECT_TRUE(result[0][1].isNull());   // SUM
    EXPECT_TRUE(result[0][2].isNull());   // AVG
    EXPECT_TRUE(result[0][3].isNull());   // MIN
    EXPECT_TRUE(result[0][4].isNull());   // MAX
}

// ===== HavingNode =====

TEST(HavingNode, FiltersGroups) {
    Schema schema = makeSchema({{"team", TypeId::STRING}, {"AVG(speed)", TypeId::DOUBLE}});
    std::vector<Row> rows = {
        {Value(std::string("A")), Value(350.0)}, // passes AVG(speed) > 300
        {Value(std::string("B")), Value(280.0)}, // fails
        {Value(std::string("C")), Value(310.0)}, // passes
    };

    // predicate: AVG(speed) > 300.0; must use double literal to match AVG's DOUBLE type
    auto pred = makeBinary(">", aggExpr("AVG", "speed"), lit(Value(300.0)));
    HavingNode having(makeScan(rows, schema), std::move(pred));
    auto result = drainAll(&having);

    ASSERT_EQ(result.size(), 2u);
    EXPECT_EQ(result[0][0].asString(), "A");
    EXPECT_EQ(result[1][0].asString(), "C");
}

// ===== DistinctNode =====

TEST(DistinctNode, Deduplication) {
    Schema schema = makeSchema({{"x", TypeId::INT}});
    std::vector<Row> rows = {
        {Value(int64_t(1))}, {Value(int64_t(2))},
        {Value(int64_t(1))}, // duplicate
        {Value(int64_t(3))}, {Value(int64_t(2))}, // duplicate
    };

    DistinctNode distinct(makeScan(rows, schema));
    auto result = drainAll(&distinct);

    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0][0].asInt(), 1);
    EXPECT_EQ(result[1][0].asInt(), 2);
    EXPECT_EQ(result[2][0].asInt(), 3);
}

// ===== SortNode =====

TEST(SortNode, SortsRows) {
    Schema schema = makeSchema({{"a", TypeId::INT}, {"b", TypeId::INT}});
    std::vector<Row> rows = {
        {Value(int64_t(2)), Value(int64_t(10))},
        {Value(int64_t(1)), Value(int64_t(30))},
        {Value(int64_t(2)), Value(int64_t(20))},
        {Value(int64_t(1)), Value(int64_t(10))},
    };

    // multicolumn sort: a ASC, then b ASC
    std::vector<OrderByItem> sort_exprs;
    sort_exprs.push_back({colRef("a"), false});
    sort_exprs.push_back({colRef("b"), false});
    SortNode sort(makeScan(rows, schema), std::move(sort_exprs));
    auto result = drainAll(&sort);

    ASSERT_EQ(result.size(), 4u);
    EXPECT_EQ(result[0][0].asInt(), 1); EXPECT_EQ(result[0][1].asInt(), 10);
    EXPECT_EQ(result[1][0].asInt(), 1); EXPECT_EQ(result[1][1].asInt(), 30);
    EXPECT_EQ(result[2][0].asInt(), 2); EXPECT_EQ(result[2][1].asInt(), 10);
    EXPECT_EQ(result[3][0].asInt(), 2); EXPECT_EQ(result[3][1].asInt(), 20);

    std::vector<OrderByItem> empty_exprs;
    empty_exprs.push_back({colRef("a"), false});
    SortNode empty_sort(makeScan({}, schema), std::move(empty_exprs));
    EXPECT_TRUE(drainAll(&empty_sort).empty());
}

// ===== LimitNode =====

TEST(LimitNode, LimitsOutput) {
    Schema schema = makeSchema({{"n", TypeId::INT}});
    std::vector<Row> rows;
    for (int i = 1; i <= 5; ++i) rows.push_back({Value(int64_t(i))});

    LimitNode limit3(makeScan(rows, schema), 3);
    EXPECT_EQ(drainAll(&limit3).size(), 3u);   // limit < total

    LimitNode limit10(makeScan(rows, schema), 10);
    EXPECT_EQ(drainAll(&limit10).size(), 5u);  // limit > total: get all rows

    LimitNode limit0(makeScan(rows, schema), 0);
    EXPECT_EQ(drainAll(&limit0).size(), 0u);   // limit 0: get nothing
}

// ===== HashJoinNode =====

TEST(HashJoinNode, BasicInnerJoin) {
    // LEFT (probe) — 3 laps rows: 2 with driver_id=1 (match), 1 with driver_id=99 (no match)
    Schema left_schema = makeSchema({
        {"lap_id",    TypeId::INT},
        {"driver_id", TypeId::INT},
        {"speed",     TypeId::DOUBLE}
    });
    std::vector<Row> left_rows = {
        {Value(int64_t(1)), Value(int64_t(1)),  Value(200.0)},
        {Value(int64_t(2)), Value(int64_t(1)),  Value(210.0)},
        {Value(int64_t(3)), Value(int64_t(99)), Value(180.0)}, // no match
    };

    // RIGHT (build) — 1 driver row, driver_id=1
    Schema right_schema = makeSchema({
        {"driver_id", TypeId::INT},
        {"name",      TypeId::STRING}
    });
    std::vector<Row> right_rows = {
        {Value(int64_t(1)), Value(std::string("Hamilton"))},
    };

    // merged schema mirrors what the planner produces: left columns then right columns
    // driver_id appears twice; indexOf() returns the first (left) occurrence
    Schema merged = makeSchema({
        {"lap_id",    TypeId::INT},
        {"driver_id", TypeId::INT},   // [1] from left
        {"speed",     TypeId::DOUBLE},
        {"driver_id", TypeId::INT},   // [3] from right (duplicate name)
        {"name",      TypeId::STRING} // [4]
    });

    HashJoinNode join(
        makeScan(left_rows,  left_schema),
        makeScan(right_rows, right_schema),
        "driver_id", "driver_id",
        merged);

    auto result = drainAll(&join);

    // only driver_id=1 rows join; driver_id=99 has no match
    ASSERT_EQ(result.size(), 2u);

    // each joined row: [lap_id, driver_id(L), speed, driver_id(R), name]
    EXPECT_EQ(result[0][0].asInt(), 1);
    EXPECT_DOUBLE_EQ(result[0][2].asDouble(), 200.0);
    EXPECT_EQ(result[0][4].asString(), "Hamilton");

    EXPECT_EQ(result[1][0].asInt(), 2);
    EXPECT_DOUBLE_EQ(result[1][2].asDouble(), 210.0);
    EXPECT_EQ(result[1][4].asString(), "Hamilton");
}

TEST(HashJoinNode, NoMatch) {
    // probe keys (1, 2) do not appear in the build side (99) — empty result
    Schema left_schema  = makeSchema({{"pid", TypeId::INT}});
    Schema right_schema = makeSchema({{"bid", TypeId::INT}});
    Schema merged       = makeSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});

    std::vector<Row> left_rows  = {{Value(int64_t(1))}, {Value(int64_t(2))}};
    std::vector<Row> right_rows = {{Value(int64_t(99))}};

    HashJoinNode join(
        makeScan(left_rows,  left_schema),
        makeScan(right_rows, right_schema),
        "pid", "bid", merged);

    EXPECT_TRUE(drainAll(&join).empty());
}

TEST(HashJoinNode, MultipleMatchesPerKey) {
    // build side has 2 rows with the same key — the single probe row must emit 2 joined rows.
    // this tests that next() correctly tracks bucket_idx_ across consecutive calls.
    Schema left_schema  = makeSchema({{"pid", TypeId::INT}});
    Schema right_schema = makeSchema({{"bid", TypeId::INT}, {"name", TypeId::STRING}});
    Schema merged       = makeSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}, {"name", TypeId::STRING}});

    std::vector<Row> left_rows  = {{Value(int64_t(5))}};
    std::vector<Row> right_rows = {
        {Value(int64_t(5)), Value(std::string("A"))},
        {Value(int64_t(5)), Value(std::string("B"))},
    };

    HashJoinNode join(
        makeScan(left_rows,  left_schema),
        makeScan(right_rows, right_schema),
        "pid", "bid", merged);

    auto result = drainAll(&join);
    ASSERT_EQ(result.size(), 2u);

    std::vector<std::string> names = {result[0][2].asString(), result[1][2].asString()};
    std::sort(names.begin(), names.end());
    EXPECT_EQ(names[0], "A");
    EXPECT_EQ(names[1], "B");
}

TEST(HashJoinNode, EmptyBuildSide) {
    // hash table is empty — no probe row can ever match
    Schema left_schema  = makeSchema({{"pid", TypeId::INT}});
    Schema right_schema = makeSchema({{"bid", TypeId::INT}});
    Schema merged       = makeSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});

    std::vector<Row> left_rows = {{Value(int64_t(1))}, {Value(int64_t(2))}};

    HashJoinNode join(
        makeScan(left_rows, left_schema),
        makeScan({},        right_schema),
        "pid", "bid", merged);

    EXPECT_TRUE(drainAll(&join).empty());
}

TEST(HashJoinNode, EmptyProbeSide) {
    // nothing to probe even though the hash table is populated
    Schema left_schema  = makeSchema({{"pid", TypeId::INT}});
    Schema right_schema = makeSchema({{"bid", TypeId::INT}});
    Schema merged       = makeSchema({{"pid", TypeId::INT}, {"bid", TypeId::INT}});

    std::vector<Row> right_rows = {{Value(int64_t(1))}, {Value(int64_t(2))}};

    HashJoinNode join(
        makeScan({},         left_schema),
        makeScan(right_rows, right_schema),
        "pid", "bid", merged);

    EXPECT_TRUE(drainAll(&join).empty());
}

// ===== full pipeline =====

TEST(Pipeline, CheckpointQuery) {
    // SELECT DISTINCT team, AVG(speed) FROM laps
    // WHERE season = 2025
    // GROUP BY team HAVING AVG(speed) > 300
    // ORDER BY team LIMIT 10

    Schema base = makeSchema({
        {"team", TypeId::STRING},
        {"speed", TypeId::DOUBLE},
        {"season", TypeId::INT}
    });
    std::vector<Row> rows = {
        {Value(std::string("Ferrari")), Value(350.0), Value(int64_t(2025))},
        {Value(std::string("Ferrari")), Value(360.0), Value(int64_t(2025))},
        {Value(std::string("McLaren")), Value(280.0), Value(int64_t(2025))},
        {Value(std::string("McLaren")), Value(290.0), Value(int64_t(2025))},
        {Value(std::string("Ferrari")), Value(340.0), Value(int64_t(2024))}, // filtered by WHERE
        {Value(std::string("RedBull")), Value(320.0), Value(int64_t(2025))},
    };

    // build tree bottom up

    // WHERE season = 2025
    auto filter = std::make_unique<FilterNode>(
        makeScan(rows, base),
        makeBinary("=", colRef("season"), lit(Value(int64_t(2025)))));

    // GROUP BY team, AVG(speed)
    Schema agg_schema = makeSchema({{"team", TypeId::STRING}, {"AVG(speed)", TypeId::DOUBLE}});
    auto aggregate = std::make_unique<HashAggregateNode>(
        std::move(filter),
        std::vector<GroupByColumn>{{"", "team"}},
        std::vector<AggregateSpec>{{"AVG", "speed", false}},
        agg_schema);

    // HAVING AVG(speed) > 300.0
    auto having = std::make_unique<HavingNode>(
        std::move(aggregate),
        makeBinary(">", aggExpr("AVG", "speed"), lit(Value(300.0))));

    // SELECT team, AVG(speed)
    Schema proj_schema = makeSchema({{"team", TypeId::STRING}, {"AVG(speed)", TypeId::DOUBLE}});
    std::vector<std::unique_ptr<Expr>> proj_exprs;
    proj_exprs.push_back(colRef("team"));
    proj_exprs.push_back(aggExpr("AVG", "speed"));
    auto project = std::make_unique<ProjectNode>(
        std::move(having), std::move(proj_exprs), proj_schema);

    // DISTINCT
    auto distinct = std::make_unique<DistinctNode>(std::move(project));

    // ORDER BY team
    std::vector<OrderByItem> pipeline_sort_exprs;
    pipeline_sort_exprs.push_back({colRef("team"), false});
    auto sort = std::make_unique<SortNode>(
        std::move(distinct), std::move(pipeline_sort_exprs));

    // LIMIT 10
    LimitNode limit(std::move(sort), 10);

    auto result = drainAll(&limit);

    // expected: Ferrari (avg=355) and RedBull (avg=320), McLaren (avg=285) fails HAVING
    ASSERT_EQ(result.size(), 2u);

    // sorted alphabetically: Ferrari, RedBull
    EXPECT_EQ(result[0][0].asString(), "Ferrari");
    EXPECT_DOUBLE_EQ(result[0][1].asDouble(), 355.0);

    EXPECT_EQ(result[1][0].asString(), "RedBull");
    EXPECT_DOUBLE_EQ(result[1][1].asDouble(), 320.0);
}

TEST(Pipeline, HashJoinWithFilterAndAggregate) {
    // SELECT team, AVG(speed)
    // FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id
    // WHERE speed > 200.0
    // GROUP BY team

    Schema laps_schema = makeSchema({
        {"driver_id", TypeId::INT},
        {"team",      TypeId::STRING},
        {"speed",     TypeId::DOUBLE}
    });
    std::vector<Row> laps_rows = {
        {Value(int64_t(1)), Value(std::string("Ferrari")), Value(300.0)},   // joins, passes WHERE
        {Value(int64_t(1)), Value(std::string("Ferrari")), Value(150.0)},   // joins, fails WHERE
        {Value(int64_t(2)), Value(std::string("McLaren")), Value(250.0)},   // joins, passes WHERE
        {Value(int64_t(99)), Value(std::string("RedBull")), Value(400.0)},  // no join match
    };

    Schema drivers_schema = makeSchema({
        {"driver_id", TypeId::INT},
        {"name",      TypeId::STRING}
    });
    std::vector<Row> drivers_rows = {
        {Value(int64_t(1)), Value(std::string("Hamilton"))},
        {Value(int64_t(2)), Value(std::string("Norris"))},
    };

    // merged schema: laps columns then drivers columns
    Schema merged = makeSchema({
        {"driver_id", TypeId::INT},   // [0] from laps
        {"team",      TypeId::STRING},
        {"speed",     TypeId::DOUBLE},
        {"driver_id", TypeId::INT},   // [3] from drivers (duplicate name, indexOf returns first)
        {"name",      TypeId::STRING}
    });

    // HashJoin
    auto join = std::make_unique<HashJoinNode>(
        makeScan(laps_rows,    laps_schema),
        makeScan(drivers_rows, drivers_schema),
        "driver_id", "driver_id",
        merged);

    // WHERE speed > 200.0
    auto filter = std::make_unique<FilterNode>(
        std::move(join),
        makeBinary(">", colRef("speed"), lit(Value(200.0))));

    // GROUP BY team, AVG(speed)
    Schema agg_schema = makeSchema({{"team", TypeId::STRING}, {"AVG(speed)", TypeId::DOUBLE}});
    auto agg = std::make_unique<HashAggregateNode>(
        std::move(filter),
        std::vector<GroupByColumn>{{"", "team"}},
        std::vector<AggregateSpec>{{"AVG", "speed", false}},
        agg_schema);

    // SELECT team, AVG(speed)
    std::vector<std::unique_ptr<Expr>> proj_exprs;
    proj_exprs.push_back(colRef("team"));
    proj_exprs.push_back(aggExpr("AVG", "speed"));
    Schema proj_schema = makeSchema({{"team", TypeId::STRING}, {"AVG(speed)", TypeId::DOUBLE}});
    ProjectNode project(std::move(agg), std::move(proj_exprs), proj_schema);

    auto result = drainAll(&project);

    // Ferrari: 300.0 row survives WHERE (150.0 filtered out); AVG = 300.0
    // McLaren: 250.0 survives; AVG = 250.0
    // RedBull: no join match — absent from results
    ASSERT_EQ(result.size(), 2u);

    Row* ferrari = nullptr;
    Row* mclaren = nullptr;
    for (auto& r : result) {
        if (r[0].asString() == "Ferrari") ferrari = &r;
        if (r[0].asString() == "McLaren") mclaren = &r;
    }
    ASSERT_NE(ferrari, nullptr);
    ASSERT_NE(mclaren, nullptr);
    EXPECT_EQ(result.end(), std::find_if(result.begin(), result.end(),
        [](const Row& r){ return r[0].asString() == "RedBull"; }));

    EXPECT_DOUBLE_EQ((*ferrari)[1].asDouble(), 300.0);
    EXPECT_DOUBLE_EQ((*mclaren)[1].asDouble(), 250.0);
}