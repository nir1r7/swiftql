# Seam audit: subquery chain (weeks 31 -> 35), pass 2

Scope: the joints between W31 (materialize-then-substitute) / W32 (IN cap removed,
semi joins) / W33 (EXISTS decorrelation) / W34 (correlated scalar + derived RTEs) /
W35 (subqueries inside derived bodies).

Part A verifies commit `8a23b9d` (the F1/F2 coupled-pair fix). Part B hunts fresh.

Status: IN PROGRESS.

---

## Part A — is `8a23b9d` real and complete?

### A1. Is the collision impossible, or merely unlikely? — IMPOSSIBLE, and for a
reason stronger than the commit message states.

`subquery_decorrelation.cc:444-459` renames positions `0..keys.size()-1` of the
body plan's output schema to `$k0..$k{n-1}` and repoints `keys[i].join_col` at
them. Three things have to hold for the bare-name lookup in
`rightKeyIndices(..., positional=false)` (`vectorized_plan_builder.cc:125-149`)
to be unique, and all three do:

1. **Pairwise distinct among themselves.** `$k` + `to_string(i)` over distinct
   `i`. Trivially true.
2. **No user identifier can be `$kN`.** Confirmed at the lexer, not assumed —
   `$` is not in the identifier alphabet and there is no quoted-identifier
   production, so no catalog column, alias or `AS d (a,b)` rename can spell one.
3. **No OTHER column of the same schema can be `$kN`.** This is the one the
   commit message hand-waves ("$ is not lexable"), and it holds for a different
   reason: the schema `rightKeyIndices` searches is
   `join_child->outputSchema()` — the `VecDerivedNode`'s schema, i.e. exactly
   `normalized`, which the width check at `:447-452` proves is
   `keys.size() + 1` columns wide. Positions `0..n-1` are the renamed keys; the
   ONE remaining column is the aggregate, named `aggregateOutputName(agg)`,
   which renders `FUNC(arg)` and cannot be `$kN`. So the search space contains
   exactly n+1 names, n of them generated-distinct and one provably outside the
   generated namespace.

The collision F1 named is therefore genuinely gone, not made unlikely.

**Nested `$scalarN` at a different level does not reopen it.** Two correlated
scalars in one block produce `$scalar0` and `$scalar1`, and BOTH publish a
column literally named `$k0` into the same merged schema. That merged schema is
never the one `rightKeyIndices` searches (it searches each build child's own
schema), and the merged schema is only read through slot-qualified lookups:
`leftKeyIndices` uses `indexOf(from_col, from_slot)` and throws on a miss
rather than falling back (`vectorized_plan_builder.cc:97-105`), and the
substituted outer `ColumnRef` is stamped `ColumnId::local(derived_slot)`
(`subquery_decorrelation.cc:494`) so `resolveColumnIndex`
(`evaluator.cc:72-79`) hits the slot-qualified overload first. Verified the
same shape for the AGGREGATE name too: two `COUNT(*)` bodies publish two
columns both named `COUNT(*)` at different slots, and the same slot-qualified
path separates them.

### A2. Is the guard still reachable? — Reachable globally, DEAD on this call
site, and that is the correct outcome (not a moved problem).

`derivedRelationSchema`'s duplicate-name check (`logical_plan.cc:497-505`) has
two callers: `buildRelation` (`logical_plan.cc:855`, `:867`) for USER-written
`FROM (...) AS d`, and the `$scalarN` site. It is still live and still
user-facing on the first — `SELECT * FROM (SELECT d.driver_id, l.driver_id
FROM drivers d JOIN laps l ON ...) x` is exactly what it is for, and there the
message's advice ("give one of them an alias") is actionable.

On the `$scalarN` site it can no longer fire: every name handed to it is either
`$kN` (generated distinct) or the single aggregate column. That is the right
shape — the guard was never `$scalarN`'s guard, it was a user-facing check that
an internal path was accidentally routed through. The fix removed the routing,
not the check. The check did not lose a caller it needed.

### A3. Order is now load-bearing. What establishes it?

`renamed[i] <-> keys[i]` is asserted purely by index. What makes index i of the
body plan's output schema the i-th correlation key:

- `body.select_list` is rebuilt as `[body_key_refs[0..n-1]..., agg_expr]`
  (`subquery_decorrelation.cc:389-396`), in `keys` order — the same loop pushes
  `group_by` and `select_list`, so they cannot disagree.
- `body_plan->output_schema` is the terminal `LogicalProject`'s schema
  (`logical_plan.cc:1105-1106`), which `buildProjectSchema` builds one column
  per select-list item, in order. **Note the commit comment cites
  `buildAggregateSchema` here; that is the wrong function** — the aggregate
  schema is one node lower. The property still holds (both are select-list
  ordered), but the cited justification is not the code that runs.
- The width check `renamed.size() == keys.size() + 1` (`:447`) then pins it.

Can the order differ between two places that must agree?
- **planner vs executor:** no. The rename is applied once, before the
  `LogicalDerived` is constructed; both `--explain` and the vectorized lowering
  read the same `LogicalDerived::output_schema`.
- **optimized vs `--no-optimize`:** no. The rename happens in
  `LogicalPlanBuilder::build`, which runs before any optimizer pass, and
  `JoinEnumeration` declines the whole tree on `containsOuterJoin`
  (`join_enumeration.cc:92-99`) — this lowering always produces a
  `JoinType::LEFT`. So enumeration cannot reorder the `$scalarN` join.
- **Volcano vs vectorized:** not applicable — `Planner::plan` refuses every
  correlated subquery outright (`planner.cc:71-79`), so Volcano never sees a
  `$kN`.

### A4. Does the synthetic name leak to a user?

Yes, in one place, and it is new with this commit. `LogicalJoin::explain()`
(`logical_plan.cc:742-749`) prints `... = keys[i].join_col`, so `--explain` on
a correlated-scalar query now renders

    LogicalLeftJoin [driver_id = $k0]

where before `8a23b9d` it rendered `[driver_id = driver_id]`. `$scalarN`
already leaked via `LogicalDerived::explain()` (`:710-711`) before this commit
and is shown in `docs/week-36-plan.md:127`, so the family of leak is
pre-existing; the `$kN` half is not. No test, oracle entry or rejection suite
pins either string (`run_tpch.py` and `test_new_queries.py` only grep for the
node NAME substring), so nothing breaks — it is a cosmetic regression only.
See L-1 below.

**Verdict on Part A: the fix is real and complete.** It closes F1 and F2
without relaxing the guard, the collision is impossible rather than unlikely,
and the ordering it now depends on is established in one place and consumed in
one place.

---

## Part B — findings

### B-1 — BLOCKER (pending execution). `SELECT *` plus a correlated scalar
subquery emits the synthetic relation's columns in the result.

`lowerCorrelatedScalars` grafts the `$scalarN` `LogicalDerived` as
`children[1]` of a **`JoinType::LEFT`, `JoinSemantics::STANDARD`** join whose
`output_schema` is a **MERGED** schema — spine columns plus
`[$k0..$k{n-1}, <agg>]` stamped at `derived_slot`
(`subquery_decorrelation.cc:467-476`). That is deliberate and correct: the
outer predicate has to be able to read the aggregate column.

But `LogicalPlanBuilder::build`'s `SELECT *` expansion runs LAST, over
`node->output_schema` (`logical_plan.cc:1086-1101`), and that node is the
filter above this merged join. It skips only `col.hidden`, and
`derivedRelationSchema` explicitly sets `hidden = false` on every column it
normalizes (`logical_plan.cc:492-494`). So the star expands over the
synthetic relation's columns too.

Failing shape:

    SELECT * FROM drivers d
    WHERE d.age > (SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id)

SQLite returns 5 columns (`drivers.*`). SwiftQL should return
**7** — `driver_id, name, nationality, team, age, $k0, COUNT(*)`.

This is a **wrong answer, not an error**: the extra columns resolve cleanly
(`resolveColumnIndex` hits `$k0` at `derived_slot`), so nothing throws.

Contrast with the other three lowerings, which are immune: `SEMI`/`ANTI` /
`ANTI_NOT_IN` joins set `output_schema` to `children[0]`'s
(`subquery_lowering.cc:92-95`, `subquery_decorrelation.cc:629-632`), so a
star over them sees only spine columns. The correlated-scalar lowering is the
one that broke that containment on purpose — and `development.md`'s "Relation
slots and query levels" says so ("A derived table's columns *are* in scope
above it") — but nothing re-narrowed the star.

**Second symptom of the same defect, inside a derived body.** `buildRelation`
computes `expected` from `blockOutputSchema` (`logical_plan.cc:855`) and
compares it against the built plan's schema (`:869-887`).
`blockOutputSchema` models the spine, the aggregate and the project
(`logical_plan.cc:505-560`) and **does not model any subquery lowering at
all**. For a non-star body the project schema is written from the select
list, so the two agree; for a `SELECT *` body they cannot. So

    SELECT COUNT(*) FROM (
      SELECT * FROM drivers d
      WHERE d.age > (SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id)
    ) x

should hit the drift check and report
`internal: derived table 'x' was bound against a 5-column schema but planned
to 7 columns` — an internal-defect message for legal SQL.

**Not introduced by `8a23b9d`**; it is a Week 34 defect. Before the rename the
extra columns were named `driver_id` and `COUNT(*)`, so the top-level symptom
was `driver_id` appearing twice in the output. The fix changed what the wrong
output looks like, not whether it is wrong.

**Coverage hole that hides it.** Every `WEEK34_CORRELATED_SCALAR_VEC_ONLY`
entry names its select list explicitly (`SELECT COUNT(*) AS n ...`,
`SELECT d.driver_id AS did ...`) — checked all of them. Not one is
`SELECT *`. TPC-H Q17 also names its columns. So the oracle cannot see this.
`SELECT *` bodies ARE used elsewhere in the suite, but only inside `EXISTS`
(where they are harmless — EXISTS reads no values) and inside derived tables
with no correlated scalar.

STATUS: to be executed once the gate releases `build/`.

### B-2 — MEDIUM (pending execution). A column alias on an UNWRAPPED correlated
scalar body breaks the substitution; the WRAPPED form of the same query works.

`lowerCorrelatedScalars` computes the substituted reference's name from the
aggregate *node*:

    const std::string agg_name = aggregateOutputName(agg);      // :374
    ...
    ref->column_name = agg_name;  ref->id = ColumnId::local(derived_slot);  // :492-494

but the derived relation's column name comes from `buildProjectSchema`, which
lets a SELECT alias override the canonical name
(`logical_plan.cc:375`: `if (!expr->alias.empty()) cols.back().name = ...`).

The two disagree exactly when the alias sits on the node that gets moved into
the rewritten select list — i.e. when the wrapper IS the aggregate:

    -- UNWRAPPED, aliased: agg_expr carries alias "a", so the relation publishes
    -- [$k0, "a"] while the outer ref looks for "AVG(l2.speed)"
    SELECT COUNT(*) FROM laps l WHERE l.speed >
      (SELECT AVG(l2.speed) AS a FROM laps l2 WHERE l2.team = l.team)

    -- WRAPPED, aliased: the alias stays on the BinaryExpr, which is lifted out;
    -- the bare AggregateExpr moved into the select list has no alias, so the
    -- relation publishes [$k0, "AVG(l2.speed)"] and it works
    SELECT COUNT(*) FROM laps l WHERE l.speed >
      (SELECT 0.5 * AVG(l2.speed) AS a FROM laps l2 WHERE l2.team = l.team)

Expected symptom for the first: `column not found: 'AVG(l2.speed)'` from
`inferExprType` (`logical_plan.cc:132`) — a message naming a column the user
never wrote, for legal SQL SQLite answers.

Not a wrong answer: the bare-name fallback searches the whole merged schema,
and `aggregateOutputName` renders `FUNC(arg)`, which no identifier can spell,
so it must miss rather than hit the wrong column. That is why this is MEDIUM
and not a BLOCKER — but it is the same "resolve by a name derived in one place
and looked up in another" shape that H-1/H-2 and F2 were, on the one path that
still does it.

It also contradicts a claim the suite is built on: `compare_against_sqlite.py`
says the wrapped and constant-outside forms "produce the SAME plan", and
Week 36's header says the same. With an alias present they do not — one plans
and one does not.

STATUS: to be executed once the gate releases `build/`.

### B-3 — MEDIUM (pending execution). A correlated subquery nested inside a
correlated body is refused, by a guard that names the wrong cause.

`splitCorrelation` decides "does this conjunct reach outside the body?" with
`collectSlots(c, slots); if (slots.find(-1) == slots.end()) -> local`
(`subquery_decorrelation.cc:52-54`). Its comment enumerates the producers of
`-1` as **two**: a correlated `ColumnRef`, and an unresolved one.

`collectSlots` has a **third** producer, in a branch the comment does not
mention (`predicate_pushdown.cc:127-131`):

    if (auto* sq = dynamic_cast<const SubqueryExpr*>(expr)) {
        collectSlots(sq->operand.get(), out);
        if (sq->correlated) out.insert(-1);
        return;
    }

`-1` there is `collectSlots`' *conservative* "cannot name it here", which is
right for pushdown (withhold it) and wrong as a correlation test: the inner
node's `correlated` flag means "reaches outside the INNER body", and for a
one-level reference that target is **the body splitCorrelation is splitting** —
i.e. body-local, exactly the classification it must get.

So a conjunct that merely *contains* a nested correlated subquery is routed to
the refusing branch, fails the `BinaryExpr && op == "="` test, and reports:

    correlated subquery: only an equality between two columns can become a join
    key (a correlated inequality has no equi-join to lower to; ...)

for a query with no inequality in it at all. Same wrong-cause-diagnostic class
as round 1's L-8, which this same function was already corrected for once
(`:83-97`).

Failing shapes (all legal SQL, all answered by SQLite):

    -- nested correlated EXISTS inside a correlated EXISTS body
    SELECT COUNT(*) FROM drivers d WHERE EXISTS (
      SELECT 1 FROM laps l WHERE l.driver_id = d.driver_id
        AND EXISTS (SELECT 1 FROM laps l2 WHERE l2.lap_id = l.lap_id))

    -- correlated scalar inside a correlated EXISTS body
    SELECT COUNT(*) FROM drivers d WHERE EXISTS (
      SELECT 1 FROM laps l WHERE l.driver_id = d.driver_id
        AND l.speed > (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team))

Both would work if the conjunct were classified local: it is then left in
`body.where`, the body is handed to `LogicalPlanBuilder::build`, and the body's
OWN lowering pass takes the inner node with its reference at level 1 — which is
exactly the route the same shape takes today under an UNCORRELATED `IN`
(`WEEK34_CORRELATED_SCALAR_VEC_ONLY` has three such entries and they pass).
The difference between "works" and "refused with the wrong reason" is only
whether the enclosing subquery happens to be correlated.

It is a refusal and not a wrong answer, so MEDIUM. **Coverage:** no suite holds
a correlated subquery nested inside a correlated body, in either the diffed or
the rejection direction — checked `WEEK33_DECORRELATED_VEC_ONLY`,
`WEEK33_CORRELATED_BINDS`, `WEEK34_CORRELATED_SCALAR_*`,
`WEEK35_SUBQUERY_IN_DERIVED_BODY_*`. The only nesting the suites hold is
uncorrelated-outer / correlated-inner.

STATUS: to be executed once the gate releases `build/`.

### B-4 — LOW. `development.md`'s "Relation slots and query levels" states a
refusal that was deleted in Week 33, and `main.cc` repeats it.

The section "**Unreachable with a correlated ref today (behind the refusal)**"
opens with two facts, of which the first is:

> 1. `Validator::validate` refuses any statement with
>    `has_correlated_subquery` ... A `ColumnRef` with `query_level > 0` exists
>    **only** inside a correlated subquery, so none reaches a plan.

That refusal is gone. `has_correlated_subquery` is **written and never read**
— grep over `src/` gives exactly three hits, all in `binder.cc` setting it and
`ast.h` declaring it. So the heading of that whole table ("Unreachable with a
correlated ref today") and its framing are false: every row under it is
reachable, and each row's real containment is a guard of its own.

The individual rows are still correct — I re-derived them:
`ChunkPruner`/`collectSimplePredicates` declines `query_level > 0`,
`buildAggregateSchema` throws, `restampSlots` is protected by `soleSlot`'s
`-1` rejection, and `AggregateSpec::relation_slot` is contained by
`refuseSurvivingCorrelatedRefs` (which walks the body's SELECT list) plus
`requireDecorrelatableBody`'s aggregate refusal. So this is a documentation
defect, not a live one. But it is the third time this section has been wrong,
and the brief's framing is exactly right: a reader who *uses* fact 1 concludes
that nothing further is needed.

`predicate_pushdown.cc:118-126` already flags this in-place ("is the refusal
Week 33 DELETED, so it justifies nothing now") — the source was corrected and
the map was not.

Same stale claim at `src/cli/main.cc:475-479`:

> materializeSubqueries TRUSTS three Validator rules — exactly one output
> column for SCALAR/IN, position restricted to WHERE/HAVING, and **no
> correlated subquery**

The pass has not trusted that since Week 33 — it routes off `sq->correlated`
itself (`subquery_materialization.cc:351`), and the comment there says so
explicitly. Harmless today; it is a third copy of a precondition that has
already caused three silent wrong answers when believed.

---

## Execution results (gate released `build/`; single short queries, no harness run)

All probes: `--catalog catalog.json --storage columnar --execution vectorized`.
SQLite side computed through the oracle's own `load_sqlite()`, so the data is
identical to the harness's.

**B-1 confirmed — WRONG ANSWER.**

```
SELECT * FROM drivers d WHERE d.age >
  (SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)

SwiftQL:  driver_id  name  nationality  team  age  $k0   COUNT(*)
          1  Driver_1  Spanish  RedBull  35   NULL  NULL
          ...  (7 columns)
SQLite:   ['driver_id','name','nationality','team','age']   (5 columns, 20 rows)
```

Same with a non-COUNT aggregate, where the synthetic columns carry real values
rather than NULLs:

```
SELECT * FROM laps l WHERE l.speed > 1.02 *
  (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) LIMIT 3

SwiftQL: ... season round  $k0         AVG(l2.speed)
         ... 2022   17     AlphaTauri  313.25153531218
SQLite:  9 columns, no $k0, no AVG column
```

And the derived-body symptom, exactly as predicted:

```
SELECT COUNT(*) AS n FROM (SELECT * FROM drivers d WHERE d.age >
  (SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)) AS x

SwiftQL: Error: internal: derived table 'x' was bound against a 5-column schema
         but planned to 7 columns (blockOutputSchema and
         LogicalPlanBuilder::build disagree)
SQLite:  20
```

**B-2 confirmed.**

```
(SELECT AVG(l2.speed) AS a  FROM laps l2 WHERE l2.team = l.team)   -> Error: column not found: 'AVG(l2.speed)'   (SQLite 4994)
(SELECT 1.02 * AVG(l2.speed) AS a FROM ...)                        -> 4037  = SQLite
(SELECT AVG(l2.speed) FROM ...)                                    -> 4994  = SQLite
```

The alias breaks the unwrapped form and not the wrapped one — the asymmetry the
README and the suite both claim does not exist.

**B-3 confirmed.** All three shapes refuse with the correlated-inequality
message; SQLite answers 20 for each.

```
EXISTS (... l.driver_id = d.driver_id AND EXISTS (SELECT 1 FROM laps l2 WHERE l2.lap_id = l.lap_id))
EXISTS (... l.driver_id = d.driver_id AND l.speed > (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team))
EXISTS (... l.driver_id = d.driver_id AND EXISTS (SELECT 1 FROM laps l2 WHERE l2.driver_id = d.driver_id))
  -> Error: correlated subquery: only an equality between two columns can become
     a join key (a correlated inequality has no equi-join to lower to; ...)
```

The same nesting under an UNCORRELATED `IN` runs and is right, which is the
proof that the shape is supportable and only the classification is wrong:

```
SELECT COUNT(*) FROM drivers d WHERE d.driver_id IN
  (SELECT l.driver_id FROM laps l WHERE EXISTS (SELECT 1 FROM laps l3 WHERE l3.lap_id = l.lap_id))
  -> 20 = SQLite
```

**A4 confirmed — `$kN` leaks into `--explain`, in all three plan sections:**

```
LogicalLeftJoin [driver_id = $k0 AND age = $k1]
  LogicalScan [drivers, 5 columns]
  LogicalDerived [$scalar0, 3 columns]
VecLeftHashJoin [driver_id = $k0 AND age = $k1] (materialize) build=derived ...
```

**A1 confirmed empirically.** Two correlated scalars whose aggregates share an
output name resolve correctly, optimized and `--no-optimize`:

```
SELECT COUNT(*) AS n FROM laps l
WHERE l.speed > (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)
  AND l.speed < 1.5 * (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.driver_id = l.driver_id)
  -> 4994 optimized, 4994 --no-optimize, 4994 SQLite
```

**Coverage-hole shapes are otherwise correct.** A correlated scalar and a
correlated EXISTS inside a derived body, with named select lists, both match
SQLite (4 rows / `{8,18}`). So the W34xW35 joint is sound; what it lacks is a
suite entry, and B-1 sits in the gap.

---

## Refusal boundary — enumerated from the code and each one executed

Everything `lowerInSubqueries` / `lowerExistsSubqueries` / `lowerCorrelatedScalars`
still refuses, with what actually fires:

| refusal (source) | fires? | pinned in a suite? |
|---|---|---|
| EXISTS body GROUP BY (`decorr.cc:22`) | yes | test_logical_plan.cc |
| EXISTS body HAVING (`:23`) | **NO — DEAD** (see B-5) | no |
| EXISTS body LIMIT (`:24`) | yes | no |
| EXISTS body DISTINCT (`:25`) | yes | no |
| EXISTS body aggregate (`:29`) | yes | test_logical_plan.cc |
| correlated non-equality conjunct (`:70`) | yes (**and over-fires — B-3**) | oracle x3 |
| non-ColumnRef side of a correlated equality (`:78`) | yes | no |
| both sides outer (`:91`) | yes | no |
| unresolved ref in a body conjunct (`:95`) | not constructed | no |
| correlated ref more than one level out (`:101`) | **NO — UNREACHABLE** (B-5) | no |
| correlated ref surviving in JOIN ON / SELECT / ORDER BY / WHERE (`:163`) | yes (SELECT, ORDER BY probed) | oracle (ON, SELECT) |
| two aggregates under one wrapper (`:249`) | yes | oracle |
| non-aggregate body / non-constant wrapper (`:268`) | yes | oracle x2 |
| scalar body LIMIT (`:295`) | yes | oracle |
| scalar body DISTINCT (`:296`) | yes | no |
| scalar body HAVING (`:297`) | yes (checked BEFORE group_by, unlike the EXISTS guard) | no |
| scalar body own GROUP BY (`:298`) | yes | no |
| scalar body select-list arity (`:301`) | **NO — DEAD**, Validator's arity rule wins (B-5) | no |
| shared body (`use_count > 1`, `:329`/`:570`) | yes (`BETWEEN` clones) | test_subquery.cc |
| no correlated equality at all (`:347`/`:584`) | yes | test_logical_plan.cc |
| `renamed.size() != keys.size()+1` (`:447`) | not constructible today | no |
| correlated IN / NOT IN (`:693`) | yes | oracle x3 |
| correlated subquery not a top-level conjunct (`:698`) | yes | oracle |
| IN computed operand (`lowering.cc:70`) | yes | no |
| IN not a top-level conjunct (`:142`) | yes | oracle |

Rejection-suite quality: the pinned substrings ARE specific (`"may hold ONE
aggregate"`, `"would have to ride as an ON residual"`, `"single aggregate"`),
not a shared tail — the Week-34 lesson held. One exception, below.

### B-5 — LOW. Three guards in this seam cannot fire.

1. **`requireDecorrelatableBody`'s HAVING refusal (`decorr.cc:23`) is dead.**
   The GROUP BY check is on the line above, so HAVING is only reached with an
   EMPTY `group_by` — which `Validator` has already refused with
   `HAVING requires GROUP BY`. Verified both ways:
   `EXISTS (... HAVING COUNT(*) > 0)` -> `HAVING requires GROUP BY`;
   `EXISTS (... GROUP BY l.team HAVING COUNT(*) > 0)` ->
   `a body with GROUP BY cannot be decorrelated`.
   Note the SCALAR guard orders the two the other way round (`:297` before
   `:298`), so ITS HAVING refusal is live — verified:
   `(SELECT AVG(...) ... GROUP BY l2.season HAVING AVG(...) > 0)` ->
   `a scalar body with HAVING cannot be decorrelated`. The two guards
   deliberately do not share code, and this is the visible consequence: one
   pair of clause checks is ordered so both fire and the other is not.

2. **`requireDecorrelatableScalarBody`'s arity refusal (`decorr.cc:301`) is
   dead.** `Validator::validate` runs first (`logical_plan.cc:899`) and refuses
   `select_star || select_list.size() != 1` for every non-EXISTS subquery
   (`validator.cc:560-566`). Verified: a two-column scalar body reports
   `scalar subquery must return exactly one column`, never
   `a correlated scalar subquery must select exactly one expression`.

3. **The correlation-depth refusal (`decorr.cc:101-103`) is unreachable, and
   B-3 is why.** To reach it, `splitCorrelation` must run on a body holding a
   level-2 reference. Every route is closed first:
   - nested inside a correlated body -> B-3 refuses at `:70`;
   - nested inside an uncorrelated `IN` body -> a level-2 ref makes the IN body
     correlated, so the IN node is correlated and `refuseUnloweredCorrelated`
     refuses (verified: `IN (SELECT l.driver_id FROM laps l WHERE EXISTS
     (SELECT 1 FROM laps l3 WHERE l3.driver_id = d.driver_id))` ->
     `a correlated IN / NOT IN is not lowered`);
   - inside a derived body -> `markCorrelated` marks the derived body's scope,
     so `Binder::relationSchema` refuses it as LATERAL (`binder.cc:196-200`);
   - inside a materialized uncorrelated body -> a level-2 ref makes that body
     correlated, so it is not materialized.

   **This is coupled to B-3 and must not be fixed independently.** Classifying
   a nested correlated subquery as body-local (the obvious B-3 fix) is what
   makes the depth guard reachable again — the inner body's level-2 refs are
   NOT decremented when the middle body is decorrelated, so the guard is the
   backstop that stops a stale level from reaching a plan. Same coupled-pair
   shape as F1/F2: relax one and the other becomes load-bearing.

### B-6 — LOW. One rejection pin does not discriminate which guard fired.

`WEEK34_CORRELATED_SCALAR_REFUSED`'s second entry pins the substring
`"LIMIT cannot be decorrelated"`, which is a suffix of **both**
`"a body with LIMIT cannot be decorrelated"` (EXISTS) and
`"a scalar body with LIMIT cannot be decorrelated"` (scalar). The entry is a
scalar query so it hits the right one today, but the assertion would pass if
the scalar guard were deleted and the EXISTS guard somehow reached. One
character (`"scalar body with LIMIT"`) fixes it. This is a single instance of
the shape the Week-34 sweep was built for, not a recurrence of the 40-entry
one.

### B-7 — LOW / coverage. No suite holds a CORRELATED subquery inside a derived
body.

`WEEK35_SUBQUERY_IN_DERIVED_BODY_VEC_ONLY`'s five entries are all
uncorrelated: a scalar in the body's WHERE, a scalar in its HAVING, an
uncorrelated EXISTS, the JOIN-position variant and the two-derived-relations
variant. `WEEK34_DERIVED_TABLE_VEC_ONLY` has a derived table inside a subquery
body, which is the other direction. So the W34xW35 joint — a correlated
subquery inside a derived body — is diffed nowhere. It works today (probed
above), and B-1's second symptom lives in exactly that gap.
