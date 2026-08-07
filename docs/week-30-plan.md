# Week 30 — Subquery Parsing + Binding

> Teaching plan. Nothing here is written into `src/`; every snippet is
> illustrative and matches the naming, headers and conventions already in the
> tree. Read the whole "Design" half before writing a line. The hard part of
> this week is not the parser — it is that **every `Expr` walker in this
> codebase was written for a single flat scope**, and a subquery is the first
> construct that makes `relation_slot` ambiguous. Slot 1 inside a subquery and
> slot 1 outside it are different relations, and nothing in the tree currently
> carries the distinction.

README bullets:

- Add nested query AST nodes and scoped name resolution
- Represent scalar, set-returning, and correlated subqueries

**Checkpoint:** Required TPC-H subquery forms bind correctly.

---

## The inventory this checkpoint is measured against

"Required TPC-H subquery forms" is not open-ended. Read off the queries:

| Form | TPC-H | Shape |
|---|---|---|
| Scalar, uncorrelated | Q11, Q22 | `having sum(...) > (select sum(...) * 0.0001 from partsupp ...)` |
| Scalar, correlated | Q2, Q17 | `where ps_supplycost = (select min(ps_supplycost) from ... where p_partkey = ps_partkey)` |
| `IN (subquery)`, uncorrelated | Q18, Q20 | `where o_orderkey in (select l_orderkey from lineitem group by l_orderkey having sum(l_quantity) > 300)` |
| `NOT IN (subquery)`, uncorrelated | Q16 | `where ps_suppkey not in (select s_suppkey from supplier where s_comment like ...)` |
| `EXISTS`, correlated | Q4, Q20, Q21, Q22 | `where exists (select * from lineitem where l_orderkey = o_orderkey and ...)` |
| `NOT EXISTS`, correlated | Q21 | `and not exists (select * from lineitem l3 where ...)` |
| Nested subquery inside a subquery | Q20 | `in (select ps_partkey from partsupp where ps_availqty > (select 0.5 * sum(l_quantity) ...))` |

Three facts fall straight out of that table and they define the week:

1. **Every one of them sits in `WHERE` or `HAVING`.** Not one TPC-H subquery
   appears in a `SELECT` list, a `GROUP BY`, an `ORDER BY` or a `JOIN ... ON`.
2. **`FROM (subquery)` is absent from the table** because Q15's derived table is
   Week 34's, and Q13's is too.
3. **`ALL` / `ANY` / `SOME` never appear.** Do not add them.

---

## What Week 30 must deliver

- `EXISTS` in the lexer; three new productions in the parser (scalar `(SELECT …)`,
  `[NOT] EXISTS (SELECT …)`, `x [NOT] IN (SELECT …)`).
- One new `Expr` subtype, `SubqueryExpr`, carrying a nested `SelectStatement`.
- A **scope chain** in the Binder: per-scope range tables, innermost-first
  resolution, and a `query_level` on `ColumnRef` that says which scope a slot is
  a position in.
- Correlation detected at bind time and recorded on the node.
- Validation: the nested query validated in its own scope; scalar/`IN` arity;
  positions restricted to `WHERE`/`HAVING`; a refusal in `ON`.
- All eighteen dispatch sites visited (`development.md` → *Extending the
  expression language*).
- A single refusal — `subqueries are parsed and bound but not yet executable
  (Week 31)` — placed after all validation, so a genuine query defect still
  outranks the temporary engine limitation.
- Four Starting-note items closed (see the table below).

## What not to build

| Not this week | Whose it is | Why it is not a judgement call |
|---|---|---|
| Executing anything | Week 31 | The checkpoint word is "bind" |
| Lowering `IN`/`EXISTS` to semi-/anti-joins | Week 32 | A different production and a different operator |
| Decorrelation | Week 33 | Needs the semi-join it lowers to |
| `FROM (subquery)` / derived tables | Week 34 | Changes the range table's *entries*, not just its nesting |
| Runtime "scalar subquery returned more than one row" | Week 31 | README bullet, verbatim |
| A cardinality rule for a subquery | Week 32 | Week 29's note: a semi/anti-join needs its own rule, and a non-multiplicative rule lives at the stamping site |
| `ALL` / `ANY` / `SOME` | nobody | No TPC-H query in the documented dialect needs them |

---

## Prerequisite knowledge

Flagged because a gap in any of these turns this week into guesswork:

- **`development.md` → Extending the expression language.** Eighteen dispatch
  sites, ten of them silent. You are adding the tenth `Expr` subtype since
  Week 25 and it is the first one that is not a leaf-ish scalar node.
- **The binder range table** (`binder.cc`): `relation_slot` is a *position*, not
  an identity. `joins[i]` → slot `i+1`. Read `Binder::resolveColumnRef` in full.
- **`JoinKey::from_slot`'s contract** (`join_condition.h`) and **the two
  numbering domains** Week 28 introduced (a leaf's own schema stamps 0; a merged
  join schema stamps binder slots). A third domain — "slot in *which* scope" —
  is what this week adds.
- **Why `GroupByColumn::expr` is a `shared_ptr`** (`ast.h`): "keeps the struct
  copyable". That precedent decides this week's ownership question.
- **SQL scoping.** A name resolves against the innermost query block that has
  it; an outer match is a *correlated* reference. Inner shadows outer, and that
  is not an ambiguity error.

---

## Design

### D1 — Two numbering domains, and why one `int` can no longer say which

`ColumnRef::relation_slot` currently means "position in *the* range table",
because there has only ever been one. A subquery creates a second range table.
Consider

```sql
SELECT d.name FROM drivers d
WHERE EXISTS (SELECT 1 FROM laps l WHERE l.driver_id = d.driver_id)
```

Inside the subquery, `laps l` is slot 0 of the *inner* range table. Outside,
`drivers d` is slot 0 of the *outer* one. Two different relations, one number.
Every consumer of `relation_slot` — `collectSlots`, `restampSlots`, `soleSlot`,
`classifyJoinCondition`, `exprKey`, `inferExprType`'s slot-first lookup,
`Schema::indexOf(name, slot)`, `ChunkPruner`'s `relation_slot < 1` test — reads
that number as a position in the range table of the query it is planning. Hand
it an inner-scope slot and it silently addresses the wrong relation.

**The fix is a second field, not a renumbering.** Add `query_level` to
`ColumnRef`: how many scopes *out* from the query the ref is written in the
relation lives. `0` = this query's own range table (which is what every ref in
the tree means today, so the default keeps every existing construction and every
hand-built test valid). `1` = the immediately enclosing query. This is
Postgres's `varlevelsup`, and it is *relative* rather than absolute on purpose:
a subquery's expression tree then means the same thing wherever the subquery
sits, which is what Weeks 33 and 34 need when they start moving subqueries
around.

The rule the rest of the engine inherits, and it must be written down at the
field:

> `relation_slot` is a position in the range table of the scope `query_level`
> steps out. Reading a slot without checking the level compares two different
> numbering domains.

Global (query-wide) slot numbering is the tempting alternative — one counter
across all scopes, so slots stay unique and nothing else changes. It is wrong
here for a concrete reason: `ChunkPruner` reads `relation_slot < 1` as
"scan-local", a leaf's own schema stamps 0, and `restampSlots` re-stamps pushed
conjuncts to 0. All three are per-scope facts. Global numbering would put an
inner query's leading relation at slot 3 and silently disable chunk pruning for
every subquery in the engine — no error, just slower, which is the failure mode
this codebase has been burned by most often.

### D2 — Ownership: `SelectStatement` is move-only, and `cloneExpr` is site 11

`SelectStatement` holds `std::unique_ptr<Expr>` members, so it is movable and
**not copyable**. `cloneExpr` (dispatch site 11) must produce a deep copy of any
`Expr`, and it throws on an unknown subtype. So the new node forces a choice:

| Option | Cost |
|---|---|
| `std::unique_ptr<SelectStatement>` + a new `cloneStatement()` | A nineteenth dispatch site, over statement *clauses* rather than expression subtypes, whose omissions are silent (a dropped `HAVING` in a cloned subquery) |
| `std::shared_ptr<SelectStatement>`, shared on clone | Two `SubqueryExpr` nodes can point at one statement |

Take the `shared_ptr`. It is the precedent already in `ast.h` —
`GroupByColumn::expr` is a `shared_ptr` for exactly this reason ("keeps the
struct copyable") — and the bound AST is read-only after binding, so sharing is
safe **on one precondition: binding must be idempotent**. That precondition is
not currently met (Task 3 is where you meet it), and it is the reason Task 3 is
not an optional cleanup this week.

Three existing call sites clone expressions that could contain a subquery:
`BETWEEN`'s desugaring (parser, before binding), `GROUP BY <alias>` /
`ORDER BY <alias>` substitution (binder), and the residual `ON` clone in both
planners. The first is pre-bind, so it shares an unbound statement and the
Binder then walks it twice; the second re-binds an already-bound tree. Both need
idempotent binding.

### D3 — Where a subquery may appear, and why the restriction is not laziness

Restrict `SubqueryExpr` to `WHERE` and `HAVING`. Refuse it in `SELECT`,
`GROUP BY`, `ORDER BY` and `JOIN ... ON`, each with its own message.

This is not "we did not get to it". Allowing a subquery in the select list means
`buildProjectSchema` must type it, `aggregateOutputName` must name a column after
it, `exprToString` must render it into that name, `checkGroupedRefs` must decide
whether a correlated ref inside it is grouped, and `substituteInto` must decide
whether to rewrite inside it. That is five schema-visible decisions bought for
zero TPC-H queries. The dialect's own rule for this is written in the README
already — decline rather than guess, one clean error per shape.

The restriction is on *position*, not on representation. All three kinds
(scalar, `EXISTS`, `IN`) are represented, which is what the README bullet asks
for. Week 34 relaxes the position rule for `FROM`.

### D4 — Why the refusal goes last, and why it is not split per engine

Week 26 split its two refusals across the engines: multi-key was deferred past
`Planner::plan`'s type checks so a real defect outranked it, while multi-way had
to refuse immediately on Volcano because a deferred check would have reported a
misleading `column not found`. That split existed because the two engines
genuinely differed in capability.

They do not differ here. Neither path can lower a subquery this week, so there
is no capability difference to preserve and no reason for two failure points.
Put the refusal at the very end of `Validator::validate`, which both
`Planner::plan` and `LogicalPlanBuilder::build` call first. One message, four
modes, and every check in `Validator` — column existence inside the subquery,
aggregate typing, arity, position — fires before it.

The cost, stated honestly: a query with *both* a subquery and an
arithmetic-over-`STRING` fault in its projection reports the subquery refusal,
because `buildProjectSchema`'s type check runs later than `Validator`. That is a
narrower masked class than it sounds — `Validator` already covers select-list
column existence and aggregate argument typing — and it is the price of one
message instead of two.

---

## The Starting notes, and where each is answered

| Note | Source | Task |
|---|---|---|
| `collectSlots` (site 8) is permissive in `classifyJoinCondition`; `predicate_pushdown.h`'s justification is false and its caller count is stale; handle the subquery node at site 8 in the same commit | Week 29's third audit → Week 30 | **Task 5** |
| `Planner::plan`'s hard-coded `preserved_slots{0}` is correct only via the `joins.size() > 1` refusal — an undocumented coupling | Week 29's third audit → Week 30 | **Task 7** |
| `ORDER BY <alias>` over an unqualified select-list column is refused in any aliased query; this is the next week that owns binder scope resolution | Week 28 audit → Week 30 | **Task 3** |
| `JoinEnumeration`'s unbound-key throw becomes live when subquery scans appear; convert it to a decline, in the shape `containsOuterJoin` uses | Week 28's foundations *and* Week 29's foundations → Week 30 | **Task 8** |

Week 29's other hand-forward notes (forced outer build side, blunt reorder
decline, the residual's scalar evaluation, the outer-join estimate, the NULL
oracle) are addressed to Weeks 32/37 and are not this week's.

---

## Task 1 — `EXISTS`, `SubqueryExpr`, and the three parser productions

### Why it matters

This is the node every other task dispatches on. Its shape decides how much
work the remaining seventeen sites are: one node with a `kind` tag is one branch
per site, three separate nodes are three. Its ownership decides whether
`cloneExpr` can work at all (D2). And the parser productions decide whether the
grammar stays unambiguous — the scalar form is `LPAREN SELECT`, which is one
token of lookahead away from the parenthesized-expression rule that has existed
since Week 4.

Downstream: the Binder (Task 2) walks it, the Validator (Task 4) refuses it by
position, and Weeks 31–33 lower it. Nothing here is throwaway.

### Conceptual explanation

**One node, three kinds.** The three forms differ in exactly two ways: whether
there is a left-hand operand (`IN` has one; scalar and `EXISTS` do not) and
whether the result is a value or a predicate. That is a `kind` enum and an
optional `operand` pointer, not three structs. Three structs would cost 3 × 18
dispatch branches and give three chances to miss one.

**Where each production sits in the grammar.** The existing grammar is in
README.md; place the new rules by asking what terminates them.

- **Scalar `(SELECT …)`** is self-delimiting (parentheses), so it belongs at
  `primary`, beside `CASE` and `SUBSTRING`. It composes with arithmetic for free
  — Q17's `< 0.2 * (select avg(...) ...)` needs exactly that.
- **`[NOT] EXISTS (SELECT …)`** is also self-delimiting and yields a predicate.
  `primary` again, for the same reason `CASE` is there: no precedence work.
- **`x [NOT] IN (SELECT …)`** is an extension of `parseCompare`'s existing `IN`
  branch by one token of lookahead after the `(`. It is *not* an extension of
  `InExpr::values` — the README's non-support table says so in as many words,
  and the reason is that `InExpr`'s whole design is a set hashed once at compile
  time, which a subquery cannot be.

**The `NOT` trap.** `parseCompare` rejects any `NOT` not followed by `BETWEEN`,
`LIKE` or `IN`, and `parsePrimary` throws `kNotSupportMessage` on a *leading*
`NOT`. `WHERE NOT EXISTS (...)` is a leading `NOT`, so it reaches `parsePrimary`
and dies there unless you add the lookahead. Both places must learn about
`EXISTS`, and `parsePrimary`'s must come *before* the `kNotSupportMessage`
throw.

**Adding a keyword costs a column name.** The project's rule, applied in Weeks
25 and 29: verify against `catalog.json` and the TPC-H schema before reserving.
`EXISTS` collides with neither (`laps`/`drivers` columns are listed in the
README's Data Domain table; every TPC-H column is prefixed). Say so in the
comment, as the Week 25 and Week 29 blocks in `lexer.cc` do. `SELECT` and `IN`
are already tokens, so `EXISTS` is the only addition.

### Code

```cpp
// src/parser/token.h — beside the Week 25 and Week 29 blocks
    // Week 30 subqueries. EXISTS is the only new keyword: SELECT and IN are
    // already tokens, and the scalar form is punctuation. Reserved rather than
    // matched as identifier text — verified against catalog.json and the TPC-H
    // schema that no column is called `exists`. ALL/ANY/SOME are deliberately
    // absent: no TPC-H query in the documented dialect uses them.
    EXISTS,
```

```cpp
// src/parser/ast.h — after SubstringExpr, before IntervalLiteral

// A nested query in an expression position (Week 30).
//
// ONE node with a kind tag, not three: the three forms differ only in whether
// there is a left-hand operand and whether the result is a value or a
// predicate. Three structs would cost three branches at each of the eighteen
// dispatch sites and give three chances to miss one.
//
// OWNERSHIP: `subquery` is a shared_ptr because SelectStatement holds
// unique_ptr members and is therefore move-only, while cloneExpr (dispatch site
// 11) must copy any Expr. Sharing rather than deep-copying is the same choice
// GroupByColumn::expr makes, for the same reason, and it is safe because the
// bound AST is read-only after binding — which holds only because
// Binder::resolveColumnRef is idempotent (see binder.cc). Two nodes sharing one
// statement is a real state: BETWEEN's desugaring and the GROUP BY / ORDER BY
// alias substitution both clone subtrees that may contain one.
//
// POSITION: legal in WHERE and HAVING only. Validator refuses every other
// clause, and validateJoinCondition (site 18) refuses ON. That is a restriction
// on position, not on representation — all three kinds are represented, which
// is what "scalar, set-returning and correlated" asks for. FROM is Week 34.
struct SubqueryExpr : Expr {
    enum class Kind {
        SCALAR,   // (SELECT one_column FROM ...) — a value
        EXISTS,   // EXISTS (SELECT ... )         — a predicate
        IN        // x IN (SELECT one_column ...) — a predicate over `operand`
    };
    Kind kind = Kind::SCALAR;
    bool negated = false;                 // NOT EXISTS / NOT IN
    // IN only; nullptr for SCALAR and EXISTS. Belongs to the ENCLOSING scope,
    // never to the subquery's — the Binder binds it against the outer range
    // table and every walker must descend into it there.
    std::unique_ptr<Expr> operand;
    std::shared_ptr<SelectStatement> subquery;
    // Set by the Binder: true when any ColumnRef inside `subquery` resolved to
    // an enclosing scope (query_level > 0 relative to that subquery). Read by
    // collectSlots to decide whether this node can be routed by relation slot
    // at all; Week 33 decorrelates on it.
    bool correlated = false;
};
```

```cpp
// src/parser/ast.h — ColumnRef gains one field
struct ColumnRef : Expr {
    std::string table_name;
    std::string column_name;
    int relation_slot = -1;
    // Week 30. How many query blocks OUT from the block this ref is written in
    // the relation lives: 0 = this query's own range table (the default, so
    // every pre-existing ref and every hand-built test tree keeps its meaning),
    // 1 = the immediately enclosing query, and so on. RELATIVE, like Postgres's
    // varlevelsup, so a subquery's tree means the same thing wherever it sits.
    //
    // !! relation_slot is a position in the range table of the scope this many
    // steps out. Reading a slot without its level compares two different
    // numbering domains — an inner slot 1 and an outer slot 1 are different
    // relations. Every walker that routes by slot must test the level first.
    int query_level = 0;
};
```

```cpp
// src/parser/ast.h — SelectStatement gains one flag
struct SelectStatement {
    ...
    // Week 30. Set by the Binder when it binds a SubqueryExpr directly inside
    // THIS statement. Exists so the "not yet executable" refusal and
    // buildScanSchema's conservative widening need no nineteenth walker over
    // the statement to find out.
    bool has_subquery = false;
};
```

```cpp
// src/parser/parser.cc — parsePrimary(), BEFORE the leading-NOT throw

    // NOT EXISTS. parseCompare's NOT lookahead only fires after a complete left
    // operand, so a LEADING NOT — which is how EXISTS is always written —
    // arrives here and would hit kNotSupportMessage below. Same one-token
    // lookahead idiom parseCompare uses: current_ is NOT, lexer_.peek() is the
    // token after it.
    if (check(TokenType::NOT) && lexer_.peek().type == TokenType::EXISTS) {
        consume();                        // NOT
        return parseExistsSubquery(/*negated=*/true);
    }
    if (check(TokenType::EXISTS)) {
        return parseExistsSubquery(/*negated=*/false);
    }

    if (check(TokenType::NOT)) {
        throw ParseError(kNotSupportMessage, current_);
    }

    // Parenthesized expression, or a SCALAR subquery. One token of lookahead
    // separates them, and SELECT can begin no expression, so the grammar stays
    // unambiguous. This is why the scalar form lives at the primary level:
    // (SELECT ...) is self-delimiting, so it composes with arithmetic for free —
    // TPC-H Q17's `< 0.2 * (select avg(...) ...)` needs exactly that.
    if (match(TokenType::LPAREN)) {
        if (check(TokenType::SELECT)) {
            auto node = std::make_unique<SubqueryExpr>();
            node->kind = SubqueryExpr::Kind::SCALAR;
            // parseSelect(), never parse(): parse() requires end-of-input, and
            // a subquery ends at its own ')'.
            node->subquery = std::make_shared<SelectStatement>(parseSelect());
            expect(TokenType::RPAREN, ") to close the subquery");
            return node;
        }
        auto expr = parseExpr();
        expect(TokenType::RPAREN, ")");
        return expr;
    }
```

```cpp
// src/parser/parser.cc — a small helper beside parsePrimary
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
```

```cpp
// src/parser/parser.cc — parseCompare(), inside the existing IN branch
    if (check(TokenType::IN)) {
        consume();
        expect(TokenType::LPAREN, "( after IN");

        // IN (subquery) is a DIFFERENT production, not a longer constant list.
        // InExpr's whole design is a set hashed once at compile time (ast.h);
        // a subquery cannot be that, and Week 32 lowers this shape to a
        // semi-/anti-join instead. One token of lookahead keeps them apart.
        if (check(TokenType::SELECT)) {
            auto node = std::make_unique<SubqueryExpr>();
            node->kind = SubqueryExpr::Kind::IN;
            node->negated = negated;
            node->operand = std::move(left);
            node->subquery = std::make_shared<SelectStatement>(parseSelect());
            expect(TokenType::RPAREN, ") to close the IN subquery");
            return node;
        }

        auto node = std::make_unique<InExpr>();
        ...   // unchanged constant-list path
    }
```

### Implementation guidance

1. Token, keyword-map entry, `parseExistsSubquery` declaration in `parser.h`.
   Build once before touching anything else: adding a `TokenType` is the kind of
   change that surfaces a missing `switch` case immediately if one exists.
2. **Do sites 11, 12 and 13 next, before any other dispatch site.** That is
   `development.md`'s recommended order and the reason is exact: `cloneExpr`,
   `inferExprType` and `evaluate` all **throw** on an unknown subtype, so
   handling them first turns every later omission into a loud failure instead of
   a quiet one. Their content is in Task 5's table.
3. Gotchas specific to this parser:
   - `parseSelect()` is re-entrant (it only touches `lexer_` and `current_`), so
     nesting works with no extra machinery. Call `parseSelect()`, **never**
     `parse()` — the latter demands `END_OF_FILE` and a subquery ends at `)`.
   - The `FROM` bare-alias branch in `parseSelect` excludes clause keywords by an
     explicit list. `RPAREN` is not an `IDENTIFIER`, so `FROM lineitem)` is
     already safe and the list needs no new entry. Verify it rather than assume.
   - `EXISTS (SELECT * FROM ...)` must parse: Q4 and Q21 both write `select *`.
     `select_star` on the nested statement is fine and Task 4's arity check must
     not trip on it.
   - `EXISTS` at the `primary` level means `EXISTS (...) + 1` parses. It is
     nonsense, it is refused later, and rejecting it in the grammar would cost a
     precedence level for no query's benefit. Leave it; note it.
4. Do **not** touch `InExpr`. `IN (speed)`, `IN (1 + 1)` and `IN ()` must keep
   their current errors — `tests/test_parser.cc`'s `InListRejectsNonConstants`
   pins them.

### Verification

```bash
cmake --build build -j && cd build && ./tests/swiftql_tests --gtest_filter='ParserTest.*'; cd ..

# all three forms parse (they will fail LATER, at the Week 31 refusal — that is
# the success condition for this task, not a failure)
./build/swiftql --catalog catalog.json --query \
  "SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)"
./build/swiftql --catalog catalog.json --query \
  "SELECT name FROM drivers d WHERE EXISTS (SELECT * FROM laps l WHERE l.driver_id = d.driver_id)"
./build/swiftql --catalog catalog.json --query \
  "SELECT name FROM drivers WHERE driver_id NOT IN (SELECT driver_id FROM laps)"
# each must report:  subqueries are parsed and bound but not yet executable (Week 31)
# NOT:               Parse error ... Expected an expression (got 'SELECT')

# the constant-list path is untouched
./build/swiftql --catalog catalog.json --query \
  "SELECT COUNT(*) FROM laps WHERE season IN (2024, 2025)"
```

Add parser unit tests that assert on the **node**, not just on "it parsed":
kind, `negated`, that `operand` is set for `IN` and null for the other two, and
that the nested statement's `from_table` is right. A test that only asserts
absence of a throw passes for the wrong reason.

---

## Task 2 — Scoped name resolution in the Binder

### Why it matters

This is the week's title. `Binder::bind` builds one `std::vector<RangeEntry>`
and hands it to every `resolveColumnRef` call; that vector *is* the assumption
that there is one scope. Everything downstream that routes by relation slot —
join-key resolution, predicate pushdown, chunk pruning, cardinality estimation —
trusts the Binder's stamps. Get the scope chain wrong and a correlated reference
resolves against the wrong table with no error at all, which is the failure mode
this codebase has spent Weeks 26–29 eliminating one site at a time.

Downstream this week: Task 4 (validation skips outer refs), Task 5
(`collectSlots` reads `correlated`), Task 6 (`has_subquery` is set here).
Downstream later: Week 33 decorrelates on exactly the flag this task sets.

### Conceptual explanation

**A scope is a range table plus a parent.** Resolution walks out: try the
innermost range table; if the name is not there, try its parent; and so on. A
name that resolves at step `k` gets `query_level = k` and `relation_slot` =
its position *in that scope's* range table. `k > 0` means the reference is
correlated.

**Inner shadows outer, and that is not ambiguity.** The existing binder throws
`ambiguous column reference` when a bare name matches more than one relation.
That check stays **per scope**. A name matching one relation in the inner scope
and one in the outer is not ambiguous — SQL says the inner one wins. Applying
the existing check across the whole chain would reject Q17.

**Preserve every existing message.** The current fallbacks are load-bearing:

- A single-relation scope resolves an unqualified name to slot 0 *without*
  checking existence, deliberately, because "existence stays Validator's job"
  and Validator's message is the one users see.
- A qualified ref whose qualifier matches nothing throws
  `unknown table qualifier`.

Both must survive. The rule that keeps them intact is: **walk out looking for a
match; if no scope in the chain matches, apply the existing fallback in the
innermost scope, unchanged.** That way a top-level query with no parent behaves
byte-identically to today, which is the property the 638-test suite is actually
measuring.

**Correlation is a property of scopes, not of one ref.** A ref resolving two
levels out makes both the scope it is in *and* the scope between correlated —
neither can be evaluated independently of the outermost one. Mark every scope
from the current one up to (but not including) the resolving one.

**The IN operand belongs to the outer scope.** `x IN (SELECT ...)` — `x` is
written in the enclosing query. Bind it with the enclosing scope, before
descending. Getting this backwards resolves `x` against the subquery's
relations, which is a wrong answer with no error.

### Code

```cpp
// src/planner/binder.h
class Binder {
    public:
        static void bind(SelectStatement& stmt, const Catalog& catalog);

    private:
        struct RangeEntry {
            std::string ref_name;    // alias if present, else the table name
            std::string table_name;  // canonical catalog table name
            const Schema* schema;
        };

        // Week 30. One Scope per query block. `parent` is the LEXICALLY
        // enclosing block, nullptr at the top level, so resolution walks OUT
        // from the innermost scope exactly as SQL scoping requires.
        //
        // Slots are per-scope: (query_level, relation_slot) is the identity,
        // and a slot read without its level compares two numbering domains.
        struct Scope {
            std::vector<RangeEntry> range_table;
            Scope* parent = nullptr;
            SelectStatement* stmt = nullptr;  // to set has_subquery
            bool correlated = false;          // a ref here resolved further out
        };

        // Returns true when this block turned out to be correlated.
        static bool bindQuery(SelectStatement& stmt, const Catalog& catalog, Scope* parent);
        static void bindExpr(Expr* expr, Scope& scope, const Catalog& catalog);
        static void resolveColumnRef(ColumnRef* col, Scope& scope);
};
```

```cpp
// src/planner/binder.cc

void Binder::bind(SelectStatement& stmt, const Catalog& catalog) {
    bindQuery(stmt, catalog, /*parent=*/nullptr);
}

bool Binder::bindQuery(SelectStatement& stmt, const Catalog& catalog, Scope* parent) {
    // table existence is Validator's error to raise (preserves its message)
    if (!catalog.hasTable(stmt.from_table)) return false;
    for (const auto& j : stmt.joins) {
        if (!catalog.hasTable(j.join_table)) return false;
    }

    Scope scope;
    scope.parent = parent;
    scope.stmt = &stmt;

    // ... the range-table construction and the duplicate-ref-name diagnostics
    // are UNCHANGED, writing into scope.range_table instead of a local vector.
    // The duplicate check stays per-scope: an inner alias may legally repeat an
    // outer one, and shadowing it is what SQL scoping means.

    // ... every clause bound exactly as before, against `scope` ...

    foldConstants(stmt);   // still last, per scope
    return scope.correlated;
}

void Binder::resolveColumnRef(ColumnRef* col, Scope& scope) {
    // Week 30. Binding is IDEMPOTENT — see Task 3. An already-stamped ref is
    // returned untouched, which is what makes it safe to walk a shared subquery
    // twice and to re-bind an alias-substituted clone.
    if (col->relation_slot >= 0) return;

    // Innermost first, then out. `level` is what lands in query_level.
    int level = 0;
    for (Scope* s = &scope; s; s = s->parent, ++level) {
        if (col->table_name.empty()) {
            // Unqualified: count matches WITHIN this scope. Ambiguity is a
            // per-scope question — a name matching one relation here and one
            // in an enclosing scope is not ambiguous, the inner one wins.
            int matches = 0, slot = -1;
            std::string table;
            for (int i = 0; i < static_cast<int>(s->range_table.size()); ++i) {
                if (!s->range_table[i].schema->hasColumn(col->column_name)) continue;
                ++matches; slot = i; table = s->range_table[i].table_name;
            }
            if (matches > 1) {
                throw std::runtime_error(
                    "ambiguous column reference: '" + col->column_name + "'");
            }
            if (matches == 1) {
                col->table_name = table;   // fully qualify, as before
                col->relation_slot = slot;
                col->query_level = level;
                markCorrelated(scope, level);
                return;
            }
        } else {
            for (int i = 0; i < static_cast<int>(s->range_table.size()); ++i) {
                const RangeEntry& rte = s->range_table[i];
                if (rte.ref_name != col->table_name) continue;
                if (!rte.schema->hasColumn(col->column_name)) {
                    throw std::runtime_error("column '" + col->column_name
                        + "' not found in '" + rte.ref_name + "'");
                }
                // keep the as-typed qualifier: aggregate output names are built
                // from it, and self-join occurrences are only distinguishable
                // by their aliases. Routing uses (level, slot), never the text.
                col->relation_slot = i;
                col->query_level = level;
                markCorrelated(scope, level);
                return;
            }
        }
    }

    // Nothing in the chain matched. Fall back to the INNERMOST scope's
    // pre-Week-30 behaviour, unchanged, so every existing error message is
    // byte-identical: a qualified ref throws `unknown table qualifier`, and an
    // unqualified one in a single-relation scope takes slot 0 with existence
    // left to Validator.
    if (!col->table_name.empty()) {
        throw std::runtime_error("unknown table qualifier: '" + col->table_name + "'");
    }
    if (scope.range_table.size() < 2) {
        col->relation_slot = 0;
        col->query_level = 0;
    }
    // otherwise leave unresolved; Validator reports "column not found"
}
```

```cpp
// src/planner/binder.cc — file-local
// A ref resolving `level` scopes out makes THIS scope correlated, and every
// scope between it and the resolving one: none of them can be evaluated
// independently of the scope that supplies the value.
static void markCorrelated(Binder::Scope& scope, int level) {
    Binder::Scope* s = &scope;
    for (int i = 0; i < level && s; ++i, s = s->parent) s->correlated = true;
}
```

```cpp
// src/planner/binder.cc — bindExpr's new branch. DISPATCH SITE 3.
    } else if (auto* sq = dynamic_cast<SubqueryExpr*>(expr)) {
        // The IN operand is written in the ENCLOSING query, so it binds against
        // THIS scope. Binding it inside the subquery's scope would resolve it
        // against the subquery's relations — a wrong answer with no error.
        bindExpr(sq->operand.get(), scope, catalog);

        if (scope.stmt) scope.stmt->has_subquery = true;

        // The body opens a new scope whose parent is this one. Binding it HERE,
        // rather than in a separate pass, is what makes resolution lexical: the
        // enclosing range table is exactly the one in scope at this point.
        sq->correlated = bindQuery(*sq->subquery, catalog, &scope);
    }
```

### Implementation guidance

1. Refactor first, add scopes second. Turn the local `range_table` into
   `Scope::range_table` and thread `Scope&` through `bindExpr` /
   `resolveColumnRef` with **no behaviour change**, and confirm all 638 tests
   still pass. Only then add the parent walk. Mixing the two makes any
   regression un-bisectable.
2. `bindExpr` now needs the `Catalog` (to bind the nested statement). That is a
   signature change on a private static — mechanical, but do it in the refactor
   step, not the behaviour step.
3. Gotchas:
   - **`foldConstants` runs per scope.** `bindQuery` ends with it, so a
     subquery's own constants are folded by its own recursion. The outer
     `foldConstants` then walks the outer tree and meets `SubqueryExpr` at
     dispatch site 14, where `foldNode` returns false — safe, no folding, no
     descent. Do not make site 14 descend: the body is already folded, and
     folding it twice is only harmless because folding is idempotent, which is
     not a property worth depending on.
   - **A shared subquery is bound twice.** `BETWEEN`'s desugaring clones its left
     operand before binding, so `(SELECT ...) BETWEEN a AND b` hands the Binder
     two `SubqueryExpr` nodes sharing one statement. Task 3's idempotence is what
     makes the second walk a no-op. Verify this shape explicitly.
   - **The early `return false` on a missing table** leaves the subquery's refs
     unbound (slot -1). That is correct — Validator reports `Table not found` for
     the nested query — but it means `sq->correlated` stays false for a query
     that never bound. Do not read `correlated` as meaningful for an unbound
     subquery.
   - **`GroupByColumn` gets no `query_level`.** A GROUP BY item is always local,
     and subqueries are refused there (Task 4). Say so at the field rather than
     adding an unused one.
4. Do **not** try to make `relation_slot` globally unique across scopes. D1 says
   why: `ChunkPruner`'s `relation_slot < 1` test, a leaf schema's slot-0
   stamping and `restampSlots`' re-stamp to 0 are all per-scope facts, and
   global numbering silently disables chunk pruning inside every subquery.

### Verification

```bash
cd build && ./tests/swiftql_tests && cd ..     # 638 must still pass, unchanged
```

Unit tests in `tests/test_binder.cc` (the file already parses+binds and asserts
on stamps, so extend that corpus):

- **Uncorrelated**: `... WHERE speed > (SELECT AVG(speed) FROM laps)` — the inner
  `speed` has `query_level == 0`, `relation_slot == 0`; `sq->correlated` false.
- **Correlated, unqualified** (Q17's shape): `SELECT l.lap_id FROM laps l WHERE
  l.speed > (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)` — `l.team`
  inside has `query_level == 1`, `relation_slot == 0`; `l2.team` has level 0;
  `sq->correlated` true.
- **Correlated across a join** (Q4's shape): the outer query joins two relations
  and the correlated ref names the second — assert `relation_slot == 1` *and*
  `query_level == 1`. This is the test that fails if you conflated the domains.
- **Shadowing is not ambiguity**: an inner relation and an outer relation both
  holding `team`, referenced bare inside — resolves to level 0, no throw.
- **Ambiguity is still per scope**: two inner relations both holding `team`,
  referenced bare inside — still throws `ambiguous column reference`.
- **Nesting two deep** (Q20's shape): a ref resolving to level 2 marks both the
  innermost and the middle scope correlated.
- **Idempotence**: bind a statement, bind it again, assert every stamp is
  unchanged and nothing throws.

---

## Task 3 — Make binding idempotent, and close the alias-rebinding bug

> **Starting note, from a Week 28 audit** (README, Week 30 section). This is the
> next week that owns binder scope resolution, and the bug is a rebinding bug —
> exactly the class D2 makes structural.

### Why it matters

Two things converge on one two-line fix.

**The user-visible half.** In any query whose relations are aliased, an
`ORDER BY` or `GROUP BY` over an *unqualified* select-list alias is refused:

```sql
SELECT name AS n FROM laps l JOIN drivers d ON l.driver_id = d.driver_id ORDER BY n
-- Error: unknown table qualifier: 'drivers'
```

It reproduces on every path, at two relations, and the `GROUP BY n` spelling
fails identically. Both are live on the current tree — run them.

**The structural half.** D2's `shared_ptr` ownership means one
`SelectStatement` can be reached by two `SubqueryExpr` nodes, so the Binder can
walk the same subquery twice. If binding is not idempotent, that is a
second-order version of the same failure — and one that arrives *silently*,
inside a nested scope, rather than as a message.

### Conceptual explanation

**The mechanism.** `Binder::resolveColumnRef`'s unqualified branch resolves the
ref and then rewrites `table_name` to the **table** name (`drivers`), while the
range entry it matched is keyed on `ref_name` — the **alias** (`d`). The stamp
is correct; the text is now unusable as a qualifier. `ORDER BY <alias>` then
substitutes a clone of the already-bound select item and calls `bindExpr` on it
again; the clone has a non-empty `table_name`, so the *qualified* branch runs,
looks for a relation called `drivers`, and finds `l` and `d`.

`GROUP BY <alias>` fails by the same route with one extra step: the alias branch
copies `cr->table_name` and `cr->relation_slot` into the `GroupByColumn`, then
the code below builds a **fresh** `ColumnRef tmp` carrying the table name but
*not* the slot, and resolves that.

**Why the obvious fix is wrong, and the README says so.** Writing `ref_name`
back instead of `table_name` would resolve it — and change the output column
name of every aggregate over an unqualified column in an aliased query, because
`aggregateOutputName` **is** `exprToString`, which renders
`table_name.column_name`. `AVG(laps.speed)` would become `AVG(l.speed)`. That is
a schema-visible change and it is not this week's.

**The fix that changes no name.** Do not re-resolve what is already resolved.
A ref carrying `relation_slot >= 0` has been through the Binder; re-running
resolution on it is at best a no-op and at worst — as here — a wrong answer.
Guard at the top of `resolveColumnRef`, and carry the stamp into the GROUP BY
temporary so the guard can see it.

This is exactly the property D2 needs, so it is one fix serving both halves.

**The tradeoff, stated.** A hand-stamped `ColumnRef` in a test tree now skips
the Binder's existence check for that ref. `Validator` re-checks by slot
(`validateJoinCondition`) and by name (`validateExpr`), so nothing becomes
unchecked; what changes is which layer reports it. That is a smaller surface
than the alternative, which is a bug reachable from ordinary SQL.

### Code

```cpp
// src/planner/binder.cc — top of resolveColumnRef (shown in Task 2's snippet)

    // Week 30. Binding is IDEMPOTENT: a ref that already carries a slot has
    // been resolved and is returned untouched.
    //
    // Three callers re-bind a clone of an already-bound expression — GROUP BY
    // <alias>, ORDER BY <alias>, and any cloneExpr'd subtree (BETWEEN's
    // desugaring, the residual ON clone) — and for an unqualified ref the
    // second pass is a WRONG ANSWER, not a no-op: the unqualified branch below
    // rewrites table_name to the TABLE name while the range table is keyed on
    // the REF name, so `SELECT name AS n FROM laps l JOIN drivers d ... ORDER BY n`
    // failed with "unknown table qualifier: 'drivers'".
    //
    // Writing ref_name back instead would also resolve it and would change the
    // OUTPUT COLUMN NAME of every aggregate over an unqualified column in an
    // aliased query — aggregateOutputName IS exprToString, which renders
    // table_name.column_name, so AVG(laps.speed) would become AVG(l.speed).
    // Not re-resolving what is already resolved changes no name at all.
    //
    // It is also the precondition for SubqueryExpr's shared_ptr (ast.h): two
    // nodes may share one statement, so the Binder may walk it twice.
    if (col->relation_slot >= 0) return;
```

```cpp
// src/planner/binder.cc — the GROUP BY loop: carry the stamp into the probe
        ColumnRef tmp;
        tmp.table_name = g.table_name;
        tmp.column_name = g.column_name;
        tmp.relation_slot = g.relation_slot;   // Week 30: an alias substitution
        tmp.query_level  = 0;                  // above already resolved this;
                                               // without the stamp the guard in
                                               // resolveColumnRef cannot see it
                                               // and the qualified path throws.
        resolveColumnRef(&tmp, scope);
        g.table_name = tmp.table_name;
        g.relation_slot = tmp.relation_slot;
```

### Implementation guidance

1. Land this **before** Task 2's scope walk if you can — it is independent of
   scoping, it is a live bug on the current tree, and having it green first
   means a Task 2 regression is unambiguous.
2. Both spellings must be fixed by the same change. If only `ORDER BY` starts
   working, you fixed the symptom (the ORDER BY rebind) rather than the cause
   (non-idempotent resolution).
3. Do not "fix" this by skipping `bindExpr` on substituted ORDER BY clones. It
   works for that one path, leaves `GROUP BY` broken, and gives D2 nothing.
4. The comment in `binder.cc` at the ORDER BY loop currently reads
   `// no-op on already-stamped clones`. It was false. Make it true, or delete
   it — a comment stating the property the code lacks is how this survived two
   audits.

### Verification

```bash
# both must now return rows; both error today
./build/swiftql --catalog catalog.json --query \
  "SELECT name AS n FROM laps l JOIN drivers d ON l.driver_id = d.driver_id ORDER BY n LIMIT 3"
./build/swiftql --catalog catalog.json --query \
  "SELECT name AS n, COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY n LIMIT 3"

# the regression this fix exists to AVOID: the output column name must not move
./build/swiftql --catalog catalog.json --query \
  "SELECT AVG(speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id"
# header must still read exactly:  AVG(laps.speed)
```

Then add both queries to `compare_against_sqlite.py`'s main suite — they return
rows, so SQLite is a real oracle for them, and a rejection test would only pin
the error message they no longer produce. Assert the aggregate output name in a
C++ test (`tests/test_planner.cc` or `test_logical_plan.cc` already assert
schema column names), because the harness normalizes rows through a dict keyed
by column name and would not notice the header changing.

---

## Task 4 — Validation: nested scope, arity, position

### Why it matters

`Validator::validate` is the only layer both engines share before they diverge,
which is why Week 29 put the STRING-vs-numeric join-key check there: one check,
four modes, one message. Everything this week refuses belongs there for the same
reason. It is also the layer that decides *which* error a doubly-broken query
reports, and this codebase has a stated discipline about that — a genuine query
defect outranks a temporary engine limitation.

### Conceptual explanation

**The nested query is validated in its own scope.** `Validator::validate` takes
a statement and a catalog and checks it against its own `FROM` schema. Run it
recursively on the subquery. It will check the subquery's tables, columns,
aggregate typing, `HAVING`-requires-`GROUP BY`, and grouping — all correctly,
because those are all scope-local questions.

**One thing is not scope-local: a correlated ref.** Inside the subquery,
`validateExpr` would check `l.team` (level 1) against the *subquery's* `FROM`
schema and report `column not found` for a perfectly legal query. The Binder
already resolved and verified it against the enclosing scope's range table, so
the right move is the one `validateJoinCondition` already makes for bound refs:
**skip what a lower layer has already established.** Test `query_level > 0`.

**Do not descend into the body from `validateExpr`.** It is tempting — the walk
is already there — and it is wrong twice over. The schema is the wrong one, and
`allow_aggregates` is the wrong flag: `WHERE x > (SELECT AVG(y) ...)` is legal
SQL, but the outer `WHERE` walk carries `allow_aggregates = false` and would
reject the subquery's own aggregate. Treat the node as opaque at the outer walk
and hand the statement to a fresh `validate` call.

**Arity is a bind-time question, cardinality is a runtime one.** A scalar
subquery returning two columns is decidable now, from `select_list.size()`, with
no planning at all — refuse it. A scalar subquery returning two *rows* is not
decidable now; Week 31's bullet says "validate scalar cardinality at runtime".
Do not attempt the second.

`EXISTS` has no arity rule: Q4 and Q21 both write `select *`, and `EXISTS` never
reads the values.

**Position: fail closed.** Add `allow_subqueries` to `validateExpr` and default
it to **false**, passing `true` only at the `WHERE` and `HAVING` call sites.
Defaulting to true would let a future call site permit subqueries by omission —
the same reasoning that made `distribute()` test `== INNER` rather than
`!= LEFT`. `ORDER BY` is not routed through `validateExpr` at all today, so it
needs its own explicit check.

**`ON` gets a throw at dispatch site 18**, in the shape that file already uses
for `AggregateExpr`. Note the ordering consequence: `Validator::validate` calls
`classifyJoinCondition` *before* `validateJoinCondition`, so for a subquery in
an `ON` clause that *also* forward-references a later relation, the
forward-reference message wins. That is consistent with the stated rule — shape
first, then contents — and worth a line in the comment so the next reader does
not treat it as a bug.

### Code

```cpp
// src/planner/validator.h
class Validator {
    public:
        static void validate(const SelectStatement& stmt, const Catalog& catalog);
    private:
        // Week 30. The body of validate(), without the final "not yet
        // executable" refusal. A nested query is validated through THIS, so a
        // subquery's own subquery does not fire the refusal from inside the
        // recursion — the refusal belongs to the whole statement, once.
        static void validateQuery(const SelectStatement& stmt, const Catalog& catalog);

        static void validateExpr(const Expr* expr, const Schema& schema,
                                 const std::string& context, const Catalog& catalog,
                                 bool allow_aggregates = true,
                                 bool allow_subqueries = false);   // fail closed
        ...
};
```

```cpp
// src/planner/validator.cc — validateExpr's ColumnRef branch
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // Week 30. A ref the Binder resolved to an ENCLOSING query names a
        // relation this scope's schema does not hold, so checking it here is a
        // false "column not found". The Binder already verified it against that
        // scope's range table — same reason validateJoinCondition trusts a
        // bound ref's slot instead of re-deriving it from table_name.
        if (col->query_level > 0) return;

        if (col->table_name.empty() && !schema.hasColumn(col->column_name)) {
            throw std::runtime_error(context + ": column not found: '" + col->column_name + "'");
        }
    }
```

```cpp
// src/planner/validator.cc — validateExpr's new branch. DISPATCH SITE 4.
    else if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        if (!allow_subqueries) {
            throw std::runtime_error(
                context + ": subqueries are supported in WHERE and HAVING only");
        }

        // The IN operand is written in THIS query, so it is checked here,
        // against this schema, with this clause's aggregate rule.
        if (sq->operand) {
            validateExpr(sq->operand.get(), schema, context, catalog,
                         allow_aggregates, allow_subqueries);
        }

        // Arity is decidable now; CARDINALITY is not (Week 31 validates "more
        // than one row" at runtime). EXISTS has no arity rule at all — Q4 and
        // Q21 both write `select *`, and EXISTS never reads the values.
        if (sq->kind != SubqueryExpr::Kind::EXISTS) {
            if (sq->subquery->select_star || sq->subquery->select_list.size() != 1) {
                throw std::runtime_error(
                    std::string(sq->kind == SubqueryExpr::Kind::IN ? "IN" : "scalar")
                    + " subquery must return exactly one column");
            }
        }

        // The body is a DIFFERENT scope: a different FROM schema, and its own
        // aggregate rule (an aggregate in a subquery's select list is legal
        // even inside an outer WHERE). Do not descend into it from here —
        // hand it to a fresh validation with its own schema.
        validateQuery(*sq->subquery, catalog);
    }
```

```cpp
// src/planner/validator.cc — validateJoinCondition. DISPATCH SITE 18.
    if (dynamic_cast<const SubqueryExpr*>(expr)) {
        // No TPC-H query puts a subquery in an ON clause, and a residual
        // carrying one would be handed to a probe loop that cannot evaluate it
        // (an outer join) or folded into the WHERE conjunction and routed by a
        // relation slot it does not have (an inner one). Decline, in the same
        // shape as the AggregateExpr branch below.
        //
        // classifyJoinCondition runs one line EARLIER in Validator::validate,
        // so a subquery that also forward-references a later relation reports
        // the forward reference first. That is the stated order — shape before
        // contents — not a defect.
        throw std::runtime_error(
            "JOIN ON: subqueries are not supported in a join condition");
    }
```

```cpp
// src/planner/validator.cc — the two call sites that opt IN, and ORDER BY
    if (stmt.where) {
        validateExpr(stmt.where.get(), schema, "WHERE", catalog,
                     /*allow_aggregates=*/false, /*allow_subqueries=*/true);
    }
    ...
    if (stmt.having) {
        validateExpr(stmt.having.get(), schema, "HAVING", catalog,
                     /*allow_aggregates=*/true, /*allow_subqueries=*/true);
    }
    ...
    // ORDER BY is not routed through validateExpr (only its ColumnRef nodes are
    // checked), so the position rule needs its own line here.
    for (const auto& item : stmt.order_by) {
        if (dynamic_cast<const SubqueryExpr*>(item.expr.get())) {
            throw std::runtime_error(
                "ORDER BY: subqueries are supported in WHERE and HAVING only");
        }
    }
```

### Implementation guidance

1. The `Catalog&` parameter on `validateExpr` is a mechanical change across ~8
   call sites in one file. Do it as its own commit so the interesting diff is
   readable.
2. **`allow_subqueries` defaults to `false`.** Resist defaulting it to `true` to
   shorten the call sites — the whole point is that a new call site must opt in.
3. The `SELECT` position is refused by `validateExpr`'s existing call at the top
   of `validate` (context `"SELECT"`, `allow_subqueries` defaulting false), and
   `GROUP BY`'s expression items by the call at the GROUP BY loop. Confirm both
   by test rather than by reading — the two call sites pass different flags and
   it is easy to hand one the wrong one.
4. Gotcha: the recursive `validateQuery` on the subquery runs `checkGroupedRefs`
   over the subquery's own select list. A *correlated* ref inside a grouped
   subquery (`SELECT ... FROM l GROUP BY x` with an outer ref in the select
   list) would be reported as ungrouped. Handle it at site 5 the same way you
   handled site 4's `ColumnRef` branch — `query_level > 0` returns — and add a
   test, because it is silent otherwise.
5. Do not add an operand-type check for `IN (subquery)` (`x IN (SELECT y)` with
   `x` STRING and `y` INT). It needs the subquery's output type, which needs
   planning, which is Week 31's. `InExpr`'s equivalent check lives in
   `inferExprType` for exactly that reason.

### Verification

```bash
# each of these is a distinct message; assert the message, not just failure
./build/swiftql --catalog catalog.json --query \
  "SELECT (SELECT AVG(speed) FROM laps) FROM drivers"
# SELECT: subqueries are supported in WHERE and HAVING only

./build/swiftql --catalog catalog.json --query \
  "SELECT team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id AND EXISTS (SELECT 1 FROM laps)"
# JOIN ON: subqueries are not supported in a join condition

./build/swiftql --catalog catalog.json --query \
  "SELECT team FROM laps WHERE speed > (SELECT speed, team FROM laps)"
# scalar subquery must return exactly one column

./build/swiftql --catalog catalog.json --query \
  "SELECT team FROM laps WHERE season IN (SELECT * FROM drivers)"
# IN subquery must return exactly one column

./build/swiftql --catalog catalog.json --query \
  "SELECT team FROM laps WHERE EXISTS (SELECT * FROM nosuchtable)"
# Table not found: 'nosuchtable'   <-- the nested query's own error, not the refusal

./build/swiftql --catalog catalog.json --query \
  "SELECT team FROM laps WHERE EXISTS (SELECT * FROM drivers WHERE nosuchcol = 1)"
# WHERE: column not found: 'nosuchcol'
```

The last two are the ones that prove the recursion happened. A query whose
subquery names a missing table must report *that*, not the Week 31 refusal.

---

## Task 5 — The eighteen dispatch sites, and the `collectSlots` note

> **Starting note, from Week 29's third audit** (README, Week 30 section).
> Three things to do, none of them a weakening of the guard, whose behaviour is
> right.

### Why it matters

`development.md` opens with the count: eighteen functions dispatch on `Expr`
subtype, **ten fail silently**. You are adding the tenth subtype since Week 25
and the first that contains a whole query. A miss here is not a crash — it is a
wrong answer somewhere far away, which is the entire reason that checklist
exists.

Site 8 in particular has a note addressed to this week by name, and it names the
exact failure: miss `SubqueryExpr` at `collectSlots` and
`ON b.k = c.k AND <subquery naming a later relation>` stops throwing "is joined
later", becomes an `on_residual` over a relation absent from that join's merged
schema, and is then evaluated against a schema that has no such column
(`plan_nodes.cc`, `vec_hash_join_node.cc`).

### Conceptual explanation

**The three documentation corrections, first.** `predicate_pushdown.h` justifies
`pruningHintForPreservedSide`'s fail-closed empty-slot branch with

> "every other caller treats empty as the conservative answer"

That is true of `soleSlot` (empty → `-1` → not pushed) and **false of
`classifyJoinCondition`**, where an empty slot set means the forward-reference
loop has nothing to iterate and the conjunct is **accepted** — which
`join_condition.cc`'s own comment states in as many words. Two files say
opposite things about one walker. Restate the justification as the one that is
actually load-bearing:

> *This* caller is the only one where an empty set would read as "mentions
> nothing unpreserved", so it must withhold.

Then fix the caller count in both places: `predicate_pushdown.cc` says "Two
callers, one walker" and `predicate_pushdown.h` names `classifyJoinCondition` as
"the second caller", while there are **three** — `development.md`'s checklist was
corrected in Week 29 and these two were not, so a reader following the "dispatch
site 8" pointer lands on the stale count. A fourth copy of the wrong claim is in
`tests/test_predicate_pushdown.cc` at `AnEmptySlotSetFailsClosed`
("both of its other callers read empty as the conservative answer"); fix it
there too, and while you are in that test, note that its comment predicts this
week by name.

**Then the rule for `SubqueryExpr` at site 8.** `collectSlots` answers "which
relation slots *of this scope* does this expression reference". A subquery's own
refs (level 0 inside it) are a different scope's relations and must contribute
nothing. Its *correlated* refs do reference this scope — that is what
correlation means — and must contribute.

The precise answer is the set of slots of the correlated refs, which needs a
level-aware walk over a whole statement. The **minimum correct** answer, and the
one to write this week, is the conservative one:

```
contribution = slots(operand)  ∪  { -1 if sq->correlated }
```

`-1` is already the walker's "unresolved, be conservative" value, and every
caller already reads it that way:

| Caller | With `-1` in the set | Correct? |
|---|---|---|
| `soleSlot` | returns `-1` → the conjunct stays above the join | Yes: a correlated conjunct cannot be routed to one relation |
| `pruningHintForPreservedSide` | `-1 ∉ preserved_slots` → withhold the hint | Yes: fail closed, exactly as the guard intends |
| `classifyJoinCondition` | `-1 <= right_slot` → no forward-reference throw | Moot: site 18 refuses a subquery in `ON` outright |

An **uncorrelated** subquery contributes nothing, which is also right: it is a
constant with respect to this scope, so `WHERE r1.x = (SELECT ...)` still has
`soleSlot == 1` and still pushes onto relation 1's scan.

**Site 9 must stay in lockstep.** `restampSlots` re-stamps a pushed conjunct's
refs to slot 0. Its `SubqueryExpr` branch restamps `operand` and stops — the
body is another scope and rewriting its slots would corrupt it. That is
consistent with site 8 and provably so: a conjunct containing a *correlated*
subquery contributes `-1`, so `soleSlot` is `-1`, so it is never pushed, so
`restampSlots` never sees one. An *uncorrelated* one needs nothing restamped
inside the body.

**Nothing at site 8 executes from the CLI this week.** Task 6's refusal fires
before any logical plan is built, so pushdown never runs on a statement
containing a subquery. Write and unit-test the branch anyway: the note requires
it in this commit, the walker is shared, and Week 31 turns it live. Say so in
the comment rather than letting the next reader assume it is covered.

### Code

```cpp
// src/planner/predicate_pushdown.cc — collectSlots. DISPATCH SITE 8.
    // Week 30. A subquery's OWN refs (query_level 0 inside it) are positions in
    // a DIFFERENT scope's range table; contributing them here would name this
    // scope's relations by an inner scope's numbering. Its CORRELATED refs do
    // reference this scope, so they must contribute something.
    //
    // The conservative form: -1 when the Binder found any correlated ref. -1 is
    // this walker's existing "unresolved, be conservative" value, and all three
    // callers already read it that way — soleSlot returns -1 (never pushed) and
    // pruningHintForPreservedSide withholds. The exact set (correlated refs'
    // slots, level-decremented) buys pushdown for a correlated conjunct, which
    // nothing can execute until Week 33 decorrelates it; land it there.
    //
    // NOT reachable from the CLI this week — Validator refuses a bound subquery
    // before any logical plan exists — but the branch ships with the node, per
    // the site-8 note in readme.md. Unit-tested directly, like
    // PruningHintForPreservedSide.AnEmptySlotSetFailsClosed does.
    if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        collectSlots(sq->operand.get(), out);   // IN's operand is THIS scope's
        if (sq->correlated) out.insert(-1);
        return;
    }
```

```cpp
// src/planner/predicate_pushdown.cc — restampSlots. DISPATCH SITE 9.
    // Lockstep with collectSlots: the operand is this scope's and is restamped;
    // the body is another scope and must not be touched. Provably never reached
    // with a correlated subquery — that contributes -1 above, so soleSlot is -1
    // and the conjunct is never pushed.
    if (auto* sq = dynamic_cast<SubqueryExpr*>(expr)) {
        restampSlots(sq->operand.get(), slot);
        return;
    }
```

```cpp
// src/planner/predicate_pushdown.h — the corrected justification
// ... The test is `slots ⊆ preserved_slots` and NOT `slots ⊆ {0}` ...
//
// An EMPTY slot set withholds, deliberately, and the reason is specific to THIS
// caller rather than shared with the others: here an empty set would read as
// "mentions nothing unpreserved" and turn the guard off, while soleSlot reads
// empty as -1 and declines to push (conservative) and classifyJoinCondition
// reads it as "no forward reference" and ACCEPTS the conjunct (permissive —
// join_condition.cc says so in as many words). collectSlots is dispatch site 8,
// where a missed Expr subtype yields an empty set, so this caller must fail
// closed on its own account and not on a claim about the others.
```

```cpp
// src/planner/predicate_pushdown.cc — the corrected count, above collectSlots
// Declared in the header since Week 27. THREE callers, one walker: soleSlot
// (here), classifyJoinCondition (join_condition.cc, Week 27) and
// pruningHintForPreservedSide (below, Week 29) — a private copy would be an
// eleventh silent dispatch site. They do NOT agree on what an empty set means;
// see the header.
```

### The full site table

| # | Site | Branch for `SubqueryExpr` | Why |
|---|---|---|---|
| 1 | `exprKey` (`expr_utils.h`) | `"SUBQUERY@" + address of `subquery.get()`, plus kind/negated and `exprKey(operand)` | Returning `"?"` collides with any other unhandled subtree, and `substituteInto` matches on `exprKey` — a subquery in `HAVING` could be rewritten into a group-key `ColumnRef`. The pointer is stable across clones *because* the statement is shared (D2), and this key is for matching, never display |
| 2 | `collectCols` (`logical_plan.cc`) | descend into `operand` only; add `if (stmt.has_subquery) return full_schema;` at the top of `buildScanSchema` | Narrowing is by bare name across one flat schema. A correlated ref names an *outer* column that must survive narrowing, and the body's names are a different scope's. Widening is the safe direction; Week 33 replaces it with the precise correlated-column set |
| 3 | `Binder::bindExpr` | Task 2 | The week's work |
| 4 | `Validator::validateExpr` | Task 4 | Position, arity, recursion |
| 5 | `checkGroupedRefs` (`validator.cc`) | `return` on `SubqueryExpr`; `return` on any `ColumnRef` with `query_level > 0` | Unreachable at the outer level (subqueries are refused in `SELECT`), but reachable *inside* a grouped subquery via the recursive validate — where a correlated ref is not the inner query's to group |
| 6 | `substituteInto` (`logical_plan.cc`) | descend into `operand` only | A post-aggregate group-key rewrite is scope-local; rewriting inside the body would point an inner ref at an outer aggregate's output column |
| 7 | `collectAggregates` (`expr_utils.h`) | descend into `operand` only, **never** the body | The sharpest silent one. `HAVING SUM(x) > (SELECT AVG(y) FROM z)` — collecting the inner `AVG(y)` as an outer `AggregateSpec` computes it over the outer relation and emits a column for it |
| 8 | `collectSlots` | above | The note |
| 9 | `restampSlots` | above | Lockstep with 8 |
| 10 | `exprToString` (`expr_utils.h`) | `"(SELECT ...)"`, `"[NOT ]EXISTS (SELECT ...)"`, `exprToString(operand) + " [NOT ]IN (SELECT ...)"` | Visible: `--explain` prints predicates. Abbreviated on purpose — no output column is ever named after a subquery, because they are refused in `SELECT` |
| 11 | `cloneExpr` (`expr_utils.h`) | copy kind/negated/correlated, `cloneExpr(operand)`, **share** `subquery` | Throws today. D2 |
| 12 | `inferExprType` (`logical_plan.cc`) | throw a *specific* message naming Week 31 | Loud already; a specific message beats `unknown Expr subtype`. The real rule (a scalar subquery's type is its single output column's) needs the subquery's schema, which is Week 31's |
| 13 | `evaluate` (`evaluator.cc`) | same | Same |
| 14 | `foldNode` (`constant_folding.cc`) | nothing — returns false | Safe. The body was folded by its own `bindQuery`; do not descend |
| 15 | `ExpressionExecutor::compileNode` | nothing — returns `nullptr` | Safe: caller falls back to `evaluate()` |
| 16 | `evalPredicate` (`columnar_eval.cc`) | nothing | Safe: routes through `PredicateExecutorCache` |
| 17 | `CardinalityEstimator::selectivity` | nothing — `FALLBACK_SELECTIVITY` | Safe. A real rule for a semi-/anti-join is Week 32's, and Week 29's note says it must be its own rule at the stamping site, never inside `joinCardinality` |
| 18 | `Validator::validateJoinCondition` | throw | Task 4 |

### Implementation guidance

1. Sites **11, 12, 13 first**, before 1–10. They throw, so they convert every
   later omission into a loud failure. This is `development.md`'s order and it
   is not advisory.
2. Then 1–10, **one test per site**. The silent ones cannot be caught by eye,
   and a test that only asserts "something threw" does not prove the right site
   caught it — assert on the message, as
   `Validation.UngroupedColumnInsideWeek25NodesIsRejected` learned to.
3. Sites 14–17 need no code. Confirm that by reading them, and say so in the
   commit message; "we did not touch it" and "we checked it degrades correctly"
   are different claims.
4. Update `development.md`'s site table with the `SubqueryExpr` row for sites 8
   and 18 at minimum, and fix the site-8 caller-count sentence there if the
   Week 29 correction left anything stale.
5. Gotcha at site 1: if you dislike an address in a key, render the statement's
   shape instead (`kind`, `from_table`, `exprKey(where)`). Do **not** leave it
   returning `"?"`.

### Verification

```bash
cd build && ./tests/swiftql_tests --gtest_filter='*Subquery*:*Slot*:PruningHint*' && cd ..
```

Direct unit tests, because the CLI cannot reach most of these this week:

- `collectSlots` on `EXISTS (SELECT ... WHERE inner.k = outer.k)` yields `{-1}`;
  on an uncorrelated `EXISTS` yields `{}`; on
  `outer.x IN (SELECT ...)` uncorrelated yields `{0}`.
- `pruningHintForPreservedSide(hint, LEFT, {0})` returns `nullptr` when the hint
  contains a correlated subquery, and the hint unchanged for an inner join.
- `cloneExpr` on a `SubqueryExpr` returns a node whose `subquery.get()` is the
  **same pointer** (shared, not deep-copied) and whose `operand` is a *different*
  pointer with equal contents.
- `collectAggregates` over `SUM(x) > (SELECT AVG(y) FROM ...)` returns exactly
  one spec — `SUM(x)`. This is site 7's whole test.
- `exprKey` gives two distinct subqueries distinct keys, and a clone the same key
  as its original.

---

## Task 6 — The refusal, placed last

### Why it matters

Without it, a bound-but-unlowerable subquery reaches `LogicalPlanBuilder::build`
and dies at `inferExprType` with a message about an unknown expression — or,
worse, slips past a walker that returns silently and produces a plan missing a
predicate. The refusal is what makes "binds correctly" a checkpoint you can
demonstrate rather than a claim.

### Conceptual explanation

D4 has the placement argument. The mechanics:

- `Validator::validate` becomes `validateQuery` + the refusal.
- The nested call in `validateExpr` uses `validateQuery`, so a subquery inside a
  subquery does not fire the refusal from inside the recursion.
- The flag is `SelectStatement::has_subquery`, set by the Binder (Task 2), so no
  nineteenth walker is needed to find out. Any statement containing a subquery
  at any depth contains one *directly* at its top level, so the top-level flag
  is always the right test.
- Both `Planner::plan` and `LogicalPlanBuilder::build` call
  `Validator::validate` first, so both engines refuse identically with no
  per-engine code.

Word the message in the shape Week 26 used, so the family is recognizable and
greppable: it states the current capability, then names the week that removes
the limit.

### Code

```cpp
// src/planner/validator.cc
void Validator::validate(const SelectStatement& stmt, const Catalog& catalog) {
    validateQuery(stmt, catalog);

    // Week 30. LAST, after every semantic check — including the nested query's
    // own, which validateQuery ran through validateExpr — so a genuine query
    // defect outranks a temporary engine limitation, the same discipline that
    // placed Week 26's multi-key refusal past the plan-time type checks.
    //
    // Here, unlike Week 26, both engines are equally incapable: neither can
    // lower a subquery, so there is no capability difference to preserve and no
    // reason for two failure points. One check, four modes, one message.
    if (stmt.has_subquery) {
        throw std::runtime_error(
            "subqueries are parsed and bound but not yet executable (Week 31)");
    }
}
```

### Implementation guidance

1. `validateQuery` is the old body verbatim — no edits beyond the rename, so the
   diff shows only the split.
2. Do not add a second refusal in `Planner::plan` or
   `VectorizedPlanBuilder`. Two refusals is how the two engines drift apart, and
   Week 29 spent a whole audit round on exactly that shape.
3. Gotcha: a *validator-only* caller that skips the Binder leaves
   `has_subquery` false and gets no refusal. That is the same allowance
   `classifyJoinCondition` makes for unbound callers, and the real pipeline
   always binds first — but if you write a C++ test that hand-builds a statement
   with a `SubqueryExpr`, set the flag or it will not be refused.

### Verification

```bash
for mode in "--storage row --execution volcano" \
            "--storage columnar --execution volcano" \
            "--storage columnar --execution vectorized" \
            "--storage columnar --execution vectorized --no-optimize"; do
  ./build/swiftql --catalog catalog.json $mode --query \
    "SELECT name FROM drivers d WHERE EXISTS (SELECT * FROM laps l WHERE l.driver_id = d.driver_id)"
done
# identical message in all four
```

---

## Task 7 — `Planner::plan`'s `preserved_slots`, derived rather than asserted

> **Starting note, from Week 29's third audit** (README, Week 30 section).

### Why it matters

Week 29 shared the pruning-hint *rule* between the two engines
(`pruningHintForPreservedSide`) and left the *input* duplicated:
`VectorizedPlanBuilder` derives the preserved set from
`join->children[0]->output_schema`, while `Planner::plan` passes a hard-coded
`std::unordered_set<int> preserved_slots{0}`.

The constant is correct **only** because of the `stmt.joins.size() > 1` refusal
110 lines above it. That is the identical undocumented coupling Week 29's
round-1 audit found for `outer` and fixed by naming the clause `jc` once — and
it was re-introduced four commits later at a new site, with the comment stating
the *conclusion* ("Volcano builds exactly one join") rather than the refusal
that guarantees it.

Relax that refusal — which its own comment invites a later week to reconsider —
and `FROM a JOIN b ON k1 LEFT JOIN c ON k2 WHERE b.x = 5` gives the two engines
different preserved sets: same rows, different work per mode, with nothing to
catch it. That is precisely the failure mode Week 27 moved the `ON`
decomposition above the scan construction to prevent.

### Conceptual explanation

The fix is three lines, and it is a *derivation*, not a new rule. The FROM
scan's own narrowed schema is already in scope at the call site, and it carries
the relation slots the scan actually presents. Build the set from it, exactly as
the vectorized builder builds its set from the left input's schema.

Today the derived set is `{0}` — `buildScanSchema` narrows a catalog schema
whose `ColumnDef::relation_slot` defaults to 0 — so this is byte-identical in
behaviour. What changes is that the value stops being something a future change
can invalidate silently.

The fallback, if you decline the derivation for some reason: state the
dependency on the refusal in the comment, in the shape the `jc` fix used
("correct only because of the `joins.size() > 1` throw at the top of this
function"). Do not leave the comment stating only the conclusion — that is the
exact defect being closed.

### Code

```cpp
// src/planner/planner.cc — replacing the hard-coded set
    // Week 30. DERIVED from the FROM scan's own schema, not asserted as {0}.
    // The constant was correct only because of the `stmt.joins.size() > 1`
    // refusal 110 lines above — an undocumented coupling of exactly the kind
    // the `jc` pointer above exists to remove. Relax that refusal and this
    // constant silently gives the two engines different preserved sets for
    // `FROM a JOIN b ON k1 LEFT JOIN c ON k2 WHERE b.x = 5`: same rows,
    // different work per mode, with nothing to catch it.
    //
    // Mirrors VectorizedPlanBuilder's JOIN case, which reads its set off
    // join->children[0]->output_schema for the same reason.
    std::unordered_set<int> preserved_slots;
    for (const ColumnDef& c : scan_schema.columns()) {
        preserved_slots.insert(c.relation_slot);
    }
    const Expr* prune_hint = pruningHintForPreservedSide(
        stmt.where.get(), outer ? JoinType::LEFT : JoinType::INNER, preserved_slots);
```

### Implementation guidance

1. `scan_schema` is already computed above the call — no reordering needed.
2. Do not extend it to the join-side scan. Volcano's join scan is never handed a
   pruning hint, and adding one is a Week 37 measurement, not a refactor.
3. This is a behaviour-preserving change today. If any test's output moves, the
   change is wrong — investigate rather than update the expectation.

### Verification

```bash
cd build && ./tests/swiftql_tests && cd ..     # all still green, no expectation edits
python3 python_tools/compare_against_sqlite.py # unchanged totals
```

Add a C++ test that asserts the derived set equals `{0}` for a single-join
Volcano plan — small, but it is the assertion that would fail the day someone
relaxes the multi-way refusal, which is the whole point.

---

## Task 8 — `JoinEnumeration`'s unbound-key throw becomes a decline

> **Starting note, from Week 28's foundations and again from Week 29's.** Both
> name Week 30.

### Why it matters

`JoinEnumeration::apply` throws when an edge endpoint is outside the range
table:

```cpp
if (e.slot_a < 0 || e.slot_a >= n || e.slot_b < 0 || e.slot_b >= n) throw ...
```

`n` is `countRelations(node)` — the number of `SCAN` nodes in the tree. The
check means "unbound key" only while every scan in the tree is a range-table
entry of the query being planned. A subquery introduces scans that are not, at
which point the condition starts firing on legitimate plans.

It is also the one place the optimized path can fail on input `--no-optimize`
accepts: `Volcano` and the written-order vectorized path never consult
`from_slot` for placement, so the same query runs to completion unoptimized.
That breaks optimized ≡ `--no-optimize`, which is a project invariant and the
differential oracle `compare_against_sqlite.py`'s fourth mode exists to check.

### Conceptual explanation

Week 29 already wrote the shape three lines above: `containsOuterJoin` returns
the node untouched. Declining to reorder keeps optimized ≡ `--no-optimize`
intact; a throw does not.

**Be precise about when it goes live.** With Task 6's refusal placed at the end
of `Validator::validate`, no statement containing a subquery reaches
`LogicalPlanBuilder::build` this week, so `countRelations` never sees a subquery
scan and the throw stays unreachable. Do not claim otherwise in the commit
message. Convert it anyway: it is three lines, it removes the invariant
violation on its own merits, and it means the week that *does* admit a subquery
scan — Week 31 for an uncorrelated one, Week 34 for a derived table — does not
have to remember a note written two weeks earlier.

**Do not silently drop the key instead.** Week 28's comment explains why that is
worse: a dropped key is a missing conjunct and therefore MORE rows if the join
had another key, or a spurious `produced a cross product` throw if it did not.
Declining the *whole tree* keeps the written order, which is always legal.

**Do not report it in `--explain`.** Week 29 reported the outer-join decline
because a decision was available and was refused. Here the pass has met a shape
it cannot describe — the same situation as the sub-3-relation and >32-relation
declines, which are silent. Adding an `order=` line would claim a search that
never ran, and Week 28's rule is that the token must keep meaning "the search
ran".

### Code

```cpp
// src/planner/join_enumeration.cc — replacing the throw in the edge loop
    for (const Edge& e : edges) {
        // An endpoint outside the range table means this pass cannot express
        // the tree in its own numbering. Two ways to get here: an unbound key
        // (from_slot -1, join_condition.h's positional-routing path), and — from
        // Week 31/34 on — a scan that is not a range-table entry of the query
        // being planned, because it belongs to a subquery.
        //
        // Week 30: DECLINE, do not throw. Volcano and the written-order
        // vectorized path never consult from_slot for placement, so a throw here
        // is the one place the optimized path can fail on input --no-optimize
        // accepts — and that equivalence is the differential oracle. Same shape
        // as containsOuterJoin's decline three lines above; silent, like the
        // <3-relation and >32-relation declines, because no ordering decision
        // was available to report.
        if (e.slot_a < 0 || e.slot_a >= n || e.slot_b < 0 || e.slot_b >= n) {
            return node;
        }
        ...
    }
```

### Implementation guidance

1. **Placement matters.** `decompose()` moves subtrees out of the tree, so a
   decline discovered afterwards has nothing clean to return — the same reason
   Week 29 put `containsOuterJoin` *before* `decompose`. The edge loop currently
   runs after `decompose`, so returning `node` there is returning a
   moved-from tree. Either hoist the endpoint check above `decompose` (walking
   the written-order tree's keys), or rebuild the written order before
   returning. **Hoisting is the correct fix** and matches the precedent; if you
   take the other route, say why in the comment.
2. Update `join_enumeration.h`'s documented decline list — it enumerates the
   conditions under which the pass is a no-op, and a reader trusts it.
3. `tests/test_join_enumeration.cc` has a test pinning the throw. Convert it to
   pin the decline: same input, assert the tree comes back in written order with
   no `order_decision`, rather than `EXPECT_THROW`.

### Verification

```bash
cd build && ./tests/swiftql_tests --gtest_filter='JoinEnumeration.*' && cd ..
python3 python_tools/test_new_queries.py     # join-order steering unaffected
```

The property to assert, and it is stronger than "it does not throw": for a tree
with an out-of-range endpoint, the rebuilt tree must be **identical** to the
written-order tree — same join order, same keys, no `order=` line in `--explain`.

---

## Task 9 — Tests, harness, and the documentation the week invalidates

### Why it matters

`compare_against_sqlite.py` cannot diff rows for anything this week ships —
every subquery query errors. That makes the **rejection suite** the checkpoint's
only end-to-end evidence, exactly as it was in Week 26, where "nothing new this
week returns rows to diff" is written into the README.

### Conceptual explanation

Two suites, two jobs:

- **`WEEK30_SUBQUERY_BINDS`** — queries that must reach the *Week 31 refusal*.
  Reaching it is the proof that lexing, parsing, scoping, correlation detection
  and validation all succeeded. A parse error or a `column not found` here is a
  failure. Run in all four modes: the refusal is engine-independent by
  construction (Task 6) and running it everywhere is what proves that.
- **`WEEK30_REJECTED`** — queries that must fail *earlier*, each with its own
  message: position violations, arity violations, an `ON` subquery, a missing
  nested table, a missing nested column. These are what stop the Week 31 refusal
  from becoming a catch-all that hides real defects.

Both go through the existing `run_rejection_suite`, which already asserts the
message rather than the fact of failure — "matching the message is what stops an
unrelated failure from passing this suite".

Task 3's two queries are different: they **return rows**, so they belong in the
main suite where SQLite is a real oracle.

### Code

```python
# python_tools/compare_against_sqlite.py

# Week 30 — subquery parsing and binding. Nothing here executes; reaching the
# Week 31 refusal IS the assertion, because it is the last thing that runs and
# everything before it (lex, parse, scope resolution, correlation, validation)
# had to succeed to get there. SQLite answers all of them, which is why they are
# a rejection suite rather than a diff suite — the same stance Week 26 took when
# multi-way joins bound but did not execute.
WEEK30_SUBQUERY_BINDS = [
    # scalar, uncorrelated (Q22's shape)
    "SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)",
    # scalar, correlated (Q17's shape)
    "SELECT l.lap_id FROM laps l WHERE l.speed > "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)",
    # scalar in HAVING, uncorrelated (Q11's shape)
    "SELECT team, AVG(speed) FROM laps GROUP BY team "
    "HAVING AVG(speed) > (SELECT AVG(speed) FROM laps)",
    # EXISTS, correlated (Q4/Q21's shape) — and `select *` inside must be legal
    "SELECT d.name FROM drivers d WHERE EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 340)",
    # NOT EXISTS (Q21) — the leading-NOT production
    "SELECT d.name FROM drivers d WHERE NOT EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id)",
    # IN (subquery), uncorrelated (Q18/Q20) — a DIFFERENT production from the
    # constant list, which still parses as an InExpr
    "SELECT name FROM drivers WHERE driver_id IN (SELECT driver_id FROM laps)",
    # NOT IN (subquery) (Q16)
    "SELECT name FROM drivers WHERE driver_id NOT IN "
    "(SELECT driver_id FROM laps WHERE speed > 340)",
    # nested two deep, with the inner one correlated to the MIDDLE scope (Q20)
    "SELECT name FROM drivers d WHERE d.driver_id IN "
    "(SELECT l.driver_id FROM laps l WHERE l.speed > "
    " (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team))",
    # correlated across a JOIN in the outer query: the ref names relation 1, so
    # (query_level 1, relation_slot 1) — the shape that fails if the two
    # numbering domains were conflated
    "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "WHERE EXISTS (SELECT * FROM laps l2 WHERE l2.team = d.team)",
]
WEEK30_BIND_EXPECT = "not yet executable (Week 31)"

# Each of these must fail EARLIER than the refusal above, and for its own
# stated reason. Without them the Week 31 refusal becomes a catch-all that
# hides a real defect behind a temporary one.
WEEK30_REJECTED = [
    ("SELECT (SELECT AVG(speed) FROM laps) FROM drivers",
     "subqueries are supported in WHERE and HAVING only"),
    ("SELECT team FROM laps GROUP BY (SELECT AVG(speed) FROM laps)",
     "subqueries are supported in WHERE and HAVING only"),
    ("SELECT team FROM laps ORDER BY (SELECT AVG(speed) FROM laps)",
     "subqueries are supported in WHERE and HAVING only"),
    ("SELECT l.team FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id AND EXISTS (SELECT * FROM laps)",
     "JOIN ON: subqueries are not supported in a join condition"),
    ("SELECT team FROM laps WHERE speed > (SELECT speed, team FROM laps)",
     "must return exactly one column"),
    ("SELECT team FROM laps WHERE season IN (SELECT * FROM drivers)",
     "must return exactly one column"),
    # the nested query's OWN faults must outrank the refusal — this is what
    # proves the recursion ran
    ("SELECT team FROM laps WHERE EXISTS (SELECT * FROM nosuchtable)",
     "Table not found"),
    ("SELECT team FROM laps WHERE EXISTS (SELECT * FROM drivers WHERE nope = 1)",
     "column not found"),
]
```

### Implementation guidance

1. Wire both suites into all four modes, beside `WEEK26_REJECTED_QUERIES`'s
   loop, and update the expected totals in `development.md` (currently
   "596 passed" over 124 queries × 4 modes + 12 rejections × 4 + the multi-way
   split) and the unit-test count ("638 tests").
2. **Documentation this week invalidates** — none of it optional:
   - README, *Syntax Deliberately Not Supported*: the `IN (subquery)` row. Its
     stated message is wrong **today** (the real error is
     `Parse error ... Expected an expression (got 'SELECT')`, because `parseUnary`
     throws before `constantValue` is reached), and after this week the shape
     parses and binds. Rewrite the row, or move it out of the table into the
     Week 30 section — do not leave a table entry describing a message the
     engine has never produced.
   - README, *Limitations*: "No subqueries or correlated expressions" becomes
     "subqueries parse and bind; execution is Weeks 31–34".
   - README, grammar block: the three new productions, and `EXISTS` in the
     reserved-word list.
   - `development.md`, *Supported SQL* and the eighteen-site table.
   - `development.md`'s site-8 caller count, if Week 29's correction left
     anything stale — the README note says these drifted once already.
3. Write the Week 30 section's "Shipped / Why it was required" table as you go,
   not at the end. It is the artifact the next week's implementer reads, and the
   four Starting-note closures belong in it.

### Verification

```bash
python3 python_tools/compare_against_sqlite.py
# every WEEK30_SUBQUERY_BINDS entry PASS in all four modes, with the Week 31
# message; every WEEK30_REJECTED entry PASS with ITS OWN message
```

---

## The verification gate, and this week's success criteria

```bash
# 1. build + unit tests
cmake --build build -j$(nproc) && cd build && ./tests/swiftql_tests && cd ..

# 2. the SQLite oracle, all four modes. Two jobs this week: the new rejection
#    suites, and proof that NOTHING ELSE MOVED — this week touches the Binder,
#    which every query goes through
python3 python_tools/compare_against_sqlite.py

# 3. the steering / invariant harness (optimized ≡ --no-optimize, join steering)
python3 python_tools/test_new_queries.py

# 4. the project's full gate (the `verify` skill: build, unit tests, SQLite
#    harness, regression across all storage/execution modes)
```

### Success criteria

1. **Every form in the inventory table binds.** All nine
   `WEEK30_SUBQUERY_BINDS` entries reach the Week 31 refusal, in all four modes,
   with the identical message.
2. **Correlation is detected, and the two numbering domains stay apart.** The
   Week 30 binder tests assert `(query_level, relation_slot)` pairs, including
   the across-a-join case where the correct answer is `(1, 1)`.
3. **Nothing that worked stopped working.** Unit tests and the SQLite harness
   are green with *no expectation edits* except the two Task 3 queries (which
   move from erroring to returning rows) and the new suites.
4. **Task 3's two queries return rows**, and `AVG(laps.speed)` is still spelled
   `AVG(laps.speed)`.
5. **All four Starting notes are closed**, each with a code change or a stated
   reason: site 8 handled and the two headers' justification and caller count
   corrected; `preserved_slots` derived; alias rebinding fixed; the enumeration
   throw converted to a decline.
6. **Eighteen sites visited**, with a test per silent site and a written note for
   the four that need no change.
7. **The docs match the engine**: the `IN (subquery)` row, the Limitations
   bullet, the grammar, and both dispatch checklists.

### Hand forward

Write these into the README's Week 30 section as *Starting notes* for the weeks
that own them. Each is a real hazard, not a wish:

- **A shared subquery statement is one statement with two parents.** `cloneExpr`
  shares the `shared_ptr` (D2), so `BETWEEN`'s desugaring and the alias
  substitutions can produce two `SubqueryExpr` nodes over one
  `SelectStatement`. Binding is idempotent, so *binding* is safe. **Lowering is
  not**: Week 31 must decide whether two nodes sharing a statement build one
  subplan or two, and building two silently doubles the work while building one
  needs a cache keyed on the statement pointer. Nothing today produces the shape
  from the CLI — `BETWEEN` over a subquery is legal syntax and unreachable in
  TPC-H — but it is one query away.
- **`collectSlots` gives a correlated subquery `-1`, which is conservative, not
  exact.** Week 33 decorrelates, and at that point the exact set (correlated
  refs' slots, level-decremented) is what lets a decorrelated conjunct be pushed.
  Land the precise walker with the decorrelation, not before: today it would buy
  pushdown for a conjunct nothing can execute.
- **`buildScanSchema` widens to the full schema whenever a statement contains a
  subquery** (site 2). That is the safe direction and it costs projection
  pushdown for every subquery query. Week 33 replaces it with the correlated
  columns actually referenced; until then, no subquery query gets narrowed
  scans, which will show up in Week 31's first benchmark as a surprise.
- **`inferExprType` and `evaluate` throw a Week-31 message for `SubqueryExpr`.**
  The real rule for a scalar subquery is "the type of its single output column",
  which needs the subquery's projection schema — that is Week 31's first job,
  and both sites must be closed in the same commit that lowers one, or the
  vectorized path will pre-allocate an output column from a throw.
- **`JoinEnumeration` now declines a tree whose scans are not all range-table
  entries.** Week 31/34 make that live for the first time. The decline is silent
  by design; if a supported query starts paying a real plan-quality cost for it,
  that is when it earns a reported decision, in the shape Week 29's
  `join-ordering=skipped (outer join)` uses.
- **`Validator::validate`'s refusal is the only thing keeping a `SubqueryExpr`
  out of the logical plan.** Ten of the eighteen dispatch sites are handled but
  unexercised. The week that deletes the refusal inherits all ten at once and
  should re-read this task's table before it does, rather than after.

---

## As built — where the implementation departed from this plan

Recorded here rather than by silently editing the plan above, so the reasoning
and the correction are both readable.

1. **The single-relation shortcut had a schema-visible half the plan missed.**
   Task 2 said to replace `range_table.size() < 2 → slot 0` with the scope walk
   and keep the *fallback* unchanged. That is necessary but not sufficient: the
   shortcut also meant a single-relation query never reached the branch that
   **qualifies** a resolved ref by rewriting `table_name`, and
   `aggregateOutputName` is `exprToString`. Removing it renamed
   `SUM((speed * 2))` to `SUM((laps.speed * 2))` and broke 14 tests. Qualification
   is now conditional on the matching block holding ≥ 2 relations — exactly what
   the shortcut achieved by never reaching the loop. This is the same hazard
   Task 3 documents, arriving from the other direction.

2. **The enumeration decline had to be hoisted, and the deep throw kept.** Task 8
   flagged the risk; the answer is `hasSlotOutsideRangeTable`, a read-only walk
   of the written-order spine placed beside `containsOuterJoin` and *before*
   `decompose()`. The post-`decompose` throw stays in place as a dead invariant,
   so reordering the two passes fails loudly instead of silently returning a
   moved-from tree.

3. **`restampSlots` (site 9) ships without a test, deliberately.** The only route
   to its `SubqueryExpr` branch is a conjunct `soleSlot` pushes below a join, and
   no statement containing a subquery reaches `PredicatePushdown` this week.
   `soleSlot` and `restampSlots` are both file-local, so testing it would mean
   exporting a symbol for the test alone. Its correctness is the argument written
   at the site — a correlated subquery yields `-1`, so it is never pushed; an
   uncorrelated one has nothing in its body to restamp — and the first half of
   that argument *is* asserted, in
   `CollectSlots.CorrelatedSubqueryContributesTheConservativeSentinel`.

4. **The masked-error class is narrower than Task 4's verification implied, and
   is now stated in the code.** Every parse, bind and validate error precedes the
   refusal — proven by four harness entries and
   `SubqueryValidation.RealQueryDefectsOutrankTheNotExecutableRefusal`. A
   *plan-time* type error (`inferExprType` on the `WHERE`, `buildProjectSchema`
   on the select list) does not, because both run after `Validator`. Dispatch
   site 12 emits the identical string, so the user sees the same message either
   way; the alternative is a second refusal site per engine, which is the drift
   Week 29 audited out. The trade-off is written at the refusal.

5. **One harness query in Task 9's draft was invalid SQL.** `SELECT team AS t
   FROM laps l JOIN drivers d ...` is ambiguous — `team` is in both relations —
   so it errored on SwiftQL and SQLite alike. Replaced with a `nationality` /
   `speed` pair, each unique to one relation, which exercises the same GROUP BY
   alias rebind. Worth recording because the whole point of the third query was
   to reach the rebind through a *computed* select item, and picking a shared
   column would have tested the ambiguity check instead.

6. **Correlated references resolve outward and are marked**, rather than being
   left unresolved. The README's Week 30 bullet is "represent scalar,
   set-returning, and **correlated** subqueries", and without outward resolution
   the checkpoint's own queries do not bind — in two different ways, which the
   first version of this note got wrong by lumping them together. Q4, Q17 and Q20
   write their correlated references **unqualified**, so leaving them unresolved
   makes `validateExpr` report `column not found` against the subquery's own
   schema. Q21 writes its correlated references **qualified** — it aliases
   `lineitem` three times (`l2.l_orderkey = l1.l_orderkey`,
   `l3.l_suppkey <> l1.l_suppkey`) — so an unresolved ref there would not have
   reached `validateExpr` at all: the Binder itself throws
   `unknown table qualifier: 'l1'`. Both shapes fail without outward resolution,
   with different messages. Week 33 owns *decorrelation* — turning the marked
   reference into a join — which is untouched here.

---

## Round 1 — what the audit found, and what it changed

Two blockers, two majors, three minors. Every one was a *consumer* that collapsed
`(query_level, relation_slot)` back to a bare slot, or a place the week's own
rules were stated but not applied. The pattern is worth naming, because Week 31
inherits it: adding a field to `ColumnRef` is not the work — finding every reader
of the field it qualifies is.

| # | Finding | Fix |
|---|---|---|
| B1 | `validateJoinCondition` and `classifyJoinCondition` route a **nested** query's `ON` refs by slot against the *inner* range table. A correlated ref there is an ordinary top-level ref of that expression carrying an *outer* slot | `validateJoinCondition` skips `query_level > 0` (the Binder verified it where it resolved). `classifyJoinCondition` refuses to make a **key** out of such a ref, so a genuinely key-less nested join reaches the cross-product refusal instead of joining on a fabricated key |
| B2 | Binding was idempotent for stamps but **not** for `SubqueryExpr::correlated`: `markCorrelated` ran only on the resolution path, which the early return skips, so a second walk cleared the flag — reachable inside one `bind()` via `BETWEEN`'s shared-statement clone | Correlation is derived from the *stamp* on the idempotent path, so it survives any number of walks. That is what keeps `collectSlots`' `-1` — and therefore `restampSlots`' safety argument — true for every node over a shared statement |
| M3 | `query_level` was dropped on the `ColumnRef` → `GroupByColumn` round trip, and the validator's skip keyed on `!table_name.empty()`, which the `>= 2` qualification rule makes depend on the **enclosing** block's relation count | `GroupByColumn` carries `query_level`; the skip tests the level. `checkGroupedRefs` compares the level too, so a slot is never matched across two range tables |
| M4 | The `ORDER BY` position rule tested only the **root** node | Routed through `validateExpr`, whose `allow_subqueries=false` default is checked at every node — the same mechanism `SELECT` and `GROUP BY` already had. No bespoke recursive walker, which would have been a nineteenth silent dispatch site |
| m5 | `collectSlots`' new comment claimed a top-level ref is always level 0 — false, and the same class of false justification the week was handed as a finding | Corrected to say *why* the branch is there: `classifyJoinCondition` calls this walker on a nested query's `ON` conjuncts |
| m6 | Dispatch site 14 (`foldNode`) was the one place the week's own rule — descend into the `IN` operand, never the body — was not applied | Branch added. Week 32 probes a semi-join on exactly that operand, and three fast paths pattern-match on the shape folding restores |
| m7 | This note's Q21 evidence was wrong | Corrected above |

B1(b) and B2 were the two that would have produced **wrong rows** rather than a
wrong message once Week 31 lowers a subquery: a fabricated join key, and a
correlated conjunct pushed onto one relation's scan. Both are now pinned by tests
confirmed failing against the pre-fix tree.

---

## Round 2 — the same class, twice more

Zero blockers; the tests and both harness files were reviewed and showed no
weakening. Two majors and a minor, and all three are the round-1 pattern again:

| # | Finding | Fix |
|---|---|---|
| M1 | The `SUM`/`AVG` argument type check indexes `stmt.joins` — **this** block's list — by the argument's slot, and `validateQuery` recurses into every nested statement. The same illegal `SUM(d.name)` inside a subquery was caught or skipped depending on the **inner** query's own join order | The check skips a correlated argument, and the Binder makes it properly: `checkCorrelatedAggregateArg` resolves `(query_level, relation_slot)` through the scope chain to the exact range entry. Skipping alone would have left a `SUM` over a `STRING` reaching execution in Week 31 with no plan-time guard |
| M2 | `exprKey` encoded a `ColumnRef` as `slot#name` with no level, so two refs differing only in level hashed alike — a *correlated* expression `GROUP BY` key satisfied an ungrouped *local* reference, and `substituteInto` rewrites on the same key | The level is prefixed above 0 only, so every pre-existing key is byte-identical and no aggregate-spec dedupe or group-key match in a subquery-free query moves |
| m3 | `AggregateSpec` carries a slot with no level — the exposure `GroupByColumn` was given a field for | Contained rather than fielded: `extractAggregates` runs after the refusal, so a correlated ref can never reach it. The invariant is now stated at the field, in the shape `JoinKey::from_slot` uses, and named in the slot-consumer table |

Round 1 fixed two sites, round 2 found two more. That is the signal the fix
should stop being per-site, so this round's real deliverable is the
**slot-consumer table** in `development.md`: every reader of `relation_slot` /
`from_slot`, classified level-aware / contained / wrong, with the reason each
contained one is safe. It is split by whether a correlated ref can reach it at
all, because that boundary — `Validator::validate` refusing `has_subquery` — is
what makes the whole second group safe, and Weeks 32 and 34 are what remove it.

**The structural change, evaluated.** Making the level part of the type
(`ColumnId { level, slot }`, so a bare `int` cannot be passed where a qualified
reference is required) would turn all five of this week's findings into compile
errors, and it is the right end state. It is not a Week 30 change: `grep -c
'relation_slot\|from_slot' src/` reports 87 non-comment mentions across six
source layers, plus every test that hand-builds a `ColumnRef`, a `GroupByColumn`
or an `AggregateSpec`. More to the point, it buys nothing while the containment
holds — the second group of consumers cannot receive a correlated ref today at
all. The week that removes the containment is the week the change pays for
itself, and the table is what makes it reviewable then. Recommend deferring to
whichever of Week 31/32/34 first lowers a correlated reference; do not fold it
into a feature week.

---

## Round 3 — auditing the enumeration itself

Zero blockers. The round's first question was whether the round-2 slot-consumer
table is **complete**, and the answer was no: the two most dangerous consumers in
the tree were in neither half of it.

| # | Finding | Fix |
|---|---|---|
| M1 | `collectSimplePredicates` / `ChunkPruner::shouldSkip` (`chunk_pruner.h`) tests `relation_slot < 1` on a WHERE-clause `ColumnRef` and then matches **by name** against the scanned table's zone maps. A correlated `(level 1, slot 0)` reads as scan-local, so with a shared column name the wrong relation's zone maps prune the scan — chunks skipped silently. Not protected by the `collectSlots`/`soleSlot` `-1` argument: on `--no-optimize` the whole un-pushed `WHERE` reaches the scan as a hint without pushdown seeing it | **Declines** a `query_level > 0` ref. A pruning hint is an optimization, so contributing nothing is correct-and-slower — the file's own "decline and fall back" pattern. Plus the table row |
| M2 | Every `GroupByColumn` consumer — `buildAggregateSchema`, `HashAggregateNode`, `VecHashAggregateNode`, `CardinalityEstimator` — reads `g.relation_slot` bare, though `GroupByColumn` is the struct round 1 *gave* a level to. Quieter than a miss: `indexOf("team", 0)` against a subquery's `drivers` child schema is a clean HIT on the wrong relation, so neither the bare-name fallback nor the `idx < 0` throw fires | **Throws** at `buildAggregateSchema`. Grouping is not an optimization and a correlated key has no correct local fallback, so it must be loud. One guard covers all four: the other three run on a plan whose schema was built there |
| m1 | The round-2 `exprKey` level prefix is not prefix-free — level 1 / slot 23 and level 12 / slot 3 both render `^123#team`, the same collision one order of magnitude out. Both halves are legal SwiftQL | One `:` separator |
| m2 | Table hygiene: `restampSlots` was listed as reachable before the refusal (its only caller runs on a built plan), and the ORDER BY bare-column existence check — a correct level consumer — was not listed at all | Moved and added. Over-inclusion sends a reader's effort at the wrong row; omission of a correct entry is a hole in the audit trail |
| m3 | The moved `SUM`/`AVG` check runs during binding, so it outranks `Validator`'s aggregate-position rule for correlated arguments only | Documented at the call site and in Week 31's notes. Both spellings are refused; only the wording differs, and moving the check back re-opens what it closed |

**What this round changes about the method, not the code.** Rounds 1 and 2 fixed
sites; round 2 produced the table so the fix would stop being per-site; round 3
shows the table is itself an artifact that can be incomplete, and that an omission
is worse than an error — `ChunkPruner` was absent while being named elsewhere in
the same file answering the *dispatch-site* question, which a reader takes as
clearance. The preamble now says so, and the dispatch-checklist sentence that
caused it now points at the slot table.

Five appearances of one class in one week is the argument for the structural
change, and the decision is recorded in the README's Week 30 starting notes as a
decision rather than a suggestion: `ColumnId { level, slot }` is deferred to
whichever of Weeks 31/32/34 first lowers a correlated reference, as its own
standalone change, never folded into a feature week.
