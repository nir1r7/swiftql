
#include "parser.h"
#include "expr_utils.h"
#include "common/date_util.h"
#include <string.h>

namespace {

// One message for both NOT rejection sites: the leading position in
// parsePrimary (`WHERE NOT x = 1`, what users write) and the postfix lookahead
// in parseCompare (`WHERE x NOT 5`). `IS NOT NULL` is listed because it is a
// supported form worth naming, even though it is consumed by parseCompare's IS
// branch and never reaches either site.
const char* const kNotSupportMessage =
    "NOT is supported only as NOT BETWEEN, NOT LIKE, NOT IN or IS NOT NULL";

std::string upperCase(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// An IN list element must be a constant. Accepts a Literal or a negated
// numeric Literal (-1), which is what parseUnary produces; everything else is
// rejected so the executor can hash the set once at compile time.
bool constantValue(const Expr* expr, Value& out) {
    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        if (lit->value.isNull()) return false;
        out = lit->value;
        return true;
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        if (un->op != "-") return false;
        auto* lit = dynamic_cast<const Literal*>(un->operand.get());
        if (!lit || lit->value.isNull()) return false;
        if (lit->value.type() == TypeId::INT)    { out = Value(-lit->value.asInt());    return true; }
        if (lit->value.type() == TypeId::DOUBLE) { out = Value(-lit->value.asDouble()); return true; }
        return false;
    }
    return false;
}

} // namespace


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
    SelectStatement stmt = parseSelect();

    // Require the whole input to be consumed. Without this the parser silently
    // ignored everything after the last clause it recognised, which turns an
    // unsupported clause into a WRONG ANSWER rather than a clean error — the one
    // outcome this dialect's error handling exists to prevent. `WHERE x LIKE
    // 'a%b' ESCAPE '\'` ran with the escape dropped, and `... LIMIT 2 GARBAGE`
    // ran as if the garbage were not there.
    //
    // A trailing semicolon is fine: run_queries.py splits on it, but a
    // hand-written --query may keep it.
    match(TokenType::SEMICOLON);
    if (!check(TokenType::END_OF_FILE)) {
        throw ParseError("unexpected trailing input after the end of the query", current_);
    }
    return stmt;
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
    stmt.from = parseTableRef();

    // Explicit joins, in written order. `while`, not `if`: joins[i] attaches
    // relation slot i+1 (Week 26). Before this, a second JOIN fell through to
    // Parser::parse's end-of-input check and raised "unexpected trailing input",
    // which was the correct pre-state — pre-Week-25 it was silently discarded
    // and the query returned a one-join answer with no error.
    //
    // The bare-alias branch needs no keyword exclusion list (unlike the FROM
    // alias above): JOIN / ON / WHERE / GROUP / ORDER / LIMIT / HAVING are all
    // their own TokenTypes, so check(IDENTIFIER) is already false for them.
    //
    // Week 29: `check`, not `match`, because the loop head now has two entry
    // tokens. LEFT and JOIN are both TokenTypes of their own, so the bare-alias
    // branch above already declines them and needs no new exclusion.
    while (check(TokenType::JOIN) || check(TokenType::LEFT)){
        SelectStatement::JoinClause join;

        if (match(TokenType::LEFT)) {
            match(TokenType::OUTER);   // noise word, as in SQL: LEFT JOIN == LEFT OUTER JOIN
            expect(TokenType::JOIN, "JOIN after LEFT [OUTER]");
            join.type = JoinType::LEFT;
        } else {
            consume();                 // the JOIN token
        }

        // Week 34: the SAME helper as the FROM position. The two alias rules
        // were already identical bar the keyword exclusion list, and letting
        // them drift is how Week 26's `relations` keying bug and Week 29's `jc`
        // bug both happened.
        join.relation = parseTableRef();

        expect(TokenType::ON, "ON");
        join.condition = parseExpr();
        stmt.joins.push_back(std::move(join));
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
        // std::stoi throws std::out_of_range, whose what() is a bare "stoi: out
        // of range" with no position and no mention of SQL. Route it through
        // ParseError like every other malformed token.
        try {
            stmt.limit = std::stoi(t.value);
        } catch (const std::out_of_range&) {
            throw ParseError("LIMIT value is too large for a 32-bit integer", t);
        }
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

    // NOT BETWEEN / NOT LIKE / NOT IN. One-token lookahead: current_ is NOT, so
    // lexer_.peek() is the token after it. A NOT in this position that is not
    // one of the three is rejected here rather than left to confuse the caller —
    // general NOT is not part of the grammar (readme.md documents it).
    bool negated = false;
    if (check(TokenType::NOT)) {
        TokenType after = lexer_.peek().type;
        if (after != TokenType::BETWEEN && after != TokenType::LIKE && after != TokenType::IN) {
            throw ParseError(kNotSupportMessage, current_);
        }
        consume();
        negated = true;
    }

    if (check(TokenType::BETWEEN)) {
        consume();
        // Bounds parse at the ADDITIVE level, never parseExpr(): BETWEEN binds
        // tighter than AND, so `a BETWEEN 1 AND 2 AND b > 3` must leave the
        // second AND for parseAndExpr. parseExpr() here would swallow it.
        auto lower = parseAdditive();
        expect(TokenType::AND, "AND in BETWEEN");
        auto upper = parseAdditive();

        // Desugar rather than add a BetweenExpr node. The desugared shape is
        // what splitConjuncts, ChunkPruner, scanColumn and selectivity() all
        // pattern-match on, so both bounds get pushdown, zone-map pruning, the
        // tight typed loop and range estimation. A node would forfeit all four.
        // NOT BETWEEN uses De Morgan, which is valid in the Kleene three-valued
        // logic both evaluators implement.
        // cloneExpr before binding: the binder then stamps both copies.
        auto lo = std::make_unique<BinaryExpr>();
        lo->op = negated ? "<" : ">=";
        lo->left  = cloneExpr(left.get());
        lo->right = std::move(lower);

        auto hi = std::make_unique<BinaryExpr>();
        hi->op = negated ? ">" : "<=";
        hi->left  = std::move(left);
        hi->right = std::move(upper);

        auto node = std::make_unique<BinaryExpr>();
        node->op = negated ? "OR" : "AND";
        node->left  = std::move(lo);
        node->right = std::move(hi);
        return node;
    }

    if (check(TokenType::LIKE)) {
        consume();
        Token pat = expect(TokenType::STRING_LITERAL, "a constant pattern string after LIKE");
        auto node = std::make_unique<LikeExpr>();
        node->operand = std::move(left);
        node->pattern = pat.value;
        node->negated = negated;
        return node;
    }

    if (check(TokenType::IN)) {
        consume();
        expect(TokenType::LPAREN, "( after IN");

        // Week 30. IN (subquery) is a DIFFERENT production, not a longer
        // constant list: InExpr's whole design is a set hashed once at compile
        // time (ast.h), which a subquery cannot be, and Week 32 lowers this
        // shape to a semi-/anti-join instead. One token of lookahead keeps them
        // apart — SELECT can begin no expression, so the grammar stays
        // unambiguous.
        if (check(TokenType::SELECT)) {
            auto sq = std::make_unique<SubqueryExpr>();
            sq->kind = SubqueryExpr::Kind::IN;
            sq->negated = negated;
            sq->operand = std::move(left);
            // parseSelect(), never parse(): parse() requires end-of-input and a
            // subquery ends at its own ')'.
            sq->subquery = std::make_shared<SelectStatement>(parseSelect());
            expect(TokenType::RPAREN, ") to close the IN subquery");
            return sq;
        }

        auto node = std::make_unique<InExpr>();
        node->operand = std::move(left);
        node->negated = negated;

        do {
            // parseUnary, not parseExpr: an element is a literal or a negated
            // literal. Anything else (a column, an arithmetic tree, a subquery)
            // is rejected — see InExpr's contract in ast.h.
            Token at = current_;
            auto element = parseUnary();
            Value v;
            if (!constantValue(element.get(), v)) {
                throw ParseError(
                    "IN accepts a list of constant values only; "
                    "IN (subquery) is not supported", at);
            }
            node->values.push_back(std::move(v));
        } while (match(TokenType::COMMA));

        expect(TokenType::RPAREN, ") after IN list");
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
    // A LEADING NOT (`WHERE NOT season = 2021`) is the form users actually
    // write, and it reaches here — parseCompare's lookahead only sees a NOT that
    // follows a complete left operand. Without this it failed with the generic
    // "Expected an expression", which says nothing about what is supported.
    // Week 30 — NOT EXISTS. parseCompare's NOT lookahead only fires after a
    // complete left operand, so a LEADING NOT — which is how EXISTS is always
    // written (TPC-H Q21) — arrives here and would hit kNotSupportMessage
    // below. Same one-token lookahead idiom parseCompare uses: current_ is NOT,
    // so lexer_.peek() is the token after it. Must precede the throw.
    if (check(TokenType::NOT) && lexer_.peek().type == TokenType::EXISTS) {
        consume();                       // the NOT
        return parseExistsSubquery(/*negated=*/true);
    }
    if (check(TokenType::EXISTS)) {
        return parseExistsSubquery(/*negated=*/false);
    }

    if (check(TokenType::NOT)) {
        throw ParseError(kNotSupportMessage, current_);
    }

    // Parenthesized expression, or a Week 30 SCALAR subquery. One token of
    // lookahead separates them. The scalar form lives at the primary level
    // because it is self-delimiting, so it composes with arithmetic for free —
    // TPC-H Q17's `< 0.2 * (select avg(...) ...)` needs exactly that.
    if (match(TokenType::LPAREN)) {
        if (check(TokenType::SELECT)) {
            auto sq = std::make_unique<SubqueryExpr>();
            sq->kind = SubqueryExpr::Kind::SCALAR;
            sq->subquery = std::make_shared<SelectStatement>(parseSelect());
            expect(TokenType::RPAREN, ") to close the subquery");
            return sq;
        }
        auto expr = parseExpr();
        expect(TokenType::RPAREN, ")");
        return expr;
    }

    if (check(TokenType::INT_LITERAL)) {
        Token t = consume();
        try {
            return std::make_unique<Literal>(Value(static_cast<int64_t>(std::stoll(t.value))));
        } catch (const std::out_of_range&) {
            throw ParseError("integer literal does not fit in a 64-bit integer", t);
        }
    }

    if (check(TokenType::FLOAT_LITERAL)) {
        Token t = consume();
        try {
            return std::make_unique<Literal>(Value(std::stod(t.value)));
        } catch (const std::out_of_range&) {
            throw ParseError("floating-point literal is outside the range of a double", t);
        }
    }

    if (check(TokenType::STRING_LITERAL)) {
        Token t = consume();
        return std::make_unique<Literal>(Value(t.value));
    }

    // CASE ... END is self-delimiting, so it lives at the primary level and
    // composes with arithmetic (SUM(CASE ... END) / SUM(...), TPC-H Q14) with
    // no precedence work. Conditions and results use parseExpr(): END
    // terminates unambiguously, so there is no BETWEEN-style trap here.
    if (check(TokenType::CASE)) {
        consume();
        auto node = std::make_unique<CaseExpr>();
        do {
            expect(TokenType::WHEN, "WHEN in CASE");
            CaseExpr::WhenClause clause;
            clause.condition = parseExpr();
            expect(TokenType::THEN, "THEN in CASE");
            clause.result = parseExpr();
            node->when_clauses.push_back(std::move(clause));
        } while (check(TokenType::WHEN));

        if (match(TokenType::ELSE)) node->else_expr = parseExpr();
        expect(TokenType::END, "END to close CASE");
        return node;
    }

    // Both the SQL-standard form (TPC-H Q22) and the comma form (SQLite). The
    // FROM inside cannot confuse parseSelect: it is consumed between the
    // parentheses, long before the select list is complete.
    if (check(TokenType::SUBSTRING)) {
        consume();
        expect(TokenType::LPAREN, "( after SUBSTRING");
        auto node = std::make_unique<SubstringExpr>();
        node->operand = parseExpr();

        if (match(TokenType::FROM)) {
            node->start = parseExpr();
            if (match(TokenType::FOR)) node->length = parseExpr();
        } else {
            expect(TokenType::COMMA, "FROM or , in SUBSTRING");
            node->start = parseExpr();
            if (match(TokenType::COMMA)) node->length = parseExpr();
        }
        expect(TokenType::RPAREN, ") after SUBSTRING arguments");
        return node;
    }

    // DATE 'YYYY-MM-DD' is literal SYNTAX, not a type: it produces a plain
    // STRING Literal. ISO-8601 sorts lexicographically, so range comparison,
    // zone-map pruning and scanColumn<std::string> all work unchanged.
    if (check(TokenType::DATE)) {
        consume();
        Token t = expect(TokenType::STRING_LITERAL, "an ISO-8601 date string after DATE");
        if (!isIsoDate(t.value)) {
            throw ParseError("date literal must be a valid ISO-8601 'YYYY-MM-DD'", t);
        }
        return std::make_unique<Literal>(Value(t.value));
    }

    // INTERVAL 'n' <unit>. Units are matched as IDENTIFIER text rather than
    // reserved keywords, so `year` stays available as a column name.
    if (check(TokenType::INTERVAL)) {
        consume();
        Token n = expect(TokenType::STRING_LITERAL, "a quoted count after INTERVAL");
        Token u = expect(TokenType::IDENTIFIER, "an interval unit (day, month, year)");

        auto node = std::make_unique<IntervalLiteral>();
        try {
            size_t consumed = 0;
            node->count = std::stoll(n.value, &consumed);
            if (consumed != n.value.size()) throw std::invalid_argument("trailing");
        } catch (const std::exception&) {
            throw ParseError("interval count must be a quoted whole number", n);
        }

        std::string unit = upperCase(u.value);
        if      (unit == "DAY"   || unit == "DAYS")   node->unit = IntervalLiteral::Unit::DAY;
        else if (unit == "MONTH" || unit == "MONTHS") node->unit = IntervalLiteral::Unit::MONTH;
        else if (unit == "YEAR"  || unit == "YEARS")  node->unit = IntervalLiteral::Unit::YEAR;
        else throw ParseError("interval unit must be day, month or year", u);
        return node;
    }

    if (check(TokenType::AVG) || check(TokenType::SUM) || check(TokenType::COUNT) || check(TokenType::MIN) || check(TokenType::MAX)) {
        Token func = consume();
        expect(TokenType::LPAREN, "(");

        auto node = std::make_unique<AggregateExpr>();
        node->function_name = func.value;

        // uppercase function name for consistency
        for (char& c : node->function_name) c = std::toupper(c);

        // Week 34 — COUNT(DISTINCT x). DISTINCT is already a TokenType (SELECT
        // DISTINCT), so nothing lexes differently. Refused for the other four
        // functions BY NAME rather than silently accepted-and-ignored: an
        // ignored DISTINCT is a wrong answer that looks like a supported query.
        if (check(TokenType::DISTINCT)) {
            consume();
            if (node->function_name != "COUNT") {
                throw ParseError("DISTINCT is supported inside COUNT only, not "
                                 + node->function_name, func);
            }
            if (check(TokenType::STAR)) {
                // COUNT(DISTINCT *) is not SQL: DISTINCT applies to a value
                // list, and `*` is not an expression here (is_star is a flag on
                // this node, not a Literal).
                throw ParseError("COUNT(DISTINCT *) is not supported; "
                                 "use COUNT(*) or COUNT(DISTINCT column)", func);
            }
            node->distinct = true;
        }

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

// [NOT] EXISTS (subquery). Self-delimiting, so it sits at the primary level
// beside CASE and needs no precedence work: `EXISTS (...) AND x = 1` folds
// through parseAndExpr unchanged.
std::unique_ptr<Expr> Parser::parseExistsSubquery(bool negated) {
    expect(TokenType::EXISTS, "EXISTS");
    expect(TokenType::LPAREN, "( after EXISTS");
    auto node = std::make_unique<SubqueryExpr>();
    node->kind = SubqueryExpr::Kind::EXISTS;
    node->negated = negated;
    node->subquery = std::make_shared<SelectStatement>(parseSelect());
    expect(TokenType::RPAREN, ") to close the EXISTS subquery");
    return node;
}

std::vector<GroupByColumn> Parser::parseColumnList(){
    auto parseOne = [&]() -> GroupByColumn {
        GroupByColumn col;
        auto expr = parseExpr();
        if (auto* cr = dynamic_cast<ColumnRef*>(expr.get())) {
            // plain (possibly qualified) column: keep the name-based fast path
            col.table_name = cr->table_name;
            col.column_name = cr->column_name;
        } else {
            col.expr = std::move(expr);   // expression key (GROUP BY season - 1)
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
// One relation in FROM or JOIN. Week 34 added the derived-table production; the
// name-and-alias rules below are Week 26's, moved here verbatim so the FROM and
// JOIN positions cannot drift.
//
//   table_ref -> IDENT [[AS] IDENT]
//              | LPAREN select_stmt RPAREN [AS] IDENT [ LPAREN ident_list RPAREN ]
//
// No lookahead ambiguity, unlike Week 30's scalar subquery in `primary`: FROM
// has no parenthesised-expression production to be confused with, so an LPAREN
// here can only open a derived table.
TableRef Parser::parseTableRef() {
    if (check(TokenType::LPAREN)) {
        consume();
        auto body = std::make_unique<SelectStatement>(parseSelect());
        expect(TokenType::RPAREN, ") to close the derived table");

        match(TokenType::AS);   // optional noise word, as in SQL

        // MANDATORY, and it is a SYNTAX fact rather than a binder one:
        // Binder::RangeEntry is keyed on the ref name, so an unaliased derived
        // entry is unreferenceable and two of them collide on the empty string.
        if (!check(TokenType::IDENTIFIER)) {
            throw ParseError("a subquery in FROM requires an alias "
                             "(FROM (SELECT ...) AS name)", current_);
        }
        std::string alias = consume().value;

        // AS d (a, b) — a positional RENAME of the derived relation's output
        // columns, not a projection. Arity is checked in the Binder, which is
        // the only place both the list and the body's schema are in hand.
        std::vector<std::string> column_aliases;
        if (match(TokenType::LPAREN)) {
            do {
                column_aliases.push_back(
                    expect(TokenType::IDENTIFIER, "column alias").value);
            } while (match(TokenType::COMMA));
            expect(TokenType::RPAREN, ") to close the column alias list");
        }
        return TableRef::derived(std::move(body), std::move(alias),
                                 std::move(column_aliases));
    }

    std::string table = expect(TokenType::IDENTIFIER, "table name").value;

    // optional table alias (e.g. FROM laps l / FROM laps AS l);
    // an explicit AS makes the alias mandatory.
    //
    // The keyword exclusion list guards the FROM position, where the next token
    // after a bare identifier can legally be a clause keyword. It is harmless in
    // the JOIN position (JOIN / ON / WHERE / ... are all their own TokenTypes,
    // so check(IDENTIFIER) is already false for them) — which is what lets one
    // helper serve both.
    std::string alias;
    if (match(TokenType::AS)) {
        alias = expect(TokenType::IDENTIFIER, "table alias after AS").value;
    } else if (check(TokenType::IDENTIFIER) &&
        !check(TokenType::JOIN) &&
        !check(TokenType::WHERE) &&
        !check(TokenType::GROUP) &&
        !check(TokenType::ORDER) &&
        !check(TokenType::LIMIT) &&
        !check(TokenType::HAVING) &&
        !check(TokenType::END_OF_FILE)) {
        alias = consume().value;
    }
    return TableRef::named(std::move(table), std::move(alias));
}
