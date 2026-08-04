#pragma once

#include "lexer.h"
#include "ast.h"
#include "parse_error.h"
#include <memory>


class Parser {
    public:
        explicit Parser(const std::string& input);

        SelectStatement parse();
    private:
        Lexer lexer_;
        Token current_; // token currently being examined

        // advance to next token and return the current one consumed
        Token consume();

        // consume token (accept and move forward) if matches expected type
        Token expect(TokenType type, const std::string& context);

        // check if type matches without consuming
        bool check(TokenType type) const;

        // consume and return token if type matches
        bool match(TokenType type);

        // grammar rule methods
        SelectStatement parseSelect();
        std::unique_ptr<Expr> parseExpr();
        std::unique_ptr<Expr> parseOrExpr();
        std::unique_ptr<Expr> parseAndExpr();
        std::unique_ptr<Expr> parseCompare();
        std::unique_ptr<Expr> parseAdditive();
        std::unique_ptr<Expr> parseMultiplicative();
        std::unique_ptr<Expr> parseUnary();
        std::unique_ptr<Expr> parsePrimary();
        std::vector<GroupByColumn> parseColumnList();
};