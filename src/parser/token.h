#pragma once

#include <string>

enum class TokenType {
    // keywords
    SELECT, FROM, WHERE, GROUP, BY, HAVING, ORDER,
    LIMIT, DISTINCT, JOIN, ON, IS, NOT, AND, OR, AS, ASC, DESC,
    NULL_KW, // NULL_KW because NULL is reserved in C++

    // aggregate functions
    COUNT, SUM, AVG, MIN, MAX,

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