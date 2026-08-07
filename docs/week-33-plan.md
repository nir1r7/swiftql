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
