# Seam audit: subquery chain (weeks 31 -> 35), pass 3

Scope: W31 (materialize-then-substitute) / W32 (IN cap removed, semi joins) /
W33 (EXISTS decorrelation) / W34 (correlated scalar + derived RTEs) / W35
(subqueries inside derived bodies).

Part A verifies pass 2's four fixes. Part B hunts what passes 1 and 2 missed.
Tree: `claude/phase5-week26-qomtkb` @ `922ca15`.

All probes: `./build/swiftql --catalog catalog.json --no-cache --storage columnar
--execution vectorized`, SQLite side through the oracle's own `load_sqlite()`, so
the data is identical to the harness's.

Status: IN PROGRESS.

---

## Part A — pass 2's four fixes

### A1. B-1 (the BLOCKER) — FIXED, and the `hidden` claim is exhaustively true.

**The claim, checked rather than accepted.** `ColumnDef::hidden`
(`src/common/schema.h:39`) has, over the whole tree:

- **exactly three READS**, and they are the three the comment names:
  - `blockOutputSchema` step 3 — `src/planner/logical_plan.cc:569`
  - `LogicalPlanBuilder::build`'s star expansion — `src/planner/logical_plan.cc:1113`
  - `Planner::plan`'s star expansion (Volcano) — `src/planner/planner.cc:431`
- **exactly three WRITES**: `logical_plan.cc:462` (`def.hidden = spec.hidden`,
  the HAVING/ORDER-BY-only aggregate producer), `logical_plan.cc:491`
  (`derivedRelationSchema` forces `false`), and
  `subquery_decorrelation.cc:655` (`col.hidden = true`, the fix).

Grep is not enough on its own, because a fourth consumer need not mention the
word: it could **drop** the flag by rebuilding a `ColumnDef` field-by-field, or
**propagate** it into a schema where one of the three readers then skips a
column it needs. I checked both directions.

**Nothing drops it.** Every place a column list is copied from another schema
copies WHOLE `ColumnDef`s, by value:

| site | what it builds | preserves `hidden`? |
|---|---|---|
| `logical_plan.cc:967-968` | the FROM/JOIN spine's merged schema | yes (`for (ColumnDef col : ...)`) |
| `subquery_decorrelation.cc:619-620` | the `$scalarN` merged join schema | yes — and this is the site that SETS it |
| `join_enumeration.cc:229`, `:270` | the reordered join's merged schema | yes, and it says so at `:269` |
| `planner.cc:349-350` | the Volcano merged join schema | yes |
| `buildProjectSchema` `logical_plan.cc:351` | `cols.push_back(table_schema.column(idx))` for a plain `ColumnRef` item | yes |
| `buildAggregateSchema` `logical_plan.cc:451` | group-key columns | yes |
| `derivedRelationSchema` `logical_plan.cc:491` | a derived relation's schema | **deliberately clears it** |

The three sites that CONSTRUCT a fresh `ColumnDef` (`logical_plan.cc:353`,
`:366`, `:370` and `:406`) are all project/aggregate OUTPUT columns, where
`hidden=false` is the right default and `:462` overrides it for the one
producer that needs it.

**Nothing propagates it where it should not.** The two ways a `hidden` column
could reach a star expansion that must emit it:

1. *Through a derived relation.* It cannot: `derivedRelationSchema` sets
   `hidden = false` on every column (`logical_plan.cc:491`), so a derived
   relation never publishes a hidden column, and the enclosing block's star
   over it emits all of them. That is correct — the derived table's own
   `build()` already narrowed its body.
2. *Through `buildProjectSchema`'s whole-ColumnDef copy.* A hidden column could
   only be copied there if the user's select list NAMED it. The two producers'
   names are `$kN` / `aggregateOutputName(agg)` (unlexable — `$` is not in the
   identifier alphabet, and `FUNC(arg)` is not an identifier) and a
   HAVING/ORDER-BY-only aggregate name, which `extractAggregates` de-dupes
   against the select list at `:611`/`:620` so a named one is never marked
   hidden. Neither is reachable.

**Nothing about RESOLUTION consults it.** `Schema::indexOf(name)` and
`indexOf(name, slot)` (`src/common/schema.cc:25-36`) compare `name` and
`relation_slot` only; `hasColumn` delegates to the first;
`qualifyIfAmbiguous` compares names. There is no fourth overload. So the
outer predicate's slot-qualified read of the aggregate column is untouched by
the fix, which is the property that makes `hidden` the right mechanism rather
than a filter that would have had to be threaded through every reader.

**The other three lowerings genuinely have no hole.** Verified at the source,
not from the fixer's report: `SEMI` / `ANTI_NOT_IN` set `output_schema` to
`left_schema`, a copy of `spine->output_schema` taken one line above the
`LogicalJoin` construction (`subquery_lowering.cc:88-95`), and `SEMI` / `ANTI`
do the identical thing at `subquery_decorrelation.cc:813-819`. Neither merges
the body's columns in, so neither widens the star's domain in the first place.
The correlated-scalar lowering is the only one that merges, and it is the only
one that marks.

**Executed.** Twelve star shapes, every one matching SQLite column-for-column:

```
SELECT * FROM drivers d WHERE d.age > (SELECT COUNT(*) FROM laps l
    WHERE l.driver_id = d.driver_id AND l.speed > 999)
  -> 5 columns, 20 rows = SQLite            (was 7 columns before the fix)

SELECT COUNT(*) AS n FROM (SELECT * FROM drivers d WHERE d.age > (...)) AS x
  -> 20 = SQLite                            (was the internal 5-vs-7 drift error)
```

and the shapes pass 2 did not reach, all correct:

- star over a spine that is a real JOIN plus a synthetic relation (14 columns);
- **two** synthetic relations in one block (5 columns);
- `SELECT DISTINCT *` — and note `LogicalDistinct` is built ABOVE
  `LogicalProject` (`logical_plan.cc:1128`), so it dedupes narrowed rows; had
  it been below, the hidden columns would have changed the row set;
- `SELECT * FROM (SELECT * ... corr scalar ...) x` — star over star;
- the WRAPPED (Q17) shape `l.speed > 1.02 * (SELECT AVG(...) ...)` — 9 columns;
- `SELECT *` with each of IN / NOT IN / EXISTS / NOT EXISTS.

**One route I expected to leak and does not:** `ORDER BY <ordinal>` over a star
query would resolve a position against the *pre-projection* schema, which is the
widened one. It cannot: column ordinals are refused outright
(`ORDER BY 5: column ordinals are not supported`), so `ORDER BY 6` on a
5-column star is a refusal here and a range error in SQLite — different
messages, both refusals.

**Verdict: A1 closed.** No fourth consumer exists, in either the read or the
copy direction, and the three that do exist agree.

### A2. B-2 — FIXED, and `agg_expr->alias.clear()` is narrow enough.

`Expr::alias` has exactly five readers in the tree:

| reader | what it does | runs when |
|---|---|---|
| `logical_plan.cc:375` (`buildProjectSchema`) | renames the projected output column | planning, select-list items only |
| `binder.cc:111` (GROUP BY) | `GROUP BY <alias>` substitution | **binding**, before planning |
| `binder.cc:160` (ORDER BY) | `ORDER BY <alias>` substitution | **binding**, before planning |
| `expr_utils.h:420` (`cloneExpr`) | copies it | anywhere |
| `constant_folding.cc:62/99/115` | preserves it across a fold | inside `Binder::bindQuery` |
| `subquery_materialization.cc:401` | copies the node's alias onto its replacement Literal | before planning |

The clear happens inside `lowerCorrelatedScalars`, i.e. inside
`LogicalPlanBuilder::build`. `build` calls `Validator::validate`
(`logical_plan.cc:913`) and **never** the Binder — `bindQuery` is called only
from `Binder::bind` and from `binder.cc:219/331` (a derived body and a subquery
body), all of which have completed before any planning. So the two
alias-consuming binder rules cannot observe the clear, and the only reader that
runs after it is `buildProjectSchema`, which is exactly the one whose
disagreement with `aggregateOutputName` was the defect.

**Is the wrapper's alias really inert?** Yes, and for a stronger reason than
"WHERE items are not select-list items". After `slot = std::move(wrapper)`
(`subquery_decorrelation.cc:723`) the wrapper is a subexpression of a WHERE
conjunct. For its alias to be read, that conjunct would have to become a
select-list item; nothing moves a WHERE expression into a select list — the
only movement in the other direction is the aggregate this code just moved INTO
the body's select list, whose alias is the one being cleared. Confirmed by
execution: the wrapped-with-alias form plans and answers, and the alias appears
nowhere in the output or the plan.

**Executed — all four forms, values and `--explain`:** see the Part A
execution block below.

### A3. B-3 — FIXED; the suppression cannot leak, but its stated reason is wrong.

The four leak routes, each closed:

1. **Re-entrancy.** `reachesOutsideThisBody` is four lines: construct guard,
   `collectSlots`, test, destruct. No lowering, no `build()`, no `refuse()`
   runs inside the window. Both call sites test the predicate to completion
   BEFORE refusing (`splitCorrelation:113`, and the `check` lambda in
   `refuseSurvivingCorrelatedRefs` calls `reachesOutsideThisBody(e)` and only
   then `refuse`), so no throw ever unwinds *through* a live guard from those
   sites either.
2. **An exception mid-walk.** `collectSlots`' only throwing call is
   `localSlot`, guarded by `isLocal()` — so it cannot throw today. The RAII
   destructor makes that irrelevant, which is the right call.
3. **A body reachable by two paths.** This one is a non-hazard, and the fix's
   comment mis-states why (see L-1). `correlated` is a field of
   **`SubqueryExpr`**, not of the shared `SelectStatement`
   (`ast.h:191`/`:196`), and `cloneExpr` creates a NEW `SubqueryExpr` that
   copies the flag while SHARING the body. So two expressions naming one body
   hold two independent flags; clearing one cannot be observed through the
   other. The guard restores by node pointer, so it restores exactly what it
   cleared.
4. **A nested lowering running while flags are suppressed.** Cannot happen —
   see (1); the window contains one pure call.

**The reach is exactly right, which is the part that actually needed checking.**
The suppression is only sound if it clears the flag on precisely the nodes
`collectSlots` will consult. Both walkers stop at a `SubqueryExpr` without
descending into `sq->subquery` (`subquery_materialization.cc:43-49`,
`predicate_pushdown.cc:127-131`), and their container-subtype dispatch lists are
**identical** — `BinaryExpr`, `IsNullExpr`, `UnaryExpr`, `AggregateExpr`,
`InExpr`, `LikeExpr`, `CaseExpr`, `SubstringExpr`, `SubqueryExpr` (operand). If
site 8 reached a subtype site 19 did not, a correlated node would be counted and
not cleared and B-3 would survive in that shape. It does not.

### A4. The depth guard — `!= 1` is the right rule.

**It is a pure depth test, and cannot fire for a non-depth reason.** By the time
`outer_side` is chosen, `l_outer != r_outer` has been established, and
`l_outer` is `!id.isLocal()`, which is `level_ != 0` (`column_id.h:49`). So
`outer_side->id.level() >= 1` always: level 0 is unreachable by construction,
and an UNRESOLVED id (`ColumnId()` — level 0, slot -1) reports `isLocal()`
true, so it is parted into the *both-local* branch above with its own message
rather than arriving here. `!= 1` and `> 1` therefore select the same set
today; `!= 1` is the safer spelling if the parting above is ever changed.

**One level out is safe in every shape I can construct** — enumerated by what
the outer slot can name, since `outer_side->id.outward().localSlot()` is
resolved by `leftKeyIndices` against the enclosing spine's schema:

(executed below)

---

## Part A — execution

(pending)

Executed. Every level-2 shape I could construct is refused **by name**, and the
message names the depth:

```
EXISTS (... l.driver_id = d.driver_id AND EXISTS (... l2.driver_id = d.driver_id))
EXISTS (... l.driver_id = d.driver_id AND l.speed > (SELECT AVG(...) ... l2.driver_id = d.driver_id))
d.age > (SELECT COUNT(*) ... l.driver_id = d.driver_id AND EXISTS (... l2.driver_id = d.driver_id))
EXISTS (... AND EXISTS (... l2.lap_id = l.lap_id AND l2.season = d.age))      -- level 2 in a NON-key conjunct
EXISTS (... AND EXISTS (... AND EXISTS (... l3.driver_id = d.driver_id)))     -- level 3
(SELECT AVG(l2.speed) ... l2.team = l.team AND EXISTS (... l3.driver_id = l.driver_id))
  -> Error: correlated subquery: a reference to a query block more than one
     level out cannot be decorrelated here
```

One level-2 shape is caught EARLIER and correctly: a level-2 ref in the inner
body's SELECT list is refused by `refuseSurvivingCorrelatedRefs`
(`... survives in its SELECT list`), because that check runs before the body's
list is replaced. Both are refusals, neither is a wrong answer.

And every ONE-level shape runs and matches SQLite. The enumeration is by what
the outer slot can name, because `outer_side->id.outward().localSlot()` is the
value `leftKeyIndices` resolves against the enclosing spine:

| outer slot names | probe | SwiftQL = SQLite |
|---|---|---|
| slot 0, single relation | `EXISTS (... = d.driver_id)` | 20 |
| slot 1 of a 2-way join | `EXISTS (... l2.lap_id = l.lap_id ...)` | 770 |
| slot 1 of a SELF-join, shared column names | `d2.age > (SELECT COUNT(*) ... = d2.driver_id)` vs `= d1.driver_id` | **5 vs 8** — the pair discriminates, and both match |
| a DERIVED relation at slot 0 | `FROM (SELECT ...) x WHERE EXISTS (... = x.driver_id)` | 20 / 2 |
| slot 2 of a 3-way join | `EXISTS (... = d3.driver_id ...)` | 0 |
| inside a derived BODY (the W34xW35 joint) | both EXISTS and scalar forms | 20 / 2 |

Nesting one level out at every depth also runs: EXISTS-in-EXISTS,
scalar-in-EXISTS, EXISTS-in-scalar, scalar-in-scalar, NOT-EXISTS-in-EXISTS and
a three-deep chain — all six match SQLite.

**Where the guard sits relative to the other refusals**, since "can it be
reached by something that is not a depth problem" also has a converse: two
shapes that ARE one level out now reach a *different* refusal because of B-3's
fix, and both messages are accurate:

```
(SELECT AVG(l2.speed) FROM laps l2 WHERE EXISTS (SELECT 1 FROM laps l3 WHERE l3.driver_id = l.driver_id))
  -> correlated subquery: no equality links the scalar subquery to the enclosing
     query, so there is no group key to decorrelate on            (SQLite: 4997)
EXISTS (SELECT 1 FROM laps l WHERE EXISTS (SELECT 1 FROM laps l3 WHERE l3.driver_id = d.driver_id))
  -> correlated subquery: no equality links the subquery to the enclosing query,
     so there is no join key to decorrelate on                    (SQLite: 20)
```

These are the shapes whose ONLY correlation runs through a nested subquery: the
node is `correlated`, but `splitCorrelation` — now correctly suppressing the
nested flag — finds no key. The refusal is truthful and the class is the
documented "decorrelation cannot express this" one. It is **not** in the
README's enumeration of refused correlated shapes (see L-2).

---

## Part B — findings

### B-1 — LOW. `WEEK33_CORRELATED_NESTED_VEC` pins a 24-way prefix, and two
`WEEK33_NESTED_TRIPWIRE_REFUSED` pins are 4- and 5-way. The file states the rule
these break, twelve lines from one of them.

I reconstructed every error-message literal in `src/` (168 of them, joining
adjacent string literals inside each `refuse(...)` / `throw
std::runtime_error(...)` / `ParseError(...)` call) and tested each of the 202
rejection-suite entries against the set. Three suites in this seam pin a needle
that more than one message satisfies:

| suite | pin | messages containing it |
|---|---|---|
| `WEEK33_CORRELATED_NESTED_VEC` (3 entries) | `"correlated subquer"` | **24** |
| `WEEK33_NESTED_TRIPWIRE_REFUSED` entry 1 | `"IN subquery"` | **4** |
| `WEEK33_NESTED_TRIPWIRE_REFUSED` entry 2 | `"HAVING"` | **5** |
| `WEEK35_SUBQUERY_IN_DERIVED_BODY_VOLCANO_REJECTED` (5 entries) | `"not supported on the Volcano path"` | **4** |

Concrete failing shapes — for each pin, a query that fires a **different** guard
and still satisfies the needle, so the entry would pass while asserting nothing
about the guard it was written for:

```
pin "HAVING"            SELECT lap_id FROM laps WHERE driver_id IN
                          (SELECT l.driver_id FROM laps l HAVING MAX(l.speed) > 0)
                        -> Error: HAVING requires GROUP BY          [Validator, not the tripwire]

pin "IN subquery"       SELECT COUNT(*) FROM laps WHERE season * 5 IN
                          (SELECT driver_id FROM drivers)
                        -> Error: IN subquery: the left operand must be a column reference

pin "correlated subquer" SELECT COUNT(*) FROM drivers d WHERE EXISTS
                          (SELECT 1 FROM laps l WHERE l.driver_id = d.driver_id LIMIT 1)
                        -> Error: correlated subquery: a body with LIMIT cannot be decorrelated
```

Why this is not merely stylistic:

- **`WEEK33_NESTED_TRIPWIRE_REFUSED` states its own purpose and its pins cannot
  serve it.** Its header says it exists to settle "whether an IN nested inside
  an IN body's HAVING gets its own diagnostic **or dies at dispatch site 12 with
  an internal-defect message**". `"IN subquery"` is a substring of
  `internal: an IN subquery reached materialization (Week 32 lowers it to a
  semi-join)` — an internal-defect message. The pin cannot distinguish the two
  outcomes the suite was written to distinguish. Both of its entries in fact
  produce the *same* message today —
  `IN subquery: supported only as a whole top-level WHERE conjunct (found one in
  HAVING)` — and neither pin names it.
- **The file states the rule.** `WEEK32_LOWERING_REFUSED`, twelve lines above
  the tripwire suite, pins the full parenthetical and explains why: *"Without it
  the two entries pin one string that both diagnostics satisfy, so a HAVING
  subquery collapsing into the generic non-top-level branch (or the reverse) is
  invisible — the same 'cannot say which guard fired' defect as the Volcano
  needles above."* And `WEEK34_DERIVED_TABLE_VOLCANO_REJECTED` (line 1166) picks
  its needle **per query by guard-ordering rule** for exactly this reason,
  while its Week-35 neighbour twenty lines earlier does not. Same file, same
  page, two standards.
- **`WEEK33_CORRELATED_NESTED_VEC`'s pin is also satisfied by the VOLCANO
  wording** (`correlated subqueries are decorrelated to a semi-join and are not
  supported on the Volcano path`), so the vec-mode entry would pass if it were
  ever run in a Volcano mode — while its sibling
  `WEEK33_CORRELATED_NESTED_VOLCANO` pins `VOLCANO_IN` *specifically* and its
  comment says pinning it "is what makes that ordering an assertion rather than
  an accident". The discrimination the file cares about in one direction is
  absent in the other.

Minimum fix, no new machinery: three needles.
`"a correlated IN / NOT IN is not lowered"` for the three
`WEEK33_CORRELATED_NESTED_VEC` entries (generated by comprehension, so one
edit), and `"whole top-level WHERE conjunct (found one in HAVING)"` for both
tripwire entries. For `WEEK35_..._VOLCANO_REJECTED`, `VOLCANO_DERIVED` — all
five fire the derived-table guard today, verified by running them, because
`Planner::plan`'s `scan()` walks only the OUTER statement's WHERE/HAVING and
these queries' subqueries live inside derived BODIES, so the IN and correlated
guards are structurally unreachable for this suite. The generic needle hides
that, and the list is built by comprehension from the VEC_ONLY list, so a query
added there later inherits the weak pin automatically.

This is the same class as pass 2's B-6 (one entry, fixed in `b24f46e`) — five
more entries of it, in three suites, one of which documents the rule it breaks.

### NULL semantics — the open coverage hole, assessed. NOT a blocker.

The brief's standing item: *"17 `NOT IN` oracle entries and not one has a NULL
in the body."* Two things have changed since pass 2 wrote that, and I checked
both.

**(a) The count is now 19, and the hole is partly closed at HEAD.**
`WEEK32_SEMI_JOIN_VEC_ONLY` carries three LEFT-JOIN-manufactured NULL entries
(the join-chain auditor's, present in this tree):

```
... WHERE driver_id NOT IN (SELECT l.driver_id FROM drivers d
                            LEFT JOIN laps l ON d.driver_id = l.driver_id
                                            AND l.speed > 99999)     -- all-NULL BODY
... FROM drivers d LEFT JOIN laps l ON ... AND l.speed > 99999
    WHERE l.driver_id NOT IN (SELECT driver_id FROM laps)            -- NULL on the OUTER side
... the same, IN rather than NOT IN
```

So "not one has a NULL" is no longer true of this tree. What is still missing is
the **MIXED** body — one NULL beside real keys — which is the case an all-NULL
body cannot reach: an implementation that only set its unmatchable flag when the
build side is *entirely* NULL passes every entry above.

**(b) The behaviour is correct, in every shape I could manufacture.** Two
independent NULL routes (expression `x/0`, and LEFT-JOIN null-extension),
against SQLite:

| shape | SwiftQL | SQLite | what a wrong impl would give |
|---|---|---|---|
| `NOT IN` all-NULL body | 0 | 0 | 20 (two-valued) |
| `IN` all-NULL body | 0 | 0 | — |
| `NOT IN` **mixed** body (`l.driver_id/(l.driver_id-5)`) | 0 | 0 | 16 (NULL dropped from build) |
| `NOT IN` **mixed** body (LEFT JOIN route) | 0 | 0 | **15** |
| ...same body with the NULL rows filtered out (counterfactual) | 15 | 15 | — |
| `IN` mixed body (LEFT JOIN route) | 5 | 5 | — |
| `NOT IN` over an EMPTY body | 20 | 20 | 0 |
| NULL outer, non-empty body, `NOT IN` | 0 | 0 | 20 |
| NULL outer, EMPTY body, `NOT IN` | **20** | 20 | 0 — the empty set makes `NOT IN` TRUE even for a NULL operand |
| NULL outer, EMPTY body, `IN` | 0 | 0 | — |
| NULL outer, `NOT EXISTS` (two-valued: KEEP the row) | **20** | 20 | 0 if `ANTI` had `ANTI_NOT_IN`'s rule |
| NULL outer, `EXISTS` | 0 | 0 | — |
| NULL correlation key on the BUILD side of `NOT EXISTS` | 20 | 20 | — |
| ...and of `EXISTS` | 0 | 0 | — |
| `NOT IN` NULL body inside a DERIVED body | 0 | 0 | — |
| `NOT IN` whose body is itself a DERIVED table, NULL-bearing | 0 | 0 | — |

The three rows in bold are the ones that part `ANTI` from `ANTI_NOT_IN` on the
same data: a NULL operand makes `NOT IN` unknown but leaves `NOT EXISTS` true,
and an empty right side makes `NOT IN` true again. All three are right.

**One producer of NULL that structurally cannot reach a `NOT IN` build key**,
worth recording because it looks like a gap and is not: a correlated scalar's
zero-row rule manufactures NULL, but its value can never become a set-membership
key — a subquery is refused in the SELECT position by the Validator
(`allow_subqueries=false`), so the only way to project it is impossible, and the
`IN` operand must be a plain `ColumnRef` (`subquery_lowering.cc:70`). So the two
NULL mechanisms in this seam do not compose.

So: **the hole is narrower than recorded and the behaviour behind it is
correct**. It stays a coverage item (L-3), not a blocker.

### Cardinality — the documented divergence has not widened.

`README.md:104` states the only deliberate cardinality divergence in the
subquery rules: a MATERIALIZED scalar over more than one row is an error here
and SQLite's first row there. Re-verified end to end at HEAD:

```
WHERE l.speed > (SELECT l2.speed FROM laps l2)                -> Error: scalar subquery
                                                                 returned more than one row
                                                                 (SQLite answers 1852)
WHERE l.speed > (SELECT l2.speed FROM laps l2 WHERE l2.lap_id = 1)      -> 3410 = SQLite
WHERE l.speed > (SELECT l2.speed FROM laps l2 WHERE l2.lap_id = 999999) -> 0    = SQLite  (zero rows -> NULL)
WHERE l.speed > (SELECT MAX(l2.speed) FROM laps l2 WHERE l2.speed > 9999) -> 0  = SQLite  (one NULL row)
```

The **correlated** path keeps the guarantee structurally rather than by a check,
and the structure still holds: `requireDecorrelatableScalarBody` refuses LIMIT /
DISTINCT / HAVING / an own GROUP BY, and `constantWrapperAggregateSlot` requires
exactly one aggregate under constant-only arithmetic — verified firing for a
non-aggregate body, for two aggregates, and for a non-constant wrapper naming an
OUTER column. The zero-row rules are exercised with predicates that
**discriminate** rather than with `> 0`:

```
WHERE (SELECT COUNT(*)     ... empty group) = 0   -> 20 = SQLite    (NULL would give 0)
WHERE (SELECT 1 + COUNT(*) ... empty group) = 1   -> 20 = SQLite    (the lifted-wrapper CASE;
                                                                    pushing it through the body
                                                                    would answer 0 and give 0 rows)
WHERE (SELECT AVG(l.speed) ... empty group) IS NULL -> 20 = SQLite
```

Nothing has widened: the error still fires, zero rows is still NULL, and no
correlated body that could return two rows for one key is admitted.

### ANY / ALL — not in either grammar.

`> ANY (SELECT ...)` / `> ALL (SELECT ...)` are parse errors in **both**
engines (SwiftQL: *unexpected trailing input*; SQLite: *syntax error*). SQLite
does not implement them either, so there is no divergence and no oracle entry is
possible in either direction. Recorded so the brief's NULL-semantics item is
closed rather than left open.

### The refusal boundary, re-derived from the code at HEAD and each one executed

Every `refuse(...)` / `throw` reachable from the three lowering passes, with a
constructed query for each and what actually fired. This supersedes pass 2's
table: B-3's fix and `b24f46e` moved three rows.

| refusal (site) | constructed shape fires it? | pinned? |
|---|---|---|
| EXISTS body GROUP BY (`decorr.cc:22`) | yes | `test_logical_plan.cc` |
| EXISTS body HAVING (`:23`) | **NO — dead**, `HAVING requires GROUP BY` wins (B-5.1, recorded) | no |
| EXISTS body LIMIT (`:24`) | yes | no |
| EXISTS body DISTINCT (`:25`) | yes | no |
| EXISTS body aggregate (`:30`) | yes | `test_logical_plan.cc` |
| correlated non-equality conjunct (`:139`) | yes — **and no longer over-fires**: the three pass-2 B-3 shapes now run | oracle x3 + `SEAM2_NESTED_CORRELATION` |
| computed side of a correlated equality (`:147`) | yes | no |
| both sides outer / unresolved (`:160`) | yes (constructed the both-outer half) | no |
| **depth `!= 1` (`:202`)** | **yes, six distinct shapes** — live since B-3 | `SEAM2_CORRELATION_DEPTH_REFUSED` x2 |
| survivor in JOIN ... ON (`:270`) | yes (needs a real inner key beside the correlated ref) | `WEEK33_CORRELATED_BINDS` |
| survivor in SELECT list (`:270`) | yes | oracle |
| survivor in ORDER BY (`:270`) | yes | no |
| survivor in WHERE (`:270`) | not constructible — `splitCorrelation` empties it | no |
| two aggregates under one wrapper (`:358`) | yes | oracle |
| non-aggregate body (`:377`) | yes | oracle |
| non-constant wrapper (`:377`) | yes **only when the wrapper names an OUTER column**; a wrapper naming a BODY column is refused earlier by the Validator's grouped-reference rule, which README:115 says is deliberate | oracle |
| scalar body LIMIT (`:404`) | yes | `WEEK34_CORRELATED_SCALAR_REFUSED` (now pinned precisely — `b24f46e`) |
| scalar body DISTINCT (`:405`) | yes | no |
| scalar body HAVING (`:406`) | yes (guard order differs from the EXISTS pair, which is why this one lives) | no |
| scalar body own GROUP BY (`:408`) | yes | no |
| scalar select-list arity (`:411`) | **NO — dead**, Validator's arity rule wins (B-5.2, recorded) | no |
| shared body, SCALAR (`:439`) | yes — `(SELECT ...) BETWEEN a AND b` | `test_subquery.cc` |
| shared body, EXISTS (`:756`) | **NO — unreachable** (see below) | no |
| shared body, IN (`lowering.cc:21`) | **NO — unreachable** (see below) | no |
| scalar: no correlated equality (`:457`) | yes — reachable **only** through B-3's new route | no |
| EXISTS: no correlated equality (`:770`) | yes — same new route | `test_logical_plan.cc` (a different shape) |
| `renamed.size() != keys.size()+1` (`:599`) | not constructible | no |
| correlated IN / NOT IN (`:878`) | yes, both polarities | oracle x3 |
| correlated not a top-level conjunct (`:883`) | yes, WHERE-under-OR and HAVING | oracle |
| IN computed operand (`lowering.cc:70`) | yes | `WEEK32_LOWERING_REFUSED` |
| IN not a top-level conjunct (`lowering.cc:142`) | yes, both positions | `WEEK32_LOWERING_REFUSED` x2 |
| materialized scalar > 1 row (`materialization.cc:230`) | yes | `WEEK31_MATERIALIZATION_REFUSED` |
| IN reached materialization (`materialization.cc:258`) | internal, unreachable by design | no |

**The two shared-body refusals for IN and EXISTS are unreachable, and it is the
same class as pass 2's B-5 rather than a new one** — recorded here as fact, not
as a finding. `cloneExpr` has twelve call sites; only `parser.cc:340` (BETWEEN's
desugaring) can duplicate a user's subquery node, and it clones the LEFT
operand of `BETWEEN`, which the grammar makes an *additive* — so a `SCALAR`
node can land there (and does: `(SELECT ...) BETWEEN 1 AND 10` fires `:439`)
while an `IN` or `EXISTS` node, which sits at predicate level, cannot. The
remaining clone sites are ON residuals (subqueries refused in `ON`) and the
Binder's select-list alias substitution (subqueries refused in `SELECT`), both
closed by the Validator. So one of the three identical `use_count() > 1` guards
fires and two cannot — pass 2's B-5 shape, two more instances.
