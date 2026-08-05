#pragma once

#include <string>

enum class TokenType {
    // keywords
    SELECT, FROM, WHERE, GROUP, BY, HAVING, ORDER,
    LIMIT, DISTINCT, JOIN, ON, IS, NOT, AND, OR, AS, ASC, DESC,
    NULL_KW, // NULL_KW because NULL is reserved in C++

    // Week 25 predicates. Interval units (day/month/year) are deliberately NOT
    // tokens: they are matched as IDENTIFIER text so `year` stays usable as a
    // column name (TPC-H Q7/Q8/Q9 alias o_year / l_year).
    BETWEEN, LIKE, IN,
    CASE, WHEN, THEN, ELSE, END,

    // aggregate functions
    COUNT, SUM, AVG, MIN, MAX,

    // Week 25 scalar function + literal syntaxes
    SUBSTRING,   // SUBSTRING(x FROM a FOR b) / SUBSTRING(x, a, b)
    FOR,         // only meaningful inside SUBSTRING
    DATE,        // DATE 'YYYY-MM-DD'
    INTERVAL,    // INTERVAL 'n' day|month|year

    // literals
    INT_LITERAL,     // i.e, 2025
    FLOAT_LITERAL,   // i.e, 312.45
    STRING_LITERAL,  // i.e, 'Ferrari'

    // identifiers; column names, table names
    IDENTIFIER,      // i.e, team, laps, speed

    // operators
    EQ,          // =
    NEQ,         // !=
    LT,          // <
    GT,          // >
    LTE,         // <=
    GTE,         // >=
    STAR,        // * (COUNT(*), SELECT *, and multiplication)
    PLUS,        // +
    MINUS,       // - (binary and unary)
    SLASH,       // /

    // punctuation
    COMMA,       // ,
    LPAREN,      // (
    RPAREN,      // )
    DOT,         // . (for table.column references)
    SEMICOLON,   // ;

    // special
    END_OF_FILE  // signals the lexer has nothing left
};

struct Token {
    TokenType type;
    std::string value;
    int line;
    int col;
};