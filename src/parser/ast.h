#pragma once
#include "common/value.h"
#include "common/column_id.h"
#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <stdexcept>

// base expression node
struct Expr {
    virtual ~Expr() = default;
    // output-column alias (SELECT expr AS name); meaningful only on
    // select-list roots, empty elsewhere — DuckDB models it the same way
    std::string alias;
};

// reference to a column
struct ColumnRef : Expr {
    std::string table_name;
    std::string column_name;
    // Relation identity assigned by the Binder: the range-table position —
    // 0 = FROM, then one per JOIN in written order (joins[i] -> i+1);
    // -1 = unresolved. Distinguishes self-join occurrences (l1 vs l2) that
    // share a canonical table_name. Evaluation resolves by (slot, name) when
    // slot >= 0, falling back to bare name otherwise.
    // Week 33. Was `int relation_slot` + `int query_level` (Weeks 16 and 30).
    // ONE field, because they were never independently meaningful: a slot is a
    // position in the range table of the scope the level blocks out, so reading
    // one without the other compares two numbering domains — an inner slot 1 and
    // an outer slot 1 are different relations. The type is what enforces it now;
    // see common/column_id.h. Unresolved by default, so every hand-built test
    // tree keeps its bare-name fallback.
    ColumnId id;
};

// literal or constant
struct Literal : Expr {
    Value value;
    // Week 31. Only meaningful when `value` is null, which the grammar cannot
    // produce (there is no NULL literal) and which constant folding refuses to
    // produce for exactly that reason. The sole source is a materialized
    // UNCORRELATED SCALAR SUBQUERY that returned zero rows, or one NULL row.
    //
    // Value has no typed null — Value::type() throws when is_null_ — and
    // inferExprType must answer for every node, so carry the type the
    // subquery's own output schema already gave us instead of inventing a
    // convention. Typing it INT unconditionally is one line shorter and wrong in
    // a reachable place: SUBSTRING((SELECT name FROM drivers WHERE id = -1),1,2)
    // would fail plan-time typing on a query whose answer is NULL.
    //
    // Defaulted and last, so every existing construction is unchanged.
    TypeId null_type = TypeId::INT;
    explicit Literal(Value v) : value(std::move(v)) {}
};

// binary express; two expressions joined by an operator
// expr (operator) expr
struct BinaryExpr : Expr {
    std::string op; // "=", "!=", "<", ">", "<=", ">=", "AND", "OR", "+", "-", "*", "/"
    std::unique_ptr<Expr> left;
    std::unique_ptr<Expr> right;
};

// unary prefix operator; only "-" for now
struct UnaryExpr : Expr {
    std::string op;   // "-"
    std::unique_ptr<Expr> operand;
};

// IS NULL / IS NOT NULL predicate
struct IsNullExpr : Expr {
    std::unique_ptr<Expr> operand;
    bool is_not_null;   // true = IS NOT NULL, false = IS NULL
};

// aggregate function call
struct AggregateExpr : Expr {
    std::string function_name;    // "AVG", "COUNT", "SUM", "MIN", "MAX"
    std::unique_ptr<Expr> argument; // nullptr when is_star = true
    bool is_star;
    // Week 34 — COUNT(DISTINCT x). LAST field, so positional brace-inits that
    // predate it stay valid: the discipline AggregateSpec::argument,
    // GroupByColumn::expr and JoinClause::type all follow.
    //
    // COUNT only. SUM/AVG(DISTINCT) are legal SQL and no TPC-H query in the
    // documented dialect needs them; MIN/MAX(DISTINCT) are no-ops that would
    // invite a reader to believe the others work. The parser refuses the other
    // four by name — see the dialect table.
    //
    // !! IT MUST REACH exprToString(). aggregateOutputName IS exprToString, and
    // extractAggregates DEDUPES SPECS BY THAT NAME, so rendering this node as
    // COUNT(x) collapses `SELECT COUNT(x), COUNT(DISTINCT x)` into ONE spec and
    // one output column that both select items then read. Five missing
    // characters, a wrong answer in a single query, no error. cloneExpr and
    // exprKey carry it for the same reason Literal::null_type is carried.
    bool distinct = false;
};

// x IN (v1, v2, ...) / x NOT IN (...) over a constant value list.
//
// The list is restricted to literals, which is what makes the whole node cheap:
// the executor hashes the set once at compile time instead of comparing k times
// per row, and — since the grammar has no NULL literal — the set can never hold
// a NULL, so SQL's three-valued IN rule collapses to "operand NULL -> NULL".
// IN (subquery) is Week 32 and lowers to a semi-join; it is a different
// production, not an extension of this list.
struct InExpr : Expr {
    std::unique_ptr<Expr> operand;
    std::vector<Value> values;   // non-empty, never NULL
    bool negated = false;        // NOT IN
};

// x LIKE 'pattern' / x NOT LIKE 'pattern'. '%' matches any sequence, '_' any
// single character. ASCII case-INSENSITIVE, matching SQLite's default so
// compare_against_sqlite.py stays the correctness oracle — a documented dialect
// choice. The pattern must be a string literal: TPC-H never computes one, and a
// constant pattern is what lets the executor analyse it once per query.
struct LikeExpr : Expr {
    std::unique_ptr<Expr> operand;
    std::string pattern;
    bool negated = false;
};

// Searched CASE: CASE WHEN c THEN r [WHEN c THEN r ...] [ELSE e] END.
// The simple form (CASE x WHEN v ...) is not supported — no TPC-H query needs
// it. A missing ELSE yields NULL, carried on the validity mask.
struct CaseExpr : Expr {
    struct WhenClause {
        std::unique_ptr<Expr> condition;   // must infer to INT (boolean-as-INT)
        std::unique_ptr<Expr> result;
    };
    std::vector<WhenClause> when_clauses;  // at least one; the parser enforces it
    std::unique_ptr<Expr> else_expr;       // nullptr when omitted
};

// SUBSTRING(x FROM start [FOR length]) / SUBSTRING(x, start [, length]).
// 1-based start, SQLite-compatible. This is the first and only scalar function:
// if a second one arrives, replace this with a FunctionExpr { name, args } plus
// a name -> kernel registry rather than adding another one-off node.
struct SubstringExpr : Expr {
    std::unique_ptr<Expr> operand;
    std::unique_ptr<Expr> start;    // 1-based
    std::unique_ptr<Expr> length;   // nullptr = to the end of the string
};

// Defined at the bottom of this file; a subquery expression holds a whole one.
struct SelectStatement;

// A nested query in an expression position (Week 30).
//
// ONE node with a kind tag, not three: the three forms differ only in whether
// there is a left-hand operand and whether the result is a value or a
// predicate. Three structs would cost three branches at each of the eighteen
// dispatch sites (development.md) and give three chances to miss one.
//
// OWNERSHIP: `subquery` is a shared_ptr because SelectStatement holds
// unique_ptr members and is therefore move-only, while cloneExpr (dispatch site
// 11) must copy any Expr. Sharing rather than deep-copying is the same choice
// GroupByColumn::expr makes, for the same reason, and it is safe because the
// bound AST is read-only after binding — which holds only because
// Binder::resolveColumnRef is IDEMPOTENT (binder.cc). Two nodes sharing one
// statement is a real state: BETWEEN's desugaring clones its left operand
// before binding, and the GROUP BY / ORDER BY alias substitution clones an
// already-bound select item.
//
// POSITION: legal in WHERE and HAVING only. Validator refuses every other
// clause and validateJoinCondition (dispatch site 18) refuses ON — no TPC-H
// query puts a subquery anywhere else, and allowing one in the select list
// would mean buildProjectSchema must type it and aggregateOutputName must name
// an output column after it. That is a restriction on POSITION, not on
// representation: all three kinds are represented.
//
// Week 34: a subquery in FROM is NOT this node. It is a TableRef (above) holding
// a whole SelectStatement, because a derived table is a RELATION of the enclosing
// block — it gets a range-table slot, its columns are in scope above it, and
// nothing about it is an expression. The two constructs share only the word
// "subquery".
struct SubqueryExpr : Expr {
    enum class Kind {
        SCALAR,   // (SELECT one_column FROM ...) — a value
        EXISTS,   // EXISTS (SELECT ...)          — a predicate
        IN        // x IN (SELECT one_column ...) — a predicate over `operand`
    };
    Kind kind = Kind::SCALAR;
    bool negated = false;                 // NOT EXISTS / NOT IN
    // IN only; nullptr for SCALAR and EXISTS. Belongs to the ENCLOSING scope,
    // never to the subquery's — the Binder binds it against the outer range
    // table and every walker must descend into it there.
    std::unique_ptr<Expr> operand;
    std::shared_ptr<SelectStatement> subquery;
    // Set by the Binder: true when some ColumnRef inside `subquery` resolved to
    // an enclosing scope. Read by collectSlots (dispatch site 8) to decide
    // whether this node can be routed by relation slot at all; Week 33
    // decorrelates on it.
    bool correlated = false;
};

// INTERVAL 'n' day|month|year.
//
// This node MUST NOT survive planning. foldConstants rewrites `date +/- interval`
// into a plain date Literal before validation, which is what keeps ChunkPruner,
// scanColumn and selectivity() on their ColumnRef-op-Literal fast paths. An
// interval that reaches inferExprType or evaluate() therefore means a malformed
// query, and both throw — the loud failure is the design.
struct IntervalLiteral : Expr {
    enum class Unit { DAY, MONTH, YEAR };
    int64_t count = 0;
    Unit unit = Unit::DAY;
};

// Week 29. Per-JOIN-CLAUSE, not per-statement: a query may mix inner and outer
// joins (A JOIN B ... LEFT JOIN C ...), and predicate pushdown and join
// enumeration both have to ask the question one join at a time. INNER is the
// default so every pre-existing brace-init and hand-built test tree keeps its
// meaning. RIGHT/FULL are out of scope: a RIGHT join is not a flag flip, since
// swapping the operands would change the merged schema's column order, which
// invariant 1 forbids.
enum class JoinType { INNER, LEFT };

// one ORDER BY entry: an expression plus sort direction
struct OrderByItem {
    std::unique_ptr<Expr> expr;
    bool desc = false;
    // Week 37. The column ordinal EXACTLY AS TYPED ("1", "-1"), or "" when this
    // item is not an ordinal. The ordinal rule (validator.cc) is a rule about
    // what the user WROTE, so it is decided by the only layer that knows — the
    // parser — and carried here rather than re-derived downstream.
    //
    // Testing Literal-ness of `expr` instead was wrong, because two rewrites
    // that run before the Validator MANUFACTURE a Literal in this position out
    // of source text that was never an ordinal: constant folding (`ORDER BY
    // 1 + 1` -> Literal(2)) and the binder's select-alias substitution
    // (`SELECT 1 AS one ... ORDER BY one` -> Literal(1)). Both legal queries
    // were refused, and the refusal quoted back "ORDER BY 2" / "ORDER BY 1" —
    // a number the user had not typed. Storing the text is what makes quoting
    // it back honest by construction.
    //
    // Only the parser sets this, so a hand-built OrderByItem is never an
    // ordinal, which is correct: nobody wrote it.
    std::string written_ordinal;
};

// one GROUP BY entry: optionally qualified, slot-stamped by the binder
// (plain value struct — keeps SelectStatement's move semantics simple)
struct GroupByColumn {
    std::string table_name;   // as typed; empty if unqualified
    std::string column_name;
    // Week 33. Was `relation_slot` + `query_level`, and the round trip between
    // this struct and ColumnRef was itself one of Week 30's five collapse bugs:
    // a GROUP BY item resolves through resolveColumnRef, which walks OUT, so an
    // outer slot was stored in an inner-scope struct with nothing to say which
    // range table it indexed. One field, carried whole. See common/column_id.h.
    ColumnId id;
    // non-null for expression grouping (GROUP BY season - 1): the group key
    // is evaluate(expr) per row instead of a column read. shared_ptr (not
    // unique_ptr) keeps the struct copyable — planner/test sites copy
    // group-by vectors, and the expr is read-only after binding, so sharing
    // is safe. Last field so positional brace-inits ({"", "grp"}) stay valid.
    std::shared_ptr<Expr> expr;
    // Week 37 — see OrderByItem::written_ordinal. Same rule, same reason:
    // `GROUP BY 1 + 1` folded to Literal(2) and was refused as "GROUP BY 2".
    // Defaulted and last, so positional brace-inits stay valid.
    std::string written_ordinal;
};

// Week 34 — a relation in FROM / JOIN. Either a catalog table name or a DERIVED
// TABLE: a whole nested query block that behaves as one relation of this block's
// range table.
//
// !! table_name IS PRIVATE, and it is the same discipline ColumnId (Week 33)
// applies to a relation slot, for the same measured reason. Encoding "derived"
// as an empty string would leave `catalog.getTable(stmt.from_table)` compiling
// everywhere it does today and reporting `Table not found: ''` on a query with
// no table-name error in it. Reading the name costs a NAMED call that states the
// caller believes this is a catalog table, and the throw is what stops that
// belief being wrong silently. That is the Week 26 move as well: std::optional
// was the type-level encoding of "at most one join", and making it a vector
// turned 14 silent sites into compile errors.
//
// OWNERSHIP is unique_ptr, NOT the shared_ptr SubqueryExpr uses, and the
// asymmetry is deliberate: SubqueryExpr shares because cloneExpr (dispatch site
// 11) must copy any Expr and a deep statement copy has silent omissions. A
// TableRef is not an Expr and nothing clones it, so sharing would import Week
// 33's use_count() > 1 problem for no benefit.
struct SelectStatement;
class TableRef {
    public:
        TableRef() = default;

        static TableRef named(std::string table, std::string alias = "") {
            TableRef r;
            r.table_name_ = std::move(table);
            r.alias_ = std::move(alias);
            return r;
        }
        // The alias is REQUIRED for a derived table and the parser enforces it:
        // Binder::RangeEntry is keyed on the ref name, so an unnamed derived
        // entry is unreferenceable and two of them collide on the empty string.
        static TableRef derived(std::unique_ptr<SelectStatement> body,
                                std::string alias,
                                std::vector<std::string> column_aliases = {});

        bool isDerived() const { return subquery_ != nullptr; }

        // THE NARROWING POINT. Call this — there is no other way to obtain the
        // string — anywhere the value is about to be used as a catalog table
        // name. `site` names the caller so a planner defect points at itself.
        const std::string& tableName(const char* site) const {
            if (subquery_)
                throw std::runtime_error(
                    std::string("internal: ") + site + " read a derived table "
                    "reference ('" + alias_ + "') as a catalog table name");
            return table_name_;
        }

        // The name a qualified reference uses, and the Binder's RangeEntry key:
        // the alias when there is one, the table name otherwise. Hoisted here
        // because binder.cc and validator.cc each computed it inline, and the
        // two must not drift (Week 26's `relations` keying bug was exactly that).
        const std::string& refName() const {
            return alias_.empty() ? table_name_ : alias_;
        }
        const std::string& alias() const { return alias_; }

        // Non-const access for the Binder and the logical planner, which bind
        // and then MOVE the body. Nothing else should need it.
        SelectStatement* body() const { return subquery_.get(); }
        std::unique_ptr<SelectStatement> takeBody() { return std::move(subquery_); }

        // AS d (a, b) — a positional RENAME of the derived relation's output
        // columns, not a projection. Empty when absent; a named table never
        // carries one, because the grammar does not offer it there.
        const std::vector<std::string>& columnAliases() const { return column_aliases_; }

    private:
        std::string table_name_;                      // empty when derived
        std::string alias_;
        std::vector<std::string> column_aliases_;
        std::unique_ptr<SelectStatement> subquery_;   // non-null == derived
};

// the full parsed query
struct SelectStatement {
    bool distinct = false;
    bool select_star = false;  // true when SELECT *

    std::vector<std::unique_ptr<Expr>> select_list;

    // Week 34: was `std::string from_table` + `std::string from_alias`. A
    // relation is no longer always a NAME.
    TableRef from;

    // optional JOINs
    struct JoinClause {
        // Week 34: was `std::string join_table` + `std::string alias`.
        TableRef relation;
        std::unique_ptr<Expr> condition; // the ON expression
        // Week 29. LAST field, so positional brace-inits that predate it stay
        // valid — the same discipline AggregateSpec::argument and
        // GroupByColumn::expr follow.
        JoinType type = JoinType::INNER;
    };
    // Written order is load-bearing: joins[i] attaches relation slot i+1 to the
    // left-deep tree built from relations 0..i. That single identity is what the
    // Binder's range table, the merged join schema, join-key routing and
    // predicate pushdown all derive their slot arithmetic from. Empty = no join.
    // (Was std::optional<JoinClause> through Phase 4 — one join only.)
    std::vector<JoinClause> joins;

    // optional WHERE
    std::unique_ptr<Expr> where; // nullptr if no WHERE

    // optional GROUP BY
    std::vector<GroupByColumn> group_by;

    // optional HAVING
    std::unique_ptr<Expr> having; // nullptr if no HAVING

    // optional ORDER BY
    std::vector<OrderByItem> order_by;

    // optional LIMIT
    std::optional<int> limit;

    // Week 30. Set by the Binder when it binds a SubqueryExpr directly inside
    // THIS statement. It existed so the "not yet executable" refusal (deleted in
    // Week 33) and buildScanSchema's conservative widening need no nineteenth
    // walker over the statement to find out; the widening is now its only
    // consumer of that kind. Any statement containing a subquery at any
    // depth contains one DIRECTLY, so the top-level flag is always the right
    // test for "this query uses a subquery".
    //
    // Week 31: it also means "a SubqueryExpr is STILL in this tree".
    // materializeSubqueries clears it once every node has been replaced by a
    // constant, which is what gives a subquery query its projection pushdown
    // back (buildScanSchema widens to the full schema while the flag is set).
    bool has_subquery = false;

    // Week 31. Set by the Binder when THIS statement contains a correlated
    // SubqueryExpr *or* when one of its subqueries does — propagated UPWARD,
    // unlike has_subquery. Correlation is RELATIVE to a block, so a node
    // correlated to a MIDDLE block leaves the top block's node uncorrelated
    // (Q20's two-deep shape). Without the propagation the top-level refusal
    // accepts such a query and a nested block refuses it later, after the outer
    // levels have already been materialized and run.
    //
    // !! WHAT THIS FLAG NO LONGER IS. It was the condition of the last refusal
    // at the end of Validator::validate, and therefore the containment
    // development.md's slot-consumer table rested on. Week 33 DELETED that
    // refusal: a ColumnRef with query_level > 0 now reaches plan nodes, and the
    // containment is the TYPE (common/column_id.h) — a level cannot be dropped
    // silently because a bare int is not assignable where a ColumnId is
    // required. The flag survives as what the Binder computes and what
    // subquery_materialization/decorrelation route on; it guards nothing by
    // itself. Three wrong answers this week came from code that kept trusting
    // the refusal after it was gone, so do not restate it as a guarantee.
    bool has_correlated_subquery = false;

    // Week 34. Set by the Binder when THIS block's FROM or JOIN list holds a
    // derived table. DELIBERATELY NOT has_subquery: that flag means "a
    // SubqueryExpr is still in this tree" and drives buildScanSchema's
    // conservative widening and Planner::plan's refusal scan, so reusing it
    // would silently turn projection pushdown off for every derived-table query
    // and give the wrong Volcano refusal message. Not propagated upward: a
    // derived table is a relation of the block it appears in, and every consumer
    // asks about its own block.
    bool has_derived_table = false;
};

inline TableRef TableRef::derived(std::unique_ptr<SelectStatement> body,
                                  std::string alias,
                                  std::vector<std::string> column_aliases) {
    TableRef r;
    r.alias_ = std::move(alias);
    r.column_aliases_ = std::move(column_aliases);
    r.subquery_ = std::move(body);
    return r;
}