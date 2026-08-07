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
