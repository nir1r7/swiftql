#include "lexer.h"
#include "token.h"
#include <stdexcept>
#include <string>
#include <unordered_map>

// line_/col_ are 1-based: error messages are read by humans
Lexer::Lexer(const std::string& input): input_{input}, pos_{0}, line_{1}, col_{1} {}

Token Lexer::nextToken() {
    skipWhitespace();

    if (isAtEnd()) {
        return {TokenType::END_OF_FILE, "", line_, col_};
    }

    char c = current();

    // identifiers and keywords start with a letter or underscore
    if (std::isalpha(c) || c == '_') return readIdentifierOrKeyword();

    // numbers start with a digit
    if (std::isdigit(c)) return readNumber();

    // single quoted strings
    if (c == '\'') return readString();

    // single character tokens
    int token_col = col_;
    advance();
    
    switch (c) {
        case ',': return {TokenType::COMMA, ",", line_, token_col};
        case '(': return {TokenType::LPAREN, "(", line_, token_col};
        case ')': return {TokenType::RPAREN, ")", line_, token_col};
        case '.': return {TokenType::DOT, ".", line_, token_col};
        case ';': return {TokenType::SEMICOLON, ";", line_, token_col};
        case '*': return {TokenType::STAR, "*", line_, token_col};
        case '+': return {TokenType::PLUS, "+", line_, token_col};
        case '-': return {TokenType::MINUS, "-", line_, token_col};
        case '/': return {TokenType::SLASH, "/", line_, token_col};
        case '=': return {TokenType::EQ, "=", line_, token_col};
        case '<':
            if (!isAtEnd() && current() == '=') {
                advance();
                return {TokenType::LTE, "<=", line_, token_col};
            }
            return {TokenType::LT, "<", line_, token_col};
        case '>':
            if (!isAtEnd() && current() == '=') {
                advance();
                return {TokenType::GTE, ">=", line_, token_col};
            }
            return {TokenType::GT, ">", line_, token_col};
        case '!':
            if (!isAtEnd() && current() == '=') {
                advance();
                return {TokenType::NEQ, "!=", line_, token_col};
            }
            throw std::runtime_error("Unexpected character '!' at col " + std::to_string(token_col));
        default:
            throw std::runtime_error(
                std::string("Unexpected character '") + c + "'");
    }
}

Token Lexer::peek() {
    int saved_pos = pos_;
    int saved_line = line_;
    int saved_col = col_;

    Token t = nextToken();

    pos_ = saved_pos;
    line_ = saved_line;
    col_ = saved_col;

    return t;
}

void Lexer::skipWhitespace() {
    // newline bookkeeping lives in advance(), the single place that moves pos_
    while (!isAtEnd() && std::isspace(current())) advance();
}

Token Lexer::readIdentifierOrKeyword() {
    int start_col = col_;
    std::string text;

    while (!isAtEnd() && (std::isalnum(current()) || current() == '_')) {
        text += advance();
    }

    // convert to uppercase for case insensitive matching
    std::string upper = text;
    for (char& ch : upper) ch = std::toupper(ch);

    auto it = KEYWORDS.find(upper);
    if (it != KEYWORDS.end()) {
        return {it->second, text, line_, start_col};
    }
    return {TokenType::IDENTIFIER, text, line_, start_col};
}

Token Lexer::readNumber() {
    int start_col = col_;
    std::string text;
    bool is_float = false;

    while (!isAtEnd() && std::isdigit(current())) {
        text += advance();
    }

    // check for decimal point
    if (!isAtEnd() && current() == '.' ) {
        is_float = true;
        // add '.'
        text += advance();
        while (!isAtEnd() && std::isdigit(current())) {
            text += advance();
        }
    }

    TokenType type = is_float ? TokenType::FLOAT_LITERAL : TokenType::INT_LITERAL;
    return {type, text, line_, start_col};
}

// handles 'single quoted strings'
Token Lexer::readString() {
    int start_col = col_;
    advance();  // consume opening '
    std::string text;

    while (!isAtEnd() && current() != '\'') {
        text += advance();
    }

    if (isAtEnd()) {
        throw std::runtime_error("Unterminated string literal");
    }

    advance();  // consume closing '
    return {TokenType::STRING_LITERAL, text, line_, start_col};
}

// character at pos_
char Lexer::current() const {
    return input_.at(pos_);
} 

// return current char and move pos_ forward
char Lexer::advance() {
    char c = input_.at(pos_++);
    // Every consumed character moves the column. Nothing did this before, so
    // line_/col_ never left their initial values and every ParseError reported
    // "line 0, col 0" regardless of where the bad token actually was.
    if (c == '\n') { line_++; col_ = 1; } else { col_++; }
    return c;
}

bool Lexer::isAtEnd() const {
    return pos_ >= input_.size();
}

const std::unordered_map<std::string, TokenType> Lexer::KEYWORDS = {
    {"SELECT",   TokenType::SELECT},
    {"FROM",     TokenType::FROM},
    {"WHERE",    TokenType::WHERE},
    {"GROUP",    TokenType::GROUP},
    {"BY",       TokenType::BY},
    {"HAVING",   TokenType::HAVING},
    {"ORDER",    TokenType::ORDER},
    {"LIMIT",    TokenType::LIMIT},
    {"DISTINCT", TokenType::DISTINCT},
    {"JOIN",     TokenType::JOIN},
    {"ON",       TokenType::ON},
    {"IS",       TokenType::IS},
    {"NOT",      TokenType::NOT},
    {"AND",      TokenType::AND},
    {"OR",       TokenType::OR},
    {"AS",       TokenType::AS},
    {"ASC",      TokenType::ASC},
    {"DESC",     TokenType::DESC},
    {"NULL",     TokenType::NULL_KW},
    {"COUNT",    TokenType::COUNT},
    {"SUM",      TokenType::SUM},
    {"AVG",      TokenType::AVG},
    {"MIN",      TokenType::MIN},
    {"MAX",      TokenType::MAX},
};