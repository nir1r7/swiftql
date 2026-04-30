#include <gtest/gtest.h>
#include "parser/token.h"
#include "parser/lexer.h"
#include "parser/ast.h"


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