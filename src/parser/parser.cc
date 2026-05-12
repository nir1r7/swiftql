
#include "parser.h"
#include <string.h>


Parser::Parser(const std::string& input): lexer_{input} {
    // load first token
    current_ = lexer_.nextToken();
}

// advance to next token and return the current one consumed
Token Parser::consume(){
    Token t = current_;
    current_ = lexer_.nextToken();
    return t;
}

// check if type matches without consuming
bool Parser::check(TokenType type) const {
    return current_.type == type;
}

// consume token (accept and move forward) if matches expected type
Token Parser::expect(TokenType type, const std::string& context){
    if (!check(type)){
        throw ParseError("Expected " + context, current_);
    }
    return consume();
}

// consume and return if type matches
bool Parser::match(TokenType type){
    if (check(type)){
        consume();
        return true;
    }
    return false;
}


SelectStatement Parser::parse(){
    return parseSelect();
}

// grammar rule methods
SelectStatement Parser::parseSelect(){
    SelectStatement stmt;

    expect(TokenType::SELECT, "SELECT");

    stmt.distinct = match(TokenType::DISTINCT);

    if (check(TokenType::STAR)) {
        consume();
        stmt.select_star = true;
    } else {
        stmt.select_list.push_back(parseExpr());
        while (match(TokenType::COMMA)) {
            stmt.select_list.push_back(parseExpr());
        }
    }

    expect(TokenType::FROM, "FROM");
    stmt.from_table = expect(TokenType::IDENTIFIER, "table name").value;

    // optional table alias (e.g. FROM laps l)
    // consume and discard the alias (for now hopefully)
    if (check(TokenType::IDENTIFIER) && 
        !check(TokenType::JOIN) && 
        !check(TokenType::WHERE) &&
        !check(TokenType::GROUP) &&
        !check(TokenType::ORDER) &&
        !check(TokenType::LIMIT) &&
        !check(TokenType::HAVING) &&
        !check(TokenType::END_OF_FILE)) {
        consume(); // discard alias
    }

    if (match(TokenType::JOIN)){
        SelectStatement::JoinClause join;
        join.join_table = expect(TokenType::IDENTIFIER, "join table name").value;
        expect(TokenType::ON, "ON");
        join.condition = parseExpr();
        stmt.join = std::move(join);
    }

    if (match(TokenType::WHERE)){
        stmt.where = parseExpr();
    }

    if (match(TokenType::GROUP)){
        expect(TokenType::BY, "BY");
        stmt.group_by = parseColumnList();
    }

    if (match(TokenType::HAVING)){
        stmt.having = parseExpr();
    }

    if (match(TokenType::ORDER)){
        expect(TokenType::BY, "BY");
        stmt.order_by.push_back(parsePrimary());
        while (match(TokenType::COMMA)) {
            stmt.order_by.push_back(parsePrimary());
        }
    }

    if (match(TokenType::LIMIT)){
        Token t = expect(TokenType::INT_LITERAL, "integer after LIMIT");
        stmt.limit = std::stoi(t.value);
    }

    return stmt;
}

std::unique_ptr<Expr> Parser::parseExpr(){
    return parseOrExpr();
}

std::unique_ptr<Expr> Parser::parseOrExpr(){
    auto left = parseAndExpr();

    while (check(TokenType::OR)){
        consume();
        auto right = parseAndExpr();
        auto node = std::make_unique<BinaryExpr>();
        node->op = "OR";
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }

    return left;
}

std::unique_ptr<Expr> Parser::parseAndExpr(){
    auto left = parseCompare();

    while (check(TokenType::AND)){
        consume();
        auto right = parseCompare();
        auto node = std::make_unique<BinaryExpr>();
        node->op = "AND";
        node->left = std::move(left);
        node->right = std::move(right);
        left = std::move(node);
    }

    return left;
}

std::unique_ptr<Expr> Parser::parseCompare(){
    auto left = parsePrimary();

    // IS NULL or IS NOT NULL
    if (check(TokenType::IS)) {
        consume();
        bool is_not = match(TokenType::NOT);

        expect(TokenType::NULL_KW, "NULL");

        auto node = std::make_unique<IsNullExpr>();
        node->operand = std::move(left);
        node->is_not_null = is_not;
        return node;
    }

    std::string op;
    if (check(TokenType::EQ)) op = "=";
    else if (check(TokenType::NEQ)) op = "!=";
    else if (check(TokenType::LT)) op = "<";
    else if (check(TokenType::GT)) op = ">";
    else if (check(TokenType::LTE)) op = "<=";
    else if (check(TokenType::GTE)) op = ">=";
    else return left;

    // consume operator
    consume();
    auto right = parsePrimary();

    auto node = std::make_unique<BinaryExpr>();
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    return node;
}

std::unique_ptr<Expr> Parser::parsePrimary(){
    // parenthesized expression
    if (match(TokenType::LPAREN)) {
        auto expr = parseExpr();
        expect(TokenType::RPAREN, ")");
        return expr;
    }

    if (check(TokenType::INT_LITERAL)) {
    Token t = consume();
    return std::make_unique<Literal>(Value(static_cast<int64_t>(std::stoll(t.value))));
    }

    if (check(TokenType::FLOAT_LITERAL)) {
        Token t = consume();
        return std::make_unique<Literal>(Value(std::stod(t.value)));
    }

    if (check(TokenType::STRING_LITERAL)) {
        Token t = consume();
        return std::make_unique<Literal>(Value(t.value));
    }

    if (check(TokenType::AVG) || check(TokenType::SUM) || check(TokenType::COUNT) || check(TokenType::MIN) || check(TokenType::MAX)) {
        Token func = consume();
        expect(TokenType::LPAREN, "(");

        auto node = std::make_unique<AggregateExpr>();
        node->function_name = func.value;

        // uppercase function name for consistency
        for (char& c : node->function_name) c = std::toupper(c);

        if (check(TokenType::STAR)) {
            consume();
            node->is_star = true;
            node->argument = nullptr;
        } else {
            node->is_star = false;
            node->argument = parseExpr();
        }

        expect(TokenType::RPAREN, ")");
        return node;
    }

    if (check(TokenType::IDENTIFIER)) {
        Token t = consume();
        auto node = std::make_unique<ColumnRef>();

        // Check for table.column dot notation
        if (check(TokenType::DOT)) {
            consume(); // consume '.'
            Token col = expect(TokenType::IDENTIFIER, "column name after '.'");
            node->table_name = t.value;
            node->column_name = col.value;
        } else {
            node->table_name = "";
            node->column_name = t.value;
        }
        return node;
    }

    throw ParseError("Expected an expression", current_);
}

std::vector<std::string> Parser::parseColumnList(){
    std::vector<std::string> cols;
    cols.push_back(expect(TokenType::IDENTIFIER, "column name").value);
    while (match(TokenType::COMMA)) {
        cols.push_back(expect(TokenType::IDENTIFIER, "column name").value);
    }
    return cols;
}