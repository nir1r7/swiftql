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
