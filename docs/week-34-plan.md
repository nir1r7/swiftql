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

## Task 3 — The Binder: a range-table entry whose schema is not a catalog table's

### Why it matters

This is the task the README names as the week's core deliverable, quoted here in
full because it is the specification:

> **The minimum fix, carried across verbatim so nobody re-derives it:** extend the
> outer range table with an entry whose "schema" is a **plan node's
> `output_schema`** rather than a catalog table's, give it a real slot, and then
> re-check every consumer in `development.md` → *Relation slots and query levels*
> against a slot that names a *derived* relation.

Everything downstream depends on this entry being a **first-class relation of the
outer block**. Not a special case, not a marker: slot `k`, referenceable as
`d.col`, resolvable by `resolveColumnRef` through the ordinary loop, and subject
to the same ambiguity and duplicate-alias rules as any other relation. The moment
it is anything less, every consumer needs a branch, and Week 30 found that class
of bug five times in one week.

### Conceptual explanation

**Three facts about scoping, and only the third is a judgement call.**

*Fact 1 — a derived table is a relation of the block it appears in.* `FROM
(SELECT ...) AS d JOIN drivers x ON d.k = x.k` has a two-entry range table, slots
0 and 1, and `d.k` resolves at slot 0 exactly as `laps.k` would. The Binder's
existing loop already does this; it needs a schema to put in the entry.

*Fact 2 — a derived table's body is a query block with its own range table.* Its
refs are level 0 **against its own** range table. This is the same fact that made
Week 31's materialization safe, and it is why `ColumnId` is the containment
rather than a new mechanism.

*Fact 3 — a derived table is NOT lateral, and this is the decision.* The body may
not reference the enclosing query's `FROM` items. `FROM laps l JOIN (SELECT *
FROM drivers d WHERE d.team = l.team) x ON ...` is `LATERAL`, and this engine has
no dependent-join operator to run it on — the same reason Week 33 refused rather
than falling back. So:

> **Bind the body against `parent`, not against the scope being built.**

That one argument choice makes sibling `FROM` items invisible to the body by
construction. It leaves the body able to reach *further* out (into a block
enclosing this one, if the whole query is itself nested), which SQL also
disallows without `LATERAL` — so catch that with the flag `bindQuery` already
returns and refuse it by name. Two lines, one refusal, no new walker.

**Where the schema comes from, and the trap.** The entry needs the body's output
schema *during binding*, but the body's output schema is produced by
`LogicalPlanBuilder::build`, which runs later and calls `Validator::validate`
first. The tempting move is to write a second, schema-only derivation. **That is
the two-paths drift Weeks 26, 28 and 30 each had to undo**, and it is worse here
than usual: the two productions would have to agree on aggregate output naming
(`aggregateOutputName` *is* `exprToString`, a byte-for-byte contract), on
`hidden` columns, and on `SELECT *` expansion. A disagreement is a silent
wrong-relation lookup, not an error.

Two honest options:

- **(a) Share the derivation.** Extract from `build` the schema decisions —
  spine merge, then `buildAggregateSchema`, then `buildProjectSchema`/star
  expansion — into one `blockOutputSchema(const SelectStatement&, const Catalog&)`
  in `logical_plan.h`, and have `build` call it too. Same argument that produced
  `key_encoding.h` and one shared `joinCardinality`: one rule, two consumers, so
  they cannot disagree.
- **(b) Derive it in the Binder and *verify* it at graft time.** Task 4 plans the
  body anyway; assert there that the planned subtree's `output_schema` matches the
  range entry's, column for column, name and type.

**Do both.** (a) is the containment; (b) is what proves (a) did not rot. And note
what makes (b) a *live* assertion rather than the dead one Week 33 deleted: it
compares two objects computed by **different code paths** — a schema derived
before planning against one produced by the plan builder — so it can genuinely
fail. Week 33's deleted assertion compared a copy of an object with the object.

**Duplicate output column names.** A derived table whose select list produces two
columns of the same name (`SELECT l.team, d.team FROM laps l JOIN drivers d ...`)
gives a range entry where `indexOf(name)` is a coin flip and `indexOf(name, slot)`
is a coin flip *too*, because Task 4 stamps every column of the entry with one
slot. A base table cannot have this (the catalog forbids it). **Refuse it at bind
time**, with the column name in the message. This is a *new* refusal and therefore
a Task 9 rejection-suite entry.

### Code snippets

```cpp
// src/planner/binder.h — the entry gains an owned schema.
//
// A catalog table's Schema outlives the query, so RangeEntry could hold a raw
// pointer. A DERIVED entry's schema is computed here and must be OWNED, so the
// Scope owns it and the entry keeps pointing. Storing Schema by value in
// RangeEntry would work too and costs a copy per resolution site that takes the
// entry by value — there are none today, which is exactly the kind of fact that
// stops being true silently, so own it once, here.
struct Scope {
    std::vector<RangeEntry> range_table;
    // Week 34. Stable storage for DERIVED entries' schemas. deque/unique_ptr,
    // never vector<Schema>: RangeEntry holds `const Schema*` and a vector
    // reallocation on the second derived table would dangle every earlier one.
    std::vector<std::unique_ptr<Schema>> owned_schemas;
    Scope* parent = nullptr;
    SelectStatement* stmt = nullptr;
    bool correlated = false;
};
```

```cpp
// src/planner/binder.cc — inside bindQuery, replacing the two places that do
// `catalog.getTable(name).schema`. ONE helper, called for FROM and for every
// JOIN, because the two must agree (the FROM/JOIN asymmetry is how Week 26's
// preserved_slots bug and Week 29's `jc` bug both happened).
const Schema* Binder::relationSchema(TableRef& ref, Scope& scope,
                                     const Catalog& catalog, Scope* parent) {
    if (!ref.isDerived()) return &catalog.getTable(ref.table_name).schema;

    // NOT LATERAL. Bind the body against `parent` — the block ENCLOSING this
    // one — never against `scope`. Passing `&scope` would let the body see this
    // block's own FROM items, which is LATERAL, and there is no dependent-join
    // operator to execute it on (same reason Week 33 refuses rather than falls
    // back). The choice of argument IS the scoping rule; there is no extra check.
    const bool body_is_correlated = bindQuery(*ref.subquery, catalog, parent);
    if (body_is_correlated)
        throw std::runtime_error(
            "derived table '" + ref.alias + "': a subquery in FROM cannot "
            "reference a column of an enclosing query (LATERAL is not supported)");

    // ONE derivation, shared with LogicalPlanBuilder::build, so the schema the
    // Binder resolves against and the schema the plan produces cannot drift.
    // Task 4 asserts they agree on the object the plan builder actually makes.
    Schema derived = blockOutputSchema(*ref.subquery, catalog);

    // AS d (a, b) — positional RENAME. Arity is checked here because this is the
    // only place both the list and the schema are in hand.
    if (!ref.column_aliases.empty()) {
        if (static_cast<int>(ref.column_aliases.size()) != derived.size())
            throw std::runtime_error(
                "derived table '" + ref.alias + "' has "
                + std::to_string(derived.size()) + " columns but "
                + std::to_string(ref.column_aliases.size())
                + " column aliases were supplied");
        std::vector<ColumnDef> renamed = derived.columns();
        for (size_t i = 0; i < renamed.size(); ++i)
            renamed[i].name = ref.column_aliases[i];
        derived = Schema(std::move(renamed));
    }

    // A base table cannot hold two columns of one name; a derived one can, and
    // then BOTH indexOf overloads are a coin flip -- the silent wrong-relation
    // class ColumnId made loud inside a block and which nothing checks across a
    // range-table boundary. Refuse, do not disambiguate.
    for (int i = 0; i < derived.size(); ++i)
        for (int j = i + 1; j < derived.size(); ++j)
            if (derived.column(i).name == derived.column(j).name)
                throw std::runtime_error(
                    "derived table '" + ref.alias + "': column '"
                    + derived.column(i).name + "' is produced twice; "
                    "give one of them an alias");

    scope.owned_schemas.push_back(std::make_unique<Schema>(std::move(derived)));
    return scope.owned_schemas.back().get();
}
```

```cpp
// The FROM entry, in bindQuery. The JOIN loop takes the identical shape.
//
// table_name stays EMPTY for a derived entry, deliberately. It is the canonical
// catalog name and there is no catalog table here; filling it with the alias
// would make the duplicate-alias diagnostic's `neither_aliased` test read a
// derived entry as an unaliased base table and give the self-join advice.
scope.range_table.push_back({
    stmt.from.refName(),
    stmt.from.table_name,                       // "" when derived
    relationSchema(stmt.from, scope, catalog, parent)
});
```

### Implementation guidance

**Order inside the function matters, and in a way that is easy to get wrong.**
`bindQuery` currently opens with an existence pre-check over `from_table` and
every `join_table`, returning `false` (not throwing) so that `Validator` keeps
ownership of the "Table not found" message. A derived ref has no name to check —
skip it there, and make sure the skip does not turn "one of the joins names a
missing table" into "bind the derived body anyway and report the missing table
afterwards". Prefer: run the existence pre-check over **named** refs only, and
bind derived bodies in the main loop as the snippet does.

**Bind the body before pushing the entry.** If the entry is pushed first, a body
that (wrongly) resolves against `&scope` would find it, and the LATERAL refusal
would never fire. Ordering is the guard.

**`has_subquery` and `has_correlated_subquery`.** Both are set by the Binder when
it binds a `SubqueryExpr`. A derived table is **not** a `SubqueryExpr` and must
not set either — `has_subquery` currently means "a `SubqueryExpr` is still in
this tree" and drives `buildScanSchema`'s conservative widening and
`Planner::plan`'s refusal scan. Add a **separate** flag,
`SelectStatement::has_derived_table`, and set it for this block only. Reusing
`has_subquery` would silently turn off projection pushdown for every derived-table
query and give the wrong Volcano refusal message.

**Alias shadowing works for free and should be tested.** An inner block's alias
may repeat an outer one — that is what SQL scoping means, and the duplicate-alias
loop is deliberately per-scope. `FROM laps d JOIN (SELECT ... FROM drivers d) x`
is legal and the two `d`s are different relations. Add the test; it is the shape
that proves the body really got its own `Scope`.

**Projection pushdown into the body is out of scope, and say so at the site.**
`buildScanSchema` narrows a *catalog* schema by bare name over one flat schema; a
derived relation's schema is its body's select list, and narrowing it means
rewriting the body's select list, which changes the body's own output schema and
therefore the range entry the outer block was bound against. That is a real
feature with a real wrong-answer failure mode, and it is not this week's — the
same disposition Week 33 recorded for `buildScanSchema` under correlation. Write
the cost at the function, with the reason, so it lands in Week 37's numbers as an
expectation rather than a surprise.

**Anticipated mistakes, specific to this tree:**

- Passing `&scope` instead of `parent` to `bindQuery`. It compiles, most queries
  still work, and the failure is a `LATERAL` query that silently binds and then
  produces wrong rows — the exact class this week exists to prevent.
- Holding `Schema` by value inside `RangeEntry` while other entries hold a
  `const Schema*` into the catalog. Two lifetimes in one field.
- `std::vector<Schema> owned_schemas` — reallocation dangles every prior entry's
  pointer, and only on the second derived table in one block.
- Forgetting that `resolveColumnRef`'s **qualification rewrite is conditional on
  the block having ≥ 2 relations** (Week 30). A one-relation derived-table query
  takes the unqualified path; a two-relation one rewrites `table_name`. Both must
  resolve. Test both, because `aggregateOutputName` is `exprToString` and a
  qualification change is schema-visible.

### Verification

- `SELECT d.team FROM (SELECT team FROM laps) AS d` binds; `d.speed` reports
  column-not-found naming `d`, not `laps`.
- `SELECT * FROM (SELECT team, speed FROM laps) AS d (a, b)` gives columns `a, b`;
  a 1-alias or 3-alias list errors with the arity message.
- The LATERAL shape is refused **by the LATERAL message**, not by
  column-not-found. This is the assertion that pins the `parent` argument; write
  it as a unit test in `tests/test_binder.cc`, since a future refactor that
  changes the argument would otherwise leave a green suite.
- Alias shadowing (`FROM laps d JOIN (SELECT ... FROM drivers d) x ON ...`) binds
  and each `d` resolves in its own block.
- Duplicate derived output name is refused with the column named.
- Ambiguity is still per-scope: an unqualified name present in both the derived
  entry and a joined base table is an **ambiguity** in the outer block (both are
  relations of the same block) — unlike a name present in an inner and an outer
  block, where the inner wins. Confirm both behaviours; they are different rules
  and the range table is what distinguishes them.

---

## Task 4 — `LogicalPlanBuilder`: graft the derived subtree, stamp it like a leaf

### Why it matters

This is where the containment actually comes down. Week 32's invariant kept two
range tables apart by never letting a body column into scope above the join; a
derived table's columns are in scope by definition. What replaces the invariant
is a **normalization**: a derived relation enters the outer tree carrying the
outer block's slot numbering and nothing else. If that normalization is complete,
every `indexOf(name, slot)` above the graft is answering a question about one
range table again, and the entire second half of `development.md`'s slot table
goes back to being safe for the *same* reason it was safe before.

Get it wrong and the failure is the wrong-relation class: `team` and `driver_id`
exist in both shipped tables, so a lookup against the wrong numbering domain is a
clean **hit**, not a miss.

### Conceptual explanation

**The rule, in one line:** *a derived relation's own output schema stamps slot
0, exactly like a leaf scan; the outer slot appears only on merged schemas.*

Why slot 0 and not the outer slot — this is counter-intuitive and it is the
correct answer for four independent reasons, all pre-existing:

1. `LogicalScan`'s own schema stamps 0 for every relation, whatever its binder
   slot. `ColumnDef::relation_slot` defaults to 0 and `buildScanSchema` never
   changes it.
2. The join loop **already re-stamps** the newly added side with `join_slot`
   (`logical_plan.cc:783`), and Week 28 stamps the left block too. So the outer
   slot is applied at the merge, by code that exists.
3. `PredicatePushdown::distribute` calls `restampSlots(c, 0)` before pushing a
   conjunct below a join, with the stated reason "below the join these execute
   against the standalone scan, whose schema stamps every column slot 0". A
   derived relation that stamped its outer slot would break that: the restamped
   conjunct's slot-0 lookup would miss and fall back to a bare name.
4. `ChunkPruner`'s `relation_slot < 1` scan-local test is a *per-scope* fact, and
   Week 30 rejected global slot numbering for exactly this reason.

**The re-stamp is mandatory, not cosmetic.** A derived body that joins two
relations produces an output schema carrying slots 0 **and 1** — the body's
numbering. Grafted unchanged, the outer plan's leaf now holds a column at slot 1
that means a *body* relation, and the two numbering domains have met inside one
schema. Flatten every column to 0 as the subtree enters the outer tree.

**A node type, or a bare subtree?** Grafting `LogicalPlanBuilder::build(body)`
directly is the smaller diff and it is the wrong one. Four walkers in this tree
find "the relation" by descending `children[0]` until they hit a `SCAN`:

| Walker | File | What it returns for a derived leaf |
|---|---|---|
| `leafScanTable` | `vectorized_plan_builder.cc:45` | the **base** table's name — attributes the derived relation's column widths to whatever table its body scans first |
| `isSingleRelation` | `vectorized_plan_builder.cc:56` | `true` for a non-joining body, so `collectSlotTables` stamps the base table at the derived slot |
| `leafScanTableOf` | `join_enumeration.cc:34` | same wrong name, feeding `leafRowWidth` and therefore the DP's cost |
| `countRelations` | `join_enumeration.cc:41` | counts every scan **inside** the body, so `n` over-counts and `hasSlotOutsideRangeTable` becomes too permissive |

All four are silent and all four feed the cost model. This is exactly the
situation Week 27 handled by making `rowWidth` **refuse to guess** about a
join-shaped input (`build=` prints `join-subtree`) rather than return a plausible
wrong number, and the same answer applies. A `LogicalDerived` node makes the
question askable:

```cpp
enum class LogicalNodeType { SCAN, DERIVED, JOIN, FILTER, AGGREGATE, PROJECT,
                             SORT, DISTINCT, LIMIT };
```

Cost, stated rather than hidden: every `switch` over `LogicalNodeType` and every
`type == SCAN` test must be visited. Where the dispatch is an exhaustive `switch`
with no `default`, that is a compile error and the type does the work. Where it is
a `while (type != SCAN) descend` loop — the four rows above — it is **not** a
compile error, so those four are visited by hand and the table above is the list.
That asymmetry is the whole reason to write the list down.

### Code snippets

```cpp
// src/planner/logical_plan.h
//
// Week 34 — a relation of this block that is a PLAN, not a table. Its child is
// the body's whole plan; this node exists to (a) carry the alias for --explain,
// (b) own the column RENAME from the column-alias list, and (c) be a wall that
// the four `descend to the SCAN` walkers stop at instead of walking through and
// naming the body's base table. (c) is the reason it is a node and not a bare
// graft: those four walkers are silent and they feed the cost model.
struct LogicalDerived : LogicalPlanNode {
    std::string alias;   // the outer block's ref name for this relation

    LogicalDerived(std::unique_ptr<LogicalPlanNode> body, std::string alias,
                   Schema schema)
        : LogicalPlanNode(LogicalNodeType::DERIVED, std::move(schema)),
          alias(std::move(alias)) {
        children.push_back(std::move(body));
    }
    std::string explain() const override;   // "Derived [d, N columns]"
};
```

```cpp
// src/planner/logical_plan.cc — ONE helper for both relation positions,
// replacing the two `make_unique<LogicalScan>` sites in build().
std::unique_ptr<LogicalPlanNode> buildRelation(TableRef& ref,
                                               const SelectStatement& outer,
                                               const Catalog& catalog) {
    if (!ref.isDerived())
        return std::make_unique<LogicalScan>(
            ref.table_name,
            buildScanSchema(outer, catalog.getTable(ref.table_name).schema));

    // The body is a query block: build() it exactly as a top-level statement,
    // which is what makes its refs level 0 against its OWN range table and its
    // Validator errors the ones the body is entitled to.
    auto body_plan = LogicalPlanBuilder::build(std::move(*ref.subquery), catalog);

    // (b) from Task 3, on the object the plan builder actually produced. This
    // assertion CAN fail -- it compares a schema derived before planning with one
    // produced by the plan builder, two different code paths -- unlike the dead
    // one Week 33 deleted, which compared a copy of an object with the object.
    // If it fires, blockOutputSchema() and build() have drifted, which is the
    // two-paths failure Task 3 exists to prevent.
    //
    //   (the Binder's schema for this ref is passed in or recomputed here;
    //    compare size, then name and type per column, and name the alias.)

    std::vector<ColumnDef> cols = body_plan->output_schema.columns();
    for (size_t i = 0; i < cols.size(); ++i) {
        if (!ref.column_aliases.empty()) cols[i].name = ref.column_aliases[i];
        // !! THE NORMALIZATION. A leaf's own schema stamps 0 (schema.h's default,
        // and what buildScanSchema leaves alone); the OUTER slot is applied by
        // the join loop's merge, and by Week 28's left-block stamp. A body that
        // joins produces slots 0 AND 1 -- the BODY's numbering -- and grafting
        // that unchanged puts two numbering domains inside one schema, which is
        // the wrong-relation class ColumnId made loud inside a block and which
        // nothing checks across a range-table boundary.
        cols[i].relation_slot = 0;
        // A body's HIDDEN aggregate columns (HAVING/ORDER BY only) are not
        // columns of the derived RELATION: SELECT * over it must not emit them,
        // and the Binder's range entry does not list them either.
        // buildProjectSchema already drops them at the body's own project, so
        // this is an assertion, not a filter -- assert it rather than re-filter,
        // or the two schemas disagree and Task 3's check fires on a real query.
    }
    return std::make_unique<LogicalDerived>(std::move(body_plan), ref.alias,
                                            Schema(std::move(cols)));
}
```

```cpp
// The four descend-to-SCAN walkers. Each needs the SAME shape of answer, and
// the shape is Week 27's: refuse to guess rather than return a plausible name.
const std::string* leafScanTableOrNull(const LogicalPlanNode* node) {
    while (node->type != LogicalNodeType::SCAN) {
        // Week 34: a DERIVED relation has no catalog table, so there is no
        // per-column avg_width to look up and no TableStats to consult. Walking
        // THROUGH it returns the body's base table and attributes that table's
        // widths to the derived relation's columns -- the identical defect Week 27
        // found in rowWidth for a join subtree and closed by falling back to the
        // uniform proxy and printing `join-subtree`.
        if (node->type == LogicalNodeType::DERIVED) return nullptr;
        if (node->children.empty()) return nullptr;
        node = node->children[0].get();
    }
    return &static_cast<const LogicalScan*>(node)->table_name;
}
```

### Implementation guidance

**Step 1 — `buildRelation` for the FROM position only**, with joins still
named-table-only. Get `SELECT * FROM (SELECT team FROM laps) AS d` returning rows
in both vectorized modes. This is the smallest thing that can be right.

**Step 2 — the JOIN position.** The join loop's merge already re-stamps the added
side with `join_slot`, so a derived join needs no new stamping code — which is
the payoff of Step 1's normalization and worth confirming by reading rather than
assuming.

**Step 3 — lowering.** `Lowering::lowerNode` (`vectorized_plan_builder.cc`) gains
a `DERIVED` case that lowers `children[0]` and returns it with the derived
node's (renamed) schema. There is **no physical derived operator** and there must
not be: a derived table is a naming and slot-normalization artifact, not a
computation. If the column-alias list renamed anything, the rename has to reach
the physical schema, because `resolveColumnIndex` in `evaluator.cc` and every
`indexOf` above the graft look the new names up.

**Step 4 — `PredicatePushdown`.** Read `distribute()` before changing anything:
at the bottom of the spine it calls `filterOnto(node, conjuncts)`, which **wraps**
rather than descends. A `WHERE` conjunct over a derived relation therefore lands
as a `LogicalFilter` *above* the `LogicalDerived`, which is semantically exactly
right — it is a filter on the derived table's output — including when the body
has `GROUP BY`, `LIMIT` or `DISTINCT`, where descending would be a wrong answer.
So the correct action here is **to change nothing and to write down why**, at
`distribute`, naming `filterOnto`'s wrapping as the reason. Pushing *into* the
body is a real optimization and is not this week's; it needs the body's own
`WHERE` and is unsound across an aggregate.

**Step 5 — the Volcano path.** `Planner::plan` builds its scan directly from
`stmt.from_table` and exactly one `HashJoinNode` from `stmt.joins`; there is no
plan shape here that can hold a subtree as a relation, and no `LogicalPlanBuilder`
to graft one. Refuse, beside the Week 32 and Week 33 refusals in the same
function, with the same wording pattern:

```
derived tables (FROM (subquery)) are not supported on the Volcano path;
use --execution vectorized
```

This is a genuine capability difference, so it lives in `Planner::plan` and not
in the shared `Validator` — Week 26's rule. **State the cost where the refusal
is**: every derived-table query is diffed against SQLite in the two vectorized
modes only, and the refusal is pinned by message in the two Volcano ones. Task 8
revisits whether that is the end state; Task 9 builds the suites.

**Anticipated mistakes:**

- Stamping the outer slot on the derived subtree's own schema. It "works" for the
  FROM-position single-relation case and breaks pushdown's `restampSlots(c, 0)`
  contract the moment a join appears.
- Letting the four descend-to-SCAN walkers walk through `LogicalDerived`. Nothing
  fails; the cost model just silently uses another table's widths, which is the
  input Week 28's DP is built on.
- Adding a `default:` to a `switch (node->type)` while adding `DERIVED`. That
  converts the compile errors this node type exists to produce back into silence.
- Forgetting `--explain`. A derived subtree with no marker prints as an
  unexplained plan fragment at the bottom of the spine. `Derived [d, N columns]`
  is a one-line `explain()` and it is the surface Week 36 debugs Q15 on.

### Verification

- `SELECT * FROM (SELECT team, AVG(speed) AS s FROM laps GROUP BY team) AS d
  WHERE d.s > 300` returns rows identical to SQLite in both vectorized modes, and
  reports the Volcano refusal by message in the other two.
- Q15's rewritten shape — a derived table joined to a base table, plus a scalar
  subquery over a second derived table — runs and diffs.
- Q13's shape with the column alias list runs and diffs. Q13 also has a `LEFT
  JOIN` inside the body, so it exercises the body-joins-two-relations case that
  the slot-0 normalization exists for.
- `--explain` on a derived-table query shows `Derived [d, N columns]` with the
  body's plan indented under it, and the merged join schema above it resolves
  `d.col` by slot.
- **The drift assertion fires when it should.** Deliberately break
  `blockOutputSchema` (rename one output column) and confirm the graft-time check
  throws naming the alias. An assertion never seen to fail is the one Week 33
  warned about.
- Every non-derived query's `--explain` is byte-identical to the pre-task run.

---
