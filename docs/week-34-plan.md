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

## Task 5 — The three synthetic-slot consumers, and the slot-table sweep

### Why it matters

The README names three consumers that break on a slot naming a derived relation,
and the sentence that makes this the week's sharpest hazard is:

> **Three consumers break on a synthetic slot, and NONE of them fails loudly** —
> which is the dangerous part, and the reason to do the range-table entry properly
> rather than stamp a plausible integer.

Task 4 does the entry properly, which disarms most of the third one. This task is
where the other two are closed and where the *whole* slot table is re-checked —
which `development.md` requires by rule ("re-check every 'contained' row on the
day a new nested-scope construct lands … Week 34's derived tables").

The reason it is affordable at all is Week 33's `ColumnId`: a consumer that reads
a slot without deciding what its level means no longer compiles. Week 30 found
this bug class **five times by audit**; the type finds it at build time. Use it —
this task is a compiler worklist plus three hand-checked walkers, not an audit
round.

### Conceptual explanation

**Consumer 1 — `JoinEnumeration::hasSlotOutsideRangeTable` (`join_enumeration.cc:91`).**

```cpp
bool hasSlotOutsideRangeTable(const LogicalPlanNode* node, int n) {
    if (join->join_slot < 1 || join->join_slot >= n) return true;
    ...
}
```

`n` is `countRelations(root)`, which counts **scans**, recursively, through every
child. Two independent errors arrive at once with derived tables:

- Body scans are counted, so `n` **over**-counts and the `>= n` test stops
  meaning "outside the range table" — it becomes too permissive and lets through a
  tree the pass cannot decompose.
- With `LogicalDerived` in place, a derived leaf contains scans that should count
  as **one** relation, and if `countRelations` is left alone it counts them
  individually. If it is taught to return 1 for `DERIVED`, `n` is right again.

`join_enumeration.cc`'s own comment predicts this by name — "from Week 34 on, a
SCAN THAT IS NOT A RANGE-TABLE ENTRY of the query being planned, because it
belongs to a subquery — at which point `slot >= countRelations()` stops meaning
'unbound key' and starts firing on legitimate plans". Week 28 and Week 29 both
recorded the fix: **re-derive it against the binder range table**.

The right value of `n` is the outer block's range-table size, which is
`1 + stmt.joins.size()` and is known at the call site. Passing it down is a
smaller and more honest change than teaching a scan-counter to count relations.

And then the **decline must be reported**. Today it is silent, in the same shape
as the `<3-relation` decline, and Week 30's hand-forward set the condition for
changing that:

> If a supported query starts paying a real plan-quality cost for it, that is when
> it earns a reported decision, in the shape Week 29's
> `join-ordering=skipped (outer join)` uses — not before.

Q15's outer query is a multi-relation join over a derived relation. The condition
is met. Print `join-ordering=skipped (derived relation)`. Note the token is
deliberately not `order=`, which has to keep meaning "the search ran".

**A second, non-obvious consequence of derived relations in the enumerator**, and
it is worth more than the decline: a derived relation has **no `TableStats`**.
`joinCardinality`'s no-statistics branch falls back to `max(l, r)`, which is not
multiplicative, so a subset containing a derived relation has an order-dependent
row count and the DP's optimal substructure does not hold for it. Week 28 named
the containment — the written-order bound in `reorder()` — and also recorded that
`method=written-floor` **has never executed**, being reachable only "from a
catalog with a stats-less table — a C++ fixture — and no test builds one, so the
path that silently changes which tree `rebuild` folds ships untried."

Week 34 makes that path reachable **from the CLI, on a real query**, for the first
time. If the enumerator declines derived trees outright, it stays unreachable and
that must be said out loud; if it does not, the fixture test Week 28 asked for is
now cheap to write as a query. Decide which, and record it.

**Consumer 2 — `Validator`'s SUM/AVG argument check (`validator.cc:205`).**

```cpp
if (col_slot > 0 && col_slot <= static_cast<int>(stmt.joins.size())) {
    target = &catalog.getTable(stmt.joins[col_slot - 1].join_table).schema;
```

The arithmetic (`slot k > 0` is `joins[k-1]`'s relation) is still correct — Task 3
preserves it. What breaks is the next step: `joins[k-1].join_table` is **empty**
for a derived join, so `catalog.getTable("")` throws or reports a nonsense table.

This check has moved once already, for the same class of reason: Week 30 round 2
found it indexing the *inner* join list with an *outer* slot, and the correlated
half moved to `Binder::checkCorrelatedAggregateArg` because "the Binder is the
only layer holding the scope chain". The same argument now applies to the local
half: **the Binder is the only layer holding the derived relation's schema.**
`Validator` has a `Catalog`; a derived relation is not in it.

Two options, and the tradeoff is a *message*, not a behaviour:

- **(a) Move the whole check into the Binder**, beside its correlated sibling,
  resolving through `scope.range_table[slot].schema` — which is correct for base
  and derived relations uniformly. Cost: the check now runs during **binding**, so
  it outranks every `Validator` rule, including "no aggregate in `WHERE`". That
  asymmetry already exists for correlated arguments and is documented at
  `binder.h`; option (a) makes it uniform, which is arguably a *fix*.
- **(b) Skip the check in `Validator` when the target ref is derived**, and let
  `inferExprType` catch `SUM(<STRING>)` later. Cost: a different message for the
  same fault depending on whether the relation is derived — the shape Week 26
  named as how two paths drift.

**Recommendation: (a).** It removes a branch rather than adding one, it makes the
existing binder/validator split uniform instead of case-by-case, and it deletes a
`stmt.joins[...]` index that has now been wrong twice.

**Consumer 3 — every `indexOf(name, slot)` above the join.** This is the one Task
4 mostly closes: after the slot-0 normalization plus the merge stamp, a derived
relation's columns carry an outer binder slot that the outer range table *can*
explain, and the duplicate-output-name refusal (Task 3) removes the one case where
the name is ambiguous within the entry. What remains is to **prove** it, consumer
by consumer, which is the sweep below.

### Code snippets

```cpp
// src/planner/join_enumeration.cc — n comes from the RANGE TABLE, not from a
// scan count. countRelations() counts SCANS, so a derived relation's body scans
// inflated it (too permissive) while LogicalDerived would hide them (too strict);
// neither number is the range table's size, which is what join_slot indexes.
// Week 28 and Week 29 both recorded this fix in advance; it is now live.
std::unique_ptr<LogicalPlanNode> JoinEnumeration::apply(
        std::unique_ptr<LogicalPlanNode> node, const Catalog& catalog,
        int range_table_size /* = 1 + stmt.joins.size(), from the builder */) {
    ...
    if (hasSlotOutsideRangeTable(node.get(), range_table_size)) {
        // Week 34: no longer silent. Week 30's condition for earning a reported
        // decision -- "a SUPPORTED query pays a real plan-quality cost" -- is met
        // by Q15, whose outer query is a multi-relation join over a derived
        // relation. Same shape as Week 29's outer-join line, and deliberately NOT
        // an `order=`, which has to keep meaning "the search ran".
        top->order_decision = "join-ordering=skipped (derived relation)";
        return node;
    }
}
```

```cpp
// src/planner/binder.cc — the LOCAL half of the SUM/AVG argument check, moved
// here from validator.cc to sit beside checkCorrelatedAggregateArg.
//
// It moved for the third time and for the same reason each time: the check needs
// a SCHEMA FOR A SLOT, and the layer that owns slot -> schema is the range table.
// Week 30 round 2 moved the correlated half here after it was found indexing the
// INNER join list with an OUTER slot. Week 34 moves the local half, because
// `stmt.joins[slot-1].join_table` is EMPTY for a derived relation and
// catalog.getTable("") is not a diagnosis.
//
// !! CONSEQUENCE, stated because it is schema-visible in the ERROR TEXT: this now
// runs during binding, so it outranks Validator's "aggregate not allowed in
// WHERE". SUM(d.name) in a WHERE clause is refused by TYPE where SUM(d.age) is
// refused by POSITION. Both are refused; only the wording differs. The asymmetry
// already existed for correlated arguments (binder.h) -- this makes it uniform
// rather than case-by-case.
void Binder::checkAggregateArgType(const AggregateExpr* agg, const Scope& scope) {
    if (agg->function_name != "SUM" && agg->function_name != "AVG") return;
    auto* col = dynamic_cast<const ColumnRef*>(agg->argument.get());
    if (!col || !col->id.isResolved()) return;

    const Scope* s = &scope;
    for (int i = 0; i < col->id.level() && s; ++i) s = s->parent;   // 0 steps when local
    if (!s) return;
    const int slot = col->id.slotInOwnScope("checkAggregateArgType");
    if (slot >= static_cast<int>(s->range_table.size())) return;

    // The one lookup that is uniform over base and DERIVED relations: the range
    // entry's schema. A derived entry's schema is the Scope's owned copy of its
    // body's output schema, which is why this cannot live in Validator.
    const Schema* sch = s->range_table[slot].schema;
    ...
}
```

### Implementation guidance

**Do this as a worklist, in this order.**

1. **Let the compiler produce the list.** Build after Task 4. Every
   `localSlot(site)` throw and every `ColumnId` type error is a consumer that must
   decide what a derived slot means to it. Fix each at the site, and put the site
   name in the `localSlot("...")` argument — that string is what a future audit
   greps for.
2. **Then walk `development.md`'s tables row by row.** Not the ones that look
   relevant — all of them. The rule is in that file: *a missing row is worse than
   a wrong one, because the next week reads it as already-checked.* For each row
   write one of: **level-aware** (which `ColumnId` call), **safe by domain**
   (which domain, and what enforces it), or **guarded** (what the guard is).
3. **Add a new section** — *Week 34 consumers (a range-table entry that is a
   plan)* — in the shape of the existing *Week 32 consumers* table, with a row
   for each of: `buildRelation`'s normalization, `LogicalDerived`, the four
   descend-to-SCAN walkers, `hasSlotOutsideRangeTable`'s new `n`,
   `checkAggregateArgType`, `distribute`/`filterOnto`, `collectSlotTables`,
   `CardinalityEstimator`'s missing `TableStats`, and `Planner::plan`'s refusal.

**Specific rows to re-check, with what to look for:**

- `collectSimplePredicates` / `ChunkPruner::shouldSkip` — the tripwire declines
  `query_level > 0`. A derived relation introduces no new level, so it stays
  armed and unreached **for that reason**; write the reason, not the verdict. But
  check the *other* half: the hint route reaches only a `LogicalScan`, and a
  derived relation is not one, so no hint is produced. Confirm by reading the
  hint's construction site, not by inference.
- `buildAggregateSchema`'s tripwire — same: a derived table adds no level. Its
  Week 33 restatement ("decorrelation refuses a GROUP BY body, so a correlated
  key here means the rewrite left one behind") **becomes false in Task 6**, which
  deliberately builds a `GROUP BY` body. That is a Task 1 sweep entry and the
  single most likely stale precondition of the week.
- `collectSlotTables` (`vectorized_plan_builder.cc:177`) — reads
  `join->join_slot` and `leafScanTable(left)`. With a derived left input,
  `isSingleRelation` returns `true` and `leafScanTable` names the body's base
  table, so the map gets a **wrong table at a real slot**. Week 32's version of
  this row was "wrong by omission three times"; do not make it four. The fix is
  `leafScanTableOrNull` and skipping the entry, which makes `rowWidth` fall back
  to the uniform proxy — Week 27's stance.
- `JoinKey::from_slot` — unchanged in meaning. At the bottom join over a derived
  left input the left child stamps 0, so `from_slot == 0`, exactly as for a leaf
  scan. Confirm `leftKeyIndices` resolves; it **throws** on a miss, deliberately,
  so a wrong normalization in Task 4 surfaces here rather than as wrong rows.
- `restampSlots` (site 9) — its `SubqueryExpr` branch stays unreachable. Its
  slot-0 restamp now also applies to conjuncts pushed onto a derived relation, and
  is correct **because** of Task 4's normalization. State the dependency; it is
  exactly the kind Week 29 found undocumented for `preserved_slots{0}`.

### Verification

- `grep -rn "localSlot(" src/` — every `site` string names a real function, and
  every one has been visited this week or is provably untouched by derived
  relations.
- Every `development.md` slot-table row has a Week 34 verdict with a *reason*.
- `--explain` on a three-relation query with a derived relation prints
  `join-ordering=skipped (derived relation)`; on a three-relation query with no
  derived relation it prints `order=… cost=… (written=…) method=dp`, byte-identical
  to before.
- A four-relation derived-table query returns the same rows optimized and
  `--no-optimize`. That equivalence is the differential oracle
  `compare_against_sqlite.py`'s fourth mode exists to be, and it is what a wrong
  `n` in `hasSlotOutsideRangeTable` would break.
- `SUM(d.name)` over a derived relation whose `name` column is a STRING is
  refused by the numeric-column message, not by `catalog.getTable("")`.
- The `method=written-floor` question is answered in writing: either "unreachable,
  because the enumerator declines any tree with a derived relation" or "reachable,
  and here is the query that reaches it".

---

## Task 6 — Q17 / Q22: correlated scalar decorrelation onto the same machinery

### Why it matters

This is the README's headline for the week and Week 33's recorded checkpoint miss.
It is also, now, the *cheap* task: Tasks 3–5 built the range-table entry for a
relation that is a plan, which was the whole blocker. Week 33's own words:

> The rewrite itself is short and is not the problem. … **The blocker is the
> join's output schema.** … A decorrelated scalar subquery is a derived table with
> an implicit join — the same shape, needing the same answer.

Do not re-derive the rewrite; it is written down. Do spend the care on the three
things Week 33 flagged as making it *materially harder than* the `EXISTS` case.

**A discrepancy to resolve rather than paper over.** The README consistently
groups "Q17, and Q22's correlated half" under correlated **scalar** subqueries. In
the standard TPC-H text, Q22's correlated component is a `not exists (select *
from orders where o_custkey = c_custkey)` — which is the `EXISTS` family Week 33
already decorrelated — while Q22's other distinguishing feature is a **derived
table** (`custsale`), which Tasks 2–4 deliver. Both readings land inside Week 34's
scope, so nothing is at risk either way; but do not claim Q22 "now works" on the
strength of Task 6 alone. Verify against the ported query in Week 36 and record
which half was which.

### Conceptual explanation

The rewrite, from the README:

```
outer  LEFT JOIN  ( SELECT team, AVG(speed) FROM laps l2 GROUP BY team )  ON team = team
```

Group the body by its correlation key, LEFT-join the result back, replace the
`SubqueryExpr` with a `ColumnRef` naming the aggregate output column. Three things
make it harder than Task 3 of Week 33:

**1. The join is `STANDARD`, so `output_schema` is MERGED.** That is precisely
the containment Tasks 3–5 replaced. The right child is a derived relation in every
sense that matters: a plan subtree occupying one slot of the outer range table,
its columns normalized to slot 0 and stamped with the join's slot at the merge.
**Build it as a `LogicalDerived`** — literally the same node — rather than as a
special case. If it is not the same node, the four descend-to-SCAN walkers, the
enumeration decline, and the slot-table rows all need a *second* argument, and the
two will drift.

**2. Zero-row and NULL semantics require `LEFT`.** SQL says a scalar subquery over
zero rows is NULL; an INNER join **drops** the outer row instead. Week 31 shipped
the typed-null `Literal` for exactly this case. So the join is LEFT-preserving,
and Week 29's rules then apply, all of them:

- pushdown will not cross to the null-supplying (derived) side — correct here,
  since a predicate on the aggregate output is not a predicate on the body;
- join enumeration declines to reorder **any** tree containing an outer join, so
  Q17 pays `join-ordering=skipped (outer join)`. That is a real, visible
  plan-quality cost and it is the honest one — it is reported, not hidden;
- the build side is forced: the preserved side must probe.

**3. The "more than one row" rule disappears, silently.** Week 31's deliberate
divergence (`scalar subquery returned more than one row`) is a *runtime
cardinality* check. After decorrelation there is no per-outer-row result to
count — the `GROUP BY` produces exactly one row per key **by construction**. For
an aggregate body that is the point. For a **non-aggregate** body
(`(SELECT name FROM drivers d2 WHERE d2.team = l.team)`) the check vanishes and a
query that should error returns an arbitrary row. **Refuse the non-aggregate
scalar body**, by name, and put it in the rejection suite.

**Recommended scope, unchanged from Week 33's own recommendation:** implement it
only for a body whose select list is a **single aggregate** with a `GROUP BY`-able
correlation equality — the Q17 shape — and refuse every other scalar shape. That
is minimum code that solves the problem, and point 3 is sound by construction only
for that shape.

### Code snippets

```cpp
// src/planner/subquery_decorrelation.h — a THIRD sibling, not a parameter on the
// EXISTS one.
//
// !! WHY NOT PARAMETERIZE requireDecorrelatableBody(). Its condition 3 says "the
// body has NO GROUP BY / HAVING / aggregate / LIMIT / DISTINCT", and THIS lowering
// requires an aggregate and ADDS a GROUP BY. Widening the shared guard with a flag
// would leave one function whose header states a rule it no longer enforces for
// half its callers -- which is the exact shape that produced three silent wrong
// answers in Week 33 and seven stale preconditions after it. Two guards, two
// headers, each true.
struct ScalarLoweringResult {
    std::unique_ptr<LogicalPlanNode> plan;
    int lowered = 0;
};

ScalarLoweringResult lowerCorrelatedScalars(std::unique_ptr<LogicalPlanNode> spine,
                                            std::vector<std::unique_ptr<Expr>>& conjuncts,
                                            const Catalog& catalog);
```

```cpp
// src/planner/subquery_decorrelation.cc
//
// UNLIKE EXISTS AND IN, a scalar subquery is NOT a whole conjunct. Q17 writes
// `l.speed > 0.2 * (SELECT AVG(...) ...)`, so the node sits arbitrarily deep
// inside the conjunct and must be REPLACED IN PLACE while the join is grafted
// onto the spine. forEachSubquery (dispatch site 19, the mutable walker that
// replaces through a slot) is the maintained walker for exactly that -- a private
// walker here would be a twentieth silent dispatch site.
void requireDecorrelatableScalarBody(const SelectStatement& body) {
    if (body.limit)      refuse("a scalar body with LIMIT cannot be decorrelated");
    if (body.distinct)   refuse("a scalar body with DISTINCT cannot be decorrelated");
    if (body.having)     refuse("a scalar body with HAVING cannot be decorrelated");
    if (!body.group_by.empty())
        refuse("a scalar body with its own GROUP BY cannot be decorrelated "
               "(the rewrite supplies the grouping)");
    if (body.select_list.size() != 1)
        refuse("a correlated scalar subquery must select exactly one expression");

    // THE LOAD-BEARING ONE. Without an aggregate, GROUP BY does not guarantee one
    // row per key, and Week 31's runtime `returned more than one row` check has
    // nowhere to live after the rewrite -- a query that SQL says is an error would
    // return an arbitrary row instead. Refusing is the only answer that keeps the
    // divergence table honest.
    std::vector<const AggregateExpr*> found;
    collectAggregates(body.select_list[0].get(), found);
    if (found.size() != 1 || found[0] != body.select_list[0].get())
        refuse("a correlated scalar subquery is decorrelated only when its select "
               "list is a single aggregate (a non-aggregate body has no "
               "one-row-per-key guarantee, so 'returned more than one row' could "
               "not be checked)");
}
```

```cpp
// The rewrite. splitCorrelation() is REUSED VERBATIM from Week 33 -- it already
// produces JoinKey{outer_col, body_col, outer_slot} plus the body-side refs.
    std::vector<JoinKey> keys;
    std::vector<std::unique_ptr<Expr>> body_key_refs, local;
    splitCorrelation(body_conjuncts, keys, body_key_refs, local);
    if (keys.empty()) refuse("no equality links the subquery to the enclosing query");

    body.where = conjoinAll(std::move(local));
    refuseSurvivingCorrelatedRefs(body);

    // GROUP BY the correlation keys, SELECT them plus the aggregate. The group
    // keys must be FIRST and in key order, because buildAggregateSchema emits
    // group columns then aggregates -- so the right-side key indices are
    // positional 0..k-1, exactly the shape Week 33's body projection produced and
    // Week 32's IN lowering had by taking body column 0.
    for (auto& ref : body_key_refs) {
        auto* cr = static_cast<ColumnRef*>(ref.get());
        body.group_by.push_back(GroupByColumn{cr->table_name, cr->column_name, cr->id, nullptr});
    }
    auto agg_expr = std::move(body.select_list[0]);
    // The aggregate's output column name is the ONE name that must be unique in
    // the MERGED schema, because the outer predicate reads it by name after
    // substitution. aggregateOutputName IS exprToString, so `AVG(l2.speed)` is
    // already distinctive -- but an outer relation could hold a column of that
    // name, and `indexOf(name, slot)` would then be answering about the wrong
    // relation. The SLOT is what disambiguates it: substitute a ColumnRef stamped
    // ColumnId::local(<the derived relation's outer slot>), never a bare name.
    const std::string agg_name = aggregateOutputName(
        static_cast<const AggregateExpr*>(agg_expr.get()));
    body.select_list.clear();
    for (auto& ref : body_key_refs) body.select_list.push_back(cloneExpr(ref.get()));
    body.select_list.push_back(std::move(agg_expr));

    auto body_plan = LogicalPlanBuilder::build(std::move(body), catalog);
    // SAME NODE as Task 4's FROM (subquery). Not a special case: if this is not a
    // LogicalDerived, then the descend-to-SCAN walkers, the enumeration decline and
    // every development.md row need a second argument, and the two will drift.
    auto derived = std::make_unique<LogicalDerived>(std::move(body_plan),
                                                    synthetic_alias, normalized);

    // MERGED schema, join_slot = the derived relation's slot in the OUTER range
    // table, join_type = LEFT. LEFT because a zero-row group must yield NULL, not
    // a dropped outer row (Week 31's typed-null Literal exists for that case).
    // Week 29's three consequences follow and are all correct here: pushdown
    // declines the derived side, the build side is forced, and enumeration
    // declines the tree -- reported as `join-ordering=skipped (outer join)`.
```

### Implementation guidance

**Where it hooks.** The same site as Weeks 32 and 33, in
`LogicalPlanBuilder::build`, and in this order:

```cpp
InLoweringResult     lowered      = lowerInSubqueries(std::move(node), conjuncts, catalog);
ExistsLoweringResult decorrelated = lowerExistsSubqueries(std::move(lowered.plan), conjuncts, catalog);
ScalarLoweringResult scalars      = lowerCorrelatedScalars(std::move(decorrelated.plan), conjuncts, catalog);   // Week 34
node = std::move(scalars.plan);
stmt.where = conjoinAll(std::move(conjuncts));
refuseUnloweredCorrelated(stmt.where.get(), "a non-top-level position");
refuseUnloweredIn(stmt.where.get(), "a non-top-level position");
```

The position is load-bearing in both directions, for the two reasons already
written at that site: the join's probe input must be the whole FROM/JOIN spine so
the correlated operand's binder slot resolves in the domain `leftKeyIndices()`
uses, and the node must leave the predicate **before** `inferExprType` walks it —
dispatch site 12 still throws on a surviving `SubqueryExpr`, deliberately.

**The slot for the derived relation.** It is a new range-table position that the
Binder never issued, because the rewrite happens at plan time. Two options and
they are not equivalent:

- Give it `range_table_size` (one past the last real slot). Simple, and every
  consumer that was taught "a slot may name a derived relation" in Task 5 already
  handles it — **provided** `hasSlotOutsideRangeTable`'s `n` is told about it.
  Since Task 5 passes `n` down from the builder, increment it here.
- Reuse `-1`, as SEMI/ANTI do. **Wrong**: `-1` means "children[1] is not in scope
  above this node", which is false here — the outer `WHERE` reads its column.

Take the first, and write the increment down where `n` is computed, or Task 5's
consumer 1 silently declines every Q17 tree for the wrong reason.

**Sites 12 and 13 close naturally.** `inferExprType` and `evaluate` throw a
Week-31 message for a surviving `SubqueryExpr`. After this task no correlated
scalar survives into a plan, which is what Week 30's note required ("both sites
must close in the *same* commit that lowers one"). Keep the throws — they are the
tripwire for a missed position — but re-read their messages: they name Week 31 and
a materialization walker, and after this week the likelier cause is an unlowered
correlated scalar. That is a Task 1 sweep entry.

**`buildAggregateSchema`'s tripwire.** Its Week 33 restatement is "decorrelation
refuses a `GROUP BY` body, so a correlated key here means the rewrite left one
behind". This task builds a `GROUP BY` body — but its group keys are the **body
side** of the correlation, which are level 0 against the body's own range table,
so the tripwire stays armed and unreached. Confirm that by running the query, not
by reading, and restate the comment with the new reason.

**Anticipated mistakes:**

- Widening `requireDecorrelatableBody` with a flag instead of writing a second
  guard. Named above; it is the week's likeliest stale-precondition defect.
- Substituting a bare-name `ColumnRef` for the aggregate output. It resolves —
  and it resolves against whichever relation holds that name first.
- Building the join `INNER` because the test data has a matching group for every
  outer row. It passes, and it is wrong on the first row with no match. Construct
  that row deliberately; the shipped catalog may not contain one.
- Forgetting `sq->subquery.use_count() > 1`. Week 33's guard exists because
  `cloneExpr` shares the statement, and `BETWEEN` clones its left operand before
  binding. `(SELECT ...) BETWEEN a AND b` is legal syntax.

### Verification

- Q17's shape returns rows identical to SQLite in both vectorized modes; the
  Volcano refusal is pinned by message in the other two.
- **A body with a zero-row group returns NULL for that outer row, not a dropped
  row.** Construct the case deliberately.
- `--explain` shows **one** `LogicalAggregate` under the join, not one per outer
  row, and prints `join-ordering=skipped (outer join)`.
- `refuseUnloweredCorrelated` still fires for a correlated scalar under an `OR`
  and in `HAVING` — the positions this task does not lower.
- A non-aggregate correlated scalar body is refused by the new message, and the
  refusal is in the rejection suite (Task 9): the diffed oracle cannot hold a
  query that errors.
- `WEEK33_CORRELATED_BINDS` **shrank**, and every shape that left it arrived in a
  diffed suite. Nothing may leave a rejection suite without arriving somewhere
  else — Week 33's own standard.

---

## Task 7 — `COUNT(DISTINCT ...)`: per-group state in both engines

### Why it matters

The README's second bullet, and TPC-H Q16's `count(distinct ps_suppkey)`. It is
independent of Tasks 2–6 and can be done first if the week runs hot. It is also
the task where the project's single most expensive past defect is one line away
from being reintroduced.

### Conceptual explanation

An aggregate today is a fixed-size accumulator: `count`, `sum`, `min_val`,
`max_val`. `COUNT(DISTINCT x)` is the first aggregate needing **unbounded
per-group state** — a set of the values seen in that group. Three consequences:

**1. The set's key must be the shared serializer, not `Value::toString()`.** This
is Week 27's most expensive finding, and it was found by the SQLite oracle only:

> `Value::toString()` formats a DOUBLE with `%.15g` for human output, which is
> lossy. Over the shipped 10k-row dataset `sector_1 + sector_2` takes **3245**
> distinct values but only **2526** distinct `%.15g` texts, so
> `SELECT DISTINCT sector_1 + sector_2 FROM laps` returned 2526 rows against
> SQLite's 3245 … in all four modes.

`src/execution/key_encoding.h` is one shared contract covering all six existing
serializers precisely so that a seventh does not lose injectivity, the NULL policy
and exact DOUBLE comparison by hand-rolling. Use it. This is the whole correctness
risk of the task.

**2. `DISTINCT` must reach the output column name.** `aggregateOutputName` **is**
`exprToString`, and it is the byte-for-byte contract between `buildAggregateSchema`
and `evaluate()`'s lookup — and `extractAggregates` **dedupes specs by that name**.
If `exprToString` renders `COUNT(DISTINCT x)` as `COUNT(x)`, then
`SELECT COUNT(x), COUNT(DISTINCT x) FROM t` collapses into **one** spec and one
output column, and both select-list entries read it. That is a silent wrong answer
in a single query, produced by a missing five characters.

**3. NULLs are already right.** SQL says `COUNT(DISTINCT x)` ignores NULLs, and
both aggregate nodes already `continue` on `val.isNull()` before touching the
accumulator. Confirm rather than change.

**Scope.** Allow `DISTINCT` on `COUNT` only. `SUM(DISTINCT)`/`AVG(DISTINCT)` are
legal SQL, no TPC-H query in the documented dialect needs them, and each is a
different accumulator (`MIN(DISTINCT x) == MIN(x)`, which is a trap of its own —
it is legal, it is a no-op, and implementing it "for free" invites the reader to
believe the others work). Refuse the others by name: minimum code that solves the
problem, and a dialect-table row.

### Code snippets

```cpp
// src/parser/ast.h
struct AggregateExpr : Expr {
    std::string function_name;
    std::unique_ptr<Expr> argument;
    bool is_star;
    // Week 34. COUNT(DISTINCT x). LAST field, so positional brace-inits that
    // predate it stay valid -- the discipline AggregateSpec::argument,
    // GroupByColumn::expr and JoinType all follow.
    //
    // !! IT MUST REACH exprToString(). aggregateOutputName IS exprToString and
    // extractAggregates DEDUPES SPECS BY THAT NAME, so rendering this node as
    // COUNT(x) collapses `SELECT COUNT(x), COUNT(DISTINCT x)` into ONE spec and
    // one output column that both select items read. Five missing characters, a
    // wrong answer, no error.
    bool distinct = false;
};
```

```cpp
// src/parser/expr_utils.h — exprToString's AggregateExpr case.
if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
    std::string arg = agg->is_star ? "*" : exprToString(agg->argument.get());
    return agg->function_name + "(" + (agg->distinct ? "DISTINCT " : "") + arg + ")";
}
// cloneExpr (dispatch site 11) must copy `distinct` for the same reason it copies
// Literal::null_type (Week 31): it is part of the node's MEANING, and dropping it
// across a clone makes the node disagree with itself. BETWEEN clones its left
// operand and the GROUP BY / ORDER BY alias substitution clones a bound select
// item, so a lost flag is reachable, not theoretical.
```

```cpp
// src/execution/vec_hash_aggregate_node.h — per-group state.
struct SpecAccum {
    int64_t non_null_count = 0;
    double  sum = 0.0;
    Value   min_val;
    Value   max_val;
    // Week 34. Present ONLY for a COUNT(DISTINCT) spec -- empty and free for
    // every other, so no existing query pays for it. Keyed through
    // key_encoding.h, NOT Value::toString(): %.15g is lossy and collapsed 3245
    // distinct doubles into 2526 texts in Week 27, in all four modes, visible
    // only to the SQLite oracle. Six serializers already share that contract;
    // this is the seventh.
    std::unordered_set<std::string> distinct_keys;
};
```

```cpp
// The accumulate loop, immediately after the existing NULL skip (which is already
// COUNT(DISTINCT)'s NULL rule -- SQL ignores NULLs -- so nothing is added there).
if (val.isNull()) continue;
sa.non_null_count++;
if (spec.distinct) {
    std::string key;
    appendKeyField(key, val);          // the shared serializer, key_encoding.h
    sa.distinct_keys.insert(std::move(key));
}
...
// materializeResults(), the COUNT branch:
if (spec.function == "COUNT") {
    int64_t n = spec.is_star           ? acc.count
              : spec.distinct          ? static_cast<int64_t>(sa.distinct_keys.size())
                                       : sa.non_null_count;
    row.push_back(Value(n));
}
```

### Implementation guidance

**Parser.** One `match(TokenType::DISTINCT)` after the `(`. `DISTINCT` is already
a `TokenType` (used by `SELECT DISTINCT`), so nothing lexes differently.
`COUNT(DISTINCT *)` must be a parse error — `DISTINCT` followed by `STAR` — and so
must `DISTINCT` on the other four functions, refused with the function named.

**Both engines, one behaviour.** `HashAggregateNode` (`plan_nodes.cc`) and
`VecHashAggregateNode` take the identical change. Volcano is the correctness
baseline (invariant 6) and `compare_against_sqlite.py`'s two Volcano legs are half
the evidence the vectorized operator is right — so `COUNT(DISTINCT)` is a **four-mode**
feature, unlike everything else this week. That is worth saying in the harness
comment, because it is the one Week 34 deliverable that restores rather than
narrows coverage.

**Sites to visit, and the ones you can skip.** Adding a *field* is not adding a
*node type*, so the nineteen dispatch sites are not all in play. The ones that
are: `exprToString` (site 1's neighbour), `cloneExpr` (site 11), `exprKey` (site
1 — two aggregates differing only in `distinct` must not share an identity, or
`substituteInto` rewrites one as the other), and `Validator`'s nested-aggregate
check (unchanged, but read it: `COUNT(DISTINCT SUM(x))` must still be refused as
a nested aggregate, not accepted because a new flag was in the way).

**Memory, stated rather than discovered.** The distinct set is unbounded and lives
until the aggregate is drained — one set per group per distinct spec. On a
high-cardinality group key over 1M rows that is real memory, and it is the first
aggregate in this engine whose state is not O(1) per group. Add it to
**Limitations**, in the shape the `IN` materialization cap used to have, and note
that spill-capable aggregation is already a Possible Extension.

**Cardinality and cost.** Grouping is unchanged, so the aggregate's output row
estimate is unchanged; `CardinalityEstimator` needs nothing. Say so in the commit
rather than leaving a reader to wonder whether it was missed.

### Verification

- `SELECT team, COUNT(DISTINCT driver_id) FROM laps GROUP BY team` diffs against
  SQLite in **all four modes**.
- `SELECT COUNT(driver_id), COUNT(DISTINCT driver_id) FROM laps` returns **two
  different numbers in two columns**. This is the `extractAggregates` dedupe test
  and it is the one that catches the `exprToString` omission; without it the bug
  is invisible.
- A DOUBLE argument: `COUNT(DISTINCT sector_1 + sector_2)` diffs against SQLite.
  This is the Week 27 repro shape and the only test that proves `key_encoding.h`
  was used rather than `toString()`.
- NULLs are excluded: build a column with NULLs (an outer join, or `x / 0`) and
  confirm `COUNT(DISTINCT x)` ignores them while `COUNT(*)` does not.
- Global (no `GROUP BY`) and empty-input cases: `COUNT(DISTINCT x)` over zero rows
  is `0`, not NULL.
- `COUNT(DISTINCT *)`, `SUM(DISTINCT x)` and `MIN(DISTINCT x)` are refused by
  message and are in the rejection suite.
- `--explain` renders `COUNT(DISTINCT driver_id)` in the aggregate node's string.

---

## Task 8 — The two inherited capability boundaries

### Why it matters

Week 33's *Handed to Week 34* list has four items. Two of them are decisions this
week must **make**, not inherit: the correct-but-slower fallback, and Volcano
semi/anti parity. The other two (`WEEK33_CORRELATED_EXPECT` being a prefix rather
than a message, and the remaining `buildScanSchema` narrowing) belong to Task 9
and Task 3 respectively.

A decision here means one of two things, and only two: **build it**, or
**re-decline it explicitly, in writing, with what changed since the last
decline**. Silently carrying a hand-off forward is the failure this project
already refuses by convention — Week 33 recorded its own miss as a miss rather
than absorbing it, and that is the standard.

### Conceptual explanation

**(a) The correct-but-slower fallback.** The README's Week 33 bullet asked for
"a correct fallback for unsupported patterns"; what shipped was a **refusal**,
named as one. The argument for refusing, restated:

> A real fallback means a dependent-join operator (re-execute the body per outer
> row), which this engine has never had, and a second execution production that
> must agree with the first on `NOT EXISTS`'s NULL semantics is the two-paths
> drift Weeks 26/28/30 each had to undo.

Nothing in Week 34 changes that argument. A dependent join is a new physical
operator on both engines, with its own NULL rule, its own cardinality rule and its
own `--explain` surface, arriving in the same week that removes the schema
containment two prior weeks were built on. **Recommendation: re-decline, and make
the re-decline informative** by recording what actually changed:

- Shapes that were refused in Week 33 and now **execute**: the correlated scalar
  aggregate body (Task 6). The refusal set shrank; say by how much.
- Shapes that remain refused, and why each is a *language* limit rather than an
  engine one where that is true: a correlated inequality (`JoinKey` holds column
  names), a computed key side, a reference more than one block out, a correlated
  `IN`, an `EXISTS` that is not a whole top-level conjunct.
- The one honest cost: SQLite answers all of these, so each is a **divergence**
  and belongs in the dialect table with a pinned message.

**(b) Volcano semi/anti parity.** Week 33 stated the shape precisely:

> It is two halves and only one is cheap: `JoinSemantics` in
> `src/execution/hash_join_node.cc` (small), plus a Volcano plan shape that can
> hold a *second* join, since `Planner::plan` builds exactly one `HashJoinNode`
> out of `stmt.joins` (not small, and its guards have undocumented couplings).

Week 33's Task 7 then **corrected the pointer**, and the correction matters: the
Volcano hash join lives in `src/planner/plan_nodes.h`, **not** in a
`src/execution/hash_join_node.cc`, and it has no `JoinSemantics` parameter at
all — so a semi/anti join is not representable there. The refusal is total
*structurally*; the `Planner::plan` refusals exist to give a named message, not
to close a hole.

That makes the "cheap half" cheaper than advertised and the expensive half
unchanged: `Planner::plan` still builds exactly one join from `stmt.joins`, and
its `preserved_slots`, its single `jc` clause pointer and its `joins.size() > 1`
refusal are coupled across 110 lines — Week 29's audit found `preserved_slots{0}`
was correct *only* because of that refusal.

**Week 34 makes this worse, and that is the news to record.** Derived tables add a
**third** Volcano capability refusal (Task 4, Step 5), on top of `IN` (Week 32)
and correlated (Week 33). The set of queries diffed in two modes rather than four
grows again. **Recommendation: re-decline for this week, and quantify it** — count
the queries in `WEEK32_SEMI_JOIN_VEC_ONLY` + `WEEK33_DECORRELATED_VEC_ONLY` +
the new Week 34 vec-only suites, and put the number in the README's Limitations.
A number is falsifiable; "some queries" is not.

The counter-argument deserves stating because it may win in Week 37: Volcano is
the correctness baseline (invariant 6), and the four-mode oracle is *the* reason
the vectorized operators are trusted. Every week that adds a vectorized-only
feature spends baseline coverage. At some point the balance tips and rebuilding
`Planner::plan` around the logical layer is cheaper than the accumulated blind
spot. Week 34 is not that week — it is already removing a containment — but the
number should be in the README so that the week where it tips is visible.

### Implementation guidance

There is no code in this task. There are four artifacts:

1. **A dialect-table row per surviving refusal**, in `README.md` →
   *Syntax Deliberately Not Supported*, in the established three-column shape
   (*Not supported* / *Rejected with* / *Why*). Week 34 adds at least: a derived
   table on the Volcano path; an unaliased derived table; a `LATERAL` derived
   table; a derived table producing two columns of one name; a column-alias-list
   arity mismatch; a non-aggregate correlated scalar body; `DISTINCT` on an
   aggregate other than `COUNT`.
2. **A Limitations bullet** stating the four-mode coverage cost with a count.
3. **An update to Week 33's *Handed to Week 34* list**, in
   `docs/week-33-plan.md`, marking each of the four items done or re-declined.
   Do this in `docs/week-33-plan.md` itself, not only here: a reader following the
   hand-off from that file must not have to know Week 34's plan exists.
4. **The re-decline, written where the code is**, not only in a doc. The
   `Planner::plan` refusal block already carries "what this costs, stated rather
   than discovered later"; extend it rather than adding a fourth parallel comment.

### Verification

- Every refusal that survives the week has: a dialect-table row, a
  rejection-suite entry with the message pinned, and a comment at the throw site.
  Three places, and the harness one is not optional — `compare_against_sqlite.py`
  cannot diff a query that errors, which is the blind spot Week 30 established the
  rejection-suite pattern for.
- Week 33's hand-off list is fully dispositioned; no item is silently carried.
- The vec-only query count in Limitations matches
  `grep -c` over the suites. A number that has to be recomputed by hand is a
  number that will be wrong by Week 36.

---

## Task 9 — Harness and tests

### Why it matters

Two rules from prior weeks, both learned expensively, both directly applicable:

**The harness blind spot.** `compare_against_sqlite.py` diffs *rows*; it cannot
hold a query that **errors**. So every newly refused shape needs a rejection-suite
entry that pins the message, or it is untested — this is why
`WEEK31_MATERIALIZATION_REFUSED`, `WEEK32_LOWERING_REFUSED` and
`WEEK33_CORRELATED_BINDS` exist. Week 34 adds seven or more refusals (Task 8).

**Week 32's lesson, quoted because it is the standard this task is held to:**

> A subquery nested inside an `IN` body died at dispatch site 12 with an
> internal-invariant message — past a **green 988-query oracle and 770 green unit
> tests**. Neither could see it, because the one test guarding that capability had
> been **narrowed, in that same week**, to a flattened stand-in that no longer
> reached through `sq->subquery->where` to the nested node. The suite was green
> about a weaker claim.

Week 34 changes `TableRef` — which touches **every test that hand-builds a
`SelectStatement`**. That is the largest mechanical test edit since Week 33's
`ColumnId`, and mechanical edits are exactly where a capability vanishes.

### Conceptual explanation

Three categories of harness work, and they are not interchangeable.

**Diffed suites** — queries that return rows, compared against SQLite. The mode
matrix is per-feature:

| Feature | Modes | Why |
|---|---|---|
| `COUNT(DISTINCT)` | **all four** | Both aggregate nodes implement it; this is the one Week 34 deliverable that does not narrow coverage |
| Derived tables | two vectorized | `Planner::plan` cannot hold a relation that is a plan (Task 4, Step 5) |
| Correlated scalar (Q17) | two vectorized | Inherits the Week 33 boundary, unchanged |

**Rejection suites** — the message pinned, in the modes where the refusal fires.
A capability refusal (Volcano) fires in two modes and the query is *diffed* in the
other two; a language refusal (unaliased derived table) fires in all four.

**Unit tests** — for what the oracle structurally cannot see: the LATERAL refusal
firing by the *right* message rather than by column-not-found; the drift assertion
in Task 4 firing when `blockOutputSchema` is broken; `join-ordering=skipped
(derived relation)` appearing on the `--explain` line.

### Code snippets

```python
# python_tools/compare_against_sqlite.py
#
# Week 34. Diffed in the two VECTORIZED modes only: Planner::plan builds exactly
# one HashJoinNode out of stmt.joins and has no plan shape that can hold a
# relation which is a PLAN, so derived tables are refused there. The refusal is
# pinned by message in the two Volcano modes below -- this suite cannot hold a
# query that errors, which is the whole reason the paired suite exists.
WEEK34_DERIVED_TABLE_VEC_ONLY = [
    # the plain shape
    "SELECT d.team FROM (SELECT team FROM laps) AS d",
    # Q15's shape: a derived table JOINED to a base relation, with an aggregate
    # body -- the case the slot-0 normalization exists for
    "SELECT dr.name, d.s FROM (SELECT driver_id, AVG(speed) AS s FROM laps "
    "GROUP BY driver_id) AS d JOIN drivers dr ON d.driver_id = dr.driver_id "
    "ORDER BY dr.name",
    # Q13's shape: column alias list, and a body that JOINS -- so the body's own
    # output schema carries slots 0 AND 1 before normalization
    "SELECT c.n, COUNT(*) FROM (SELECT dr.name, COUNT(l.lap_id) FROM drivers dr "
    "LEFT JOIN laps l ON dr.driver_id = l.driver_id GROUP BY dr.name) AS c (n, k) "
    "GROUP BY c.n ORDER BY c.n",
    # a derived table INSIDE a subquery body -- two nesting mechanisms at once,
    # which is the shape Week 32 shipped a regression in and no suite could see
    "SELECT team FROM laps WHERE speed > (SELECT AVG(x.s) FROM "
    "(SELECT AVG(speed) AS s FROM laps GROUP BY team) AS x)",
]

WEEK34_DERIVED_TABLE_VOLCANO_REJECTED = [
    (query, "not supported on the Volcano path")
    for query in WEEK34_DERIVED_TABLE_VEC_ONLY
]

# All FOUR modes: both aggregate nodes implement it. The second query is the one
# that catches an exprToString that forgot DISTINCT -- extractAggregates dedupes
# specs by aggregateOutputName, so without the five characters these two select
# items collapse into ONE column and both read it.
WEEK34_DISTINCT_AGG_QUERIES = [
    "SELECT team, COUNT(DISTINCT driver_id) FROM laps GROUP BY team ORDER BY team",
    "SELECT COUNT(driver_id), COUNT(DISTINCT driver_id) FROM laps",
    # the Week 27 repro shape: %.15g collapsed 3245 distinct doubles into 2526
    # texts, in all four modes, visible only to this oracle
    "SELECT COUNT(DISTINCT sector_1 + sector_2) FROM laps",
    "SELECT COUNT(DISTINCT team) FROM laps WHERE season = 1900",   # empty -> 0
]

# Refused in ALL four modes: these are LANGUAGE refusals, not capability ones.
WEEK34_REFUSED = [
    ("SELECT * FROM (SELECT team FROM laps)",                  "requires an alias"),
    ("SELECT * FROM laps l JOIN (SELECT team FROM drivers d WHERE d.team = l.team) "
     "AS x ON x.team = l.team",                                "LATERAL is not supported"),
    ("SELECT * FROM (SELECT l.team, d.team FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id) AS x",                     "is produced twice"),
    ("SELECT * FROM (SELECT team FROM laps) AS d (a, b)",      "column aliases were supplied"),
    ("SELECT COUNT(DISTINCT *) FROM laps",                     "DISTINCT"),
    ("SELECT SUM(DISTINCT speed) FROM laps",                   "DISTINCT"),
]
```

### Implementation guidance

**The changed-test discipline, made concrete.** Week 34 will edit at least
`tests/test_parser.cc`, `test_binder.cc`, `test_planner.cc`, `test_logical_plan.cc`,
`test_vec_plan_builder.cc`, `test_join_enumeration.cc`, `test_predicate_pushdown.cc`,
`test_execution.cc`, `test_vectorized.cc` and `test_subquery.cc` — most of them
only because `stmt.from_table = "laps"` becomes `stmt.from = TableRef{"laps"}`.

For **every test this week edits or deletes**, name the assertion that left and
where it went. Week 33's round-4 audit did exactly this for
`NoLongerRefusesALargeInSet` and found nothing lost; that is the standard, and it
takes minutes. Write the translation rules down so a reviewer can check them
mechanically, exactly as Week 33 did for the `ColumnId` migration:

```
stmt.from_table = "laps";                  ->  stmt.from = TableRef{"laps"};
stmt.from_table = "laps"; from_alias = "l" ->  stmt.from = TableRef{"laps", nullptr, "l"};
stmt.joins.push_back({"drivers", "d", ...}) ->  ... TableRef{"drivers", nullptr, "d"} ...
```

No assertion may be weakened. A test that needed more than a translation was
asserting something about the *old encoding*; say what, in the commit message.

**Two harness traps Week 35's note already documents, and they bite here.**

- A query with **no `ORDER BY` must be sorted before diffing**. A derived table
  changes physical emission order legitimately, and that is not a result
  difference. Several of the suite queries above carry an explicit `ORDER BY` for
  this reason; the rest must go through the sorted comparison.
- `normalize()` keys rows by column **name**. `SELECT *` over a derived table
  whose column names came from a column-alias list is fine; `SELECT *` over a
  derived table joining two relations that share a column name is not — and Task 3
  refuses that shape, which is one more reason the refusal is right.

**Unit tests the oracle cannot replace** — write these, they are the ones that
pin decisions rather than results:

- `BinderTest.DerivedTableCannotSeeTheEnclosingBlocksRelations` — asserts the
  **LATERAL message**. This is what pins the `parent` argument in Task 3; a
  refactor that changes it would otherwise leave a green suite.
- `LogicalPlanTest.DerivedRelationColumnsAreStampedSlotZero` — reads the
  `LogicalDerived`'s `output_schema` directly. The normalization is the week's
  central invariant and nothing else asserts it directly.
- `LogicalPlanTest.GraftAssertionFiresWhenBlockOutputSchemaDrifts` — the Task 4
  drift check, exercised. An assertion never seen to fail is the one Week 33
  deleted for being unfailable.
- `JoinEnumerationTest.ReportsTheDeclineForADerivedRelation` — asserts the
  `join-ordering=skipped (derived relation)` string, not just that the order is
  unchanged.
- `ExecutionTest` / `VectorizedTest` pairs for `COUNT(DISTINCT)` over a DOUBLE
  column, asserting the two engines agree — the four-mode oracle covers this too,
  but the operator-level test is what localizes a failure to an engine.

### Verification — the week's success criteria

The week is done when all of these hold, and not before:

1. **Build clean**, no new warnings.
2. **Unit suite green**, with a written account of every changed test.
3. **`compare_against_sqlite.py` green in all four modes**, including the new
   diffed and rejection suites.
4. `WEEK33_CORRELATED_BINDS` has **shrunk**, and every shape that left it arrived
   in a diffed suite. Nothing leaves a rejection suite without arriving somewhere.
5. **Optimized ≡ `--no-optimize`** on every new query, in both vectorized modes.
   That equivalence is what a wrong `n` in `hasSlotOutsideRangeTable` breaks, and
   it is the fourth mode's whole reason to exist.
6. Every pre-existing query's `--explain` output is **byte-identical**.
7. Task 1's sweep checklist is fully ticked, each with a finding rather than a
   "checked".

---

## Task 10 — The closing sweep

### Why it matters

Task 1 built the worklist. This is where it is discharged, and it is a task rather
than a chore because of the measured history: **Week 33 shipped three silent wrong
answers from stale guarantees, two of them in header comments, and a sweep
afterwards found seven more.** The comments were not decoration; they were the
preconditions later code was written against.

Week 34 is worse-exposed than Week 33 was, because Week 33 *removed* a refusal
while Week 34 removes an **invariant that two weeks were built on**. Anything that
reasoned "a body column is never in scope above the join" reasoned from something
that is now conditional.

### Conceptual explanation

The discipline, stated as a rule you can check someone against:

> Every citation is rewritten to state **what is now true and why**, never that it
> was reviewed. "Checked in Week 34" is not a finding and reads to the next week
> exactly like the stale text did.

And the specific trap this project has hit three times: a comment that is *still
true* but for a **different reason** than it says. `buildAggregateSchema`'s
tripwire is the live example — it stays armed and unreached after Task 6, but its
Week 33 reason ("decorrelation refuses a `GROUP BY` body") becomes false, and the
new reason (the body's group keys are level 0 against the body's own range table)
is a different sentence. Leaving the old reason is the shape that produced two of
Week 33's three wrong answers.

### Implementation guidance

Discharge in this order, because each layer's truth depends on the one below.

**1. Code comments and headers** — Task 1's list. The highest-risk five:

- `src/planner/logical_plan.h`, the `LogicalJoin::semantics` invariant block. It
  is still true *for `SEMI`/`ANTI`*. What must change is the surrounding claim
  that it is "the whole containment for the two-range-table problem this node
  introduces" — it is now **one of two** containments, the other being Task 4's
  slot-0 normalization. Name both, and point at each other.
- `src/planner/subquery_decorrelation.h`, condition 3. "A body with an aggregate
  cannot be decorrelated" is now true of `lowerExistsSubqueries` and false of
  `lowerCorrelatedScalars`. Two guards, two headers, each true of its own.
- `src/planner/join_enumeration.cc`, the `hasSlotOutsideRangeTable` block, which
  predicts this week by name. Replace the prediction with what happened: `n` is
  now the range-table size passed from the builder, and the decline is reported.
- `src/storage/chunk_pruner.h`, the scan-local justification. Still correct;
  re-derive it under derived relations (a derived relation is not a `LogicalScan`,
  so it receives no hint) and say *that*, rather than "unchanged".
- `src/planner/plan_nodes.h` — the Volcano `HashJoinNode`. Week 33 found the
  README and Week 32's hand-off both pointed at a nonexistent
  `src/execution/hash_join_node.cc`. If any citation of that path survives
  anywhere, this is the week to kill it.

**2. `development.md`.** Two edits, and the second is the one that carries value
forward:

- Rewrite the sentence "**Week 34's derived tables break this containment for
  real**" into a record of *how* it was broken and *what replaced it*.
- Add the **Week 34 consumers** table (Task 5, step 3). Every row states
  level-aware / safe-by-domain / guarded **with its reason**. A missing row is
  worse than a wrong one.

**3. `README.md`.** Six places:

- The subquery bullet in *In Scope* — `FROM (subquery)` no longer says "is Week 34".
- The dialect table — remove the "FROM (subquery) is Week 34" pointers from the
  two rows that carry them, add Task 8's new rows.
- *Limitations* — the derived-table Volcano bullet with the counted cost, the
  `COUNT(DISTINCT)` memory bullet, and an update to the correlated-subquery bullet
  (correlated scalars over an aggregate body now execute).
- The **Week 34 section** — a *Shipped / Why it was required* table in the shape
  every prior Phase 5 week uses, plus the checkpoint mark. If any part of the
  checkpoint is not met, record it as a **miss**, as Week 33 did, not as a
  qualified success.
- Week 33's checkpoint line, which says the correlated-scalar family is a miss —
  it stays as the record of Week 33, but should point forward to where it closed.
- The 37-week plan table row for Week 34.

**4. `docs/week-33-plan.md` → *Handed to Week 34*.** All four items dispositioned
(Task 8).

**5. Re-run Task 1's five greps.** Every new hit is this week's own code and is
accounted for.

### Verification

- Task 1's checklist has no unticked entries, and every entry's resolution names a
  fact rather than an activity.
- `grep -rn "Week 34" src/ tests/ python_tools/` returns only *records* of what
  Week 34 did — no surviving promises. Any remaining forward pointer names Week 35
  or later.
- `grep -rn "hash_join_node.cc" .` returns nothing outside historical plan
  documents.
- A reader who opens `logical_plan.h` cold can state, from the comments alone, why
  a derived relation's columns are safe above the join and why a semi-join's body
  columns are not in scope. If they cannot, the sweep is not finished.

---

## Progress

*(Task 1's sweep checklist lives here, and every task's outcome — including
anything cut or re-declined — is recorded here rather than left to be inferred
from the diff. A successor agent resumes from this section.)*

- [x] Task 1 — refusal inventory and sweep checklist (below)
- [x] Task 2 — `TableRef` landed. `table_name_` private, `tableName(site)` is the
      narrowing point (the `ColumnId::localSlot` discipline). 94 sites; unique_ptr
      ownership, with the asymmetry vs `SubqueryExpr` stated at the field.
- [x] Task 3 — Binder derived range-table entries. `Scope::owned_schemas`;
      `relationSchema` binds the body against `parent` (NOT LATERAL, the argument
      IS the rule); `has_derived_table` is a SEPARATE flag from `has_subquery`.
      Known boundary recorded: at top level there is no parent, so a lateral
      reference reports as an ordinary unresolved qualifier.
- [x] Task 4 — graft. `LogicalDerived` + `VecDerivedNode`;
      `derivedRelationSchema` flattens to slot 0; `blockOutputSchema` shared with
      the Binder and asserted against the built plan at the graft; four
      descend-to-SCAN walkers stopped at the node; `Validator` rebuilt around one
      range table; `Planner::plan` refuses derived tables (third Volcano refusal).
- [ ] Task 3 — Binder: derived range-table entries
- [ ] Task 4 — `LogicalPlanBuilder`: graft and normalize
- [x] Task 5 — the three synthetic-slot consumers. **One of the three did not
      break the way three prior weeks predicted**, and that is the week's main
      finding: `hasSlotOutsideRangeTable` does NOT fire for a derived relation.
      What was wrong was `countRelations`, which counted SCANS and so over-counted
      a derived body's; with it counting the spine, a derived relation is an
      ordinary in-range range-table entry and the search reorders it (`method=dp`,
      optimized ≡ `--no-optimize`, verified). No reported decline was added — a
      decline string that cannot fire is Week 33's dead-assertion failure.
      Consumer 2 (`Validator`'s `stmt.joins[slot-1]`) closed by rebuilding
      `validateQuery` around one range table. Consumer 3 (`indexOf(name, slot)`)
      closed by Task 4's normalization plus the duplicate-output-name refusal.
      **What IS newly live:** a derived relation has no `TableStats`, so
      `joinCardinality`'s non-multiplicative `max(l, r)` branch — and therefore
      Week 28's `method=written-floor` path, which had never executed — is
      reachable from the CLI for the first time.
- [x] Task 6 — Q17 landed. `lowerCorrelatedScalars`, a third sibling at the same
      site; reuses `splitCorrelation` verbatim; builds a `LogicalDerived` (the same
      node, not a special case); the join is **LEFT** for the zero-row NULL rule.
      A non-aggregate body is refused. A separate guard, not a widened
      `requireDecorrelatableBody`.
- [x] Task 7 — `COUNT(DISTINCT ...)` — landed. Both engines; keyed through
      `key_encoding.h`; `DISTINCT` reaches `exprToString`/`exprKey`/`cloneExpr`;
      `DISTINCT` on the other four aggregates and `COUNT(DISTINCT *)` refused at
      the parser. Harness: `WEEK34_DISTINCT_AGG_QUERIES` (all four modes) and
      `WEEK34_DISTINCT_AGG_REFUSED`.
- [x] Task 8 — both RE-DECLINED explicitly, dispositioned in
      `docs/week-33-plan.md` → *Handed to Week 34*. The dependent-join fallback
      stays a refusal, but the refused set **shrank** (Q17's shape now executes).
      Volcano parity again not attempted, and the cost is now **counted** rather
      than described: 56 queries diffed in two modes against 168 in four, stated
      in README Limitations.
- [x] Task 9 — harness suites for all three families, diffed and rejected, plus
      11 C++ tests pinning the decisions the oracle structurally cannot see (the
      slot-0 stamping, the `parent`-not-`&scope` LATERAL argument, LEFT-not-INNER
      in the scalar rewrite, `has_subquery` staying false). **No existing test was
      changed, narrowed or repointed** except by the mechanical `TableRef`
      translation in Task 2, whose rules are in that commit message.
- [x] Task 10 — closing sweep. Source: `ast.h`, `logical_plan.h` (the invariant
      is now one of two, both named), `join_enumeration.h` (prediction replaced by
      the finding), `vectorized_plan_builder.cc` (the build-order argument Week 34
      actually dissolved), `subquery_decorrelation.h`. Docs: `development.md`
      (how the containment was replaced, plus a Week 34 consumer table),
      `README.md` (In Scope, 8 dialect rows, 4 Limitations bullets, the Week 34
      section, the 37-week plan row), `docs/week-33-plan.md` (hand-off
      disposition).

### Still open, handed to Week 35/36

- **`buildScanSchema` narrowing** — still widens for a subquery query, and now
  also does not descend into a derived body. The derived case is a *different*
  problem: narrowing means rewriting the body's select list, which changes the
  schema the enclosing block was BOUND against. Stated at `buildRelation`.
- **`WEEK33_CORRELATED_EXPECT` is still a prefix.** Week 34 pins per-shape
  messages for its own refusals but did not retrofit Week 33's.
- **Volcano semi/anti parity**, now three families deep. Counted in Limitations.
- **Q22 is not claimed closed.** In the standard TPC-H text its correlated
  component is a `not exists` (Week 33's), while what it additionally needs is
  the derived table this week delivers. Verify against the ported query in
  Week 36 and record which half was which.
- **`method=written-floor` is reachable from the CLI for the first time**, via a
  derived relation's missing `TableStats`. Week 28 asked for a fixture; a query
  can do it now.

**If the week runs short**, cut in this order and say so out loud: Task 6 first
(cut to a refusal, never to a partial implementation — a wrong Q17 costs more than
a refused one, which is Week 33's own judgement), then Task 8's quantification.
**Do not cut Tasks 1, 5 or 10.** They are what stop the week's removals from
becoming next week's silent wrong answers, and they are the only tasks whose
absence is invisible in a green suite.

### Task 1 — the sweep checklist (built BEFORE any refusal was removed)

Produced by the five greps in Task 1, run against the tree at commit `fe17f40`
(Week 33's close), before a line of Week 34 code existed. Each entry is ticked in
Task 10 with **what is now true**, never with "checked".

**A. Forward promises coming due — `grep -rn "Week 34"`**

- [ ] `src/parser/ast.h:155` — "FROM is Week 34". The week arrived.
- [ ] `src/planner/join_enumeration.cc:77` — "from Week 34 on, a SCAN THAT IS NOT
      A RANGE-TABLE ENTRY … `slot >= countRelations()` stops meaning 'unbound
      key' and starts firing on legitimate plans." **Now live.** Task 5.
- [ ] `src/planner/join_enumeration.h:67,71` — same prediction, second spelling.
- [ ] `src/planner/subquery_materialization.cc:156` — "a SelectStatement DURING
      planning — Week 34's derived tables are the candidate".
- [ ] `src/planner/vectorized_plan_builder.cc:300` — "no STANDARD join is ever
      built above a semi join … Week 34's derived tables are that week."
      **Task 6 builds exactly that**, so this build-order fact dies.
- [ ] `python_tools/compare_against_sqlite.py:702` — per-shape correlated
      expectations "Week 34's to add when the shapes stop moving". Task 9.
- [ ] `development.md:378` — "`FROM (subquery)` is Week 34".
- [ ] `development.md:650` — "re-check every 'contained' row … Week 34's derived
      tables". Task 5.
- [ ] `development.md:778` — "**Week 34's derived tables break this containment
      for real**". Must become a record of *how*, plus what replaced it.
- [ ] `development.md:786` — `CardinalityEstimator` SEMI/ANTI row: "Week 34's
      derived tables are what would have made it a wrong estimate."
- [ ] `development.md:788` — `hasSlotOutsideRangeTable` row.

**B. The Week 32 containment, in every spelling**

- [ ] `src/planner/logical_plan.h:145-158` — the `LogicalJoin::semantics`
      invariant block: "the whole containment for the two-range-table problem
      this node introduces". Still true *for SEMI/ANTI*; no longer *whole*.
- [ ] `src/planner/subquery_lowering.cc` — the schema-equality assertion and the
      comment that calls it "what keeps the two domains from meeting".
- [ ] `src/planner/subquery_decorrelation.cc:243-281` — the same claim, restated
      at the `LogicalJoin` construction, plus the record of the deleted dead
      assertion. Task 6 adds a STANDARD join beside it.
- [ ] `development.md:753,781` — the "one plan, two range tables" section header
      and its lead paragraph.

**C. Refusals this week removes or narrows**

- [ ] `Validator` — no `FROM (subquery)` refusal exists today (the shape is a
      *parse* error), so the removal is at the grammar. Any comment saying "FROM
      is not supported" is in class A.
- [ ] `src/planner/subquery_decorrelation.h` conditions 1–4, and
      `requireDecorrelatableBody`'s condition 3 in the `.cc` — "a body with an
      aggregate cannot be decorrelated" stops being universally true in Task 6.
      **Highest-risk stale precondition of the week.**
- [ ] `src/planner/planner.cc` — the Volcano refusal block (IN, correlated).
      Task 4 adds a third; the "what this costs" paragraph must be extended, not
      duplicated.
- [ ] `README.md` dialect table rows citing `FROM (subquery)` as Week 34: the
      subquery-position row and the In-Scope subquery bullet.

**D. Binder-slot readers — the `ColumnId` worklist**

Not enumerated by hand: Task 5 lets the compiler produce it. The prose rows in
`development.md`'s two tables plus the *Week 32 consumers* table are the audit
trail to update, and every `localSlot("site")` string is the greppable index.

- [ ] Every `development.md` slot-table row carries a Week 34 verdict **with a
      reason** (level-aware / safe-by-domain / guarded).
- [ ] New section: *Week 34 consumers (a range-table entry that is a plan)*.

**E. Walkers that find "the relation" by descending to a `SCAN` — the live ones**

These are not stale prose; they are silent defects the moment a derived relation
exists, and every one of them feeds the cost model.

- [ ] `leafScanTable` (`vectorized_plan_builder.cc:46`) — returns the **body's
      base table** for a derived leaf; attributes its widths to the derived
      relation's columns. Week 27's `rowWidth` fix is the precedent.
- [ ] `isSingleRelation` (`vectorized_plan_builder.cc:57`) — returns `true` for a
      non-joining derived body, so `collectSlotTables` stamps the wrong table at
      a real slot.
- [ ] `leafScanTableOf` (`join_enumeration.cc:35`) — same wrong name, feeding
      `leafRowWidth` and therefore the DP's cost.
- [ ] `countRelations` (`join_enumeration.cc:42`) — counts scans **inside** the
      body, so `n` over-counts and `hasSlotOutsideRangeTable` becomes too
      permissive. Fix: `n` is the range-table size, passed from the builder.
- [ ] `predicate_pushdown.cc:227` (`scan_child->type != SCAN`) and `:373` — read
      both; confirm a derived child is handled or provably not reached.
- [ ] `cardinality_estimator.cc:310` and `vectorized_plan_builder.cc:251` — the
      two `case LogicalNodeType::SCAN:` dispatches. A new enum value makes these
      compile errors only if the switch has no `default:`.
