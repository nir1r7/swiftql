# Week 26 — Multi-Way Join Language + Binding

Teaching plan for the Phase 5 / Week 26 checkpoint. Working branch:
`claude/phase5-week26-qomtkb` (everywhere the workflow says `main`, read that).

**Checkpoint (README):** *Multi-table queries produce a qualified logical join tree.*

That is the whole bar. **Execution is Week 27.** Nothing here lowers a general
join tree to `VecPlanNode`s, and nothing here makes a three-table query return
rows. TPC-H 22/22 is the phase goal; it informs *which* generalizations are worth
making (multi-key equi-joins for Q9, slot-correct pushdown for Q12/Q14/Q16/Q19),
but it is not this week's deliverable.

---

## What Week 26 must deliver

| README bullet | Tasks |
|---|---|
| Parse multiple explicit `JOIN ... ON` clauses | 1, 2 |
| Extend binding and logical planning to arbitrary relation counts | 3, 5, 6 |
| Lift the `classifyJoinCondition` restriction (multi-key equi-joins) | 4, 5 |

## The Starting notes, and where each is answered

| Starting note | Answered in |
|---|---|
| Relaxing `classifyJoinCondition` makes `Validator::validateJoinCondition` (site 18) a live silent dispatch site — extend both in the same commit | **Task 4 + Task 5**, which are one commit by construction |
| `relation_slot` is an `int` already; widen the range table to N. Four binary-assumption sites: `classify()`, `relation_slot == 1` in `validator.cc`, `ChunkPruner`'s `relation_slot < 1`, `SelectStatement::join` | Task 1 (`join` → `joins`, 14 src sites), Task 5 (`validator.cc` slot branch), Task 7 (`classify()`), Task 7 §"ChunkPruner stays as it is" |
| The pushdown hazard is `classify()`, and it is a **wrong answer** | **Task 7** — the one task that must land before the tree can grow a third scan |
| A second `JOIN` is a clean parse error today (Week 25's end-of-input rule), the correct pre-state | Task 2 §Verification — confirm the pre-state *before* editing, so the new tests prove the parser and not the old silent truncation |

## Design decision that shapes everything

One number ties the whole week together:

```
range-table slot of stmt.joins[i]'s relation  ==  i + 1
```

The Binder assigns slots positionally (`FROM` = 0, then each `JOIN` in written
order). Once `joins` is an ordered vector, every other layer derives its
generalization from that arithmetic:

- merged join schema stamps the newly added side with `i + 1` (Task 5),
- `classifyJoinCondition` routes keys by comparing against `i + 1` (Task 4),
- predicate pushdown routes a conjunct to the subtree owning its slot (Task 7),
- `validator.cc` maps slot `k > 0` back to `stmt.joins[k - 1].join_table` (Task 5).

Do **not** invent a second numbering. A scan's *local* schema keeps slot 0 (that
is what `restampSlots(..., 0)` and `ChunkPruner`'s `relation_slot < 1` test rely
on); global slots exist only on merged join schemas and on bound `ColumnRef`s.

**Prerequisite knowledge for the whole week:** how `relation_slot` flows —
`Binder::resolveColumnRef` stamps refs, `Schema::indexOf(name, slot)` resolves
them, `ColumnDef::relation_slot` stamps merged schemas. Read `src/planner/binder.cc`
and the `Schema` header before starting. Week 26 adds **no new `Expr` subtype**, so
the 18-site dispatch checklist in `development.md` is *not* a per-task chore this
week — only site 18 applies, and only because Task 4 relaxes the gate in front of it.

---

## Task 1 — `SelectStatement::join` → `SelectStatement::joins`

### Why it matters

`std::optional<JoinClause>` is the type-level encoding of "at most one join".
Fourteen sites in `src/` branch on it (plus five test helpers), and every one of
them is a place where a second relation could be silently dropped. Changing the
type is what turns the rest of the week into *compile errors* instead of silent
omissions — the same lever the codebase already uses for dispatch sites 11–13
(`cloneExpr`, `inferExprType`, `evaluate`), which throw precisely so mistakes are
loud. Downstream: Binder, Validator, `buildScanSchema`, `LogicalPlanBuilder`,
`Planner`, `constant_folding`, `main.cc`.

### Conceptual explanation

A left-deep join tree over relations `R0 JOIN R1 JOIN R2` is built by folding:
start with the scan of `R0`, then for each clause in written order attach the
next scan as the right child. An ordered `vector<JoinClause>` is exactly the
input that fold needs, and its index *is* the relation slot minus one. An
`optional` cannot express order because there is nothing to order.

### Code

```cpp
// src/parser/ast.h — inside SelectStatement
    // optional JOINs
    struct JoinClause {
        std::string join_table;
        std::string alias;                // empty if the JOIN table has no alias
        std::unique_ptr<Expr> condition;  // the ON expression
    };
    // Written order is load-bearing: joins[i] attaches relation slot i+1 to the
    // left-deep tree built from relations 0..i. That single identity is what the
    // Binder's range table, the merged join schema, join-key routing and
    // predicate pushdown all derive their slot arithmetic from. Empty = no join.
    // (Was std::optional<JoinClause> through Phase 4 — one join only.)
    std::vector<JoinClause> joins;
```

Mechanical migration idiom at the 14 call sites:

```cpp
// before                                  // after
if (stmt.join.has_value()) {               for (const auto& j : stmt.joins) {
    ... stmt.join->join_table ...              ... j.join_table ...
}                                          }

if (stmt.join.has_value()) foldNode(stmt.join->condition);   // constant_folding.cc:185
for (auto& j : stmt.joins) foldNode(j.condition);            // ->
```

### Implementation guidance

1. Change the field, then build. Fix the compile errors in this order —
   `binder.cc`, `validator.cc`, `logical_plan.cc`, `planner.cc`,
   `constant_folding.cc`, `main.cc`, then `tests/`.
2. In `main.cc` (~line 268) the CSV/stats load must loop over every join clause.
   Keep the `hasStats` guard: stats are table-scoped, and a self-join names the
   same table twice.
3. In `buildScanSchema` (`logical_plan.cc:243`) collect columns from **every**
   `ON` condition. Missing one narrows a join key out of the scan schema and the
   query dies later with `column not found` — dispatch-site failure mode #2, by
   another route.
4. Test helpers (`test_binder.cc:39`, `test_planner.cc:114`,
   `test_vec_plan_builder.cc:31,83`, `test_predicate_pushdown.cc:102`) load
   tables for the joined side. Loop them, and keep the `!table_rows.count(...)`
   guard — a self-join must not load twice.

**Gotchas**
- Do not rename `JoinClause` or its fields; the diff is large enough already.
- Resist "while I'm here" cleanups in `planner.cc`'s self-join copy logic — Task 8
  makes multi-join unreachable there anyway.

### Verification

Build clean, then `cd build && ./tests/swiftql_tests` — **524 tests, 0 failures**,
no behaviour change. This task is a pure refactor; if a test's *result* changes,
something was mistranslated.

---

## Task 2 — Parse multiple explicit `JOIN ... ON` clauses

### Why it matters

The parser is the only gate that decides whether a three-table query is a
syntax error or an AST. Everything downstream this week is unreachable until it
produces two `JoinClause`s. Downstream: the entire task list.

### Conceptual explanation

`parseSelect` is a straight-line sequence of optional clause parsers. The join
clause is currently `if (match(JOIN)) {...}`; multi-way is the same production
under a Kleene star. No precedence question arises, because `JOIN` is a keyword
token and the `ON` expression is parsed by `parseExpr()`, which stops at the
next keyword. Grammar delta:

```
select_stmt → SELECT [DISTINCT] select_list FROM table_ref
              (JOIN table_ref ON expr)*          ← Week 26: was [ ... ]
              [WHERE expr] ...
```

### Code

```cpp
// src/parser/parser.cc — in parseSelect(), replacing the single `if (match(JOIN))`
    // Explicit joins, in written order. `while`, not `if`: joins[i] attaches
    // relation slot i+1 (Week 26). Before this, a second JOIN fell through to
    // Parser::parse's end-of-input check and raised "unexpected trailing input",
    // which was the correct pre-state — pre-Week-25 it was silently discarded
    // and the query returned a one-join answer with no error.
    while (match(TokenType::JOIN)) {
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
        stmt.joins.push_back(std::move(join));
    }
```

### Implementation guidance

1. `if` → `while` plus `push_back`. That is the entire parser change.
2. The bare-alias branch needs **no** keyword exclusion list (unlike the `FROM`
   alias branch above it). `JOIN`, `ON`, `WHERE`, `GROUP`, `ORDER`, `LIMIT`,
   `HAVING` are their own `TokenType`s, so `check(TokenType::IDENTIFIER)` is
   already false for all of them. Adding an exclusion list here would be
   redundant code that reads like it is load-bearing.
3. Do not add `INNER` / `LEFT` / comma-joins. `LEFT OUTER` is Week 29; comma
   joins are not in the documented dialect.

**Gotcha:** `parseExpr()` for the `ON` clause greedily parses an `AND`-chain —
that is exactly what multi-key `ON` needs, and it is also why
`ON a.x = b.x AND a.season = 2025` parses fine and must be rejected *semantically*
in Task 4, not syntactically here.

### Verification

New tests in `tests/test_parser.cc`, alongside the existing `ParserTest.Join`:

```cpp
TEST(ParserTest, MultipleJoinClauses) {
    Parser p("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id "
             "JOIN sj c ON b.grp = c.id");
    auto stmt = p.parse();
    ASSERT_EQ(stmt.joins.size(), 2u);
    EXPECT_EQ(stmt.joins[0].alias, "b");
    EXPECT_EQ(stmt.joins[1].alias, "c");
}
```

Before editing the parser, confirm the pre-state the Starting notes describe:
that query must currently fail with `unexpected trailing input after the end of
the query`. If it fails with something else, stop — an assumption is wrong.

---

## Task 3 — Binder: an N-entry range table

### Why it matters

The Binder is the only place that assigns `relation_slot`. Every slot-based
mechanism downstream — `exprKey` identity, `Schema::indexOf(name, slot)`,
join-key routing, pushdown classification, `ChunkPruner`'s scan-local test —
consumes what this function stamps. If slots stop at 1, the rest of the week is
building on sand.

### Conceptual explanation

`Binder::bind` already builds a `std::vector<RangeEntry>`, and
`resolveColumnRef` already loops over it by index: qualified refs match
`ref_name` at any position, unqualified refs count matches across all entries
and reject ambiguity. **Those two functions need no changes.** The binary
assumption lives only in `bind()`'s construction: it pushes exactly two entries
and compares `range_table[0]` with `range_table[1]` for a duplicate ref name.

Two relations give one pair to check; N relations give N(N-1)/2. The check must
become "unique against every prior entry", because `FROM sj a JOIN sj b ... JOIN
sj a ...` is a duplicate that never touches the [0]/[1] pair.

### Code

```cpp
// src/planner/binder.cc — Binder::bind
    if (!catalog.hasTable(stmt.from_table)) return;
    for (const auto& j : stmt.joins) {
        // table existence stays Validator's error to raise (preserves its message)
        if (!catalog.hasTable(j.join_table)) return;
    }

    std::vector<RangeEntry> range_table;
    const Schema& from_schema = catalog.getTable(stmt.from_table).schema;
    range_table.push_back({
        stmt.from_alias.empty() ? stmt.from_table : stmt.from_alias,
        stmt.from_table,
        &from_schema
    });

    for (const auto& j : stmt.joins) {
        const Schema& join_schema = catalog.getTable(j.join_table).schema;
        range_table.push_back({
            j.alias.empty() ? j.join_table : j.alias,
            j.join_table,
            &join_schema
        });

        // Two relations sharing a ref name are unresolvable — every qualified
        // reference is ambiguous. Compare against EVERY prior entry, not just
        // the previous one: with three relations the clash can be between
        // entries 0 and 2. Distinguish the aliasless self-join (needs aliases,
        // matching SQLite) from a duplicated alias across tables.
        const RangeEntry& added = range_table.back();
        for (size_t prior = 0; prior + 1 < range_table.size(); ++prior) {
            if (range_table[prior].ref_name != added.ref_name) continue;
            if (range_table[prior].table_name == added.table_name) {
                throw std::runtime_error(
                    "self-join requires table aliases to disambiguate the two references to '"
                    + added.table_name + "'");
            }
            throw std::runtime_error(
                "duplicate table alias '" + added.ref_name
                + "': each side of a join needs a distinct name");
        }
    }
    ...
    for (auto& j : stmt.joins) bindExpr(j.condition.get(), range_table);   // was: if (stmt.join...)
```

### Implementation guidance

1. Note the message change: `stmt.from_table` → `added.table_name`. For two
   relations these are the same string, so the existing message is preserved
   byte-for-byte; for a `laps JOIN drivers JOIN laps` clash it now names the
   table that actually repeats.
2. `&join_schema` binds to a catalog-owned `Schema`; that reference is stable for
   the life of the `Catalog`, same as the existing code. Do not copy schemas into
   the range table.
3. Keep the `return` (not `throw`) on a missing table. Validator owns that
   message, and there are tests on it.
4. `resolveColumnRef`'s `range_table.size() < 2` early return stays — it is the
   single-relation fast path, not a binary assumption.

**Gotcha:** ambiguity gets *more common* with three relations. `laps` and
`drivers` both have `team`, so an unqualified `team` across a 3-way join now
throws `ambiguous column reference`. That is correct SQL behaviour, and it is
why the tests below qualify everything.

### Verification

`tests/test_binder.cc`:

```cpp
TEST(Binder, ThreeRelationsGetAscendingSlots) {
    Catalog cat(CATALOG);
    auto stmt = bindQuery(
        "SELECT a.id, b.grp, c.val FROM sj a "
        "JOIN sj b ON a.grp = b.id JOIN sj c ON b.grp = c.id", cat);
    EXPECT_EQ(dynamic_cast<ColumnRef*>(stmt.select_list[0].get())->relation_slot, 0);
    EXPECT_EQ(dynamic_cast<ColumnRef*>(stmt.select_list[1].get())->relation_slot, 1);
    EXPECT_EQ(dynamic_cast<ColumnRef*>(stmt.select_list[2].get())->relation_slot, 2);
}

TEST(Binder, DuplicateAliasAcrossNonAdjacentRelationsRejected) {
    Catalog cat(CATALOG);   // clash is entries 0 and 2, never the 0/1 pair
    ...expect "duplicate table alias" / "self-join requires table aliases"...
}
```

---

## Task 4 — `classifyJoinCondition`: multi-key equi-joins

### Why it matters

This function is the gate the Phase 4 audit installed so that a malformed `ON`
could never silently execute as something else (compound conditions produced
out-of-bounds key indices, `<` executed as `=`, same-relation conditions were
rerouted across sides). It is called from three places — `Validator::validate`,
`Planner::plan`, `LogicalPlanBuilder::build` — and it is also, per the Starting
notes, the thing standing in front of dispatch site 18. Relaxing it without
Task 5's validator work means `ON a.x = b.x AND a.team LIKE 'F%'` validates
nothing.

TPC-H Q9 joins `partsupp` to `lineitem` on `(ps_partkey, ps_suppkey)`. Without
multi-key equi-joins that query cannot be expressed at all.

### Conceptual explanation

An `ON` clause is a conjunction of predicates. Week 26 accepts exactly the shape
that hash-joins can execute as keys:

- **flatten** the `AND`-chain (the parser built it left-deep),
- each conjunct must be `ColumnRef = ColumnRef`,
- the two refs must be in **different** relations,
- **one of them must be the relation this JOIN introduces** (`right_slot`), and
  the other must belong to a relation already in the left tree (slot `< right_slot`).

Everything else still throws:

| Rejected | Why, and when it arrives |
|---|---|
| `OR` inside `ON` | Not a key list. No planned support |
| `<`, `!=`, ... | Non-equality `ON` conjuncts become post-join residuals in **Week 27** (TPC-H Q21) |
| `a.x = 5`, `a.x = b.y + 1` | Both sides must be plain column refs; a computed key is not a hash-table key |
| `a.x = a.y` | Same relation on both sides — a filter, not a join key |
| `a.x = c.x` in `FROM a JOIN b ON ...` where `c` joins later | Forward reference: `c` is not in the tree yet. New failure mode, only reachable with 3+ relations |

The right/left assignment is what `JoinKey` records, so the logical plan never
has to re-derive which side a key column lives on.

### Code

```cpp
// src/planner/join_condition.h
#pragma once

#include "parser/ast.h"
#include <string>
#include <vector>

// One equi-join key. from_col resolves against the already-joined (left) input,
// join_col against the relation this JOIN introduces. from_slot is the left
// column's binder slot — kept because the left input's merged schema can hold
// the same column name at several slots (laps.team and drivers.team), so a
// bare-name lookup there is a coin flip.
struct JoinKey {
    std::string from_col;
    std::string join_col;
    int from_slot = -1;
};

// Validate and decompose an ON condition into one or more equi-join keys.
// `right_slot` is the binder range-table slot of the relation this JOIN adds
// (stmt.joins[i] -> i + 1).
//
// Week 26 accepts a single equality or an AND-chain of them (multi-key
// equi-joins, required for TPC-H Q9). Non-equality operators, OR, computed
// operands and same-relation conjuncts still throw a specific error instead of
// silently degrading. Routing non-equality ON conjuncts as post-join residual
// filters is Week 27.
//
// Slot routing requires a bound statement. When a ref carries no slot
// (validator-only callers that skip the Binder), keys fall back to positional
// routing (left = already-joined side) and the cross-relation check is skipped
// — the real pipeline always binds first.
std::vector<JoinKey> classifyJoinCondition(const Expr* condition, int right_slot);
```

```cpp
// src/planner/join_condition.cc
namespace {

// Flatten an AND-chain without taking ownership — the ON tree still belongs to
// the statement. Same recursion shape as splitConjuncts() in predicate_pushdown.cc.
void flattenAnd(const Expr* pred, std::vector<const Expr*>& out) {
    auto* bin = dynamic_cast<const BinaryExpr*>(pred);
    if (bin && bin->op == "AND") {
        flattenAnd(bin->left.get(), out);
        flattenAnd(bin->right.get(), out);
        return;
    }
    out.push_back(pred);
}

} // namespace

std::vector<JoinKey> classifyJoinCondition(const Expr* condition, int right_slot) {
    std::vector<const Expr*> conjuncts;
    flattenAnd(condition, conjuncts);

    std::vector<JoinKey> keys;
    for (const Expr* c : conjuncts) {
        auto* bin = dynamic_cast<const BinaryExpr*>(c);
        if (!bin || bin->op == "OR") {
            throw std::runtime_error(
                "JOIN ON: condition must be an equality, or an AND-chain of equalities, "
                "between one column from each joined table");
        }
        if (bin->op != "=") {
            // Week 27 routes these as residual post-join filters; until then a
            // clean refusal beats executing '<' as '='.
            throw std::runtime_error(
                "JOIN ON: non-equality join conditions are not supported (got '" + bin->op + "')");
        }
        auto* lc = dynamic_cast<const ColumnRef*>(bin->left.get());
        auto* rc = dynamic_cast<const ColumnRef*>(bin->right.get());
        if (!lc || !rc) {
            throw std::runtime_error(
                "JOIN ON: both sides of the join equality must be column references");
        }

        if (lc->relation_slot < 0 || rc->relation_slot < 0) {
            keys.push_back({lc->column_name, rc->column_name, lc->relation_slot});
            continue;   // unbound: positional routing, as documented in the header
        }
        if (lc->relation_slot == rc->relation_slot) {
            throw std::runtime_error(
                "JOIN ON: condition must compare a column from each joined table; "
                "both sides reference '" + lc->table_name + "'");
        }
        // BOTH halves of the rule, or the same forward reference is accepted or
        // rejected purely on operand order.
        const ColumnRef* joined_ref;   // the side in relation `right_slot`
        const ColumnRef* left_ref;     // the side that must already be joined
        if (rc->relation_slot == right_slot)      { joined_ref = rc; left_ref = lc; }
        else if (lc->relation_slot == right_slot) { joined_ref = lc; left_ref = rc; }
        else {
            // Neither side is the relation being joined in: with two relations
            // this was unreachable, with three it is a forward reference
            // (ON a.x = c.x before c is joined) or a stale join.
            throw std::runtime_error(
                "JOIN ON: each condition must reference the table being joined; '"
                + lc->table_name + "." + lc->column_name + " = " + rc->table_name + "."
                + rc->column_name + "' does not");
        }
        if (left_ref->relation_slot > right_slot) {
            // Half two. keys[k].from_col is DEFINED to resolve against
            // children[0], so accepting a reference to a later relation rewires
            // the key to whatever column of that name the left tree has — and
            // explain() renders a plan indistinguishable from the correct one.
            throw std::runtime_error(
                "JOIN ON: '" + left_ref->table_name + "." + left_ref->column_name
                + "' references a table that is joined later; a condition may only "
                  "reference the table being joined and tables already joined");
        }
        keys.push_back({left_ref->column_name, joined_ref->column_name,
                        left_ref->relation_slot});
    }
    return keys;
}
```

### Implementation guidance

1. `struct JoinConditionKeys { from_col; join_col; }` is retired in favour of
   `std::vector<JoinKey>` — do not keep both, and do not keep a "first key plus
   extras" shape. A consumer that reads only the first key of a multi-key join
   returns wrong rows silently; deleting the type makes every consumer a compile
   error instead.
2. Update all three call sites in the same edit — they will not compile
   otherwise (that is the point). `Validator` and `LogicalPlanBuilder` iterate
   `stmt.joins` and pass `i + 1`; `Planner` passes `1` (it only ever sees one
   join after Task 8).
3. **Error-message substrings are pinned by existing tests.** Keep
   `"non-equality"` (two tests) and `"each joined table"` (one test) verbatim.
   `JoinOnValidation.LiteralOperandRejected` matches on `"column"`, which the new
   both-sides message still contains.
4. **`JoinOnValidation.CompoundConditionRejected` must be rewritten, not
   deleted.** It currently asserts `ON a.id = b.id AND a.grp = b.grp` is rejected
   with `"compound"` — that is precisely the restriction being lifted. Replace it
   with an acceptance test that checks two keys come back, and add a new
   rejection test for a *mixed* compound (`ON a.id = b.id AND a.grp < b.grp`)
   asserting `"non-equality"`. Leaving the old test to fail, or deleting it
   without a replacement, both lose the guard.
5. Do not "helpfully" accept `ON a.x = b.x AND a.season = 2025` by treating the
   second conjunct as a filter. It is a residual, and residuals are Week 27.

### Verification

`tests/test_binder.cc`, in the `JoinOnValidation` block:

```cpp
TEST(JoinOnValidation, MultiKeyEquiJoinAccepted) {
    Catalog cat(CATALOG);
    EXPECT_EQ(joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND a.grp = b.grp", cat), "");
}

TEST(JoinOnValidation, MixedCompoundWithNonEqualityRejected) { ... "non-equality" ... }
TEST(JoinOnValidation, ForwardReferenceToLaterRelationRejected) {
    // FROM sj a JOIN sj b ON a.grp = c.id JOIN sj c ON ...
    ... "table being joined" ...
}
```

Plus a direct unit test on the returned keys (order and side assignment):
`classifyJoinCondition` on `ON b.grp = a.id` with `right_slot = 1` must yield
`{from_col="id", join_col="grp", from_slot=0}` — i.e. it normalizes, it does not
trust operand order.

---

## Task 5 — Validator over N relations, and dispatch site 18

### Why it matters

`Validator::validateJoinCondition` is **site 18** in `development.md`'s dispatch
checklist, and its documented status is *"Dormant until Week 26 … extend this
function in the same commit that relaxes `classifyJoinCondition`."* It recurses
`ColumnRef` and `BinaryExpr` only. It is unreachable today because
`classifyJoinCondition` rejects everything that is not a single `=` between two
`ColumnRef`s — Task 4 removes that shield for `AND`-chains. Its failure mode is
silent: no column-existence check inside the new shapes.

`validator.cc` also carries the second of the four hardcoded binary assumptions:
the `col->relation_slot == 1` branch in the aggregate-argument type check, which
maps "slot 1" to "the join table".

### Conceptual explanation

Two independent generalizations:

1. **Slot → schema mapping.** Slot 0 is the `FROM` table; slot `k > 0` is
   `stmt.joins[k - 1].join_table`. Everywhere `validator.cc` reaches for "the
   join table's schema", it must index by that.
2. **Site 18's dispatch.** After Task 4, an `ON` clause is an `AND`-chain of
   comparisons — still only `BinaryExpr`/`ColumnRef` in the *accepted* shapes.
   But `validateJoinCondition` runs *before* rejection in some call orders, and
   Week 27 will legalize residual conjuncts containing Week 25 nodes. Extend the
   dispatch now, in this commit, exactly as `development.md` instructs: it is
   four small branches, and the alternative is a checklist entry that lies.

Note the ordering already in `Validator::validate`: shape first
(`classifyJoinCondition`), then column existence (`validateJoinCondition`). Keep
it — a `SUBSTRING` in an `ON` clause should report the shape error, not a
confusing column error.

### Code

```cpp
// src/planner/validator.h — the relation list replaces the (schema, name) x2 params
    static void validateJoinCondition(
        const Expr* expr,
        const std::vector<std::pair<std::string, const Schema*>>& relations);
```

```cpp
// src/planner/validator.cc
void Validator::validateJoinCondition(
        const Expr* expr,
        const std::vector<std::pair<std::string, const Schema*>>& relations) {
    if (!expr) return;

    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        if (col->table_name.empty()) {
            for (const auto& [name, schema] : relations) {
                if (schema->hasColumn(col->column_name)) return;
            }
            throw std::runtime_error(
                "JOIN ON: column '" + col->column_name + "' not found in any joined table");
        }
        for (const auto& [name, schema] : relations) {
            if (name != col->table_name) continue;
            if (!schema->hasColumn(col->column_name)) {
                throw std::runtime_error("JOIN ON: column '" + col->column_name
                    + "' not found in table '" + name + "'");
            }
            return;
        }
        return;   // unknown qualifier: an alias, resolved by the Binder
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        validateJoinCondition(bin->left.get(), relations);
        validateJoinCondition(bin->right.get(), relations);
        return;
    }
    // DISPATCH SITE 18. Silent on an unhandled subtype: no column-existence
    // check inside it. Dormant until Week 26 relaxed classifyJoinCondition;
    // Week 27 legalizes residual ON conjuncts, which is when these shapes
    // actually arrive. Keep in lockstep with validateExpr above.
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr))  { validateJoinCondition(un->operand.get(), relations); return; }
    if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)){ validateJoinCondition(isn->operand.get(), relations); return; }
    if (auto* in = dynamic_cast<const InExpr*>(expr))     { validateJoinCondition(in->operand.get(), relations); return; }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr))   { validateJoinCondition(lk->operand.get(), relations); return; }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        for (const auto& w : c->when_clauses) {
            validateJoinCondition(w.condition.get(), relations);
            validateJoinCondition(w.result.get(), relations);
        }
        validateJoinCondition(c->else_expr.get(), relations);   // nullptr-safe
        return;
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        validateJoinCondition(sub->operand.get(), relations);
        validateJoinCondition(sub->start.get(), relations);
        validateJoinCondition(sub->length.get(), relations);    // nullptr-safe
        return;
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        throw std::runtime_error(
            "JOIN ON: aggregate functions are not allowed in a join condition");
    }
    // Literal / IntervalLiteral: nothing to check
}
```

```cpp
// src/planner/validator.cc — Validator::validate, the JOIN block
    if (!stmt.joins.empty()) {
        // Keyed by REF NAME (alias when there is one), mirroring the Binder's
        // range table. Keying by table name makes every aliased qualifier fall
        // through validateJoinCondition's unknown-qualifier escape, so its
        // column check does nothing for exactly the shapes Week 26 adds.
        std::vector<std::pair<std::string, const Schema*>> relations{
            {stmt.from_alias.empty() ? stmt.from_table : stmt.from_alias, &schema}};
        for (const auto& j : stmt.joins) {
            if (!catalog.hasTable(j.join_table)) {
                throw std::runtime_error("Join table not found: '" + j.join_table + "'");
            }
            relations.push_back({j.alias.empty() ? j.join_table : j.alias,
                                 &catalog.getTable(j.join_table).schema});
        }
        for (size_t i = 0; i < stmt.joins.size(); ++i) {
            if (!stmt.joins[i].condition) continue;
            // shape first (equi-join keys), then column existence — a shape error
            // is the more useful message when both are wrong
            classifyJoinCondition(stmt.joins[i].condition.get(), static_cast<int>(i) + 1);
            validateJoinCondition(stmt.joins[i].condition.get(), relations);
        }
    }
```

```cpp
// src/planner/validator.cc — the aggregate-argument type check (the `== 1` branch)
            const Schema* target = nullptr;
            if (col->relation_slot > 0
                && col->relation_slot <= static_cast<int>(stmt.joins.size())) {
                // slot k > 0 is joins[k-1]'s relation — the one arithmetic
                // identity the whole multi-way generalization rests on
                target = &catalog.getTable(stmt.joins[col->relation_slot - 1].join_table).schema;
            } else if (col->relation_slot == 0 || col->table_name.empty()) {
                target = &schema;
            } else if (col->table_name == stmt.from_table) {
                target = &schema;
            } else {
                for (const auto& j : stmt.joins) {
                    if (col->table_name != j.join_table) continue;
                    target = &catalog.getTable(j.join_table).schema;
                    break;
                }
            }
```

### Implementation guidance

1. The remaining `validator.cc` join sites are the GROUP BY existence fallbacks
   (~lines 210, 214) for unbound callers — turn the two `stmt.join.has_value()`
   checks into loops over `stmt.joins`. They are name-based fallbacks; do not
   try to make them slot-aware.
2. Site 18's `AggregateExpr` branch **throws** rather than falling through. An
   aggregate in `ON` is meaningless, `classifyJoinCondition` already rejects it
   for accepted shapes, and a loud site is cheaper to debug than a silent one.
3. Update the site-18 row in `development.md` in this same commit (Task 9) — the
   note explicitly ties them together.

**Gotcha:** `relations` holds `const Schema*` into the catalog and
`std::pair<std::string, const Schema*>` copies the table name. Build it once per
statement, not once per conjunct.

### Verification

```cpp
TEST(JoinOnValidation, UnknownColumnInsideMultiKeyConditionRejected) {
    Catalog cat(CATALOG);
    // UNqualified: a qualified `a.nope` is caught earlier, by the Binder, so it
    // proves nothing about site 18. An unqualified name matching no relation is
    // left unresolved (slot -1), slips past classifyJoinCondition's
    // unbound-positional branch, and reaches validateJoinCondition — which is
    // the only thing standing between it and execution.
    std::string err = joinOnValidationError(
        "SELECT a.id FROM sj a JOIN sj b ON a.id = b.id AND nope = b.grp", cat);
    EXPECT_NE(err.find("nope"), std::string::npos) << err;   // assert on the message,
}                                                            // not just that it threw
```

Pin the other half too: a *qualified* bad column in the same chain is the
Binder's error, and the Week 25 shapes are still refused on shape by
`classifyJoinCondition` — so site 18's new branches are present but dormant
until Week 27. Say that in the tests rather than implying coverage that is not
there.

The "assert on the message" rule is not decoration — `development.md` cites
`Validation.UngroupedColumnInsideWeek25NodesIsRejected` as a test that passes for
the wrong reason without it.

---

## Task 6 — `LogicalJoin`: multi-key, N-way, slot-stamped

### Why it matters

This is the checkpoint itself: *multi-table queries produce a qualified logical
join tree*. "Qualified" is the operative word — the merged schema must carry a
distinct `relation_slot` per relation, or `Schema::indexOf(name, slot)` cannot
tell `laps.team` from `drivers.team` and every downstream resolution becomes a
first-match coin flip.

Downstream consumers of `LogicalJoin`: `CardinalityEstimator` (JOIN case),
`PredicatePushdown` (Task 7), `VectorizedPlanBuilder` (Week 27),
`--explain` output.

### Conceptual explanation

Build the tree by folding left:

```
                     LogicalJoin(join_slot=2)
                    /                        \
      LogicalJoin(join_slot=1)          LogicalScan(R2)   ← slots stamped 2
     /                       \
LogicalScan(R0)         LogicalScan(R1)                   ← slots stamped 1
  (local slot 0)
```

Schema stamping rule: **the left child's schema is copied unchanged** (it already
carries slots `0 .. join_slot-1` from the joins beneath it); only the newly added
right side is stamped with `join_slot`. A standalone scan keeps slot 0 locally —
that local/global split is what keeps `restampSlots(..., 0)` and `ChunkPruner`'s
`relation_slot < 1` test correct at any relation count (Task 7).

`LogicalJoin` therefore needs two new pieces of state: the key vector, and
`join_slot` (the plan tree has no access to the statement, so the estimator and
pushdown cannot re-derive it).

### Code

```cpp
// src/planner/logical_plan.h
#include "planner/join_condition.h"   // JoinKey

// inner equi-join. keys[k].from_col resolves against children[0]'s schema,
// keys[k].join_col against children[1]'s. Multi-key since Week 26 (TPC-H Q9).
// join_slot is the binder relation slot of children[1] — it stamps the merged
// schema and is what predicate pushdown routes conjuncts by. Left-deep only:
// children[1] is always a single relation (join enumeration, Week 28, keeps
// that shape).
struct LogicalJoin : LogicalPlanNode {
    std::vector<JoinKey> keys;
    int join_slot;

    LogicalJoin(std::unique_ptr<LogicalPlanNode> from_child,
                std::unique_ptr<LogicalPlanNode> join_child,
                std::vector<JoinKey> keys, int join_slot, Schema merged)
        : LogicalPlanNode(LogicalNodeType::JOIN, std::move(merged)),
          keys(std::move(keys)), join_slot(join_slot) {
        children.push_back(std::move(from_child));
        children.push_back(std::move(join_child));
    }
    std::string explain() const override;
};
```

```cpp
// src/planner/logical_plan.cc
std::string LogicalJoin::explain() const {
    std::string s = "LogicalJoin [";
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) s += " AND ";
        s += keys[i].from_col + " = " + keys[i].join_col;
    }
    return s + "]";
}
```

```cpp
// src/planner/logical_plan.cc — LogicalPlanBuilder::build, replacing the `if (stmt.join...)` block
    // joins, folded left-deep in written order: joins[i] attaches relation i+1
    for (size_t i = 0; i < stmt.joins.size(); ++i) {
        const auto& jc = stmt.joins[i];
        const int join_slot = static_cast<int>(i) + 1;   // range-table position

        const TableMetadata& join_meta = catalog.getTable(jc.join_table);
        auto join_scan = std::make_unique<LogicalScan>(
            jc.join_table, buildScanSchema(stmt, join_meta.schema));

        // routes keys by binder-assigned slot — the only way to disambiguate a
        // self-join's occurrences of the same table
        std::vector<JoinKey> keys = classifyJoinCondition(jc.condition.get(), join_slot);

        // Output schema order is always [relation 0 columns, relation 1, ...] —
        // fixed logical order. The left child already carries slots 0..join_slot-1;
        // only the newly added side is stamped, so qualified references resolve to
        // the correct relation even when several share a column name. By-value loop
        // var: a reference would mutate the join scan's own schema.
        std::vector<ColumnDef> merged_cols = node->output_schema.columns();
        for (ColumnDef col : join_scan->output_schema.columns()) {
            col.relation_slot = join_slot;
            merged_cols.push_back(col);
        }

        // no build/probe swap decision here — that's a physical concern
        node = std::make_unique<LogicalJoin>(std::move(node), std::move(join_scan),
                                             std::move(keys), join_slot, Schema(merged_cols));
    }
```

```cpp
// src/planner/cardinality_estimator.cc — JOIN case, kept in lockstep with the merge above
            // one NDV per key; independent-key assumption, same shape as the
            // single-key formula it generalizes
            // Track "did any key contribute an NDV" SEPARATELY from the
            // product. An NDV of 1 is a usable statistic (every left row
            // matches every right row, so l*r/1 is exact) but leaves the
            // divisor at 1.0 — testing `divisor > 1.0` sends it to the
            // no-statistics fallback and underestimates a constant-key join by
            // the table size.
            double divisor = 1.0;
            bool have_ndv = false;
            for (const JoinKey& k : join.keys) {
                const ColumnStatsEntry* lk = left.find(k.from_col, k.from_slot);
                const ColumnStatsEntry* rk = right.find(k.join_col, -1);
                int64_t ndv = std::max(lk ? lk->stats->distinct_count : int64_t(0),
                                       rk ? rk->stats->distinct_count : int64_t(0));
                if (ndv > 0) { divisor *= static_cast<double>(ndv); have_ndv = true; }
            }
            node.estimated_rows = have_ndv ? (l * r) / divisor : std::max(l, r);
            ...
            StatsContext out = std::move(left);
            for (ColumnStatsEntry e : right.entries) {
                e.relation_slot = join.join_slot;   // was hardcoded 1
                out.entries.push_back(std::move(e));
            }
```

### Implementation guidance

1. Do the header change first and let the compiler enumerate the consumers. Every
   one of them is a place that assumed a single key.
2. `explain()` for a single key is byte-identical to the old output
   (`LogicalJoin [driver_id = driver_id]`), so existing explain assertions keep
   passing. Verify that rather than assuming it.
3. `Planner::plan` (Volcano) builds physical nodes directly and has no
   `LogicalJoin`; it consumes `classifyJoinCondition` output and needs
   `keys[0]` plus Task 8's guard.
4. `join_condition.h` is now included by `logical_plan.h`. It only pulls in
   `parser/ast.h`, which `logical_plan.h` already includes — no cycle.
5. **Tradeoff, stated plainly:** `JoinKey::from_slot` is one extra field. It is
   populated for free in Task 4 (the classifier already holds both slots) and is
   *used this week* by the estimator's `left.find(k.from_col, k.from_slot)`.
   Without it, that lookup is bare-name on a merged schema where the same name
   can appear at several slots — a wrong estimate today and a wrong column bind
   in Week 27. Carrying it is smaller than reconstructing it later.

### Verification

`tests/test_logical_plan.cc`:

```cpp
TEST(LogicalPlan, ThreeWayJoinIsLeftDeepWithAscendingSlots) {
    Catalog cat(CATALOG);
    auto plan = buildLogical(
        "SELECT a.id, c.val FROM sj a JOIN sj b ON a.grp = b.id "
        "JOIN sj c ON b.grp = c.id", cat);

    const auto* top = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_NE(top, nullptr);
    EXPECT_EQ(static_cast<const LogicalJoin*>(top)->join_slot, 2);
    EXPECT_EQ(top->children[0]->type, LogicalNodeType::JOIN);   // left-deep
    EXPECT_EQ(top->children[1]->type, LogicalNodeType::SCAN);

    // qualified: every relation owns its slot in the merged schema
    std::set<int> slots;
    for (const auto& col : top->output_schema.columns()) slots.insert(col.relation_slot);
    EXPECT_EQ(slots, (std::set<int>{0, 1, 2}));
}

TEST(LogicalPlan, MultiKeyJoinKeepsBothKeys) {
    // ON a.id = b.id AND a.grp = b.grp -> keys.size() == 2,
    // explain() renders "LogicalJoin [id = id AND grp = grp]"
}
```

This pair of tests *is* the Week 26 checkpoint. Everything else supports them.

---

## Task 7 — `classify()` returns a slot, not a side

### Why it matters

From the Starting notes, verbatim in substance: this is a **wrong answer**, not
just lost pushdown. `classify()` collapses "references neither slot 0 nor a mix"
into `PushTarget::JOIN`, and `pushIntoJoin` attaches those conjuncts to
`join->children[1]`. With two relations that is exactly right. With three in a
left-deep tree, `children[1]` is one specific scan, so a conjunct belonging to a
different relation is filtered **against the wrong table** — evaluated against a
schema where its column either does not exist (loud) or, worse, exists with a
different meaning (silent).

Task 6 is what grows the third scan. This task must land in the same commit, or
the tree that lands is a bug generator.

### Conceptual explanation

Replace the three-way `FROM / JOIN / RESIDUAL` enum with:

- **which slot** a conjunct references (`-1` for none, several, or unresolved), then
- **which subtree owns that slot**, walking the left-deep spine: at a `LogicalJoin`,
  `children[1]` *is* relation `join_slot`; every lower slot is inside `children[0]`;
  at the bottom of the spine sits relation 0's scan.

Re-stamping is unchanged in *intent*: a conjunct pushed below the join executes
against a standalone scan whose schema stamps every column slot 0, so its refs are
re-stamped to 0. Slot-0 conjuncts are already 0 and need no re-stamp.

**`ChunkPruner` stays exactly as it is.** Its `relation_slot < 1` scan-local test
looks wrong for N relations and is not: it only ever sees conjuncts that have
already been pushed below a join and re-stamped to 0, or conjuncts sitting above a
standalone single-relation scan. Both are slot 0 at any relation count. The
`>= 1` refs it must keep ignoring are the residual/un-pushed ones routed to the
FROM scan as a pruning hint, where a shared column name would make name-based
pruning wrong. Do not touch it — and do not "fix" it to `< N`.

### Code

```cpp
// src/planner/predicate_pushdown.cc — replaces PushTarget / classify()

// The single relation slot a conjunct references, or -1 when it references none
// (constant), several (a cross-relation residual), or an unresolved ref.
//
// Week 26: this returns a SLOT, not a side. The old three-way enum collapsed
// "not slot 0" into JOIN, and pushIntoJoin attached those conjuncts to
// join->children[1]. With two relations that was exactly right; with three or
// more in a left-deep tree children[1] is one specific scan, so a conjunct
// belonging to a different relation was filtered against the wrong table.
int soleSlot(const Expr* conjunct) {
    std::unordered_set<int> slots;
    collectSlots(conjunct, slots);
    if (slots.size() == 1 && !slots.count(-1)) return *slots.begin();
    return -1;
}

// Attach each bucket to the subtree that owns its relation. The tree is
// left-deep: at a JOIN, children[1] is exactly relation `join_slot` and every
// lower slot lives in children[0]; the bottom of the spine is relation 0's scan.
std::unique_ptr<LogicalPlanNode> distribute(
        std::unique_ptr<LogicalPlanNode> node,
        std::map<int, std::vector<std::unique_ptr<Expr>>>& by_slot,
        const Catalog& catalog) {
    if (node->type == LogicalNodeType::JOIN) {
        auto* join = static_cast<LogicalJoin*>(node.get());
        auto it = by_slot.find(join->join_slot);
        if (it != by_slot.end()) {
            // below the join these execute against a standalone scan, whose
            // schema stamps every column slot 0 — re-stamp so slot lookups hit
            // directly and ChunkPruner can act on the hint
            for (auto& c : it->second) restampSlots(c.get(), 0);
            join->children[1] = filterOnto(std::move(join->children[1]),
                                           std::move(it->second), catalog);
            by_slot.erase(it);
        }
        join->children[0] = distribute(std::move(join->children[0]), by_slot, catalog);
        return node;
    }

    // bottom of the left spine: relation 0's scan (already slot 0, no re-stamp)
    auto it = by_slot.find(0);
    if (it == by_slot.end()) return node;
    auto conjuncts = std::move(it->second);
    by_slot.erase(it);
    return filterOnto(std::move(node), std::move(conjuncts), catalog);
}

// Rewrite a WHERE filter sitting directly above a join tree.
std::unique_ptr<LogicalPlanNode> pushIntoJoin(std::unique_ptr<LogicalFilter> filter,
                                              const Catalog& catalog) {
    auto join = std::unique_ptr<LogicalPlanNode>(filter->children[0].release());

    std::vector<std::unique_ptr<Expr>> conjuncts;
    splitConjuncts(std::move(filter->predicate), conjuncts);   // filter is now empty

    // std::map, not unordered_map: the leftover loop below must be deterministic
    std::map<int, std::vector<std::unique_ptr<Expr>>> by_slot;
    std::vector<std::unique_ptr<Expr>> residual;
    for (auto& c : conjuncts) {
        int slot = soleSlot(c.get());
        if (slot < 0) residual.push_back(std::move(c));
        else          by_slot[slot].push_back(std::move(c));
    }

    join = distribute(std::move(join), by_slot, catalog);

    // Nothing should be left: slots come from the same range table the tree was
    // built from. Anything unclaimed stays above the join — correct, just slower.
    for (auto& [slot, parts] : by_slot)
        for (auto& c : parts) residual.push_back(std::move(c));

    return filterOnto(std::move(join), std::move(residual), catalog);
}
```

### Implementation guidance

1. `PredicatePushdown::apply`'s dispatch does **not** change: a `FILTER` whose
   child is a `JOIN` is still the entry point (the `WHERE` sits above the whole
   left-deep tree), and `FILTER`-over-`SCAN` still just orders conjuncts.
2. `collectSlots` and `restampSlots` (sites 8 and 9) need no new branches — Week
   26 adds no `Expr` subtype. They still must stay in lockstep with each other.
3. `filterOnto` → `orderByWork` → `scanStats` returns an empty context unless the
   child is a `SCAN`. Every push target here is a scan, so conjunct ordering keeps
   working; do not add stats plumbing for join subtrees (that is Week 28's
   cost-aware ordering, which `development.md` explicitly defers).
4. Keep the leftover-bucket loop even though it should never fire. It converts a
   future structural surprise (a bushy tree from Week 28) into a slow-but-correct
   plan rather than a dropped predicate.

**Gotcha:** the old code cast `filter->children[0]` to `LogicalJoin*` immediately.
`distribute` needs the generic node type because it recurses through both joins
and the bottom scan — do not reintroduce the eager cast.

### Verification

Existing pushdown tests must all still pass unchanged (two-relation behaviour is
a special case of the new routing) — that is the primary regression signal. Then
add to `tests/test_predicate_pushdown.cc`:

```cpp
TEST(PredicatePushdown, ThreeWayJoinPushesEachConjunctToItsOwnRelation) {
    Catalog cat(CATALOG);
    auto plan = buildPushed(
        "SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id JOIN sj c ON b.grp = c.id "
        "WHERE a.val > 100 AND b.val > 150 AND c.val > 200", cat);

    // top join owns relation 2: its right child carries c's predicate
    const auto* top = findNode(plan.get(), LogicalNodeType::JOIN);
    ASSERT_EQ(top->children[1]->type, LogicalNodeType::FILTER);
    EXPECT_NE(exprToString(static_cast<const LogicalFilter*>(top->children[1].get())
                           ->predicate.get()).find("200"), std::string::npos);

    // inner join owns relation 1; relation 0's filter sits on the bottom scan
    const auto* inner = top->children[0].get();
    ASSERT_EQ(inner->type, LogicalNodeType::JOIN);
    ... assert "150" on inner->children[1], "100" on inner->children[0] ...

    // and nothing was left above the join tree
}
```

Write the *negative* form too: assert that `c`'s predicate is **not** on `b`'s
scan. That is the exact bug the old `classify()` would produce, and a test that
only checks "three filters exist" would pass with all three misrouted.

---

## Task 8 — A clean refusal at physical lowering, until Week 27

### Why it matters

Week 26 ends with a logical tree that no executor can run. The project's stated
principle — repeated across the README's divergence table — is that an
unsupported query must be *a clean error, not a wrong answer*. Phase 1 set the
precedent exactly here: hash join was "parsed and planned in this phase but
execution is stubbed — join queries return a clean not-yet-implemented error at
runtime."

Without this task, a three-way join would reach `VectorizedPlanBuilder`, whose
build-side decision walks `children[0]` to a leaf scan and whose
`VecHashJoinNode` takes a single key pair — it would produce *something*,
unverified, and probably wrong.

### Conceptual explanation

The refusal belongs at **physical lowering only**. Putting it in
`LogicalPlanBuilder` would fail the checkpoint (the logical tree must build);
putting it in the parser or validator would fail Tasks 2–5. Two builders need
the guard: `VectorizedPlanBuilder::build` and `Planner::plan` (row/Volcano).

### Code

```cpp
// src/planner/vectorized_plan_builder.cc — pre-pass in build(), beside countScans
// Week 26/27 boundary: the logical layer now builds arbitrary join trees, but
// this builder still lowers exactly one single-key equi-join. Refuse loudly
// rather than lower a shape VecHashJoinNode cannot express — the Phase 1
// stubbed-hash-join stance: a clean "not yet implemented" beats a wrong answer.
// Two walks, join count first — the same order Planner::plan checks in. A query
// that is both multi-way and multi-key must report the same reason in every
// mode; interleaving the checks in one preorder walk made the vec path answer
// "multi-key" where Volcano answered "multi-way".
void countJoins(const LogicalPlanNode* node, int& joins_seen) {
    if (node->type == LogicalNodeType::JOIN) ++joins_seen;
    for (const auto& child : node->children) countJoins(child.get(), joins_seen);
}

void checkKeyCounts(const LogicalPlanNode* node) {
    if (node->type == LogicalNodeType::JOIN &&
        static_cast<const LogicalJoin*>(node)->keys.size() != 1) {
        throw std::runtime_error(
            "multi-key equi-joins are planned but not yet executable (Week 27)");
    }
    for (const auto& child : node->children) checkKeyCounts(child.get());
}

void checkLowerable(const LogicalPlanNode* root) {
    int joins_seen = 0;
    countJoins(root, joins_seen);
    if (joins_seen > 1) {
        throw std::runtime_error(
            "multi-way joins are planned but not yet executable (Week 27)");
    }
    checkKeyCounts(root);
}
```

```cpp
// src/planner/planner.cc — Volcano path, right after Validator::validate
    if (stmt.joins.size() > 1) {
        throw std::runtime_error(
            "multi-way joins are planned but not yet executable (Week 27)");
    }
    ...
    std::vector<JoinKey> keys = classifyJoinCondition(stmt.joins[0].condition.get(), 1);
    if (keys.size() != 1) {
        throw std::runtime_error(
            "multi-key equi-joins are planned but not yet executable (Week 27)");
    }
```

### Implementation guidance

1. Count joins over the **whole tree**, not by inspecting `children[0]->type`.
   After pushdown a join's child is a `FILTER` over a `JOIN`, and a type check
   one level down would miss it.
2. Use one message string per case in both builders so a user sees the same text
   in every mode.
3. `main.cc` already catches `std::exception` and prints `Error: ...` with exit
   code 1 — nothing to add there.
4. **Known one-week limitation, accept it:** `--explain` on the vectorized path
   prints its captured sections *after* lowering, so `--explain` of a three-way
   join also hits this error. Reordering `main.cc` to print the logical section
   first is real work for one week of benefit, and Week 27 makes it moot. The
   checkpoint is demonstrated by the Task 6 unit tests. Do not build it.

### Verification

```cpp
TEST(VecPlanBuilder, ThreeWayJoinRefusedUntilWeek27) {
    Catalog cat(CATALOG);
    try {
        buildVec("SELECT a.id FROM sj a JOIN sj b ON a.grp = b.id "
                 "JOIN sj c ON b.grp = c.id", cat);
        FAIL() << "expected a refusal";
    } catch (const std::runtime_error& e) {
        // assert on the message: "something threw" does not prove the right
        // guard caught it — a missing table or a bad key would also throw
        EXPECT_NE(std::string(e.what()).find("not yet executable"), std::string::npos)
            << e.what();
    }
}
```

CLI smoke test — the error must be clean, not a crash or a wrong table:

```bash
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized \
  --query "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id \
           JOIN drivers d2 ON d.driver_id = d2.driver_id"
# Error: multi-way joins are planned but not yet executable (Week 27)   -> exit 1
```

---

## Task 9 — Documentation, and the verification gate

### Why it matters

Two documents make specific, checkable claims that Week 26 falsifies. Leaving
them stale is how the next week's developer (you) builds on a false premise —
which is exactly the failure `development.md`'s own dispatch-checklist correction
in Week 25 was about.

### What to change

| File | Change |
|---|---|
| `development.md` | Site 18 row: drop "**Dormant until Week 26**", state that Week 26 extended it and Week 27 will exercise it with residual `ON` conjuncts. Update the "Supported SQL" block: `[JOIN other_table ON ...]` → repeatable, with `AND`-chained equality keys |
| `README.md` | Week 26 checkpoint ✅ plus a short "shipped / why it was required" note in the Week 24–25 house style: `join` → `joins`, `classify()` returning a slot, site 18. Grammar block: `[JOIN table_ref ON expr]` → `(JOIN table_ref ON expr)*`. Limitations: "Single join only — multi-way joins not supported" → multi-way joins parse, bind and plan; execution is Week 27 |

**Do not** add multi-way join queries to the row-diffing suites in
`compare_against_sqlite.py` this week — they cannot execute, so there is nothing
to diff. That is Week 27's step 5 in the recommended order ("land the feature,
then add queries").

What *does* belong in the oracle now is the other half of the contract: a
`WEEK26_REJECTED_QUERIES` block plus a `run_rejection_suite`, run in the same
four modes, asserting each unsupported shape fails **with its own message**.
SQLite is not the oracle there — it accepts all of them; the property under test
is SwiftQL's own "clean error, never a wrong answer". It is strictly additive:
no existing comparison is relaxed, and matching the message is what stops an
unrelated failure from passing. Cover both refusal sites (Volcano and
vectorized), both shape rejections (`non-equality`, `AND-chain`), the forward
reference, both duplicate-name diagnostics, and the now-more-likely ambiguous
unqualified column.

### Verification — the gate for the whole week

Run in this order, from the project root; every step must pass before the week is
done (`verification-before-completion`: evidence before assertions):

```bash
cmake --build build -j$(nproc)

cd build && ./tests/swiftql_tests
# 524 existing + the new Week 26 tests, 0 failures.
# Must be run from inside build/ — tests resolve "../catalog.json".

cd .. && python3 python_tools/compare_against_sqlite.py
# 440 passed, 0 failed, 0 errors — unchanged. This is the regression signal:
# 110 queries x 4 modes, all still single-join, proving the `joins` refactor
# and the pushdown rewrite did not disturb the two-relation path.
```

Then three CLI smoke checks:

```bash
# 1. single join, all three modes, results identical to before
./build/swiftql --catalog catalog.json --query \
  "SELECT l.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team"
./build/swiftql --catalog catalog.json --storage columnar --query "...same..."
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized --query "...same..."

# 2. three-way join: clean refusal, exit 1, no crash (Task 8)
# 3. malformed ON: the specific error, not a generic one
./build/swiftql --catalog catalog.json --query \
  "SELECT a.id FROM sj a JOIN sj b ON a.grp < b.id"   # -> "non-equality"
```

### Corrections from the round-1 audit

Four things below were wrong in this plan as first written; the code and the
snippets above now carry the fixed form. Recorded because the failure mode is
identical in each: a rule stated in prose but only half-checked in code.

| Was | Why it was wrong |
|---|---|
| `classifyJoinCondition` checked only "one side is the relation being joined" | The rule has two halves. Without the second (`the other operand's slot < right_slot`), the *same* forward reference is rejected on the right operand and accepted on the left — and an accepted one rewires `keys[0].from_col` to a column of the left tree, producing a wrong join tree that `explain()` renders identically to a correct one. The borrowed name need not even exist in that relation |
| `divisor > 1.0` in the estimator | An NDV of 1 is a usable statistic, not a missing one. It fell to the `max(l, r)` fallback and underestimated a constant-key join by the table size, on the *single-key* path this task called behaviour-preserving. Track `have_ndv` separately |
| One preorder walk in `checkLowerable` | A query that is both multi-way and multi-key reported different reasons on the two engines, breaking the "same text in every mode" rule this plan states two tasks earlier. Count joins first, keys second |
| `relations` keyed by table name | Every aliased qualified `ON` reference fell through the unknown-qualifier escape, so site 18 checked nothing for exactly the shapes Week 26 adds. Key by ref name |

The lesson for later weeks: when a task's prose states a conjunction, write one
test per conjunct, and write the operand-swapped form of every asymmetric rule.
`JoinOnValidation.KeysNormalizeOperandOrder` existed *because* operand order is
not trusted — and the forward-reference test was still written one way only.

Round 2 found two more. One was fixed; one was considered and deliberately left.

| Finding | Outcome |
|---|---|
| The join estimator's new slot-keyed left lookup went through `StatsContext::find`, whose bare-name fallback returns *some other relation's* column when the slot misses — and it misses whenever the key's own relation has no `TableStats`, because a stats-less scan contributes no entries | **Fixed.** `from_slot` was introduced this week exactly to disambiguate a merged context that can hold one name at several slots; honouring it only when it happens to hit makes it advisory. The JOIN case now uses a slot-exact `findExact`, so a miss means "no statistic" — which `have_ndv` already models. `find`'s fallback stays for unbound refs and hand-built contexts, which have no relation identity to be exact about |
| A query that is *both* unlowerable and otherwise malformed reports the type error on the vec path and the Week-27 refusal on Volcano | **Left in place, deliberately.** Volcano has no logical layer: `Planner::plan` builds physical operators directly, and `HashJoinNode` holds one key pair and two inputs, so a multi-way or multi-key query can neither be represented nor reach the plan-time type checks that follow. Aligning the two would mean giving Volcano multi-way planning it will never execute. Both messages are true, neither engine accepts what the other rejects, and Week 27 turns the divergence into an ordinary capability difference. Documented in the README's Week 26 scope paragraph and folded into the Week 27 note about narrowing Volcano's message |

Round 3 overturned one of those calls and found a regression in a round-1 fix.

| Finding | Outcome |
|---|---|
| Site 18's ref-name keying (the round-1 MIN-3 fix) **rejected a legal single-join query** in all four modes: `Binder::resolveColumnRef` rewrites an *unqualified* ref's `table_name` to its relation's TABLE name, so a relation aliased to that name matched instead and the column was checked against the wrong schema | **Fixed.** For a bound ref, check the relation the Binder resolved it against — by slot, since `relations` is built in range-table order. Name matching survives only for the validator-only callers the round-1 fix was written for. The fix's own helper skipped the Binder, and `development.md` restated the same false premise, which is what hid it |
| The round-2 rejection of the divergent-refusal finding was **wrong for the multi-key case** | **Overturned and fixed.** The merged join schema is built from the two children's schemas and never reads `keys`, so the plan-time type checks are valid for a multi-key query — the early refusal was statement order, not representability. It is now deferred to the end of planning, after all of them — the projection's runs last, via `buildProjectSchema`, and stopping one statement short of it left the same divergence for a SELECT-list fault. The multi-way half stands, for a reason I had stated too broadly: `Planner::plan` builds one join, so with three relations the merged schema is missing a relation and a deferred check would report a misleading `column not found` |
| `findExact` was applied to one of five `find` call sites with the same exposure | **Fixed.** The join key was not special among them; `selectivity`'s three sites and the AGGREGATE group-key site all see merged contexts. One shared `findForRef` now expresses the rule once: exact when a slot is present, bare-name only when it is not |

Two lessons, both about how the earlier rounds were closed. A fix whose test
helper bypasses the pipeline it changed proves nothing about that pipeline —
`validateOnlyJoinError` skipped the Binder, so it could not see a defect that
binding *causes*. And a finding closed by reasoning rather than by running the
code is only as good as the reasoning: the claim that Volcano "cannot reach the
plan-time type checks" was two lines away from being disproved by reading
`planner.cc`.

### Definition of done

- [ ] A three-relation query parses, binds with slots 0/1/2, and builds a
      left-deep `LogicalJoin` tree whose merged schema carries all three slots.
- [ ] `ON a.x = b.x AND a.y = b.y` yields two `JoinKey`s; a mixed compound with a
      non-equality conjunct still throws with `"non-equality"`.
- [ ] `Validator::validateJoinCondition` dispatches every `Expr` subtype, changed
      in the same commit as `classifyJoinCondition`, and `development.md`'s site-18
      row says so.
- [ ] Predicate pushdown routes each conjunct to its own relation in a three-way
      tree, with a negative assertion proving it is not landing on the wrong scan.
- [ ] Multi-way and multi-key joins refuse cleanly at both physical builders.
- [ ] 524+ unit tests pass; `compare_against_sqlite.py` still reports 440 passed.
- [ ] Nothing was built that the checkpoint did not require — no execution, no
      join enumeration, no `--explain` reordering, no new harness queries.
