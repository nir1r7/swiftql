
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
        // select item: expr [AS alias] — the AS keyword is required for an
        // alias (SQLite's implicit "expr alias" form is a documented non-goal)
        auto parseSelectItem = [&]() {
            auto expr = parseExpr();
            if (match(TokenType::AS)) {
                expr->alias = expect(TokenType::IDENTIFIER, "alias after AS").value;
            }
            return expr;
        };
        stmt.select_list.push_back(parseSelectItem());
        while (match(TokenType::COMMA)) {
            stmt.select_list.push_back(parseSelectItem());
        }
    }

    expect(TokenType::FROM, "FROM");
    stmt.from_table = expect(TokenType::IDENTIFIER, "table name").value;

    // optional table alias (e.g. FROM laps l / FROM laps AS l);
    // an explicit AS makes the alias mandatory
    if (match(TokenType::AS)) {
        stmt.from_alias = expect(TokenType::IDENTIFIER, "table alias after AS").value;
    } else if (check(TokenType::IDENTIFIER) &&
        !check(TokenType::JOIN) &&
        !check(TokenType::WHERE) &&
        !check(TokenType::GROUP) &&
        !check(TokenType::ORDER) &&
        !check(TokenType::LIMIT) &&
        !check(TokenType::HAVING) &&
        !check(TokenType::END_OF_FILE)) {
        stmt.from_alias = consume().value;
    }

    if (match(TokenType::JOIN)){
        SelectStatement::JoinClause join;
        join.join_table = expect(TokenType::IDENTIFIER, "join table name").value;

        // optional join table alias (i.e, JOIN drivers d / JOIN drivers AS d)
        if (match(TokenType::AS)) {
            join.alias = expect(TokenType::IDENTIFIER, "table alias after AS").value;
        } else if (check(TokenType::IDENTIFIER)) {
            join.alias = consume().value;
        }

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
        {
            OrderByItem item;
            item.expr = parseExpr();
            item.desc = match(TokenType::DESC);
            if (!item.desc) match(TokenType::ASC);
            stmt.order_by.push_back(std::move(item));
        }
        while (match(TokenType::COMMA)) {
            OrderByItem item;
            item.expr = parseExpr();
            item.desc = match(TokenType::DESC);
            if (!item.desc) match(TokenType::ASC);
            stmt.order_by.push_back(std::move(item));
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
    auto left = parseAdditive();

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
    auto right = parseAdditive();

    auto node = std::make_unique<BinaryExpr>();
    node->op = op;
    node->left = std::move(left);
    node->right = std::move(right);
    return node;
}

// additive → multiplicative (('+' | '-') multiplicative)*
std::unique_ptr<Expr> Parser::parseAdditive(){
    auto left = parseMultiplicative();

    while (check(TokenType::PLUS) || check(TokenType::MINUS)){
        std::string op = consume().value;
        auto node = std::make_unique<BinaryExpr>();
        node->op = op;
        node->left = std::move(left);
        node->right = parseMultiplicative();   // tighter level binds first
        left = std::move(node);                // left-associative fold
    }

    return left;
}

// multiplicative → unary (('*' | '/') unary)*
std::unique_ptr<Expr> Parser::parseMultiplicative(){
    auto left = parseUnary();

    while (check(TokenType::STAR) || check(TokenType::SLASH)){
        std::string op = consume().value;
        auto node = std::make_unique<BinaryExpr>();
        node->op = op;
        node->left = std::move(left);
        node->right = parseUnary();
        left = std::move(node);
    }

    return left;
}

// unary → '-' unary | primary
std::unique_ptr<Expr> Parser::parseUnary(){
    if (check(TokenType::MINUS)){
        consume();
        auto node = std::make_unique<UnaryExpr>();
        node->op = "-";
        node->operand = parseUnary();   // recursion allows "- -x"
        return node;
    }
    return parsePrimary();
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

std::vector<GroupByColumn> Parser::parseColumnList(){
    auto parseOne = [&]() -> GroupByColumn {
        GroupByColumn col;
        col.column_name = expect(TokenType::IDENTIFIER, "column name").value;
        if (match(TokenType::DOT)) {
            col.table_name = col.column_name;
            col.column_name = expect(TokenType::IDENTIFIER, "column name after '.'").value;
        }
        return col;
    };
    std::vector<GroupByColumn> cols;
    cols.push_back(parseOne());
    while (match(TokenType::COMMA)) {
        cols.push_back(parseOne());
    }
    return cols;
}