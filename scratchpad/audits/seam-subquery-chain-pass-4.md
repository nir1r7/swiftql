# Seam audit: subquery chain (weeks 31 -> 35), pass 4

Scope: W31 (materialize-then-substitute) / W32 (IN -> semi/anti) / W33 (EXISTS
decorrelation) / W34 (correlated scalar + derived RTEs) / W35 (subqueries inside
derived bodies).

Tree: `claude/phase5-week26-qomtkb` @ `b2bc70e` (code identical to the gated
`9da0494`). Pass 3 came back 0/0/0/4-LOW; this pass re-checks fix round 3's three
incursions into this seam, and hunts what four passes have not reached.

All probes: `./build/swiftql --catalog catalog.json --no-cache`, four modes
(`col-vec`, `col-vec --no-optimize`, `row storage/Volcano`,
`columnar storage/Volcano`), SQLite through the oracle's own
`load_from_catalog(CATALOG_PATH)` so the data is byte-identical to the harness's.

Status: **COMPLETE.**

---

## B-1 — BLOCKER. The type-through-division refusal does not cross the
## materialization cut, and a scalar subquery turns it into a silent wrong answer.

```
SELECT COUNT(*) AS n FROM laps l
WHERE 7 / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END) FROM laps l2) > 3

  col-vec               10000        <-- WRONG
  col-vec --no-optimize 10000        <-- WRONG, identically (both legs agree)
  row storage / Volcano     0
  columnar / Volcano        0
  SQLite                    0
```

The same defect with no aggregate at all, through the PROJECT site:

```
SELECT COUNT(*) AS n FROM laps l
WHERE 7 / (SELECT CASE WHEN l2.lap_id = 2 THEN 2 ELSE 0.5 END
           FROM laps l2 WHERE l2.lap_id = 2) > 3

  col-vec / col-vec --no-optimize    10000     <-- WRONG
  both Volcano modes / SQLite            0
```

The other polarity of the `/`, and a second magnitude, both wrong the same way:

```
SELECT COUNT(*) AS n FROM laps l
WHERE (SELECT MAX(CASE WHEN l2.round > 10 THEN 7 ELSE 0.5 END) FROM laps l2) / 2 > 3
  col-vec 10000   Volcano 0   SQLite 0

SELECT COUNT(*) AS n FROM laps l
WHERE (SELECT MAX(CASE WHEN l2.round > 10 THEN 999999999999999 ELSE 0.5 END)
       FROM laps l2) / 2 > 499999999999999
  col-vec 10000   Volcano 0   SQLite 0
```

And **inside a derived body**, where Volcano cannot even adjudicate:

```
SELECT COUNT(*) AS n FROM
  (SELECT l.lap_id AS k FROM laps l
   WHERE 7 / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END)
              FROM laps l2) > 3) t
  col-vec 10000   SQLite 0   (Volcano: derived-table capability refusal)
```

### The three controls that make it the stored-type story and nothing else

| control | change | result |
|---|---|---|
| `THEN 2.0` instead of `THEN 2` | no INT branch, no type to lose | **all four modes and SQLite: 10000** |
| `MIN(...)` instead of `MAX(...)` | the order statistic picks the REAL branch, so no INT ever arrives | **all four modes and SQLite: 10000** |
| `(SELECT COUNT(*) ...)` as the divisor | INT-declared column, nothing narrows | **all four modes and SQLite: 0** |

The middle one is the analogue of `TYPEFIX_DIV_GUARDS_VEC_ONLY`'s "armed shape
where no INT ever arrives" guard: the value-driven half of the rule survives the
cut correctly. Only the arming half leaks.

### The sharpest witness: one conjunct flips a refusal into a wrong answer

```
-- UNCORRELATED body: materialized, two plans
SELECT COUNT(*) FROM laps l
WHERE 7 / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END)
           FROM laps l2) > 3
  -> col-vec: 10000            SQLite: 0        WRONG

-- add `AND l2.driver_id = l.driver_id`: CORRELATED, decorrelated into the SAME plan
SELECT COUNT(*) FROM laps l
WHERE 7 / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END)
           FROM laps l2 WHERE l2.driver_id = l.driver_id) > 3
  -> Error: vectorized execution cannot materialize the integer 2 into a DOUBLE
     result column that another expression divides. ...
```

Same CASE, same `7 / x > 3`, same engine. One correlated conjunct is the whole
difference between a correct refusal and 10000 rows.

And the identical arithmetic written as a **derived table** — which is one plan —
is refused today by the round-3 fix:

```
SELECT COUNT(*) FROM (SELECT CASE WHEN lap_id = 2 THEN 2 ELSE 0.5 END AS x
                      FROM laps WHERE lap_id = 2) t
WHERE 7 / t.x > 3
  -> Error: vectorized execution cannot materialize the integer 2 ... that
     another expression divides.
```

So the rule exists, is armed correctly for a derived table, for a correlated
body, and for a decorrelated `$scalarN` relation — the three shapes the brief
named. It is the **fourth** shape, the materialization cut, that it cannot see.

### Why

`collectIntOrigins` runs **once per `VectorizedPlanBuilder::build`**
(`src/planner/vectorized_plan_builder.cc:1093-1094`), and `materializeSubqueries`
cuts the query into **two independent builds**:

1. `runOnce` moves the body out and calls the runner
   (`src/planner/subquery_materialization.cc:206`), which is `runVectorizedToRows`
   (`src/cli/main.cc:129-143`) — its own `LogicalPlanBuilder::build`, its own
   `VectorizedPlanBuilder::build`, its own `collectIntOrigins`. Inside **that**
   plan there is no `/`, so no origin is armed, so `appendColumnValue` narrows
   the INT into the DOUBLE column silently. That is correct for that plan: it is
   exactly what `TYPEFIX_DIV_GUARDS_ALL_MODES`' third entry pins ("the aggregate
   PRODUCED but not DIVIDED" must answer).
2. The value crosses the cut as a `Value` inside a `Literal`
   (`buildReplacement`, `subquery_materialization.cc:230-243`). It has **already
   been flattened to DOUBLE** by step 1. The outer plan's `collectIntOrigins`
   sees a `Literal` whose `value.type()` is DOUBLE and correctly concludes there
   is nothing to arm — `taintWalk` bottoms out at "Literal, and anything with no
   column underneath it" (`vectorized_plan_builder.cc:222`).

Neither walk is wrong on its own. The taint would have to travel from a node in
plan A to a `/` in plan B, and no walk spans both: `IntObservableMap` is keyed by
`const LogicalPlanNode*` of a tree that has been destroyed by the time the outer
tree exists.

`foldConstants` (called at the end of `materializeSubqueries`) then folds
`7 / 2.0` to `3.5` on the vectorized leg and `7 / 2` to `3` on Volcano, so the
divergence is baked into the outer AST before either engine plans — which is why
**both optimizer legs agree** and why the `--no-optimize` invariant reports it
clean.

The **magnitude** half of the family does NOT leak, and that is worth stating
because it is what makes this specifically a taint-walk boundary and not a
general materialization hole: `narrowToDoubleColumn`'s 1e15 bound is
value-driven and unconditional, so it fires inside the body's own run —

```
... > (SELECT MAX(CASE WHEN l2.round > 10 THEN 9007199254740993 ELSE 0.5 END) FROM laps l2)
  -> Error: ... cannot materialize the integer 9007199254740993 into a DOUBLE
     result column without changing it
```

— while the same shape at `999999999999999` (below the bound) answers in all
four modes. Only the armed TYPE rule needs a walk, and only the walk stops at the
cut.

### Why no suite sees it

* **The diffed suites cannot hold it structurally.** It is a wrong ANSWER, so it
  would have to be a `run_query_suite` entry. No entry in
  `compare_against_sqlite.py` puts a mixed-type `CASE` inside a subquery body.
  The whole `TYPEFIX_DIV_*` family (11 queries) is single-plan: three derived
  levels, a FILTER, a HAVING, a GROUP BY, and not one subquery. `FROM (SELECT`
  is the same plan; `(SELECT` in a predicate is a different one, and that is the
  boundary the family never crosses — its own selector
  (`if "FROM (SELECT" in q`) says so.
* **The two-leg optimizer invariant cannot see it**: both legs are wrong
  identically. (Pass 2 already recorded that this seam gets nothing from that
  check — all three lowerings and materialization run in both legs. This is the
  second wrong answer to hide behind it.)
* **The four-mode census cannot see it either**: Volcano is *right* here, and the
  modes are each compared against SQLite, never against one another.

### Minimum fix

`buildReplacement`'s SCALAR arm already holds both halves of the discrepancy:
`res.rows[0][0]` (the Value, correct) and `res.schema.column(0).type` (the
declared type, DOUBLE). Two options:

* **Preferred — preserve the Value's own type.** The Value out of `drainVec` has
  already been coerced to DOUBLE by the chunk; the fix is to stop the body's own
  materialization from flattening a value that is about to leave the plan
  anyway. The narrowest form: arm every column of a body run by the SUBQUERY
  RUNNER, i.e. treat the runner's root as an observer. That makes the shape
  *refuse* rather than answer wrongly, with the message that already exists, and
  it is one call site.
* **Cheaper and answer-preserving, but a second rule**: have `runVectorizedToRows`
  return the pre-narrowing `Value` for a single-cell scalar result. Then
  materialized scalars agree with Volcano by construction rather than by a guard.

I would not fix it at `appendColumnValue`: the narrowing is correct there for
every non-escaping column, and widening it re-refuses queries
`TYPEFIX_DIV_GUARDS_*` pins as answerable.

A regression entry costs one line, and it must be a **four-mode diffed** entry
(not a rejection entry): the query above returns rows in every mode, so only a
diff against SQLite discriminates.

---

## Part A — fix round 3's three incursions

### A1. The `LIMIT` cut is now plan-independent in every shape I can construct,
### and the "left order-variant" ground for the injected caps IS true.

`deterministicCut` (`logical_plan.cc:995-1005`) is applied on **any**
`stmt.limit`, so the caps `runOnce` injects (`subquery_materialization.cc:197-201`)
get it too — they are not exempted. Verified by execution rather than by reading:

| shape | col-vec | col-vec --no-opt | row/Volcano | col/Volcano | SQLite |
|---|---|---|---|---|---|
| scalar body over a JOIN, user `LIMIT 1`, no ORDER BY | 9998 | 9998 | 9998 | 9998 | 9985 |
| same, `ORDER BY l.driver_id LIMIT 1` (a TIED declared key) | 9985 | 9985 | 9985 | 9985 | 9985 |
| injected EXISTS cap over a JOIN | 20 | 20 | 20 | 20 | 20 |
| injected NOT EXISTS cap over a JOIN | 20 | 20 | 20 | 20 | 20 |
| injected SCALAR cap 2 over a JOIN returning one row | 1852 | 1852 | 1852 | 1852 | 1852 |
| user `LIMIT 3` in a scalar body | error: scalar subquery returned more than one row | | | | (SQLite 9985) |

The 9998/9985 row is the documented tie-at-a-cut divergence, not a defect: the
project chose `optimized == --no-optimize` over SQLite agreement and says so
(`deterministicCut`'s "WHAT THIS DELIBERATELY CHANGES", and
`run_engine_agreement_suite`'s "SQLite cannot adjudicate a tie at a LIMIT cut").
All four SwiftQL modes agree, which is the property that was bought.

**The ground is true, and here is why rather than that it is asserted.** For the
injected caps the observable is not the row:

* EXISTS, cap 1 — the observable is `!res.rows.empty()`, i.e. `n > 0`, a function
  of the row COUNT only. `min(user_limit, 1)` preserves emptiness for any user
  limit including 0 (`LIMIT 0` -> empty here and empty in SQLite).
* SCALAR, cap 2 — the observable is: `n >= 2` throws, `n == 1` yields the one row
  (unique, so order-free), `n == 0` yields NULL. Order-invariant in all three
  branches. Two rows prove "more than one" whichever two they are.

The one case where the ground genuinely fails is a **user-written `LIMIT 1` in a
scalar body**, where `min(1, 2) = 1` and one row is chosen out of many. That is
the case `deterministicCut` covers, and it is covered soundly because
`rowLess`'s fall-through to `compareCanonical` runs **even when declared ORDER BY
keys tie** (`sort_comparator.h:198-212`) — so `SORT` really is a total order and
`orderIsPlanStable(SORT) == true` is honest, not an approximation. The ORDER-BY-
with-ties row above is the discriminating probe for that: had the tie-break not
run under a declared key, `ORDER BY l.driver_id LIMIT 1` over a join would have
been plan-dependent and the four modes would not all read 9985.

**No route past the cut found.** `orderIsPlanStable` returns `false` for `JOIN`
and for any node type not in the switch, and recurses through `DERIVED`,
`FILTER`, `AGGREGATE`, `PROJECT`, `DISTINCT`, `LIMIT`. A semi/anti join is a
`LogicalJoin` and is therefore unstable-by-default rather than argued about,
which is the right default for this seam (an IN or EXISTS lowering under a LIMIT
gets the cut).

### A2. The join-key type refusal: all four producers behave identically, and the
### positional/by-name split resolves the pair the probe actually compares.

The rule lives once, on the finished tree (`Validator::validateJoinKeyTypes`,
`validator.cc:185-213`), and resolves **exactly** as the physical builder does:
`li` by `(from_col, from_slot)` — the same expression as `leftKeyIndices`
(`vectorized_plan_builder.cc:457-458`) — and `ri` positionally for
`semantics != STANDARD`, by name otherwise, which is `rightKeyIndices`
(`:485-510`) character for character. The physical builder additionally asserts
its lowered inputs have the same width as their logical schemas
(`:846-851`), so the two cannot silently diverge.

Executed across all four producers, including the multi-key shapes no suite
holds:

| producer | probe | fired |
|---|---|---|
| `lowerInSubqueries` (SEMI) | `l.team IN (SELECT d.driver_id ...)` | `IN / EXISTS subquery: ...` |
| `lowerInSubqueries` (ANTI_NOT_IN) | `l.team NOT IN (...)` | `NOT IN subquery: ...` |
| `lowerExistsSubqueries` (SEMI) | `EXISTS (... d.driver_id = l.team)` | `IN / EXISTS subquery: ...` |
| `lowerExistsSubqueries` (ANTI) | `NOT EXISTS (...)` | `NOT EXISTS subquery: ...` |
| correlated-scalar rewrite (STANDARD) | `(SELECT COUNT(*) ... d.driver_id = l.team) > 0` | `join key: ... and the subquery's key column` |
| **multi-key EXISTS, bad key SECOND** | `d.driver_id = l.driver_id AND d.age = l.team` | names **`'team'` and `'age'`** |
| **multi-key EXISTS, bad key FIRST** | `d.age = l.team AND d.driver_id = l.driver_id` | names **`'team'` and `'age'`** |
| **multi-key correlated scalar** | `d.driver_id = l.driver_id AND d.age = l.team` | names **`'team'`** and the subquery's key column |
| multi-key, both well typed | `d.driver_id = l.driver_id AND d.team = l.team` | **answers 10000 = SQLite** |

The two multi-key rows are the ones that would expose a positional-resolution
error: if `ri = k` paired the wrong body column, the message would name the wrong
column pair or the refusal would be missed. It names the right pair from either
position, and the well-typed twin still answers — so the check is exact and not
merely conservative.

**INT vs DOUBLE keys are correctly NOT refused**, and that is a real property
rather than an omission: `keyFieldText` routes an integral double through the
integer path, so `7.0` and the INT `7` produce the same text and still join
(`key_encoding.h:66-77`). `TypeId` has exactly three members, so `l_str == r_str`
partitions the space with nothing left over. Verified:
`l.driver_id IN (SELECT CASE WHEN d.age > 30 THEN d.driver_id ELSE 0.5 END ...)`
answers 5591 = SQLite — a mixed INT/DOUBLE body key that matches correctly.

### A3. The taint walk DOES handle the three shapes the brief named. It is the
### fourth — the materialization cut — that it cannot reach. That is B-1.

* **A correlated body / a decorrelated `$scalarN` relation** — one plan. The
  `$scalarN` `LogicalDerived` is `children[1]` of a STANDARD LEFT `LogicalJoin`,
  so `collectIntOrigins`' JOIN case takes the `width == left+right` branch and
  the derived body's origins reach the outer predicate's `taintWalk`. **Refuses
  correctly** (message quoted above).
* **A subquery inside a derived body** — when the offending expression and the
  `/` are both inside the derived body, one plan again, and it answers correctly
  where it should: `FROM (SELECT ... WHERE 7 / (CASE WHEN round>10 THEN 2 ELSE
  0.5 END) > 3) t` gives 4185 = SQLite, because a CASE evaluated inline in a
  predicate is never materialized into a DOUBLE column. When the subquery inside
  the derived body is the materialized one, the cut reappears — see B-1's fourth
  probe.
* **An IN body** — the mixed CASE as the semi-join's build key is correct and must
  not be armed, because `key_encoding.h` normalizes it; measured 5591 = SQLite.
  `collectIntOrigins`' JOIN case discards the right side's origin sets for a
  SEMI/ANTI width, which is the right call.

### A4. `assert_refusal_pins_discriminate` does NOT cover this seam. See B-2.

---

## B-2 — MEDIUM. The pin-discrimination gate covers one of this seam's eighteen
## refusal suites, and running it over the other seventeen reports four findings
## on a tree the gate calls green.

`main()` calls `assert_refusal_pins_discriminate` exactly twice
(`compare_against_sqlite.py:3902` and `:3909`): once on
`B32_JOIN_KEY_TYPE_REJECTED` and once on the pooled INT-narrowing family. Of this
seam's rejection suites it therefore covers **`B32_JOIN_KEY_TYPE_REJECTED` and
nothing else** — not even its own Volcano twin. `sweep_rejection_suites` is not a
substitute: its behavioural leg asks only "does this query still error with this
substring in SOME mode", which is the exact question a rotted pin answers yes to.

Executed. The harness's own function, over the seam's eighteen suites, 146 pins:

```
WEEK33_NESTED_TRIPWIRE_REFUSED: pin 0 ('IN subquery') ALSO matches entry 1's
  message — it cannot tell the two producers apart
WEEK33_NESTED_TRIPWIRE_REFUSED: pin 1 ('HAVING') ALSO matches entry 0's message
WEEK34_CORRELATED_SCALAR_REFUSED: pin 0 ('single aggregate') ALSO matches entry 4's
WEEK34_CORRELATED_SCALAR_REFUSED: pin 4 ('wrapped in constant arithmetic') ALSO
  matches entry 0's message
==== seam total findings: 4
```

The two clusters, with the actual messages:

**`WEEK33_NESTED_TRIPWIRE_REFUSED` — both entries produce ONE message.**

```
[0] pin 'IN subquery'  ->  IN subquery: supported only as a whole top-level
                           WHERE conjunct (found one in HAVING)
[1] pin 'HAVING'       ->  IN subquery: supported only as a whole top-level
                           WHERE conjunct (found one in HAVING)
```

Pass 3 reported this by reconstruction; it is now reproduced by execution, and
the check that would have caught it exists and is not pointed at it. Twelve lines
above the suite, `WEEK32_LOWERING_REFUSED` pins the FULL parenthetical
(`'whole top-level WHERE conjunct (found one in HAVING)'`) and its comment
explains why — the same file states the rule this suite breaks.

**`WEEK34_CORRELATED_SCALAR_REFUSED` entries 0 and 4 fire the SAME guard — NEW,
pass 3 did not have this one.**

```
[0] "... > (SELECT l2.speed FROM laps l2 WHERE l2.team = l.team)"
    pin 'single aggregate'
[4] "... > (SELECT CASE WHEN AVG(l2.speed) > 1 THEN 1 ELSE 0 END
             FROM laps l2 WHERE l2.team = l.team)"
    pin 'wrapped in constant arithmetic'

both -> correlated subquery: a correlated scalar subquery is decorrelated only
        when its select list is a single aggregate, optionally wrapped in
        constant arithmetic (TPC-H Q17's `0.2 * AVG(...)`); ...
```

Entry 4 was written to witness the *non-constant wrapper* branch and witnesses
the same `refuse` call as entry 0. It is a vacuous entry: delete it and nothing
about the engine stops being asserted. Both its pin and entry 0's are satisfied
by the other's message, so neither can tell which of the two shapes broke.

**The pooled (cross-suite) matrix, which is the shape the E-10 rot actually
took.** The function exempts equal pins, so a family whose entries all share one
weak needle passes per-suite. Pooling the seam's 146 messages finds the case pass
3 named and this run confirms by execution:

```
WEEK35_SUBQUERY_IN_DERIVED_BODY_VOLCANO_REJECTED: pin 'not supported on the
  Volcano path'
  own:  derived tables (FROM (subquery)) are not supported on the Volcano path
  ALSO: multi-way joins are not supported on the Volcano path
  ALSO: IN subqueries are lowered to a semi-join and are not supported on ...
  ALSO: correlated subqueries are decorrelated to a semi-join and are not ...
```

Four distinct producers behind one needle, across seven other suites. Its
neighbour twenty lines away (`WEEK34_DERIVED_TABLE_VOLCANO_REJECTED`) picks its
needle per query by guard-ordering rule, and `B32_JOIN_KEY_TYPE_VOLCANO_REJECTED`
does the same by classification function — three standards in one file.

Also, honestly: the pooled matrix produces **false positives** that a fix should
not chase. `WEEK36_CORRELATED_RESIDUAL_REFUSED`'s pin matching
`WEEK34_CORRELATED_SCALAR_REFUSED`'s message, and
`WEEK34_DERIVED_TABLE_VOLCANO_REJECTED`'s matching `WEEK35`'s, are the SAME
producer reached from two suites — which is the case the per-suite function
exempts by pin equality and the pooled one cannot. Ten pooled findings; three
clusters are real.

**Minimum fix, no new machinery.** Pass the seam's rejection suites to the
function `main()` already calls — one loop over a list of `(suite, extra_args)`
pairs, beside the two calls that exist. Then fix the four it reports: two needles
in `WEEK33_NESTED_TRIPWIRE_REFUSED` (the full parenthetical, as its neighbour
already uses), one query in `WEEK34_CORRELATED_SCALAR_REFUSED` entry 4 that
actually reaches the non-constant-wrapper branch (a wrapper naming an OUTER
column does; a `CASE` does not), and `VOLCANO_DERIVED` for the five
`WEEK35_..._VOLCANO_REJECTED` entries.

The gate's commit message says "sixteen pins, each matched against its own
producer and no other, zero findings". That is true of sixteen pins. There are
146 in this seam, four of them defective, and the check that proves the property
is one argument away from covering them.

---

## B-3 — LOW. The join-key refusal's named loss is wider than `'16'`: an ordinary
## string-encoded key is refused too.

`b091fef` named the cost as a literal: `l.driver_id IN (SELECT '16' ...)`
returned 495 matching SQLite and now errors. The class is wider than a
hand-written literal, because any STRING expression whose text is a canonical
integer rendering agreed with the INT before and is refused now:

```
SELECT COUNT(*) FROM laps l
WHERE l.driver_id IN (SELECT SUBSTRING(d.name, 8, 2) FROM drivers d)

  SQLite: 10000
  SwiftQL: IN / EXISTS subquery: cannot join a STRING column with a numeric one
           ('driver_id' and 'SUBSTRING(d.name, 8, 2)')
```

`drivers.name` is `Driver_1 … Driver_20`, so the extracted keys are `'1' … '20'`
— every one of them the INT's canonical rendering, and `keyFieldText` is identity
on a STRING and decimal on an INT, so all twenty matched by the encoding's own
rule. (I did not run a pre-fix binary; both binaries in the tree are post-fix.
The claim is derived from `key_encoding.h:66-77`, and SQLite's 10000 is measured.)

This is not a new defect — the refusal is deliberate, uniform, and the trade was
made knowingly. It is a **documentation** item: "a string-encoded key column
joined to an integer one" is a real schema pattern, and the recorded loss reads
as a corner case about a literal. One clause in the message's own README entry
would fix it. No suite change needed; the shape is already covered by the
producer's existing pin.

---

## B-4 — LOW. Pass 3's four LOWs are all still open, and re-confirmed at HEAD.

* **Pass 3 B-1** (weak rejection pins) — superseded and extended by B-2 above,
  now executed rather than reconstructed, with one new instance.
* **Pass 3 B-2** (`SuppressNestedCorrelation`'s comment justifies its RAII with
  two facts that are both false) — unchanged at `subquery_decorrelation.cc:76-83`.
  Re-confirmed: `forEachSubquery`'s `SubqueryExpr` case still visits `sq->operand`
  and returns without touching `sq->subquery`
  (`subquery_materialization.cc:43-49`), and `correlated` is still a field of
  `SubqueryExpr` (`ast.h:196`), not of the shared `SelectStatement`. The
  construction remains right; the stated reason remains wrong.
* **Pass 3 B-3** (B-3's fix opened a refusal class in no README table and no
  suite) — unchanged. `README.md:108` still enumerates the refused correlated
  shapes without "a body whose only correlation runs through a nested subquery",
  and neither shape is pinned.
* **Pass 3 B-4** (`NOT IN` with a MIXED NULL body has no oracle entry) —
  unchanged, and the behaviour behind it re-verified correct at HEAD by both NULL
  routes, with the counterfactual that discriminates:

| shape | SwiftQL (both legs) | SQLite | a wrong impl |
|---|---|---|---|
| `NOT IN` all-NULL body (`x/(s-s)`) | 0 | 0 | 20 |
| `NOT IN` **mixed** body (`x/(x-5)`) | 0 | 0 | 16 |
| `NOT IN` over an EMPTY body | 20 | 20 | 0 |
| NULL outer, non-empty body, `NOT IN` | 0 | 0 | 20 |
| NULL outer, EMPTY body, `NOT IN` | 20 | 20 | 0 |
| NULL outer, `NOT EXISTS` (two-valued: KEEP) | 20 | 20 | 0 |

---

## What else was probed and found correct

Recorded so a fifth pass does not re-derive it.

* **Cardinality — the documented divergence has not widened.**
  `l.speed > (SELECT l2.speed FROM laps l2)` still raises `scalar subquery
  returned more than one row` where SQLite answers 1852; a one-row body answers
  3410 = SQLite; a zero-row body is NULL and answers 0 = SQLite. A user `LIMIT 3`
  in a scalar body raises the same cardinality error rather than silently taking
  a row (SQLite takes the first) — the cap is `min(user, 2)`, so the throw
  survives a user limit above 2 and is bypassed only at `LIMIT 1`, which is the
  order-determined case A1 covers.
* **Zero rows is NULL, with discriminating predicates rather than `> 0`:**
  `(SELECT COUNT(*) ... empty group) = 0` -> 20; `(SELECT 1 + COUNT(*) ... ) = 1`
  -> 20 (the lifted-wrapper CASE — substituting 0 for the whole wrapper would
  answer 0 rows); `(SELECT AVG(...) ...) IS NULL` -> 20. All = SQLite.
* **The mixed-CASE type story inside a predicate is correct and must stay
  unarmed.** `WHERE 7 / (CASE WHEN round > 10 THEN 2 ELSE 0.5 END) > 3` answers
  4185 = SQLite inside a derived body, 20 = SQLite inside an IN body and inside a
  correlated EXISTS body. Nothing is materialized, so nothing narrows. A fix for
  B-1 that widened the rule to "any mixed CASE reaching a `/`" would break all
  three.
* **`res.schema` types survive the cut correctly for NULL**: a zero-row scalar
  over a DOUBLE-declared mixed CASE is NULL in every mode (`... IS NULL` -> 10000
  = SQLite), so `lit->null_type` is not part of B-1.
* **STRING crossing the cut into a numeric comparison** refuses in every mode
  (`Type mismatch in Value comparison`) where SQLite answers 0 — a pre-existing
  recorded dialect divergence, not this seam's.
* **Checked INT overflow crossing the cut**:
  `(SELECT COUNT(*) ...) * 9223372036854775807 > 0` raises the checked-arith error
  in all four modes where SQLite answers 10000 — again a recorded divergence
  (`SwiftQL does not promote to DOUBLE`), and it is uniform across the cut.
* **The two dead guards** pass 3 logged (B-5.1 EXISTS-body HAVING, B-5.2 scalar
  select-list arity) are still dead, and the three `use_count() > 1` guards still
  have exactly one reachable member. Nothing moved.

---

## Summary

| severity | count | findings |
|---|---|---|
| **BLOCKER** | 1 | B-1 |
| **HIGH** | 0 | — |
| **MEDIUM** | 1 | B-2 |
| **LOW** | 2 | B-3, B-4 |

* **B-1 (BLOCKER)** — the type-through-division refusal is armed by a walk that
  runs once per plan, and `materializeSubqueries` cuts the query into two plans.
  A mixed-type `CASE` in a scalar subquery body is flattened to DOUBLE by the
  body's own run and crosses the cut as a DOUBLE `Literal`, so a `/` in the outer
  query silently gets REAL division: **10000 rows where SQLite and both Volcano
  modes return 0**. Reachable through both materializing sites (PROJECT and
  AGGREGATE), both polarities of the `/`, and inside a derived body. Adding one
  correlated conjunct to the same query turns it into a correct refusal, which is
  the sharpest witness that the rule exists and this shape evades it. Both
  optimizer legs are wrong identically, so the `--no-optimize` invariant reports
  it clean; no diffed entry in the harness puts a mixed `CASE` inside a subquery
  body, so nothing can see it. The magnitude half of the same family does NOT
  leak, which localizes the defect precisely to the arming walk.
* **B-2 (MEDIUM)** — `assert_refusal_pins_discriminate`, the check written
  because a pin rotted silently, is pointed at two families and covers exactly
  one of this seam's eighteen refusal suites. Running the harness's own function
  over the other seventeen reports **four findings**, executed: both
  `WEEK33_NESTED_TRIPWIRE_REFUSED` entries produce one message and neither pin
  discriminates, and `WEEK34_CORRELATED_SCALAR_REFUSED` entries 0 and 4 fire the
  same `refuse` call so entry 4 asserts nothing (new this pass). Pooling the
  seam's 146 messages adds the `WEEK35` needle that four distinct producers
  satisfy. One loop beside the two existing calls closes the coverage.
* **B-3 (LOW)** — the join-key refusal's recorded cost reads as a corner case
  about a hand-written `'16'`; an ordinary `SUBSTRING`-derived string key
  (`'1' … '20'` against `driver_id`) is lost the same way and SQLite answers
  10000. Documentation, not behaviour.
* **B-4 (LOW)** — pass 3's four LOWs are all still open and were re-confirmed at
  HEAD, including the `NOT IN` mixed-NULL-body coverage hole (behaviour behind it
  correct in six discriminating shapes).

**Verdict: the seam is NOT clean.** One blocker — a silent wrong answer produced
by the vectorized path where both Volcano modes and SQLite agree on the right
one, in exactly the class fix round 3 shipped a refusal for, escaping through the
one boundary that refusal's walk cannot cross. The lowering machinery itself
continues to hold: routing, NULL rules, cardinality, correlation depth, the
`LIMIT` cut's plan-independence and all four join-key producers were probed again
and every one is correct. What failed is the same thing that failed in pass 2 —
a value crossing a boundary that each side of the boundary reasons about
correctly on its own.
