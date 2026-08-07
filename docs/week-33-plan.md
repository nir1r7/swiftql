# Week 33 — Correlated Subqueries

**Checkpoint (README):** Required correlated TPC-H queries execute correctly.
Decorrelate the correlated patterns required by TPC-H; retain a correct fallback
for unsupported patterns.

**The shape of the week.** Week 33 removes `Validator::validate`'s
`has_correlated_subquery` refusal, which means a `ColumnRef` with
`query_level > 0` reaches a plan node for the first time in the project's life.
The README binds a deferred structural change to exactly that trigger, so this
week is two weeks stacked: a standalone mechanical refactor that must land and
verify on its own, and only then the correlation feature.

## Tasks

1. **`ColumnId { level, slot }` — the standalone prerequisite refactor.**
   Its own commit(s), suite green on both sides, no feature work in the diff.
2. **Remove the refusal; disarm the two Week 30 tripwires by replacing them.**
   `ChunkPruner::collectSimplePredicates` and `buildAggregateSchema`.
3. **Decorrelate `EXISTS` / `NOT EXISTS` into the Week 32 semi/anti join.**
   No new operator — only the rewrite that produces the join keys.
4. **Decorrelate correlated scalar subqueries (Q17-shape) into a group-by join.**
5. **The correct fallback for everything not decorrelated.**
   A refusal, message-pinned, plus the harness consequence.
6. **Volcano semi/anti parity — the route back to a four-mode oracle.**
7. **The three surfaces no audit reached.**
   `refuseUnloweredIn` call sites, Volcano `HashJoinNode` refusal totality,
   `setCostDecision`'s consumption of `rowWidth`.
8. **Precise `collectSlots` / `restampSlots` and `buildScanSchema` narrowing.**
9. **Tests, oracle suites, and the changed-test discipline.**

Sections follow in that order.

**Prerequisite knowledge for the week:** the difference between a *schema* slot
and a *binder-resolution* slot (`development.md` → *Relation slots and query
levels*, the paragraph beginning "Two different things are called
`relation_slot`"); what decorrelation is as a relational-algebra rewrite (a
correlated subquery is a dependent join; decorrelating it is turning that
dependent join into an ordinary one); and Week 32's `lowerInSubqueries`
(`src/planner/subquery_lowering.cc`), which is the shape Task 3 extends.

---

## Task 1 — `ColumnId { level, slot }`: the standalone prerequisite refactor

### Why it matters

Today `Validator::validate` ends with one check:

```cpp
if (stmt.has_correlated_subquery) {
    throw std::runtime_error(
        "correlated subqueries are not yet executable (Week 33)");
}
```

That single `if` is the **containment** the whole codebase rests on. A
`ColumnRef` with `query_level > 0` exists only inside a correlated subquery; that
check refuses every such statement; therefore every consumer below the validator
sees level-0 refs from exactly one range table. `development.md` →
*Relation slots and query levels* splits its consumer table into two halves on
precisely this line, and the entire second half — `buildAggregateSchema`,
`HashAggregateNode`, `VecHashAggregateNode`, `CardinalityEstimator`,
`AggregateSpec`'s consumers, `collectSimplePredicates` — is marked safe **only
because the refusal exists**.

Task 3 deletes that `if`. The moment it is gone, every row in that second half
becomes live *simultaneously*, and each one fails silently: `indexOf("team", 0)`
against the wrong block's schema is a clean **hit**, not a miss, so neither the
bare-name fallback nor the `idx < 0` throw fires. You get wrong groups, wrong
chunks pruned, wrong rows — with a green build.

The evidence that this is not hypothetical is in the README's Week 30 record:
**five separate bugs in one week**, all the same shape — a consumer collapsing
`(query_level, relation_slot)` into a bare slot:

| Bug | How it failed |
|---|---|
| `validateJoinCondition` | Indexed the *inner* `relations` with an outer slot → `column not found` on a legal query |
| `classifyJoinCondition` | Built a `JoinKey` out of a correlated ref → **a join the user never wrote** |
| `GroupByColumn`'s round trip | Stored an outer slot in an inner-scope struct; the skip keyed on `!table_name.empty()`, which depends on the *enclosing* block's relation count |
| `exprKey` | Encoded `slot#name` with no level, so a correlated group key satisfied an ungrouped *local* reference |
| The `SUM`/`AVG` argument check | Indexed `stmt.joins` — the inner list — with an outer slot |

Two more (`collectSimplePredicates` in `chunk_pruner.h`, and every
`GroupByColumn` consumer) were **missing from `development.md`'s table
altogether** until a third audit round went looking for completeness. That is the
decisive argument against continuing to patch: the containment is a *prose list*
that has already been proven incomplete twice, and the next reader treats a
missing row as considered-and-dismissed.

Making the level part of the **type** — so a bare `int` cannot be passed where a
qualified reference is required — converts that entire bug class from *silent
wrong answers* into *compile errors*. The compiler, not an audit round, produces
the list of consumers. That is what is being bought.

**Why it is a separate step, landing and verifying before any feature work.**
The README's rule, recorded since Week 30 and restated in Week 32's hand-off:
*its own standalone change, in whichever week first lowers a correlated
reference, never folded into a feature week.* Measured cost: **87 non-comment
mentions** of `relation_slot` / `from_slot` across six source layers (binder,
validator, logical plan, pushdown, both plan builders, chunk pruning), plus every
test that hand-builds a `ColumnRef`, `GroupByColumn` or `AggregateSpec`. A rename
that large inside a semantics change makes every subsequent diff unreadable —
and Week 32 already shipped a regression that hid behind a green 988-query oracle
and 770 green unit tests. A diff you cannot read is a diff in which the next one
hides.

### Conceptual explanation

**The type is small; the discipline is the point.** A `relation_slot` is a
*position in a range table*. Since Week 30 a query can have more than one range
table, and `query_level` says which — relative, like Postgres's `varlevelsup`:
0 = this block, 1 = the immediately enclosing block. The pair `(level, slot)` is
the identity. An `int` on its own is not.

**There are two different things called `relation_slot`, and only one of them
can be wrong.** This distinction decides the entire scope of the refactor, so
get it right before touching a line:

- `ColumnDef::relation_slot` (`src/common/schema.h`) is a **schema slot**. A
  `Schema` is built for exactly one query block, so there is no level to lose,
  and every reader of it is safe by construction. **Do not migrate it.**
  `Schema::indexOf(name, slot)` keeps its `int` parameter.
- `ColumnRef::relation_slot`, `GroupByColumn::relation_slot`,
  `AggregateSpec::relation_slot`, `JoinKey::from_slot` and the slot argument to
  `ColumnStats::find` / `findExact` / `findForRef` carry a slot that came from
  **binder resolution** and can therefore name an enclosing block's relation.
  These are the migration's subject.

Migrating the schema slot too would double the diff, touch execution kernels that
have no scope question at all, and blur exactly the distinction the change
exists to make legible.

**Where the level is legitimately dropped.** Some consumers genuinely want a bare
slot, and correctly so — `JoinKey::from_slot`'s own header says it "carries NO
query level", and `classifyJoinCondition` refuses to build a key from a
`level > 0` operand *precisely* to keep that true. Those sites do not disappear;
they become **named narrowing points**. Instead of an implicit `int`-to-`int`
assignment that reads as nothing, they call an accessor whose name states the
claim being made, and which fails loudly if the claim is false. Three or four
such call sites, each one greppable, replaces "87 mentions, trust the prose".

**What makes a new consumer safe tomorrow.** The permanent value is not the
one-time sweep — it is that a *future* week cannot write
`int slot = ref.relation_slot;` and have it compile. That is why `ColumnId` must
have **no implicit conversion to `int`**: no converting constructor, no
`operator int()`. If it converts, the refactor buys the one-time enumeration and
nothing after it.

### Code snippets

A new header, `src/common/column_id.h` — `common/` because `parser/ast.h`,
`planner/logical_plan.h` and `planner/join_condition.h` all already depend on
`common/`, and nothing in `common/` depends on them (Layer 1 is the foundation;
no module reaches past it).

```cpp
#pragma once
#include <stdexcept>
#include <string>

// Week 33. The identity of a bound column reference: a range-table POSITION
// plus WHICH range table it is a position in.
//
// Before this type, `relation_slot` was a bare int and `query_level` a separate
// field on the same struct, so any consumer could read one without the other.
// That collapse was found FIVE times in Week 30 alone (see the README's Week 30
// table) and twice more only when development.md's consumer table was itself
// audited for completeness. Every one of those failures was SILENT: an
// indexOf(name, slot) against the wrong block's schema is a clean HIT on the
// wrong relation, not a miss.
//
// !! NO implicit conversion to int, deliberately. The whole value of the type is
// that `int slot = ref.id.slot;` must be something a reader can see and grep
// for, and that a future consumer cannot write `f(ref.id)` where `f(int)` was
// meant. Adding operator int() to shorten a diff gives the one-time sweep back
// and forfeits everything after it.
struct ColumnId {
    // How many query blocks OUT the relation lives, relative to the block the
    // reference is WRITTEN in. 0 = this block's own range table. RELATIVE, like
    // Postgres's varlevelsup, so a subquery's tree means the same thing
    // wherever it sits.
    int level = 0;
    // Position in the range table of the scope `level` steps out:
    // 0 = FROM, then one per JOIN in written order (joins[i] -> i+1).
    // -1 = unresolved (the pre-binder state, and the value hand-built test
    // trees rely on to fall back to bare-name resolution).
    int slot = -1;

    // Named constructors. `local(s)` is the overwhelmingly common case and the
    // one every pre-Week-30 call site meant; spelling it out is what makes the
    // rare `outer` sites visible in a grep.
    static ColumnId local(int s)            { return ColumnId{0, s}; }
    static ColumnId outer(int lvl, int s)   { return ColumnId{lvl, s}; }
    static ColumnId unresolved()            { return ColumnId{0, -1}; }

    bool isLocal()    const { return level == 0; }
    bool isResolved() const { return slot >= 0; }

    // THE NAMED NARROWING POINT. Call this — never `.slot` — anywhere the value
    // is about to be used as a position in THIS block's range table or in a
    // schema built for this block. The name is the claim; the throw is what
    // stops the claim being wrong silently. `site` names the caller so a
    // planner defect points at itself rather than at this header.
    int localSlot(const char* site) const {
        if (level != 0)
            throw std::runtime_error(
                std::string("internal: ") + site + " read a correlated column "
                "reference as a local relation slot (query level "
                + std::to_string(level) + ")");
        return slot;
    }

    // Decorrelation moves a reference from an inner block to the block that
    // supplies it, which is exactly a level decrement. Task 3 uses this; it
    // lives here so the arithmetic has one home rather than being open-coded at
    // each rewrite site.
    ColumnId outward() const {
        if (level == 0)
            throw std::runtime_error(
                "internal: cannot move a local column reference outward");
        return ColumnId{level - 1, slot};
    }

    bool operator==(const ColumnId& o) const {
        return level == o.level && slot == o.slot;
    }
    bool operator!=(const ColumnId& o) const { return !(*this == o); }
};
```

`ColumnRef` after the change — note the field **replaces both** old fields and
keeps their position, so the struct's layout story and the "last field" discipline
elsewhere are undisturbed:

```cpp
// reference to a column
struct ColumnRef : Expr {
    std::string table_name;
    std::string column_name;
    // Was `int relation_slot` + `int query_level` (Weeks 16 and 30). One field,
    // because they were never independently meaningful: reading a slot without
    // its level compares two numbering domains.
    ColumnId id = ColumnId::unresolved();
};
```

A consumer that is already level-aware becomes shorter and states the same rule
— `collectSimplePredicates` (`src/storage/chunk_pruner.h`) is the model:

```cpp
// before:
//   if (col && lit && !lit->value.isNull()
//       && col->query_level == 0 && col->relation_slot < 1) { ... }
// after: one condition, and the level test cannot be forgotten because
// `.slot` is not reachable without deciding what to do about `.level`.
if (col && lit && !lit->value.isNull()
    && col->id.isLocal() && col->id.slot < 1) {
    out.emplace_back(col->column_name, bin->op, lit->value);
}
```

A narrowing point — `lowerInSubqueries` building a `JoinKey`, whose `from_slot`
is documented as carrying no level:

```cpp
// JoinKey::from_slot carries NO query level (join_condition.h). The operand
// belongs to the ENCLOSING query and is bound at level 0 there, so the claim is
// true — and localSlot() is where it is now CHECKED rather than commented.
JoinKey key{operand->column_name,
            body_plan->output_schema.columns()[0].name,
            operand->id.localSlot("lowerInSubqueries")};
```

And a test that hand-builds a tree:

```cpp
// before: GroupByColumn{"l", "team", 1}
// after — the level is now impossible to omit by accident, and impossible to
// supply positionally by mistake:
GroupByColumn g{"l", "team", ColumnId::local(1)};
```

### Implementation guidance

Do this with the refusal **still armed**. Task 1 must not change one answer the
engine gives. That is what makes the verification meaningful: the suite is green
about the *same claim* on both sides of the commit.

1. **Take the baseline.** Build, run the 770 unit tests, run the full oracle
   suite, and record the numbers in the commit message. Also record
   `grep -rn "relation_slot\|from_slot" --include=*.h --include=*.cc src/ | wc -l`
   — 137 raw, 87 non-comment — so the after-count is comparable.
2. **Add `src/common/column_id.h` alone and build.** It compiles against nothing
   and breaks nothing. Land it or keep it as the first hunk; either way the tree
   builds.
3. **Migrate `ColumnRef` first** — it has the largest fan-out, so every other
   type's sites are already visited by the time you reach them. Delete both
   fields, add `ColumnId id`, and then let the compiler produce the worklist.
   **Do not add a compatibility shim** (`int relation_slot() const`). A shim
   makes the diff small and leaves the collapse expressible, which is the one
   thing the change exists to prevent.
4. **Classify each compiler error against `development.md`'s consumer table
   before fixing it.** The table already says what each consumer is: *level-aware*
   (it tests the level → the fix is mechanical, `col->query_level == 0` becomes
   `col->id.isLocal()`), *level-agnostic but safe by contract* (the fix is
   `localSlot("<site>")`, and the contract moves from a comment into a check), or
   *contained by the refusal* (the second half of the table — these are Task 2's
   and Task 3's subject; here they get `localSlot()` too, which keeps today's
   behaviour and turns tomorrow's violation loud). Any error at a site the table
   does **not** list is a finding: add the row.
5. **Build after each type, run the full suite after each type.** `ColumnRef` →
   green; `GroupByColumn` → green; `AggregateSpec` → green. Three verified
   points, not one.
6. **Then the tests.** Hand-built trees are the bulk of the remaining churn
   (`test_binder.cc` 20 mentions, `test_join_enumeration.cc` 14,
   `test_logical_plan.cc` 13, `test_vec_plan_builder.cc` 12 and six more files).
   The mechanical rule: a positional `{"l","team",1}` becomes
   `{"l","team",ColumnId::local(1)}`; a `.relation_slot = 2` assignment becomes
   `.id = ColumnId::local(2)`. **Change the spelling and nothing else.** If a
   test's assertion has to change to keep it passing, stop — that is a behaviour
   change hiding in a rename, and it is exactly the Week 32 failure mode.

Mistakes this codebase specifically invites:

- **Migrating `ColumnDef::relation_slot`.** It is in `common/schema.h`, it is
  named identically, and it is *not* the same thing. If you find yourself
  editing `src/execution/vec_hash_aggregate_node.cc`'s schema lookups or
  `Schema::indexOf`'s signature, you have crossed the line. Migrating it also
  drags in `src/common/schema.cc`'s three mentions and every merged-join-schema
  construction for no benefit.
- **Positional brace-initialisation.** `GroupByColumn` and `AggregateSpec` both
  document a "last field, so positional brace-inits stay valid" discipline
  (`GroupByColumn::expr`, `AggregateSpec::argument`, `JoinClause::type`).
  Replacing a mid-struct `int` with a `ColumnId` keeps the *arity* but changes
  the type at that position, so `{"COUNT", "", true}` still compiles and
  `{"AVG", "speed", true, 1}` does not. That asymmetry is deliberate — the
  second form is the one that needs a decision.
- **`exprKey` (`src/parser/expr_utils.h`, dispatch site 1).** It became
  level-aware in Week 30 round 2 and encodes the pair into a string. Keep the
  encoding byte-identical for level 0, or every `GROUP BY` key match and every
  `substituteInto` rewrite changes behaviour at once — a rename with a semantic
  edit inside it. Assert this with a test on the produced string, not by reading.
- **`ColumnStats::find` / `findExact` / `findForRef`
  (`src/planner/cardinality_estimator.h`).** These take a slot from binder
  resolution and are *not* in the consumer table's first half by name. Narrow at
  the call site with `localSlot("CardinalityEstimator::…")`, and add the rows.
- **Forgetting that `-1` means two things.** `slot == -1` is "unresolved /
  resolve by bare name", and several consumers depend on it (`AggregateSpec`'s
  comment says so explicitly). `ColumnId::unresolved()` must be `{0, -1}`, not
  `{-1, -1}`, or every hand-built test tree changes meaning.

### Verification

Success criteria, all four required before Task 2 begins:

1. `cmake --build build -j$(nproc)` clean, no warnings introduced.
2. `cd build && ./tests/swiftql_tests` — the same test count and 0 failures as
   the baseline. **Same count**: a refactor that changes the number of tests has
   either dropped one or added a behaviour.
3. `python3 python_tools/compare_against_sqlite.py` — the full oracle suite green
   with the same query count, in all four modes. A refactor that changes an
   answer is a bug, not a refactor.
4. `grep -rn "relation_slot" --include=*.h --include=*.cc src/` returns **only**
   `ColumnDef::relation_slot` and its readers. `grep -rn "query_level" src/`
   returns nothing outside comments. Any other hit is an unmigrated site.

Two additions that make the guarantee permanent rather than momentary:

- A unit test pinning the narrowing: build a `ColumnId::outer(1, 0)`, call
  `localSlot("test")`, `EXPECT_THROW`. Without it, `localSlot` is a comment with
  extra steps.
- A unit test pinning `exprKey`'s encoding for a level-0 ref against the exact
  string it produced before the change.

Then update `development.md` → *Relation slots and query levels*: the two-half
split by "reachable / behind the refusal" is about to stop being the containment
(Task 3 deletes the refusal), and the table's new job is to record, per consumer,
**which of `isLocal()` / `localSlot()` / real level-aware behaviour** it now
uses. Do that edit in this commit, not later — the table's whole value is that it
is never behind the code.

Commit alone, with the before/after counts in the message. `git log --stat`
should show a commit that touches 20-odd files and changes no test assertion.

---

## Task 2 — Remove the refusal; replace both Week 30 tripwires

### Why it matters

The refusal is one `if` at the end of `Validator::validate`, and its placement is
load-bearing in a way that is easy to destroy while deleting it. Week 30 put it
there so that **every parse, bind and validate error a query is entitled to fires
first** — a bad nested table, a bad nested column, an ungrouped reference, a
wrong arity, a disallowed position. And it is **one** check, so the four modes
agree *by construction* rather than by copies of a guard that can drift, which is
what Week 29 spent an audit round undoing.

Deleting it arms two tripwires that have been in the tree, unreached, since
Week 30:

- `collectSimplePredicates` (`src/storage/chunk_pruner.h`) **declines** any ref
  with `query_level > 0`.
- `buildAggregateSchema` (`src/planner/logical_plan.cc`) **throws** on a
  `GroupByColumn` with `query_level > 0`.

They differ **on purpose**, and the difference is the design principle to
preserve. Pruning is an *optimization*: contributing nothing is
correct-and-slower, so declining is the right answer forever. Grouping is
*semantics*, and its failure mode is `indexOf("team", 0)` scoring a clean hit on
the **wrong relation** — no local fallback can repair that, so it must be loud.

The README's instruction is explicit: **replace them with real behaviour, do not
delete them.**

### Conceptual explanation

After Task 3's decorrelation, a correlated reference should not survive into a
scan predicate or a group key at all — the rewrite converts it into a join key
against an ordinary column of the *outer* relation. So the honest "real
behaviour" for both sites is:

- **`ChunkPruner`:** keep declining. A `level > 0` ref reaching here means either
  a shape decorrelation did not handle (Task 5's fallback path) or a defect;
  either way, contributing no pruning hint is safe. What changes is the
  *comment*: it currently says "unreachable today", and after Task 3 it is
  reachable for the fallback shapes. Restate the reason as the one that is
  actually load-bearing — *a correlated ref names a relation this scan is not
  scanning, so it can prune nothing here* — rather than "the validator refuses
  it".
- **`buildAggregateSchema`:** the throw stays, and it stays a *planner-defect*
  message, because after decorrelation a group key that is still correlated means
  the rewrite left one behind. This is the single guard for the whole
  `GroupByColumn` consumer set (`HashAggregateNode`, `VecHashAggregateNode`,
  `CardinalityEstimator`), all three of which run on a plan whose schema was
  built here — if it throws, they never see the key.

The subtlety Task 1 buys you: after the migration, both of these are expressed as
`id.isLocal()` / `id.localSlot(...)`, so the tripwires are no longer *ad hoc
`if`s a reader can mistake for defensive noise — they are the two places the
type's narrowing is deliberately not taken.

### Code snippets

```cpp
// src/planner/validator.cc — what replaces the refusal.
//
// NOT a second refusal somewhere else, and NOT one per engine. Week 30's
// placement is the asset: one check, at the end of validate(), which both
// Planner::plan and LogicalPlanBuilder::build reach first. Task 5's fallback
// refusal goes in ONE place too, for the same reason (see Task 5).
//
// The check is simply gone. What must NOT happen here:
//   - moving it into LogicalPlanBuilder "because that is where lowering is";
//     Planner::plan then accepts a query the vectorized path refuses, which is
//     the four-mode drift Week 29's audit rounds were about;
//   - keeping a narrowed version ("correlated scalar subqueries are not yet
//     executable") — that belongs at the decorrelation site, which knows WHICH
//     shape it declined and can say so.
```

```cpp
// src/storage/chunk_pruner.h — the tripwire, restated rather than deleted.
//
// Week 33: this is now REACHABLE. Decorrelation (subquery_decorrelation.cc)
// rewrites the correlated shapes TPC-H needs into join keys, so a level > 0 ref
// should not arrive here — but the Task 5 fallback shapes are refused rather
// than rewritten, and a defect in the rewrite would arrive here too.
//
// Still a DECLINE, and now for the reason that survives the refusal's removal:
// a correlated ref names a relation this scan is not scanning, so it can supply
// no pruning hint about this table's zone maps. Contributing nothing is
// correct-and-slower; matching by NAME against the scanned table's zone maps
// (which is what the code below does) would prune the WRONG relation's chunks
// silently, on the --no-optimize path where the collectSlots/soleSlot
// containment never applies.
if (col && lit && !lit->value.isNull()
    && col->id.isLocal() && col->id.slot < 1) {
    out.emplace_back(col->column_name, bin->op, lit->value);
}
```

### Implementation guidance

1. Do this **after** Task 1 is committed and green, and **before** Task 3's
   rewrite exists — with the refusal gone and no decorrelation yet, a correlated
   query will fail *somewhere*, and where it fails is diagnostic information you
   want to see once, deliberately. Run each query in
   `WEEK33_CORRELATED_BINDS` (`python_tools/compare_against_sqlite.py`, seven
   shapes) and write down the message each produces. That list is Task 3's and
   Task 5's worklist.
2. Expect `inferExprType` and `evaluate` (dispatch sites 12 and 13) to throw
   their Week-31 messages for a surviving `SubqueryExpr`. Week 30's note is
   explicit that both sites must close **in the same commit that lowers one** —
   so they close in Task 3/4, not here. Leaving them throwing between Task 2 and
   Task 3 is fine and is why Tasks 2-4 may share one commit if the intermediate
   state is not independently verifiable; Task 1 is the one that must stand alone.
3. Update `development.md` → *Relation slots and query levels*: the
   "reachable / behind the refusal" split is now obsolete. Rewrite the heading of
   the second half as what it actually becomes — *reachable, guarded by
   `ColumnId`'s narrowing* — and re-check every row in it. This is the audit the
   README warns is worth more than the green gate.

### Verification

- Every query in `WEEK33_CORRELATED_BINDS` produces a message that names the
  shape it declined, not `correlated subqueries are not yet executable`.
- `grep -rn "not yet executable (Week 33)" src/ python_tools/` returns nothing.
- A unit test for each tripwire, asserting the *new* behaviour directly rather
  than via a query: a `ColumnRef` at `ColumnId::outer(1,0)` contributes no
  simple predicate; a `GroupByColumn` at `ColumnId::outer(1,0)` makes
  `buildAggregateSchema` throw. Both are cheap and both outlive the week.

---

## Task 3 — Decorrelate `EXISTS` / `NOT EXISTS` into the Week 32 semi/anti join

### Why it matters

This is the week's actual deliverable and the TPC-H unlock: Q4 and Q21 are
correlated `EXISTS` / `NOT EXISTS`. **The operator already exists.** Week 32
built `JoinSemantics::{SEMI, ANTI}`, `VecHashJoinNode`'s set-probe, and
`lowerInSubqueries` — the whole lowering site, refusal set and schema invariant.
Week 33 should need **no new operator**: only the rewrite that produces the join
keys.

That is the single most important scoping fact of the week. If you find yourself
writing an operator, stop and re-read `src/planner/subquery_lowering.cc` — the
work is a sibling of `lowerInSubqueries`, roughly its length, at the same call
site in `LogicalPlanBuilder::build`.

### Conceptual explanation

A correlated `EXISTS` is a **dependent join**: for each outer row, run the body
with that row's values substituted. Decorrelation turns it into an ordinary join
by *promoting the correlation predicate into the join condition*.

```
WHERE EXISTS (SELECT * FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 340)
```

The body's `WHERE` splits into two classes of conjunct:

- **local** — every ref is `level 0` against the body's own range table
  (`l.speed > 340`). These stay inside the body's plan, as an ordinary
  `LogicalFilter` under the join's right child.
- **correlated equality** — an `=` whose one side is `level 0` in the body and
  whose other side is `level 1` (`l.driver_id = d.driver_id`). These become
  `JoinKey`s: `join_col` from the body side, `from_col`/`from_slot` from the
  outer side, whose `ColumnId` is level 1 in the body and therefore **level 0 in
  the outer block** — one `outward()` call, which is exactly why that method
  exists on `ColumnId`.

The result is `LogicalJoin{semantics = SEMI}` (or `ANTI` for `NOT EXISTS`) with
the outer spine as `children[0]` and the body's plan as `children[1]` — the
identical node shape `lowerInSubqueries` already builds. The correlation is gone:
the body plan contains no `level > 0` ref, because every one of them left as a
join key.

**Three invariants the rewrite must preserve, all inherited from Week 32:**

1. **`output_schema` IS the left child's**, not a merged schema. Week 32 asserts
   this in code rather than leaving it as an observation, and the assertion is
   what keeps the body's slot numbering out of the outer plan. One plan already
   holds two range tables; the containment is that nothing from the body is ever
   in scope above the join. Week 34's derived tables break this for real —
   Week 33 must not.
2. **`join_slot = -1`.** `children[1]` is not a relation of this block's range
   table, so there is no slot to name it by, and every reader of `join_slot`
   declines on `semantics != STANDARD`.
3. **A whole-conjunct construct.** `lowerInSubqueries` extracts only top-level
   `WHERE` conjuncts, and for the same reason: `EXISTS(...) OR x > 5` has no
   disjunctive semi-join to lower to. Correlated `EXISTS` inherits that
   restriction verbatim — and inherits the refusal that goes with it (Task 5).

**What `JoinKey` can and cannot express.** It holds column **names** and an
`int from_slot`. So a correlated conjunct qualifies as a key only when both sides
are plain `ColumnRef`s under `=`. `l.speed > d.age` is a correlated *inequality*
— a real dependent join with no equi-join lowering in this engine, and there is
no cross-product operator to run it on. That is Task 5's territory, and it is the
same boundary the dialect already documents for `JOIN ... ON`.

### Code snippets

A new `src/planner/subquery_decorrelation.{h,cc}`, sibling to
`subquery_lowering.{h,cc}` — a separate file because the routing question ("is
this shape decorrelatable?") is different from Week 32's ("is this `IN` in a
lowerable position?"), and because Week 32's header documents *its* preconditions
as "no correlated subquery anywhere", which stops being true this week and must
be restated rather than silently invalidated.

```cpp
// Splits a correlated body's WHERE into (join keys, residual local conjuncts).
// Returns false when the body is not decorrelatable by this rewrite, leaving
// `keys` and `local` untouched — the caller then refuses (Task 5) rather than
// falling back to a second production, which is the two-paths problem Weeks
// 26/28/30 each had to undo.
bool splitCorrelation(const Expr* body_where,
                      std::vector<JoinKey>& keys,
                      std::vector<std::unique_ptr<Expr>>& local);
```

```cpp
// The classification, per top-level conjunct of the body's WHERE.
// splitConjuncts() (predicate_pushdown) already does the AND flattening — reuse
// it; a private walker here would be a nineteenth silent dispatch site, which is
// exactly what Week 30 refused to add for the ORDER BY position rule.
for (auto& c : body_conjuncts) {
    auto* bin = dynamic_cast<BinaryExpr*>(c.get());
    if (!bin || bin->op != "=") { local.push_back(std::move(c)); continue; }

    auto* l = dynamic_cast<ColumnRef*>(bin->left.get());
    auto* r = dynamic_cast<ColumnRef*>(bin->right.get());
    if (!l || !r) { /* not both plain refs -> stays local, or refuse if it
                       mentions an outer ref at all (see below) */ }

    // Exactly one side must be level 1 (the outer block) and the other level 0
    // (this body). Anything else is not this rewrite's shape:
    //   - both level 0  -> an ordinary local predicate, keep it local;
    //   - both level 1  -> a predicate on the OUTER row alone; correct but not a
    //                      join key. It belongs in the OUTER WHERE, and hoisting
    //                      it is a separate decision — refuse for now (Task 5);
    //   - level >= 2    -> correlated past the immediately enclosing block
    //                      (Q20's shape). Refuse; see the note in Task 5.
    const bool l_outer = !l->id.isLocal(), r_outer = !r->id.isLocal();
    if (l_outer == r_outer) { /* both or neither: not a key */ }

    const ColumnRef* body  = l_outer ? r : l;
    const ColumnRef* outer = l_outer ? l : r;
    if (outer->id.level != 1) { /* refuse: level >= 2 */ }

    // from_slot is a slot in the OUTER range table. `outer->id` is level 1 in
    // THIS block; one step outward makes it level 0 there, and localSlot() then
    // narrows it at a named point instead of by an unremarked assignment.
    keys.push_back(JoinKey{outer->column_name,
                           body->column_name,
                           outer->id.outward().localSlot("splitCorrelation")});
}
```

Grafting it, alongside Week 32's call in `LogicalPlanBuilder::build`
(`src/planner/logical_plan.cc`, near line 778):

```cpp
// Same site, same conjunct vector, same ordering discipline as Week 32:
// AFTER the FROM/JOIN spine exists and BEFORE the WHERE LogicalFilter is built.
InLoweringResult in_lowered  = lowerInSubqueries(std::move(node), conjuncts, catalog);
ExLoweringResult ex_lowered  = lowerExistsSubqueries(std::move(in_lowered.plan),
                                                     conjuncts, catalog);
// ...then refuseUnloweredIn / refuseUnloweredExists on what is left.
```

### Implementation guidance

1. **Read `src/planner/subquery_lowering.cc` end to end first.** It is 110 lines
   and it answers, in code, every structural question this task has: where the
   body is planned (`LogicalPlanBuilder::build`, recursively), how ownership of
   the shared `SelectStatement` is handled (moved out, with a `use_count() > 1`
   refusal because `cloneExpr` shares), where the schema invariant is asserted,
   and what `join_slot = -1` means.
2. **Reuse `planBody`'s ownership shape verbatim,** including the
   `use_count() > 1` refusal. A shared body statement is one statement with two
   parents; only one of them can be lowered from it, and the second would plan an
   emptied statement. This is not a hypothetical — `(SELECT ...) BETWEEN a AND b`
   produces the shape through `cloneExpr`.
3. **Plan the body *after* stripping the correlated conjuncts, not before.** If
   the body's `WHERE` still contains `l.driver_id = d.driver_id` when
   `LogicalPlanBuilder::build` runs on it, that ref reaches `validateExpr`,
   `collectSlots`, `buildScanSchema` and the pruning hint inside a block where it
   is meaningless. Strip first, plan second — this is the single most likely
   source of a confusing failure in this task.
4. **An `EXISTS` body's select list is irrelevant** and must stay so. Q4 and Q21
   both write `SELECT *`, and the README records that `EXISTS` deliberately has
   no arity rule. The semi-join probes keys only; nothing from the body is
   projected. Do not "helpfully" narrow the body's projection to the key column —
   `buildScanSchema` already handles narrowing, and a special case here would be
   a second production.
5. **An *uncorrelated* `EXISTS` stays materialized.** Week 32's header says so
   explicitly: its value does not depend on the outer row, so a semi-join would
   turn a foldable constant into a pipeline breaker. Route on the node's
   `correlated` flag, which the Binder derives from the stamps (and which
   survives repeated walks since Week 30 round 1).
6. **`ANTI` and NULLs.** `NOT EXISTS` is an anti-join and — unlike `NOT IN` —
   has **no** NULL subtlety: `EXISTS` is a pure existence test, never UNKNOWN.
   Do not copy `NOT IN`'s NULL rule into it. Week 32's `build_had_unmatchable_key_`
   machinery in `vec_hash_join_node.cc` exists for `NOT IN`; confirm by reading
   whether the anti path consults it unconditionally, because if it does, a
   correlated `NOT EXISTS` over a body with a NULL key would wrongly return zero
   rows. **This is a specific thing to test, not to assume.**
7. **Cardinality.** Week 29's starting note, pointed at from Week 32's, is
   binding: a semi/anti join needs its **own** rule, never a reuse of the outer
   join's, and a non-multiplicative rule lives at the stamping site and never
   inside `joinCardinality` or the join search's optimal substructure goes with
   it. If Week 32 already added one, decorrelated joins get it for free — verify
   that, do not re-derive it.

### Verification

- The three `EXISTS`/`NOT EXISTS` shapes in `WEEK33_CORRELATED_BINDS` move out of
  the rejection suite and into a **diffed** suite against SQLite. That move is
  the checkpoint.
- `--explain` on each: the plan shows `SEMI`/`ANTI` with the correlated column
  as a join key, and the join's `output_schema` is the left child's. Week 32's
  in-code assertion already enforces the second half; make sure your new node
  goes through it (share the helper rather than re-asserting inline).
- `--no-optimize` and optimized produce identical rows (`optimizer-diff` skill).
- A correlated `NOT EXISTS` whose body's key column contains a NULL returns the
  same rows as SQLite. This is item 6 above, and it is the one place a
  copy-paste from `NOT IN` produces a wrong answer that looks plausible.

---

## Task 4 — Correlated scalar subqueries (the Q17 shape)

### Why it matters

Q17 is `WHERE l.speed > 0.2 * (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team =
l.team)` — the first shape in `WEEK33_CORRELATED_BINDS`, and the one form where
"no new operator" stops being free. An `EXISTS` needs only a membership answer;
a scalar subquery needs a **value per outer row**.

### Conceptual explanation

The standard decorrelation: group the body by its correlation key and join the
result back.

```
outer  ⋈(team)  ( SELECT team, AVG(speed) AS agg FROM laps l2 GROUP BY team )
```

That is an ordinary `LogicalAggregate` under an ordinary `LogicalJoin`
(`semantics = STANDARD`), and the `SubqueryExpr` in the outer `WHERE` is replaced
by a `ColumnRef` naming the join's aggregate output column.

Three things make this materially harder than Task 3, and each is a legitimate
reason to defer it to Task 5's fallback rather than ship it half-built:

1. **The join is not semi/anti, so `output_schema` is a MERGED schema** — which
   breaks the one containment Week 32 established and Task 3 preserves ("nothing
   from the body is in scope above the join"). The aggregate's output column
   genuinely *is* in scope above this join, because the outer predicate reads it.
   That is the same property the README says Week 34's derived tables break "for
   real". Landing it here means landing a piece of Week 34's problem early.
2. **The NULL/zero-row semantics differ from an inner join.** SQL says a scalar
   subquery over zero rows is NULL — Week 31 shipped the typed-null `Literal` for
   exactly this. An inner join *drops* the outer row instead. Correct
   decorrelation therefore needs a **LEFT** join, so unmatched outer rows survive
   null-extended — and Week 29's rules then apply: pushdown will not cross to the
   null-supplying side and join enumeration declines to reorder any tree
   containing one.
3. **The "more than one row" rule.** Week 31's deliberate divergence
   (`scalar subquery returned more than one row`) is a *runtime cardinality*
   check. After decorrelation there is no per-outer-row result to count — the
   `GROUP BY` makes exactly one row per key by construction. For an aggregate
   body that is fine and is the point. For a **non-aggregate** body
   (`(SELECT name FROM drivers d2 WHERE d2.team = l.team)`) the check disappears
   silently, and a query that should error returns an arbitrary row.

### Implementation guidance

**Recommended scope, and the tradeoff stated rather than hidden:** implement
Task 4 **only** for a body whose select list is a single aggregate with a
`GROUP BY`-able correlation equality — the Q17 shape — and refuse every other
scalar shape via Task 5. Rationale, in the project's own terms (minimum code that
solves the problem): Q17 is what the phase goal needs, the aggregate body is the
one shape where point 3 above is sound by construction, and the merged-schema
question in point 1 is Week 34's to answer properly.

If the week runs short, **Task 4 is the one to cut** — cut it to Task 5's
refusal, not to a partial implementation. Q4 and Q21 (Task 3) are two of the 22;
a wrong Q17 costs more than a refused one.

Steps, if implemented: split the body's `WHERE` with the same `splitCorrelation`
helper from Task 3; require every correlated conjunct to be an equality (the
group key); build `LogicalAggregate{group_by = correlated keys, aggregates =
the body's single spec}`; wrap in `LogicalJoin{STANDARD, LEFT-preserving}`;
replace the `SubqueryExpr` node in the outer expression with a `ColumnRef` at
`ColumnId::local(<the join's slot>)` naming the aggregate output column. Sites 12
and 13 (`inferExprType`, `evaluate`) then close naturally, because no
`SubqueryExpr` survives into the plan — which is what Week 30's note requires
("both sites must close in the *same* commit that lowers one").

### Verification

- Q17's shape returns rows identical to SQLite, in every mode the query supports.
- A body with a NULL/zero-row group returns NULL for that outer row, not a
  dropped row. Construct the case deliberately; the shipped catalog may not
  contain one.
- `--explain` shows one `LogicalAggregate` under the join, not one per outer row.

---

## Task 5 — The correct fallback for unsupported patterns

### Why it matters

The README's second bullet for the week is *"retain a correct fallback for
unsupported patterns"*, and in this engine **the correct fallback is a refusal**,
not a second execution production. That is not a shortcut — it is the rule Weeks
26, 28 and 30 each had to re-establish after a drift: two productions that must
agree on the same semantics is exactly the failure mode. Week 32's
`subquery_lowering.h` states it in as many words for `IN`.

### Conceptual explanation

Shapes that must be refused after Tasks 3 and 4, each with its own message naming
what it declined:

| Shape | Why |
|---|---|
| Correlated `EXISTS` not a whole top-level `WHERE` conjunct (under an `OR`, or in `HAVING`) | A semi-join is a whole-conjunct construct; inherited verbatim from Week 32's `IN` rule |
| A correlated conjunct that is not an equality (`l.speed > d.age`) | `JoinKey` holds column names under `=`; there is no cross-product operator |
| A correlated conjunct whose sides are not both plain `ColumnRef`s | `JoinKey` holds names, not expressions — the same rule as the `IN` operand |
| `level >= 2` (correlated past the immediately enclosing block) | The rewrite promotes one level outward; skipping a block needs the intermediate join to exist first. Q20's shape is `IN`-of-scalar and is reached differently — check which of the two it actually needs before assuming it is refused |
| A correlated **scalar** body that is not the Q17 shape | Task 4's stated scope |

**One refusal site per shape, and never one per engine.** Week 30's placement
rule stands: `Planner::plan` and `LogicalPlanBuilder::build` both run
`Validator::validate` first, so anything that is a *dialect* rule belongs there
and the four modes agree by construction. Anything that is a genuine **capability
difference** between engines (Task 6's Volcano gap) belongs at that engine's
entry point — that is Week 26's rule, and it is why the Volcano `IN` refusal
lives in `Planner::plan`.

**The harness blind spot, and what it forces.** `compare_against_sqlite.py`
diffs *rows*. A query that errors produces none, so **the diffed oracle suite
cannot hold a query that is refused** — it can only be pinned, by message, in a
rejection suite. This is why `WEEK31_MATERIALIZATION_REFUSED`,
`WEEK32_LOWERING_REFUSED` and `WEEK33_CORRELATED_BINDS` exist. Every message you
add in this task needs a rejection-suite entry in the same commit, or it is
untested: a refusal with no test is a string that can change silently, and the
next week reads the green suite as coverage.

### Implementation guidance

- Follow `refuseUnloweredIn`'s shape exactly (`subquery_lowering.cc`): a walker
  over what is left after lowering, called on the residual `WHERE` and on
  `HAVING`, throwing a message that names the *clause*. Its purpose is to keep a
  missed position **loud** — without it the node reaches `inferExprType`
  (dispatch site 12) and dies with an internal-defect message pointing at the
  wrong pass. Your `refuseUnloweredExists` / `refuseUnloweredCorrelated` needs
  the same tripwire for the same reason.
- Add a README row to *Syntax Deliberately Not Supported* for every refusal that
  SQLite would answer — that table's discipline is that each entry states the
  message and the reason, and divergences are named as divergences.
- `WEEK33_CORRELATED_BINDS` does not disappear; it **shrinks**. Shapes Task 3/4
  execute move into a diffed suite; shapes refused here stay, with their
  `_EXPECT` message updated from `correlated subqueries are not yet executable
  (Week 33)` to the specific new one. A rejection suite whose expectation string
  still names Week 33 after this week is a suite that is passing for the wrong
  reason.

### Verification

- Every shape in the table above has a rejection-suite entry pinning its exact
  message, in every mode that reaches it.
- No query anywhere still produces `correlated subqueries are not yet executable`.
- Each refusal message names the shape, not the week.

---

## Task 6 — Volcano semi/anti parity: the route back to a four-mode oracle

### Why it matters

Week 32 shipped option (b): `IN (subquery)` is **refused outright on the Volcano
path** (`WEEK32_SEMI_JOIN_VOLCANO_REJECTED`), so every set-membership query is
diffed against SQLite in **two** modes rather than four. The columnar/row ×
volcano coverage that every other feature in this engine carries does not exist
for set membership.

Task 3 decorrelates *into those same operators*, so **Week 33 inherits the halved
coverage for correlated `EXISTS` too** — Q4 and Q21 would land with half the
oracle confidence every other feature has. The README calls closing this "the
cheapest available increase in confidence for the whole area", and it is the
only route back to the four-mode baseline.

### Conceptual explanation

Volcano is the **correctness baseline**, not the feature-complete path — that is
the project's stated stance, and the reason the four-mode diff is worth what it
is. A capability that exists only on the vectorized path has no independent
check: the vectorized answer is compared against SQLite, but nothing compares two
independent implementations of *this engine's* semantics.

The work is `JoinSemantics` in `src/execution/hash_join_node.cc` plus the same
NULL/unmatchable rule the vectorized side has. Structurally a Volcano semi-join
is the simplest operator in the file: build the hash set from the right input,
then in `next()` return each left row whose key is (semi) / is not (anti)
present, emitting the **left row unchanged** — the output schema is the probe
schema, so there is no merge to write.

The blocker is not the operator, it is the plan shape: `Planner::plan` builds
exactly one `HashJoinNode` out of `stmt.joins`, and a semi-join is a **second**
join for any query whose `FROM` already joins. That is the stated reason Week 32
refused rather than implemented. So Task 6 has two halves, and only the first is
cheap: (a) the operator, (b) a Volcano plan shape that can hold a second join.

### Implementation guidance

Scope honestly. **(a) alone, restricted to a single-relation `FROM`**, restores
four-mode coverage for exactly the queries Volcano can already shape — which
includes Q4's and Q21's `EXISTS` shape (`FROM drivers d WHERE EXISTS (...)`, one
outer relation). That is a real and large fraction of the week's new queries, at
a fraction of (b)'s cost. Queries whose `FROM` already joins keep the Week 32
refusal, with its message unchanged.

Do **not** relax `Planner::plan`'s `joins.size() > 1` refusal as a side effect.
Week 29's audit found `preserved_slots{0}` is correct *only* because of that
refusal; Week 30 fixed it to derive the set from the FROM scan's own schema, but
the general lesson stands — that function has undocumented couplings to its own
guards, and this is not the week to test them.

### Verification

- The `EXISTS` queries added in Task 3 move from a two-mode diff to a **four-mode**
  diff in `compare_against_sqlite.py`. Count the modes explicitly; this is the
  deliverable.
- Volcano and vectorized return byte-identical rows for every semi/anti query
  (this is the check that has not existed since Week 32).
- The NULL case from Task 3 item 6, run on the Volcano path too — two
  implementations of one NULL rule is precisely what this task exists to check.

---

## Task 7 — The three surfaces audit round 4 did not reach

### Why it matters

Week 32's hand-off names these so that Week 33 "does not read the green gate as
coverage of them". Each is a place where a *read* stands in for a *test*.

### The three, and what to do

**(1) `refuseUnloweredIn`'s call sites in `LogicalPlanBuilder::build`**
(`src/planner/logical_plan.cc`, lines 784 and 818). Round 4 left this as an
explicit hunch: it did not confirm the tripwire runs on **every entry** to
`build` rather than once at the top. That decides whether an `IN` nested inside
an `IN` body's `HAVING` gets its own diagnostic or dies at dispatch site 12 with
an internal-defect message. The closing round read the two call sites and they
are inside `build`'s own body — the reassuring reading — **but that is a read,
not a test, and no test pins it.** `build` recurses (`planBody` calls it), so the
claim is checkable directly: **Week 33 nests deeper than any week so far, so pin
it.** Write the query — an `IN` subquery whose body has a `HAVING` containing
another `IN` — assert the message names `HAVING`, and put it in the rejection
suite.

**(2) The Volcano `HashJoinNode` refusal path.**
`build_had_unmatchable_key_` exists only in
`src/execution/vec_hash_join_node.{h,cc}`. That is consistent with
`WEEK32_SEMI_JOIN_VOLCANO_REJECTED` but **does not prove the refusal is total** —
a semi-join reaching `src/execution/hash_join_node.cc` would find no NULL rule
there at all. Task 6 either implements the rule (making the question moot for the
shapes it covers) or the refusal must be proven total for the rest. Prove it by
test, not by grep: attempt to construct a Volcano plan containing a
`JoinSemantics::SEMI` node and assert it is refused before execution.

**(3) `setCostDecision`'s consumption of `rowWidth`**
(`src/planner/vectorized_plan_builder.cc`) — never traced end to end. It is why
half the `collectSlotTables` rationale was unconfirmed for three audit rounds.
Read the corrected comment in that file and in `development.md` **before trusting
anything written about that block**. Task 3 adds a new join whose children have
asymmetric widths (a semi-join's output is its left child's, so `rowWidth` above
it must not include the body's columns) — if `setCostDecision` sums both
children's widths, the decorrelated plan is mis-costed. Trace it: one query, one
`--explain` with the `cost=` string, hand-computed.

### Verification

Each of the three produces a **test**, not a note. The standard the README sets
is that a read is not a test — three tests, named after the surface they pin.

---

## Task 8 — Precise `collectSlots` / `restampSlots`, and `buildScanSchema`

### Why it matters

Two Week 30 hand-forwards are addressed to this week by name, and both are
performance rather than correctness — which is why they come after the feature
work, and why they are cuttable.

- **`collectSlots` gives a correlated subquery `-1`**, which is conservative, not
  exact. The precise answer is the correlated refs' slots, decremented by one
  level — `ColumnId::outward()` is exactly that operation, and it exists because
  of this note. The README says: *land it with Week 33's decorrelation, not
  before*, because today it would buy pushdown for a conjunct nothing can
  execute. **`restampSlots` (site 9) must move with it** — its body branch is
  currently unreachable *by argument*, and the precise set is what changes that
  argument.
- **`buildScanSchema` declines to narrow when a statement uses a subquery**, so
  no subquery query gets projection pushdown from its correlated columns. Week 31
  already returned pushdown to *materialized* queries by clearing `has_subquery`;
  what remains is the correlated case, where the fix is to collect the correlated
  columns actually referenced and keep exactly those.

### Implementation guidance

Do these **only after Tasks 3-6 are green**, and each as its own commit. Both
change *which* plan is produced, not which rows come out, so the verification is
a plan diff plus an unchanged row diff — `optimizer-diff` is the right tool.
The specific hazard: `collectSlots`' `-1` is what currently keeps `soleSlot` and
`pruningHintForPreservedSide` conservative. Making it precise removes a
containment that three callers rely on, so re-read `predicate_pushdown.h`'s
justification block first — Week 29's audit found that block's stated reason is
false for one of its three callers (`classifyJoinCondition` **accepts** on an
empty set where `soleSlot` declines), and that is the caller a precise set
changes most.

### Verification

Rows unchanged (full oracle suite); plans changed in the expected direction
(`--explain` shows a filter below the join, or a narrower scan schema); no
correlated conjunct is ever pushed onto a scan.

---

## Task 9 — Tests, oracle suites, and the changed-test discipline

### Why it matters

**This is the task the week is most likely to fail at, and it has a precedent.**
Week 32 shipped a regression — a subquery nested inside an `IN` body died at
dispatch site 12 with an internal-invariant message — past a **green 988-query
oracle and 770 green unit tests**. Neither could see it, because the one test
guarding that capability (`tests/test_subquery.cc`) had been **narrowed, in that
same week**, to a flattened scalar-outer stand-in that no longer reached through
`sq->subquery->where` to the nested node. The suite was green about a weaker
claim.

Week 33 changes more subquery tests than any week so far: Task 1 rewrites every
hand-built `ColumnRef` / `GroupByColumn` / `AggregateSpec` in nine test files,
and Tasks 2-5 move queries between rejection and diffed suites wholesale.

**The rule: treat every test this week edits or deletes as a place a capability
can silently vanish, and for each one name the assertion that left and where it
went.** The round-4 audit did exactly this for `NoLongerRefusesALargeInSet` and
found nothing lost — that is the standard, and it is cheap when done per-edit and
impossible when done at the end.

### Implementation guidance

- **Task 1's test churn is spelling-only.** If an assertion changes, that is a
  behaviour change hiding in a rename. Verify with
  `git diff --stat` per test file and by reading only the changed lines: they
  should be constructor arguments, never `EXPECT_*` arguments.
- **Suite moves are two-sided.** A query leaving `WEEK33_CORRELATED_BINDS` must
  arrive in a diffed suite in the same commit. A query that leaves and arrives
  nowhere is a capability that stopped being tested. Diff the suite lists
  before/after and account for every query.
- **Add the nesting depth Week 33 introduces.** Q20's shape — `IN` whose body's
  `WHERE` contains a correlated scalar — is the deepest thing the engine has ever
  planned, and it is the exact shape that hid Week 32's regression. It needs a
  unit test that reaches *through* the outer node to the nested one
  (`sq->subquery->where`), not a flattened stand-in.
- **Rejection-suite expectations must be updated, not left.** Any `_EXPECT`
  string still naming "Week 33" after this week is a test passing for the wrong
  reason.

### Verification — the week's success criteria

1. `cmake --build build -j$(nproc)` clean.
2. `cd build && ./tests/swiftql_tests` — 0 failures, count **≥** the baseline;
   every decrease accounted for in the commit message by name.
3. `python3 python_tools/compare_against_sqlite.py` — green, with the correlated
   `EXISTS` / `NOT EXISTS` shapes **diffed** rather than rejected, in four modes
   if Task 6 lands and two if it does not (state which).
4. Every remaining refusal pinned by message in a rejection suite.
5. `--no-optimize` and optimized agree on every new query; Volcano and vectorized
   agree on every query both support.
6. `development.md` → *Relation slots and query levels* rewritten for the
   post-refusal world, with a row per consumer stating what makes it safe **now**
   — not what the refusal used to guarantee.

The `verify` skill runs 1-3 as one gate; run it before claiming any task done,
and re-run it between Task 1 and Task 2 specifically, because that boundary is
the one the README requires to be independently green.
