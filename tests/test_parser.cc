#include <gtest/gtest.h>
#include "parser/token.h"
#include "parser/lexer.h"
#include "parser/ast.h"
#include "parser/parser.h"


TEST(LexerTest, BasicSelect) {
    Lexer lexer("SELECT team FROM laps");
    EXPECT_EQ(lexer.nextToken().type, TokenType::SELECT);
    EXPECT_EQ(lexer.nextToken().type, TokenType::IDENTIFIER);
    EXPECT_EQ(lexer.nextToken().type, TokenType::FROM);
    EXPECT_EQ(lexer.nextToken().type, TokenType::IDENTIFIER);
    EXPECT_EQ(lexer.nextToken().type, TokenType::END_OF_FILE);
}

TEST(LexerTest, CaseInsensitive) {
    Lexer lexer("select FROM Where");
    EXPECT_EQ(lexer.nextToken().type, TokenType::SELECT);
    EXPECT_EQ(lexer.nextToken().type, TokenType::FROM);
    EXPECT_EQ(lexer.nextToken().type, TokenType::WHERE);
}

TEST(LexerTest, IntLiteral) {
    Lexer lexer("2025");
    Token t = lexer.nextToken();
    EXPECT_EQ(t.type, TokenType::INT_LITERAL);
    EXPECT_EQ(t.value, "2025");
}

TEST(LexerTest, FloatLiteral) {
    Lexer lexer("312.45");
    Token t = lexer.nextToken();
    EXPECT_EQ(t.type, TokenType::FLOAT_LITERAL);
    EXPECT_EQ(t.value, "312.45");
}

TEST(LexerTest, StringLiteral) {
    Lexer lexer("'Ferrari'");
    Token t = lexer.nextToken();
    EXPECT_EQ(t.type, TokenType::STRING_LITERAL);
    EXPECT_EQ(t.value, "Ferrari");  // note: no quotes in value
}

TEST(LexerTest, Operators) {
    Lexer lexer("= != < > <= >=");
    EXPECT_EQ(lexer.nextToken().type, TokenType::EQ);
    EXPECT_EQ(lexer.nextToken().type, TokenType::NEQ);
    EXPECT_EQ(lexer.nextToken().type, TokenType::LT);
    EXPECT_EQ(lexer.nextToken().type, TokenType::GT);
    EXPECT_EQ(lexer.nextToken().type, TokenType::LTE);
    EXPECT_EQ(lexer.nextToken().type, TokenType::GTE);
}

TEST(LexerTest, AggregateQuery) {
    Lexer lexer("SELECT AVG(speed) FROM laps");
    EXPECT_EQ(lexer.nextToken().type, TokenType::SELECT);
    EXPECT_EQ(lexer.nextToken().type, TokenType::AVG);
    EXPECT_EQ(lexer.nextToken().type, TokenType::LPAREN);
    EXPECT_EQ(lexer.nextToken().type, TokenType::IDENTIFIER);
    EXPECT_EQ(lexer.nextToken().type, TokenType::RPAREN);
    EXPECT_EQ(lexer.nextToken().type, TokenType::FROM);
    EXPECT_EQ(lexer.nextToken().type, TokenType::IDENTIFIER);
}

TEST(LexerTest, IsNullTokens) {
    Lexer lexer("speed IS NOT NULL");
    EXPECT_EQ(lexer.nextToken().type, TokenType::IDENTIFIER);
    EXPECT_EQ(lexer.nextToken().type, TokenType::IS);
    EXPECT_EQ(lexer.nextToken().type, TokenType::NOT);
    EXPECT_EQ(lexer.nextToken().type, TokenType::NULL_KW);
}

TEST(LexerTest, Peek) {
    Lexer lexer("SELECT team");
    EXPECT_EQ(lexer.peek().type, TokenType::SELECT);
    EXPECT_EQ(lexer.peek().type, TokenType::SELECT); // still SELECT
    EXPECT_EQ(lexer.nextToken().type, TokenType::SELECT); // now consumed
    EXPECT_EQ(lexer.nextToken().type, TokenType::IDENTIFIER);
}

TEST(ParserTest, SimpleSelect) {
    Parser p("SELECT team FROM laps");
    auto stmt = p.parse();
    EXPECT_EQ(stmt.from_table, "laps");
    EXPECT_EQ(stmt.select_list.size(), 1);
    EXPECT_FALSE(stmt.distinct);
}

TEST(ParserTest, WhereClause) {
    Parser p("SELECT speed FROM laps WHERE season = 2025");
    auto stmt = p.parse();
    EXPECT_NE(stmt.where, nullptr);
    // where should be a BinaryExpr with op "="
    auto* bin = dynamic_cast<BinaryExpr*>(stmt.where.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "=");
}

TEST(ParserTest, GroupBy) {
    Parser p("SELECT team, AVG(speed) FROM laps GROUP BY team");
    auto stmt = p.parse();
    EXPECT_EQ(stmt.group_by.size(), 1);
    EXPECT_EQ(stmt.group_by[0].column_name, "team");
    EXPECT_TRUE(stmt.group_by[0].table_name.empty());
    EXPECT_EQ(stmt.select_list.size(), 2);
}

TEST(ParserTest, GroupByPreservesQualifier) {
    Parser p("SELECT b.grp, COUNT(*) FROM sj a JOIN sj b ON a.id = b.id GROUP BY b.grp");
    auto stmt = p.parse();
    ASSERT_EQ(stmt.group_by.size(), 1);
    EXPECT_EQ(stmt.group_by[0].table_name, "b");
    EXPECT_EQ(stmt.group_by[0].column_name, "grp");
}

TEST(ParserTest, Distinct) {
    Parser p("SELECT DISTINCT team FROM laps");
    auto stmt = p.parse();
    EXPECT_TRUE(stmt.distinct);
}

TEST(ParserTest, Limit) {
    Parser p("SELECT team FROM laps LIMIT 10");
    auto stmt = p.parse();
    ASSERT_TRUE(stmt.limit.has_value());
    EXPECT_EQ(stmt.limit.value(), 10);
}

TEST(ParserTest, Having) {
    Parser p("SELECT team, AVG(speed) FROM laps GROUP BY team HAVING AVG(speed) > 300");
    auto stmt = p.parse();
    EXPECT_NE(stmt.having, nullptr);
}

TEST(ParserTest, IsNull) {
    Parser p("SELECT speed FROM laps WHERE speed IS NOT NULL");
    auto stmt = p.parse();
    auto* isnull = dynamic_cast<IsNullExpr*>(stmt.where.get());
    ASSERT_NE(isnull, nullptr);
    EXPECT_TRUE(isnull->is_not_null);
}

TEST(ParserTest, Join) {
    Parser p("SELECT team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id");
    auto stmt = p.parse();
    ASSERT_EQ(stmt.joins.size(), 1u);
    EXPECT_EQ(stmt.joins[0].join_table, "drivers");
}

// Week 26: the join clause is a Kleene star, not an option. Before this a
// second JOIN hit Parser::parse's end-of-input check ("unexpected trailing
// input") — a clean error, but not an answer.
TEST(ParserTest, MultipleJoinClauses) {
    Parser p("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id JOIN sj c ON b.grp = c.id");
    auto stmt = p.parse();
    ASSERT_EQ(stmt.joins.size(), 2u);
    EXPECT_EQ(stmt.joins[0].join_table, "sj");
    EXPECT_EQ(stmt.joins[0].alias, "b");
    EXPECT_EQ(stmt.joins[1].alias, "c");
    ASSERT_NE(stmt.joins[1].condition, nullptr);
}

// Clauses after the last JOIN still parse — the loop must not swallow them.
TEST(ParserTest, MultipleJoinsFollowedByOtherClauses) {
    Parser p("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id JOIN sj c ON b.grp = c.id "
             "WHERE a.val > 100 ORDER BY a.id LIMIT 2");
    auto stmt = p.parse();
    ASSERT_EQ(stmt.joins.size(), 2u);
    ASSERT_NE(stmt.where, nullptr);
    ASSERT_EQ(stmt.order_by.size(), 1u);
    EXPECT_EQ(stmt.limit.value(), 2);
}

// Week 29. LEFT and OUTER are reserved TokenTypes rather than identifier text
// (the way interval units are), because the bare-alias branch in parseSelect
// would otherwise read `FROM laps LEFT JOIN ...` as the alias `LEFT`.
TEST(ParserTest, LeftJoinIsAnOuterJoin) {
    Parser p("SELECT team FROM drivers LEFT JOIN laps ON drivers.driver_id = laps.driver_id");
    auto stmt = p.parse();
    ASSERT_EQ(stmt.joins.size(), 1u);
    EXPECT_EQ(stmt.joins[0].join_table, "laps");
    EXPECT_EQ(stmt.joins[0].type, JoinType::LEFT);
}

TEST(ParserTest, LeftOuterJoinIsTheSameJoin) {
    Parser p("SELECT team FROM drivers d LEFT OUTER JOIN laps l ON d.driver_id = l.driver_id");
    auto stmt = p.parse();
    ASSERT_EQ(stmt.joins.size(), 1u);
    EXPECT_EQ(stmt.joins[0].alias, "l");
    EXPECT_EQ(stmt.joins[0].type, JoinType::LEFT);
}

// A bare JOIN keeps meaning INNER, and the two kinds mix in one statement — the
// join type is a property of the CLAUSE, not of the query.
TEST(ParserTest, InnerAndOuterJoinsMixInOneStatement) {
    Parser p("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id LEFT JOIN sj c ON b.grp = c.id");
    auto stmt = p.parse();
    ASSERT_EQ(stmt.joins.size(), 2u);
    EXPECT_EQ(stmt.joins[0].type, JoinType::INNER);
    EXPECT_EQ(stmt.joins[1].type, JoinType::LEFT);
}

// LEFT is not swallowed as the FROM table's alias — the reason it is a keyword.
TEST(ParserTest, LeftIsNotReadAsATableAlias) {
    Parser p("SELECT drivers.team FROM drivers LEFT JOIN laps ON drivers.driver_id = laps.driver_id");
    auto stmt = p.parse();
    EXPECT_TRUE(stmt.from_alias.empty());
    ASSERT_EQ(stmt.joins.size(), 1u);
}

TEST(ParserTest, LeftWithoutJoinIsSyntaxError) {
    Parser p("SELECT team FROM drivers LEFT laps ON drivers.driver_id = laps.driver_id");
    EXPECT_THROW(p.parse(), ParseError);
}

TEST(ParserTest, AndPrecedence) {
    Parser p("SELECT team FROM laps WHERE season = 2025 AND speed > 300");
    auto stmt = p.parse();
    // top level should be AND
    auto* bin = dynamic_cast<BinaryExpr*>(stmt.where.get());
    ASSERT_NE(bin, nullptr);
    EXPECT_EQ(bin->op, "AND");
}

TEST(ParserTest, BadQueryThrows) {
    Parser p("SELECT FROM laps");  // missing select list
    EXPECT_THROW(p.parse(), ParseError);
}
TEST(ParserTest, TableAliasWithAsKeyword) {
    Parser p("SELECT l.team FROM laps AS l JOIN drivers AS d ON l.driver_id = d.driver_id");
    auto stmt = p.parse();
    EXPECT_EQ(stmt.from_alias, "l");
    ASSERT_EQ(stmt.joins.size(), 1u);
    EXPECT_EQ(stmt.joins[0].alias, "d");
}

TEST(ParserTest, AsWithoutAliasIsSyntaxError) {
    Parser p("SELECT team FROM laps AS WHERE season = 2025");
    EXPECT_THROW(p.parse(), ParseError);
}


// ===== Week 24: arithmetic tokens + precedence + unary minus =====

TEST(LexerTest, ArithmeticOperators) {
    Lexer lexer("+ - / *");
    EXPECT_EQ(lexer.nextToken().type, TokenType::PLUS);
    EXPECT_EQ(lexer.nextToken().type, TokenType::MINUS);
    EXPECT_EQ(lexer.nextToken().type, TokenType::SLASH);
    EXPECT_EQ(lexer.nextToken().type, TokenType::STAR);
}

TEST(ParserTest, MultiplicationBindsTighterThanAddition) {
    Parser parser("SELECT 1 + 2 * 3 FROM laps");
    SelectStatement stmt = parser.parse();
    ASSERT_EQ(stmt.select_list.size(), 1u);

    auto* root = dynamic_cast<BinaryExpr*>(stmt.select_list[0].get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, "+");
    auto* rhs = dynamic_cast<BinaryExpr*>(root->right.get());
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(rhs->op, "*");
}

TEST(ParserTest, ParensOverridePrecedence) {
    Parser parser("SELECT (1 + 2) * 3 FROM laps");
    SelectStatement stmt = parser.parse();

    auto* root = dynamic_cast<BinaryExpr*>(stmt.select_list[0].get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, "*");
    auto* lhs = dynamic_cast<BinaryExpr*>(root->left.get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->op, "+");
}

TEST(ParserTest, ArithmeticIsLeftAssociative) {
    Parser parser("SELECT 10 - 4 - 3 FROM laps");
    SelectStatement stmt = parser.parse();

    // (10 - 4) - 3, not 10 - (4 - 3)
    auto* root = dynamic_cast<BinaryExpr*>(stmt.select_list[0].get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, "-");
    auto* lhs = dynamic_cast<BinaryExpr*>(root->left.get());
    ASSERT_NE(lhs, nullptr);
    EXPECT_EQ(lhs->op, "-");
    auto* r = dynamic_cast<Literal*>(root->right.get());
    ASSERT_NE(r, nullptr);
    EXPECT_EQ(r->value.asInt(), 3);
}

TEST(ParserTest, UnaryMinusBindsTighterThanMultiplication) {
    Parser parser("SELECT -speed * 2 FROM laps");
    SelectStatement stmt = parser.parse();

    auto* root = dynamic_cast<BinaryExpr*>(stmt.select_list[0].get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, "*");
    auto* un = dynamic_cast<UnaryExpr*>(root->left.get());
    ASSERT_NE(un, nullptr);
    EXPECT_EQ(un->op, "-");
    EXPECT_NE(dynamic_cast<ColumnRef*>(un->operand.get()), nullptr);
}

TEST(ParserTest, DoubleUnaryMinus) {
    Parser parser("SELECT - -5 FROM laps");
    SelectStatement stmt = parser.parse();

    auto* outer = dynamic_cast<UnaryExpr*>(stmt.select_list[0].get());
    ASSERT_NE(outer, nullptr);
    auto* inner = dynamic_cast<UnaryExpr*>(outer->operand.get());
    ASSERT_NE(inner, nullptr);
    EXPECT_NE(dynamic_cast<Literal*>(inner->operand.get()), nullptr);
}

TEST(ParserTest, ComparisonBindsLooserThanArithmetic) {
    Parser parser("SELECT team FROM laps WHERE speed * 2 > 600 AND season = 2025");
    SelectStatement stmt = parser.parse();

    auto* root = dynamic_cast<BinaryExpr*>(stmt.where.get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, "AND");
    auto* cmp = dynamic_cast<BinaryExpr*>(root->left.get());
    ASSERT_NE(cmp, nullptr);
    EXPECT_EQ(cmp->op, ">");
    auto* mul = dynamic_cast<BinaryExpr*>(cmp->left.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, "*");
}

TEST(ParserTest, ArithmeticInsideAggregate) {
    Parser parser("SELECT SUM(speed * 2) FROM laps");
    SelectStatement stmt = parser.parse();

    auto* agg = dynamic_cast<AggregateExpr*>(stmt.select_list[0].get());
    ASSERT_NE(agg, nullptr);
    EXPECT_FALSE(agg->is_star);
    auto* arg = dynamic_cast<BinaryExpr*>(agg->argument.get());
    ASSERT_NE(arg, nullptr);
    EXPECT_EQ(arg->op, "*");
}

TEST(ParserTest, OrderByExpression) {
    Parser parser("SELECT team FROM laps ORDER BY speed * 2 DESC");
    SelectStatement stmt = parser.parse();

    ASSERT_EQ(stmt.order_by.size(), 1u);
    EXPECT_TRUE(stmt.order_by[0].desc);
    auto* mul = dynamic_cast<BinaryExpr*>(stmt.order_by[0].expr.get());
    ASSERT_NE(mul, nullptr);
    EXPECT_EQ(mul->op, "*");
}

TEST(ParserTest, CountStarStillParses) {
    // STAR keeps its COUNT(*) role: consumed before expression parsing starts
    Parser parser("SELECT COUNT(*) FROM laps WHERE speed * 2 > 600");
    SelectStatement stmt = parser.parse();
    auto* agg = dynamic_cast<AggregateExpr*>(stmt.select_list[0].get());
    ASSERT_NE(agg, nullptr);
    EXPECT_TRUE(agg->is_star);
}


// ===== Week 24: expression aliases =====

TEST(ParserTest, SelectExpressionAlias) {
    Parser parser("SELECT speed * 2 AS double_speed, team FROM laps");
    SelectStatement stmt = parser.parse();

    ASSERT_EQ(stmt.select_list.size(), 2u);
    EXPECT_EQ(stmt.select_list[0]->alias, "double_speed");
    EXPECT_TRUE(stmt.select_list[1]->alias.empty());
}

TEST(ParserTest, AggregateAlias) {
    Parser parser("SELECT SUM(speed) AS total FROM laps");
    SelectStatement stmt = parser.parse();
    EXPECT_EQ(stmt.select_list[0]->alias, "total");
    EXPECT_NE(dynamic_cast<AggregateExpr*>(stmt.select_list[0].get()), nullptr);
}


// ===== Week 24: GROUP BY expressions =====

TEST(ParserTest, GroupByExpression) {
    Parser parser("SELECT COUNT(*) FROM laps GROUP BY season - 2000, team");
    SelectStatement stmt = parser.parse();

    ASSERT_EQ(stmt.group_by.size(), 2u);
    ASSERT_NE(stmt.group_by[0].expr, nullptr);        // expression key
    EXPECT_TRUE(stmt.group_by[0].column_name.empty());
    EXPECT_EQ(stmt.group_by[1].expr, nullptr);        // plain column keeps fast path
    EXPECT_EQ(stmt.group_by[1].column_name, "team");
}

TEST(ParserTest, GroupByQualifiedColumnStillPlain) {
    Parser parser("SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team");
    SelectStatement stmt = parser.parse();

    ASSERT_EQ(stmt.group_by.size(), 1u);
    EXPECT_EQ(stmt.group_by[0].expr, nullptr);
    EXPECT_EQ(stmt.group_by[0].table_name, "l");
    EXPECT_EQ(stmt.group_by[0].column_name, "team");
}


// ============================================================
// Audit fix: parse error positions
// ============================================================
// line_/col_ were initialized but never advanced — advance() moved pos_ without
// touching col_ — so every ParseError reported "line 0, col 0" no matter where
// the offending token was. Positions are 1-based, as a human reads them.

TEST(ParseErrorPosition, ReportsTheColumnOfTheOffendingToken) {
    try {
        Parser("SELECT team, FROM laps").parse();
        FAIL() << "expected a ParseError";
    } catch (const ParseError& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("line 1"), std::string::npos) << msg;
        // "FROM" starts at column 14 of the statement
        EXPECT_NE(msg.find("col 14"), std::string::npos) << msg;
        EXPECT_EQ(msg.find("col 0"), std::string::npos) << msg;
    }
}

TEST(ParseErrorPosition, CountsLinesAcrossNewlines) {
    try {
        Parser("SELECT team\nFROM laps\nWHERE speed >").parse();
        FAIL() << "expected a ParseError";
    } catch (const ParseError& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("line 3"), std::string::npos) << msg;
    }
}

TEST(ParseErrorPosition, NumericLiteralOverflowIsAParseErrorNotStoll) {
    // std::stoll's what() is a bare "stoll: out of range" — no position, no SQL
    try {
        Parser("SELECT 99999999999999999999 FROM laps").parse();
        FAIL() << "expected a ParseError";
    } catch (const ParseError& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("64-bit integer"), std::string::npos) << msg;
        EXPECT_NE(msg.find("line 1"), std::string::npos) << msg;
        EXPECT_EQ(msg.find("stoll"), std::string::npos) << msg;
    }

    try {
        Parser("SELECT team FROM laps LIMIT 99999999999999999999").parse();
        FAIL() << "expected a ParseError";
    } catch (const ParseError& e) {
        std::string msg = e.what();
        EXPECT_NE(msg.find("LIMIT"), std::string::npos) << msg;
        EXPECT_EQ(msg.find("stoi"), std::string::npos) << msg;
    }
}

TEST(ParseErrorPosition, LargestValidIntLiteralStillParses) {
    // INT64_MAX itself must not be rejected by the overflow guard
    auto stmt = Parser("SELECT 9223372036854775807 FROM laps").parse();
    ASSERT_EQ(stmt.select_list.size(), 1u);
    auto* lit = dynamic_cast<Literal*>(stmt.select_list[0].get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value.asInt(), INT64_MAX);
}

// ===== Week 25: predicates + scalar functions =====

TEST(LexerTest, Week25Keywords) {
    Lexer lexer("BETWEEN like In case WHEN then ELSE end substring FOR date INTERVAL");
    EXPECT_EQ(lexer.nextToken().type, TokenType::BETWEEN);
    EXPECT_EQ(lexer.nextToken().type, TokenType::LIKE);
    EXPECT_EQ(lexer.nextToken().type, TokenType::IN);
    EXPECT_EQ(lexer.nextToken().type, TokenType::CASE);
    EXPECT_EQ(lexer.nextToken().type, TokenType::WHEN);
    EXPECT_EQ(lexer.nextToken().type, TokenType::THEN);
    EXPECT_EQ(lexer.nextToken().type, TokenType::ELSE);
    EXPECT_EQ(lexer.nextToken().type, TokenType::END);
    EXPECT_EQ(lexer.nextToken().type, TokenType::SUBSTRING);
    EXPECT_EQ(lexer.nextToken().type, TokenType::FOR);
    EXPECT_EQ(lexer.nextToken().type, TokenType::DATE);
    EXPECT_EQ(lexer.nextToken().type, TokenType::INTERVAL);
}

// Every keyword added is a column name taken away, so the interval units are
// deliberately NOT reserved — TPC-H Q7/Q8/Q9 alias o_year / l_year.
TEST(LexerTest, IntervalUnitsAreNotReservedWords) {
    Lexer lexer("day month year");
    EXPECT_EQ(lexer.nextToken().type, TokenType::IDENTIFIER);
    EXPECT_EQ(lexer.nextToken().type, TokenType::IDENTIFIER);
    EXPECT_EQ(lexer.nextToken().type, TokenType::IDENTIFIER);
}

TEST(ParserTest, BetweenDesugarsToTwoComparisons) {
    // No BetweenExpr node: the desugared shape is what splitConjuncts,
    // ChunkPruner, scanColumn and selectivity() all pattern-match on.
    Parser p("SELECT team FROM laps WHERE season BETWEEN 2020 AND 2024");
    auto stmt = p.parse();
    auto* root = dynamic_cast<BinaryExpr*>(stmt.where.get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, "AND");
    ASSERT_NE(dynamic_cast<BinaryExpr*>(root->left.get()), nullptr);
    EXPECT_EQ(dynamic_cast<BinaryExpr*>(root->left.get())->op, ">=");
    EXPECT_EQ(dynamic_cast<BinaryExpr*>(root->right.get())->op, "<=");
}

TEST(ParserTest, NotBetweenDesugarsByDeMorgan) {
    Parser p("SELECT team FROM laps WHERE season NOT BETWEEN 2020 AND 2024");
    auto stmt = p.parse();
    auto* root = dynamic_cast<BinaryExpr*>(stmt.where.get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, "OR");
    EXPECT_EQ(dynamic_cast<BinaryExpr*>(root->left.get())->op, "<");
    EXPECT_EQ(dynamic_cast<BinaryExpr*>(root->right.get())->op, ">");
}

// The precedence trap: BETWEEN binds tighter than AND, so the bounds must be
// parsed at the additive level. parseExpr() there would swallow the second AND.
TEST(ParserTest, BetweenBindsTighterThanAnd) {
    Parser p("SELECT team FROM laps WHERE season BETWEEN 2020 AND 2024 AND speed > 300");
    auto stmt = p.parse();
    auto* root = dynamic_cast<BinaryExpr*>(stmt.where.get());
    ASSERT_NE(root, nullptr);
    EXPECT_EQ(root->op, "AND");
    // right operand is the speed comparison, not part of the range
    auto* right = dynamic_cast<BinaryExpr*>(root->right.get());
    ASSERT_NE(right, nullptr);
    EXPECT_EQ(right->op, ">");
}

TEST(ParserTest, InListParsesConstants) {
    Parser p("SELECT team FROM laps WHERE season IN (2020, 2021, -3)");
    auto stmt = p.parse();
    auto* in = dynamic_cast<InExpr*>(stmt.where.get());
    ASSERT_NE(in, nullptr);
    EXPECT_FALSE(in->negated);
    ASSERT_EQ(in->values.size(), 3u);
    EXPECT_EQ(in->values[2].asInt(), -3);   // negated literal is still a constant

    Parser np("SELECT team FROM laps WHERE season NOT IN (2020)");
    auto nstmt = np.parse();
    EXPECT_TRUE(dynamic_cast<InExpr*>(nstmt.where.get())->negated);
}

TEST(ParserTest, InListRejectsNonConstants) {
    // IN (subquery) is Week 32 and lowers to a semi-join; a column or an
    // arithmetic tree would defeat the compile-time hashing of the set
    EXPECT_THROW(Parser("SELECT team FROM laps WHERE season IN (speed)").parse(), ParseError);
    EXPECT_THROW(Parser("SELECT team FROM laps WHERE season IN (1 + 1)").parse(), ParseError);
    EXPECT_THROW(Parser("SELECT team FROM laps WHERE season IN ()").parse(), ParseError);
}

TEST(ParserTest, LikeRequiresAConstantPattern) {
    Parser p("SELECT team FROM laps WHERE team LIKE 'Fer%'");
    auto stmt = p.parse();
    auto* lk = dynamic_cast<LikeExpr*>(stmt.where.get());
    ASSERT_NE(lk, nullptr);
    EXPECT_EQ(lk->pattern, "Fer%");
    EXPECT_FALSE(lk->negated);

    Parser np("SELECT team FROM laps WHERE team NOT LIKE 'Fer%'");
    EXPECT_TRUE(dynamic_cast<LikeExpr*>(np.parse().where.get())->negated);

    EXPECT_THROW(Parser("SELECT team FROM laps WHERE team LIKE team").parse(), ParseError);
}

TEST(ParserTest, GeneralNotIsRejectedWithATargetedMessage) {
    // NOT is supported only in the four postfix/IS forms; anything else must
    // fail cleanly rather than confuse the caller further up the grammar
    EXPECT_THROW(Parser("SELECT team FROM laps WHERE season NOT = 1").parse(), ParseError);
}

TEST(ParserTest, SearchedCaseParsesBothWithAndWithoutElse) {
    Parser p("SELECT CASE WHEN season = 2024 THEN 1 WHEN season = 2023 THEN 2 ELSE 0 END AS m FROM laps");
    auto stmt = p.parse();
    auto* c = dynamic_cast<CaseExpr*>(stmt.select_list[0].get());
    ASSERT_NE(c, nullptr);
    EXPECT_EQ(c->when_clauses.size(), 2u);
    EXPECT_NE(c->else_expr, nullptr);
    EXPECT_EQ(c->alias, "m");

    Parser p2("SELECT CASE WHEN season = 2024 THEN 1 END AS m FROM laps");
    EXPECT_EQ(dynamic_cast<CaseExpr*>(p2.parse().select_list[0].get())->else_expr, nullptr);
}

TEST(ParserTest, CaseRequiresWhenAndEnd) {
    EXPECT_THROW(Parser("SELECT CASE ELSE 1 END AS m FROM laps").parse(), ParseError);
    EXPECT_THROW(Parser("SELECT CASE WHEN 1 = 1 THEN 2 FROM laps").parse(), ParseError);
    EXPECT_THROW(Parser("SELECT CASE WHEN 1 = 1 THEN 2 ELSE 3 FROM laps").parse(), ParseError);
}

TEST(ParserTest, SubstringAcceptsBothSyntaxes) {
    // SQL-standard form (TPC-H Q22). SQLite only accepts the comma form, which
    // is why the correctness harness uses commas and this form is unit-tested.
    // the statement must outlive the pointers into it — parse() returns by value
    Parser standard("SELECT SUBSTRING(team FROM 1 FOR 3) AS t FROM laps");
    SelectStatement s1 = standard.parse();
    auto* a = dynamic_cast<SubstringExpr*>(s1.select_list[0].get());
    ASSERT_NE(a, nullptr);
    EXPECT_NE(a->length, nullptr);

    Parser commas("SELECT SUBSTRING(team, 1, 3) AS t FROM laps");
    SelectStatement s2 = commas.parse();
    auto* b = dynamic_cast<SubstringExpr*>(s2.select_list[0].get());
    ASSERT_NE(b, nullptr);
    EXPECT_NE(b->length, nullptr);

    // omitted length: to the end of the string
    Parser open("SELECT SUBSTRING(team, 2) AS t FROM laps");
    SelectStatement s3 = open.parse();
    ASSERT_NE(dynamic_cast<SubstringExpr*>(s3.select_list[0].get()), nullptr);
    EXPECT_EQ(dynamic_cast<SubstringExpr*>(s3.select_list[0].get())->length, nullptr);

    Parser open2("SELECT SUBSTRING(team FROM 2) AS t FROM laps");
    SelectStatement s4 = open2.parse();
    ASSERT_NE(dynamic_cast<SubstringExpr*>(s4.select_list[0].get()), nullptr);
    EXPECT_EQ(dynamic_cast<SubstringExpr*>(s4.select_list[0].get())->length, nullptr);
}

TEST(ParserTest, DateLiteralIsAValidatedStringLiteral) {
    // DATE is literal SYNTAX, not a type: ISO-8601 sorts lexicographically, so
    // a STRING Literal keeps zone-map pruning and scanColumn<std::string>
    Parser p("SELECT team FROM laps WHERE team > date '1998-12-01'");
    SelectStatement stmt = p.parse();
    auto* bin = dynamic_cast<BinaryExpr*>(stmt.where.get());
    ASSERT_NE(bin, nullptr);
    auto* lit = dynamic_cast<Literal*>(bin->right.get());
    ASSERT_NE(lit, nullptr);
    EXPECT_EQ(lit->value.type(), TypeId::STRING);
    EXPECT_EQ(lit->value.asString(), "1998-12-01");

    EXPECT_THROW(Parser("SELECT team FROM laps WHERE team > date '1998-02-30'").parse(), ParseError);
    EXPECT_THROW(Parser("SELECT team FROM laps WHERE team > date '12/01/1998'").parse(), ParseError);
    EXPECT_THROW(Parser("SELECT team FROM laps WHERE team > date 19981201").parse(), ParseError);
}

TEST(ParserTest, IntervalLiteralParsesUnitsCaseInsensitively) {
    auto unitOf = [](const char* sql) {
        Parser p(sql);
        SelectStatement stmt = p.parse();   // must outlive the pointers below
        auto* bin = dynamic_cast<BinaryExpr*>(stmt.where.get());
        EXPECT_NE(bin, nullptr);
        auto* iv = dynamic_cast<IntervalLiteral*>(bin->right.get());
        EXPECT_NE(iv, nullptr);
        return iv->unit;
    };
    EXPECT_EQ(unitOf("SELECT team FROM laps WHERE team > interval '90' day"),
              IntervalLiteral::Unit::DAY);
    EXPECT_EQ(unitOf("SELECT team FROM laps WHERE team > interval '3' MONTHS"),
              IntervalLiteral::Unit::MONTH);
    EXPECT_EQ(unitOf("SELECT team FROM laps WHERE team > interval '1' Year"),
              IntervalLiteral::Unit::YEAR);

    EXPECT_THROW(Parser("SELECT team FROM laps WHERE team > interval '1' fortnight").parse(),
                 ParseError);
    EXPECT_THROW(Parser("SELECT team FROM laps WHERE team > interval 'x' day").parse(),
                 ParseError);
}

// The parser must consume the whole input. It used to stop at the last clause
// it recognised and silently discard the rest, which turns an unsupported
// clause into a wrong answer instead of an error: `LIKE 'a%b' ESCAPE '\'` ran
// with the escape dropped. Pre-existing since Week 4; Week 25's LIKE is what
// made a silently-ignored trailing clause dangerous rather than merely sloppy.
TEST(ParserTest, TrailingInputIsRejected) {
    EXPECT_THROW(Parser("SELECT team FROM laps LIMIT 2 TOTAL GARBAGE").parse(), ParseError);
    EXPECT_THROW(Parser("SELECT team FROM laps WHERE team LIKE 'a%' ESCAPE '\\'").parse(),
                 ParseError);
    EXPECT_THROW(Parser("SELECT team FROM laps WHERE season = 2024 BOGUS").parse(), ParseError);

    // a trailing semicolon is still fine — run_queries.py splits on it, but a
    // hand-written --query may keep it
    EXPECT_NO_THROW(Parser("SELECT team FROM laps LIMIT 2;").parse());
    EXPECT_NO_THROW(Parser("SELECT team FROM laps LIMIT 2").parse());
}
