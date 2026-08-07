# Week 34 — Derived Tables + Distinct Aggregates

**Checkpoint (README):** Rewritten Q15 and distinct aggregates are supported.
Bind and execute subqueries in `FROM`; add per-group state for
`COUNT(DISTINCT ...)`.

**And the starting notes, which are not optional.** The README's Week 34 block
opens with *"Q17 AND Q22'S CORRELATED HALF ARE WEEK 34 DELIVERABLES, not
nice-to-haves"*. Week 33 decorrelated `EXISTS` / `NOT EXISTS` and recorded a
checkpoint **miss** on correlated scalar subqueries, for a structural reason that
lands inside this week's scope. The sentence that settles it, carried across
verbatim so nobody re-derives it:

> *A decorrelated scalar subquery is a derived table with an implicit join.*

Whatever this week builds for `FROM (subquery)` is the same machinery Q17 needs.
Building it and then not spending it on Q17 leaves the phase's TPC-H target short
at Week 36 for no additional work.

**The shape of the week.** Week 34 is the week that **removes a containment**
rather than adding a feature behind one. Weeks 32 and 33 both rest on one
invariant — *a `SEMI`/`ANTI` join's `output_schema` **is** its left child's, so
nothing from the body is ever in scope above the join* — and `logical_plan.h`
calls that "the whole containment for the two-range-table problem this node
introduces". A derived table's columns **are** in scope above it. That sentence
is the entire week: everything hard here follows from it, and so does the sweep
discipline in Task 1 and Task 10.

---

## Prerequisite knowledge

Read these before writing a line. Each is load-bearing for a specific task, not
background reading.

1. **The difference between a *schema* slot and a *binder-resolution* slot.**
   `development.md` → *Relation slots and query levels*, the paragraph beginning
   "Two different things are called `relation_slot`". `ColumnDef::relation_slot`
   (`common/schema.h`) is a position in one block's schema and has no level to
   lose; `ColumnId` (`common/column_id.h`) is a binder slot and can name an
   enclosing block. Tasks 3, 4 and 5 are entirely about the boundary between them.
2. **`ColumnId`'s API** (`src/common/column_id.h`): `isLocal()`, `isResolved()`,
   `localSlot(site)` (throws on a correlated id), `slotInOwnScope(site)` (the
   two-user escape hatch), `outward()`. Week 33 paid 87 sites for this; Task 5
   is where the return arrives, because a consumer that forgets the level no
   longer compiles.
3. **The three slot-stamping rules already in the tree**, all in
   `LogicalPlanBuilder::build` (`src/planner/logical_plan.cc:729`): a leaf scan's
   own schema stamps **0**; a merged join schema stamps the newly added side with
   `join_slot`; Week 28's enumeration also stamps the merged schema's *left*
   block with the leftmost relation's binder slot. Task 4 is the fourth case and
   it must behave exactly like the first.
4. **Week 32's `lowerInSubqueries`** (`src/planner/subquery_lowering.cc`) and
   **Week 33's `lowerExistsSubqueries`** (`src/planner/subquery_decorrelation.cc`).
   Task 6 is a third sibling at the same call site, and `splitCorrelation` is
   already factored for it.
5. **Week 29's outer-join rules**, because Task 6's join must be `LEFT`:
   pushdown declines the null-supplying side, join enumeration declines the whole
   tree, and the build side is forced.
6. **`src/execution/key_encoding.h`** — the one shared key serializer. Task 7's
   distinct set is a seventh consumer of it, and hand-rolling a key again loses
   injectivity, the NULL policy and exact DOUBLE comparison, each of which was
   violated by an operator that restated the rules locally.

---

## Tasks

1. **The refusal inventory — build the sweep worklist *first*, not last.**
2. **`TableRef`: grammar, AST, parser.** One compile-error-driven change, its own
   commit, derived form still refused at the end of it.
3. **The Binder: a range-table entry whose schema is not a catalog table's.**
4. **`LogicalPlanBuilder`: graft the derived subtree, stamp it like a leaf.**
5. **The three synthetic-slot consumers, and the slot-table sweep.**
6. **Q17 / Q22: correlated scalar decorrelation onto the same machinery.**
7. **`COUNT(DISTINCT ...)` — per-group state in both engines.**
8. **The two inherited capability boundaries: the dependent-join fallback, and
   Volcano semi/anti parity.**
9. **Harness and tests: diffed suites, rejection suites, changed-test discipline.**
10. **The closing sweep — every comment, precondition and assertion citing a
    refusal this week removed.**

Sections follow in that order. Tasks 2→3→4 are a chain and must land in that
sequence; Task 5 is the sweep that makes Task 4 safe; Task 6 depends on Task 4;
Task 7 is independent of all of them and is the one task that can be done in
parallel or first if the week runs hot.

---

## Task 1 — The refusal inventory: build the sweep worklist first

### Why it matters

This is not paperwork and it is not a closing chore. It is the first task
because it is the week's largest known hazard, and the project has already paid
for learning it: **Week 33 shipped three silent wrong answers from exactly this
shape**, two of them living in *header comments*, and a sweep afterwards found
**seven more stale preconditions**. The rule the project learned the hard way is
written into this week's brief:

> When a refusal, guard or invariant is removed or changed, every comment,
> precondition, assertion and header citing it must be swept and re-verified.

Week 34 removes refusals — `FROM (subquery)` is rejected today, and Week 32's
schema containment is about to stop being universal — so it creates precisely
this hazard. A comment that states a guarantee which has silently become false is
worse than no comment: the next week reads it as already-checked and skips the
verification. `development.md` says the same thing about its own tables ("a
missing row is worse than a wrong one").

Doing the inventory **first** also changes the work. Half these citations are
*preconditions other code depends on*, so finding them up front tells you what
Tasks 3–6 must preserve. Finding them at the end tells you only what to rewrite.

### Conceptual explanation

There are three distinct kinds of citation and they need three different actions:

| Kind | Example | Action |
|---|---|---|
| **A statement of fact that becomes false** | `logical_plan.h`: "Nothing from children[1] is ever in scope above this node" (still true for SEMI/ANTI; the *file-level* claim that this is "the whole containment for the two-range-table problem" is not) | Rewrite to state what is now true **and what replaced the containment** |
| **A precondition another pass relies on** | `chunk_pruner.h`'s scan-local `relation_slot < 1` justification; `join_condition.h`'s `from_slot` contract | Re-derive it under the new world. If it still holds, say *why it still holds*, not that it was checked |
| **A pointer to a future week** | `ast.h`: "FROM is Week 34"; `join_enumeration.cc`: "from Week 34 on, a SCAN THAT IS NOT A RANGE-TABLE ENTRY…" | The week arrived. Replace the promise with the answer |

The failure mode that makes this urgent is specific: a `grep` for the *feature*
("derived", "FROM (subquery)") does not find these. They cite the **refusal** or
the **invariant**, not the feature. So grep for the refusal's vocabulary.

### Code snippets

The searches that produce the worklist. Run all of them, keep the output in the
plan file, and tick entries off in Task 10.

```bash
# 1. Everything that names this week by number — promises coming due.
grep -rn "Week 34" src/ tests/ python_tools/ development.md README.md

# 2. The Week 32 containment, in every spelling it is written in.
grep -rn "output_schema IS\|is its left child\|two-range-table\|two range tables" \
     src/ development.md README.md

# 3. Refusal messages this week may delete or narrow. Each is a dialect-table
#    row AND a python_tools rejection-suite entry AND a comment somewhere.
grep -rn "not yet executable\|are not supported on the Volcano path\|cannot be decorrelated" \
     src/ python_tools/ README.md

# 4. Every reader of a binder slot. This is Task 5's worklist as well; ColumnId
#    turned it into a compiler worklist, but the *comments* are still prose.
grep -rn "localSlot\|slotInOwnScope\|from_slot\|join_slot" src/ | grep -v "^.*://"

# 5. The four walkers that descend to a SCAN by assumption rather than by test.
grep -rn "type != LogicalNodeType::SCAN\|type == LogicalNodeType::SCAN" src/
```

Search 5 is the one that finds live defects rather than stale prose. It returns
`leafScanTable` (`vectorized_plan_builder.cc:45`), `isSingleRelation`
(`:56`), `leafScanTableOf` (`join_enumeration.cc:34`) and `countRelations`
(`:41`). Every one of them walks `children[0]` until it hits a `SCAN` and then
draws a conclusion about "the relation" — and every one of them is wrong for a
derived relation, silently, in the cost model. Task 4 and Task 5 own them; Task 1
only has to *find* them before the code that trips them is written.

### Implementation guidance

Produce a checklist with a line per citation, in this shape, and keep it in this
file under a **Progress** heading:

```
[ ] src/planner/logical_plan.h:145   LogicalJoin invariant block — "the whole
    containment" is no longer whole. State what Task 4 replaced it with.
[ ] src/parser/ast.h:155             "FROM is Week 34" — the week arrived.
[ ] src/planner/join_enumeration.cc:78  "from Week 34 on, a SCAN THAT IS NOT A
    RANGE-TABLE ENTRY … starts firing on legitimate plans" — now LIVE.
```

Known starting set, from reading the tree (expect the greps to add to it — this
is a floor, not a ceiling):

- `src/parser/ast.h` — `SubqueryExpr`'s position comment ("FROM is Week 34").
- `src/planner/logical_plan.h` — the `LogicalJoin::semantics` invariant block.
- `src/planner/subquery_lowering.h` — its stated preconditions.
- `src/planner/subquery_decorrelation.h` — condition 3, "a body with an aggregate
  cannot be decorrelated", which **stops being universally true in Task 6** and
  is the single highest-risk stale comment of the week.
- `src/planner/join_enumeration.cc` — the `hasSlotOutsideRangeTable` block, which
  literally predicts this week.
- `src/planner/join_condition.h` — the `JoinKey::from_slot` contract.
- `src/storage/chunk_pruner.h` — the scan-local justification.
- `src/planner/predicate_pushdown.h` / `.cc` — the caller count and the
  fail-closed empty-slot justification (Week 29 found these already disagree).
- `src/planner/planner.cc` — the Volcano refusal block, which Task 8 adds to.
- `development.md` → *Relation slots and query levels*: the sentence "**Week 34's
  derived tables break this containment for real**" is a forward promise that
  must become a record, plus a **new table section** for Week 34's consumers.
- `README.md` — feature-scope bullet on subqueries, four rows of the dialect
  table, four Limitations bullets, the Week 34 section itself and the 37-week
  plan row.
- `docs/week-33-plan.md` → *Handed to Week 34*: four items. Each must end the
  week marked **done** or **re-declined with a reason**, never silently dropped.

### Verification

- The checklist exists before Task 2's first commit, and every entry is ticked
  before the week's last commit.
- For each ticked entry the commit message or the comment itself says *what is
  now true*, not that it was reviewed. "Checked" is not a finding.
- Re-run all five greps at the end of the week. New hits introduced by this
  week's own code are the expected result; a hit you cannot account for is an
  unfinished sweep.

---

## Task 2 — `TableRef`: grammar, AST, parser

### Why it matters

`SelectStatement` currently encodes "a relation is a table name" in its **type**:
`std::string from_table` plus `std::string from_alias`, and
`JoinClause::join_table` plus `JoinClause::alias`. A derived table is a relation
that is not a name, so that encoding has to go — and the way it goes decides
whether the rest of the week is a compiler-driven enumeration or an audit round.

This is the exact situation Week 26 met with `SelectStatement::join`:

> `std::optional` is the type-level encoding of "at most one join", and 14 sites
> in `src/` branched on it. Making it a vector turns every one into a **compile
> error** rather than a place a second relation is silently dropped.

Same move, same reason. Every site that reads `stmt.from_table` today either (a)
needs to learn about derived tables, or (b) is provably named-table-only and
should say so at the site. A `std::string` that is sometimes empty gives you
neither; it gives you `catalog.hasTable("")` returning false and a
`Table not found: ''` message on a query that has no table-name error in it.

### Conceptual explanation

The grammar gains one production and it slots into the existing `table_ref`,
which both `FROM` and every `JOIN` already use:

```
table_ref    → IDENT [[AS] IDENT]
             | LPAREN select_stmt RPAREN [AS] IDENT [ LPAREN ident_list RPAREN ]
                                                ↑ Week 34: alias REQUIRED,
                                                  optional column alias list
```

Three language decisions, each with a reason rather than a preference:

**1. The alias is mandatory for a derived table.** SQL standard requires it, but
the engine-level reason is stronger: `Binder::RangeEntry` is keyed on `ref_name`,
and the duplicate-alias check (`binder.cc:56`) compares ref names. An unaliased
derived entry has no `ref_name`, so every qualified reference to it is
unresolvable and two unaliased derived tables in one `FROM` clause collide on the
empty string. Refuse it by name at parse time.

**2. One token of lookahead after `(` separates a derived table from nothing
else**, because `FROM` has no parenthesised-expression production at all. This is
strictly easier than Week 30's `LPAREN select_stmt RPAREN` in `primary`, which
had to be separated from a parenthesised expression. Peek for `SELECT`.

**3. The column alias list renames, it does not project.** `AS d (a, b)` renames
the derived relation's output columns positionally. An arity mismatch is an error
(SQLite's is `table "d" has N columns but M values were supplied`; ours can be
plainer). It is in scope because the README's Week 29 section says so in as many
words — Q13's *"derived table and column list remain Week 34"* — and because it
is genuinely cheap: it is a rename over a `Schema` that Task 3 already builds.

**Ownership.** `SelectStatement` is move-only (it holds `unique_ptr` members),
which is why `SubqueryExpr::subquery` is a `shared_ptr`. Two options:

- `std::unique_ptr<SelectStatement>` — stricter, no sharing to reason about, and
  a derived table genuinely has one parent. But `Validator::validateQuery` takes
  `const SelectStatement&` and `collectQueryTables` walks a const statement
  before it is moved into the builder, so a `unique_ptr` is fine for both.
- `std::shared_ptr<SelectStatement>` — mirrors `SubqueryExpr` exactly, which
  means Week 33's `use_count() > 1` refusal shape is already understood in the
  codebase and reviewers recognise it.

**Recommendation: `unique_ptr`.** The `shared_ptr` on `SubqueryExpr` exists for
one specific reason — `cloneExpr` must copy any `Expr`, and a deep statement copy
has silent omissions — and that reason does not apply here: a `TableRef` is not
an `Expr` and nothing clones it. Choosing `shared_ptr` "for consistency" would
import the two-parents problem (and Week 33's guard against it) for free.
State this choice at the field, because the *next* reader will notice the
asymmetry with `SubqueryExpr` and deserve the reason.

### Code snippets

```cpp
// src/parser/ast.h — replaces `from_table`/`from_alias` and
// `JoinClause::join_table`/`alias`.
//
// Week 34. A relation in FROM / JOIN is no longer always a NAME. Encoding that
// as an empty string would make `catalog.hasTable("")` the test for "is this
// derived", which is the shape Week 26 removed when SelectStatement::join stopped
// being a std::optional: a type that cannot express the distinction turns every
// consumer into a compile error instead of a place the distinction is dropped.
struct TableRef {
    // Exactly one of these is set. `subquery` non-null == derived.
    std::string table_name;                      // catalog name; empty when derived
    std::unique_ptr<SelectStatement> subquery;   // Week 34: FROM (SELECT ...)

    // The name a qualified reference uses, and the Binder's RangeEntry key.
    // REQUIRED when `subquery` is set (the parser enforces it): the range table
    // is keyed on this, so an unnamed derived entry is unreferenceable and two of
    // them collide on the empty string.
    std::string alias;

    // Week 34: AS d (a, b) — positional RENAME of the derived relation's output
    // columns, not a projection. Empty when absent. Named-table refs never carry
    // one (the grammar does not offer it there), which is why the arity check
    // lives in the Binder beside the schema it renames.
    std::vector<std::string> column_aliases;

    bool isDerived() const { return subquery != nullptr; }
    // The name a range-table entry is keyed on. For a named table with no alias
    // that is the table name — the identity binder.cc already computes inline at
    // two sites, hoisted here so the two cannot drift.
    const std::string& refName() const {
        return alias.empty() ? table_name : alias;
    }
};
```

```cpp
// src/parser/parser.cc — one helper, called from BOTH the FROM position and the
// JOIN loop, so the two cannot diverge (they already share the alias rules).
TableRef Parser::parseTableRef() {
    TableRef ref;

    if (check(TokenType::LPAREN)) {
        consume();                                   // '('
        // No lookahead ambiguity here, unlike Week 30's scalar subquery: FROM has
        // no parenthesised-expression production to be confused with.
        ref.subquery = std::make_unique<SelectStatement>(parseSelect());
        expect(TokenType::RPAREN, ") to close the derived table");

        match(TokenType::AS);                        // optional noise word
        // MANDATORY, and refused here rather than in the Binder because it is a
        // syntax fact: the range table is keyed on the alias.
        if (!check(TokenType::IDENTIFIER))
            throw ParseError("a subquery in FROM requires an alias "
                             "(FROM (SELECT ...) AS name)", current_);
        ref.alias = consume().value;

        if (match(TokenType::LPAREN)) {              // AS d (a, b)
            do {
                ref.column_aliases.push_back(
                    expect(TokenType::IDENTIFIER, "column alias").value);
            } while (match(TokenType::COMMA));
            expect(TokenType::RPAREN, ") to close the column alias list");
        }
        return ref;
    }

    ref.table_name = expect(TokenType::IDENTIFIER, "table name").value;
    // ... the existing alias rules, unchanged, moved here verbatim ...
    return ref;
}
```

### Implementation guidance

**Step 1 — count the blast radius before starting.**

```bash
grep -rn "from_table\|from_alias\|join_table\|\.alias" src/ tests/ | wc -l
```

Expect this to be smaller than Week 33's 87 and larger than it looks: the known
sites are `binder.cc` (range-table construction, twice), `validator.cc` (FROM and
JOIN existence, the `relations` vector, the SUM/AVG check's
`stmt.joins[slot-1].join_table`), `logical_plan.cc` (`build`'s scan construction,
twice), `planner.cc` (the whole Volcano single-join path),
`subquery_materialization.cc` (`collectQueryTables`), plus every test that
hand-builds a `SelectStatement`.

**Step 2 — land it with the derived form refused.** This is the Week 26 and Week
30 stance and it is what makes the commit reviewable: parse it, bind it, then
refuse at the end of `Validator::validate` with
`FROM (subquery) is parsed and bound but not yet executable`. The whole suite
must be green on both sides of this commit, with zero behaviour change for every
existing query.

**A judgement call to make deliberately, not by default:** do **not** add that
interim refusal to `compare_against_sqlite.py`. The harness pins the *shipped*
dialect, and a refusal that will not survive to Friday would be a test written to
be deleted. Rejection-suite entries are for refusals that **survive the week** —
which Task 9 enumerates. (Week 26 and Week 30 both added harness rows for interim
refusals because in those weeks the refusal *was* the week's end state.)

**Step 3 — mistakes specific to this codebase.**

- `parseSelect()` is recursive and `Parser::parse` requires end-of-input at the
  top level only, so a derived body ending early will report a confusing trailing
  -input error at the outer level. Make the `RPAREN` expectation message name the
  derived table, as the snippet does.
- The FROM-position bare-alias branch has an explicit keyword exclusion list
  (`!check(JOIN) && !check(WHERE) && …`) while the JOIN-position one does not,
  because JOIN/ON/WHERE/… are their own `TokenType`s and `check(IDENTIFIER)` is
  already false for them. When you hoist both into one `parseTableRef`, **keep
  the exclusion list** — it is guarding the FROM position, where the next token
  after a bare identifier can legally be a clause keyword.
- `SelectStatement` is move-only and now holds a `unique_ptr` two levels down.
  Anything that copies a statement stops compiling; there is nothing that does
  today, and that is worth confirming rather than assuming.
- Do **not** give `TableRef` an `operator==` or a `clone()`. Nothing needs one,
  and adding one invites the deep-copy-with-silent-omissions problem Week 30
  refused for `cloneExpr`.

### Verification

- `./build/swiftql --query "SELECT * FROM (SELECT team FROM laps) AS d"` reaches
  the interim refusal, and `--query "SELECT * FROM (SELECT team FROM laps)"`
  reports the missing alias — a *parse* error, before any catalog lookup.
- Every pre-existing query is byte-identical in all four modes: run the full
  `compare_against_sqlite.py` and diff the output against the pre-commit run, not
  just its pass/fail line.
- `--explain` output is byte-identical for every existing query. `TableRef` is an
  AST change; if a plan string moved, something read `refName()` where it used to
  read `table_name`.
- The unit suite is green with **no test's assertion weakened**. Tests that
  hand-build `stmt.from_table = "laps"` become `stmt.from = TableRef{"laps"}`;
  that is a mechanical translation and any test that needed more than a
  translation is a test that was asserting something about the old encoding —
  say what, in the commit message. (Week 32's lesson: a changed test is where a
  capability disappears.)

---
