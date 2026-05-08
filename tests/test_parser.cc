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
    EXPECT_EQ(stmt.group_by[0], "team");
    EXPECT_EQ(stmt.select_list.size(), 2);
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
    ASSERT_TRUE(stmt.join.has_value());
    EXPECT_EQ(stmt.join->join_table, "drivers");
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