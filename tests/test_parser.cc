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
TEST(ParserTest, TableAliasWithAsKeyword) {
    Parser p("SELECT l.team FROM laps AS l JOIN drivers AS d ON l.driver_id = d.driver_id");
    auto stmt = p.parse();
    EXPECT_EQ(stmt.from_alias, "l");
    ASSERT_TRUE(stmt.join.has_value());
    EXPECT_EQ(stmt.join->alias, "d");
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
