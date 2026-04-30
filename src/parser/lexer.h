#pragma once

#include "token.h"
#include <string>
#include <unordered_map>

class Lexer {
public:
    explicit Lexer(const std::string& input);

    Token nextToken();
    Token peek();

private:
    std::string input_;
    int pos_; // current cursor position
    int line_;
    int col_;

    void skipWhitespace();
    Token readIdentifierOrKeyword();
    Token readNumber();
    Token readString(); // handles 'single quoted strings'

    char current() const;   // character at pos_
    char advance();         // return current char and move pos_ forward
    bool isAtEnd() const;

    // maps keyword strings to their TokenType
    static const std::unordered_map<std::string, TokenType> KEYWORDS;
};