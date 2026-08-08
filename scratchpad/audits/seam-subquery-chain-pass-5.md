# Seam audit: subquery chain (weeks 31 -> 35), pass 5 — FINAL

Scope: W31 (materialize-then-substitute) / W32 (IN -> semi/anti) / W33 (EXISTS
decorrelation) / W34 (correlated scalar + derived RTEs) / W35 (subqueries inside
derived bodies).

Tree: `claude/phase5-week26-qomtkb` @ `b14d086`, the gated tree (910 unit / oracle
1722·0·0 / regression 340·0·0 / TPC-H 20-of-22 / three pin matrices 7·17·5).
`build/` verified up to date under `flock` before any probe.

All probes: `./build/swiftql --catalog catalog.json --no-cache --format tsv`,
four modes (`columnar/vectorized`, the same `--no-optimize`, `row/volcano`,
`columnar/volcano`), SQLite through the oracle's own
`load_from_catalog(CATALOG_PATH)` so the data is byte-identical to the harness's.

Status: **IN PROGRESS** (written incrementally; this line is the last thing to
change).

---

## B-1 — BLOCKER. The arming request crosses the cut through every expression
## slot except one: `divWalk` does not walk a `SUM`/`AVG` argument, so
## `HAVING SUM(int_col / (SELECT <mixed CASE>))` is a silent wrong answer.

Fix round 4 closed pass 4's B-1 by sending the arming *request* inward. The
request is computed by `divWalk` (`subquery_materialization.cc:253-345`), whose
own docstring says it "Mirrors taintWalk in vectorized_plan_builder.cc, one AST
level up". It does not mirror it at one node: the `AggregateExpr` arm.

```cpp
// subquery_materialization.cc:268-275   (divWalk)
if (auto* agg = dynamic_cast<const AggregateExpr*>(e)) {
    if (agg->function_name == "COUNT") { out.may_be_int = true; return out; }
    if (agg->function_name == "SUM" || agg->function_name == "AVG") return out;
    return divWalk(agg->argument.get(), rt, observed, catalog, depth);
}
```

`SUM` and `AVG` return **without walking `agg->argument`**. The early return is
right about the aggregate's own type (both emit a DOUBLE, so `may_be_int` is
correctly false) and wrong about `observed`, which is a pure side channel: every
other non-arithmetic arm of the same walk — `IsNullExpr`, `InExpr`, `LikeExpr`,
`SubstringExpr` — recurses *for the side effect alone* and then discards the
result. `SUM`/`AVG` are the only arms that drop the subtree.

The plan-level walk it mirrors has no such hole:
`collectIntOrigins`' `AGGREGATE` case calls
`taintWalk(spec.argument, cs, child, armed)` for **every** aggregate
(`vectorized_plan_builder.cc:426`), outside the `order_stat` test that follows.
So the identical arithmetic is armed in one plan and unarmed across the cut.

### The failing shape

```
SELECT l.team FROM laps l GROUP BY l.team
HAVING SUM(l.round / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END)
                      FROM laps l2)) > 5900
ORDER BY l.team

  col-vec               7 rows   AlphaTauri Alpine Ferrari McLaren Mercedes RedBull Williams   <-- WRONG
  col-vec --no-optimize 7 rows   identically wrong (both legs agree)
  row storage / Volcano 4 rows   Alpine Ferrari McLaren RedBull
  columnar / Volcano    4 rows   Alpine Ferrari McLaren RedBull
  SQLite                4 rows   Alpine Ferrari McLaren RedBull
```

`5900` is discriminating by construction, not by luck. Per team, `SUM(round/2)`
(INTEGER division, what Volcano and SQLite do) against `SUM(round/2.0)` (REAL,
what the vectorized path does after the body's `2` is flattened to `2.0`):

| team | INT-division sum | REAL-division sum |
|---|---|---|
| AlphaTauri | 5671 | 5926.0 |
| Alpine | 8958 | 9327.0 |
| Ferrari | 15268 | 15917.0 |
| McLaren | 8966 | 9325.0 |
| Mercedes | 5814 | 6054.0 |
| RedBull | 9398 | 9786.5 |
| Williams | 5782 | 6016.0 |

Three teams straddle 5900, which is exactly the 7-against-4 measured.

### `AVG`, and the other polarity of the `/`

```
HAVING AVG(l.round / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END)
                      FROM laps l2)) > 6.1
  col-vec / col-vec --no-optimize  6 rows      <-- WRONG
  both Volcano modes / SQLite      1 row (RedBull)

HAVING SUM((SELECT MAX(CASE WHEN l2.round > 10 THEN 7 ELSE 0.5 END) FROM laps l2)
           / l.round) > 1000
  col-vec / col-vec --no-optimize  7 rows      <-- WRONG
  both Volcano modes / SQLite      1 row (Ferrari)
```

The subquery is the *divisor* in the first two and the *dividend* in the third:
the arming request never leaves the outer AST in either polarity, because it is
the enclosing `SUM`/`AVG` that swallows it, not anything about the `/`.

### The three controls that make it the aggregate arm and nothing else

| control | change | result |
|---|---|---|
| `MAX(l.round / (SELECT <same body>)) > 5` | `divWalk`'s MIN/MAX arm **does** recurse | **refuses**, in both vectorized modes, with the round-3 message |
| `SUM(l.round / 2)` — same division, INT literal instead of the subquery | no cut to cross | all four modes and SQLite agree, 4 rows |
| `COUNT(*) > 7 / (SELECT <same body>)` — same HAVING, subquery **outside** the aggregate | `divWalk` reaches it | **refuses**, in both vectorized modes |

The first and third are the sharp ones: the rule exists, fires from the same
clause, on the same body, and is defeated by wrapping the division in `SUM`.

### The sharpest witness — the same shape in ONE plan refuses

```
SELECT SUM(t.x / 2) AS s
FROM (SELECT CASE WHEN lap_id = 2 THEN 2 ELSE 0.5 END AS x FROM laps) t

  col-vec / col-vec --no-optimize
    -> Error: vectorized execution cannot materialize the integer 2 into a
       DOUBLE result column that another expression divides. ...
  (AVG in place of SUM: the same refusal)
```

An aggregate argument holding a `/` over a mixed-`CASE` value is refused when
the value is produced inside the same build (`collectIntOrigins` walks
`spec.argument`) and answered wrongly when it is produced across the
materialization cut (`divWalk` does not). One walk mirrors the other everywhere
except here.

### Why no suite sees it

Same three structural reasons pass 4's B-1 had, unchanged by fix round 4:

* it is a wrong ANSWER, so only a `run_query_suite` (diffed) entry could hold it,
  and the round-4 cut-family entries added at `bb67beb` put the subquery under a
  bare `/` in `WHERE`/`HAVING`, never under an aggregate;
* both optimizer legs are wrong identically, so the `--no-optimize` invariant
  reports it clean;
* the four-mode census compares each mode against SQLite, never against another
  mode, and Volcano is right here.

### Minimum fix

One line, and it is the line the file's own convention already writes four times:

```cpp
if (agg->function_name == "SUM" || agg->function_name == "AVG") {
    divWalk(agg->argument.get(), rt, observed, catalog, depth);   // side effect only
    return out;                                                    // ... type still not INT
}
```

`COUNT` deserves the same treatment for uniformity, though I could not construct
an observable failure through it (see "probed and found correct" below). Nothing
about `may_be_int` changes; only `observed` gains the subtree. The
counter-probes above are the regression entries: the `SUM` query as a **diffed
four-mode** entry (it returns rows in every mode, so only a diff discriminates)
and the `MAX` query as the rejection twin that proves the arm still fires.

---

## B-2 — HIGH. A derived relation's output schema is computed at BIND time, so a
## subquery that contributes to it reaches the internal-defect guard.

`Binder::bind` builds a derived relation's range-table entry by calling
`blockOutputSchema(*ref.body(), catalog)` (`binder.cc:287`), which runs
`inferExprType` over the body's select list — and `inferExprType`'s
`SubqueryExpr` arm is the loud internal guard
(`logical_plan.cc:296-299`). `materializeSubqueries` runs 139 lines later in the
pipeline (`main.cc:411` binds, `main.cc:509` validates, `main.cc:550`
materializes). So any subquery whose value is needed to *type the derived
relation* is met by the guard before either the Validator's position rule or the
materialization pass can reach it.

Two reachable shapes, both in all four modes, both against a SQLite that answers:

```
-- (1) a subquery in a derived body's SELECT list
SELECT COUNT(*) AS n FROM (SELECT (SELECT COUNT(*) FROM drivers d) AS q FROM laps l) x

  all four modes -> Error: internal: a subquery reached type inference without
                    being materialized (materializeSubqueries must run before planning)
  SQLite         -> 10000

-- (2) a subquery inside an AGGREGATE ARGUMENT in a derived body's HAVING
SELECT COUNT(*) AS n FROM (SELECT l.team AS t FROM laps l GROUP BY l.team
  HAVING SUM(l.round + (SELECT COUNT(*) FROM drivers d)) > 0) x

  all four modes -> the same internal message
  SQLite         -> 7
```

Shape (2) also holds for `MAX`, `COUNT`, and for a subquery that *is* the whole
aggregate argument (`MAX((SELECT COUNT(*) FROM drivers d))`), and for a derived
relation reached through a `JOIN` rather than `FROM`. It is not a `has_subquery`
problem: adding a second subquery to the same body's `WHERE` (which certainly
sets the flag) does not change the outcome.

### The controls that localize it to "contributes to the derived schema"

| shape | result |
|---|---|
| derived body **WHERE** holds a scalar subquery | answers 1634 = SQLite |
| derived body **HAVING** holds a scalar subquery **outside** any aggregate | answers 7 = SQLite |
| derived body **ORDER BY** holds one | clean `ORDER BY: subqueries are supported in WHERE and HAVING only` |
| the SAME body's HAVING at **top level**, subquery inside the aggregate | **answers**, 7 teams = SQLite |
| the SAME select-list subquery at **top level** | clean `SELECT: subqueries are supported in WHERE and HAVING only` |

WHERE and a non-aggregate HAVING never enter `blockOutputSchema`; ORDER BY is
refused by a rule that runs on the body during `validateQuery`'s recursion, which
is reached because the *body's own* validation happens before the outer block
types it. The two failing shapes are exactly the two that must be typed to give
the derived relation a schema: the select list, and a HAVING aggregate that
`buildAggregateSchema` names as an output column.

### What this costs, separately for each shape

* Shape (2) is a **lost answer**: the identical query one nesting level out
  answers correctly in every mode. There is no working mode — the internal guard
  fires on the Volcano path too, before its derived-table capability refusal.
* Shape (1) is a **lost diagnostic**: the top-level form gets the position rule's
  own sentence; the derived form gets an internal-defect report for the same
  user error. `logical_plan.cc:279-284` states as fact that "the remaining
  positions — SELECT list, GROUP BY, ORDER BY, ON — hold no subquery at all …
  Validator's POSITION rule … refuse them outright", and calls that "verified
  rather than assumed" with a top-level witness. It is true at top level and
  false inside a derived body, and the guard's own reachability argument is what
  the witness above falsifies.

This is the same class the Week-35 comment in `materializeSubqueries`
(`subquery_materialization.cc:540-557`) records as fixed: "a scalar subquery
inside `FROM (SELECT ...)` reached type inference unsubstituted and tripped the
internal guard … an INTERNAL error surfaced to the user, on TPC-H Q22." That fix
moved the *materialization* walk into derived bodies. It did not move the
*typing* of a derived relation, which happens earlier and in another file.

### Minimum fix

Two independent halves, and they should not be conflated:

* Shape (1) is a validation-order problem: the Validator's position rule already
  refuses it, it just never runs first. The cheapest honest fix is for
  `blockOutputSchema`'s select-list walk to report the position rule's message
  rather than the internal one when it meets a `SubqueryExpr` — the rule exists,
  it is a `dynamic_cast` away, and no behaviour changes for any query that works
  today.
* Shape (2) is a real capability gap: a derived body's HAVING aggregate cannot be
  typed before its subquery is substituted. Refusing it by name ("a subquery
  inside an aggregate argument in a derived table's HAVING is not supported") is
  minimum code and turns an internal-defect report into a stated limit; making it
  answer means materializing a derived body's subqueries before the enclosing
  block types it, which is a pipeline-order change and larger than this seam.

Either way, both shapes need a rejection entry. Neither is in any suite today —
`WEEK35_SUBQUERY_IN_DERIVED_BODY_*` covers `WHERE` only.

---

## B-3 — HIGH. Decorrelation removes the guarding conjunct from the AND cascade,
## so a may-raise conjunct written after a correlated equality is evaluated on
## rows the written order excluded.

`expr_totality.h` states the rule this seam has to keep:

> a conjunct of a filter is evaluated on the rows for which every conjunct
> WRITTEN BEFORE IT evaluated TRUE … Nothing may change that set for an
> expression that CAN RAISE: not a plan rewrite, not a storage-level chunk skip,
> not an engine's choice of when to be lazy.

Decorrelation is a plan rewrite, and it changes that set. It lifts the correlated
equality out of the body's conjunct list and turns it into a join key; the
conjuncts written *after* it stay behind as the body's own filter, which runs on
the whole build side.

```
SELECT COUNT(*) AS n FROM laps l WHERE l.driver_id = 1
  AND EXISTS (SELECT 1 FROM drivers d
              WHERE d.driver_id = l.driver_id            -- guard, written FIRST
                AND SUBSTRING(d.name, d.age - 29, 1) = 'D')   -- raises when age < 30

  col-vec / col-vec --no-optimize -> Error: SUBSTRING: start position must be >= 1
  both Volcano modes              -> correlated-subquery capability refusal
  SQLite                          -> 0
```

`drivers` holds seven rows with `age < 30` (ids 4, 5, 10, 11, 15, 18, 20), for
which `d.age - 29 <= 0`. The guard `d.driver_id = l.driver_id` is false on all
seven for every outer row the query keeps, so the written order says the
`SUBSTRING` is never evaluated on them.

The discriminator is **SwiftQL against itself**, not SQLite — SQLite accepts
`SUBSTRING` start positions SwiftQL refuses by dialect, so it cannot adjudicate
the raise. The pair that does:

| the guard | result |
|---|---|
| `d.driver_id = l.driver_id` (correlated equality, lifted to a join key) | **raises** |
| `d.driver_id = 1` (body-local equality, stays a conjunct) | **answers 0** |

Same body, same relation, same raiser in the same position, differing only in
whether the guarding conjunct is the one decorrelation takes away. The
body-local form honours the cascade exactly as `expr_totality.h` says it must;
the correlated form does not.

All three correlated lowerings show it — `EXISTS`, `NOT EXISTS`, and a
correlated scalar (`(SELECT COUNT(*) … ) > 0`) — and the uncorrelated scalar
control with a body-local guard answers.

The observable is a loud error, never a wrong answer: the semi-join's key still
filters the result, so only the *set of rows an expression is evaluated on*
moves. But the failing query has no working mode (Volcano refuses correlated
subqueries by capability) and the shape is ordinary SQL.

### Minimum fix

The correlated equality is `firstMayRaise`'s problem the moment it is lifted.
Either (a) decorrelation declines to lift an equality that has a may-raise
conjunct written after it — `firstMayRaise` is already shared by three consumers
and this is a fourth caller, and the refusal message for it already exists in
the `correlated subquery: <what it declined>` family — or (b) the residue keeps
its position by riding as an `ON` residual above the key, which is precisely the
operator work Week 36 established and declined in the open (README's correlated
row). (a) is the minimum code; (b) is the answer.

---

## Part A — the round-4 fix, attacked slot by slot

### A1. Every expression slot `divWalk` can reach, and the one it drops

`forEachStatementExpr` (`subquery_materialization.cc:97-104`) covers `where`,
`having`, `select_list`, `order_by` and each join's `condition`; `group_by` is
walked separately at `:578-580` because its expression is a `shared_ptr`. That
is every slot a `SelectStatement` owns. Of those, only two can hold a subquery at
all, and I confirmed each by execution rather than by reading the Validator:

| slot | reachable? | measured |
|---|---|---|
| `WHERE` | yes | armed; refuses |
| `HAVING`, subquery outside an aggregate | yes | armed; refuses |
| `HAVING`, subquery **inside** an aggregate argument | yes | **not armed — B-1** |
| `SELECT` list | no | `SELECT: subqueries are supported in WHERE and HAVING only` |
| `ORDER BY` | no | `ORDER BY: subqueries are supported in WHERE and HAVING only` |
| join `ON` | no | `JOIN ON: subqueries are not supported in a join condition` |
| `GROUP BY` | no | unreachable (alias substitution only, and the select list refuses) |

So the slot enumeration is complete; the hole is one level down, inside the
`AggregateExpr` arm of the walk itself.

### A2. Nesting, multiplicity, and bodies inside bodies — all correct

Every one of these refuses in both vectorized modes, with the round-3 message,
and answers in Volcano where Volcano can run it:

* **two subqueries in one division** — `(SELECT <mixed 7>) / (SELECT <mixed 2>)`
  refuses; the mixed body that is *not* divided in the same `WHERE`
  (`(SELECT <mixed>) > 3 AND 7 / (SELECT COUNT(*)) > 0`) still answers 0 = SQLite,
  so the arming is per-body and not per-statement.
* **a body nested past 3 levels** — the division inside a level-2, level-3 and
  level-4 body all refuse (`materializeSubqueries` recurses through `runOnce`,
  and each level computes its own `divWalk` against its own range table). Level 4
  measured, not extrapolated.
* **a subquery inside a derived body** — refuses (the derived recursion at
  `:558-565` runs before the `has_subquery` fast path).
* **an IN body dividing by a materialized mixed scalar** — refuses, correctly:
  `d.age / (SELECT <mixed>)` over an INT column would otherwise be the same
  wrong answer.
* **a subquery whose value feeds another subquery** is not expressible: a
  subquery in a select list is refused, so a body's *value* can never be another
  body's operand. The reachable neighbour — an inner subquery in the outer
  body's `WHERE` — does not carry the outer body's value and correctly does not
  arm it.

### A3. The over-firing repairs hold, with one corner left open

* **`/` arms only when both operands can be INT.** `(SELECT <mixed 2>) / 2.0 > 0.9`
  answers 10000 in all four modes = SQLite. `t.s / (SELECT <mixed>)` where `t.s`
  is a REAL derived column answers 10000 = SQLite (that is 5a4b04a's fix). The
  INT twin `t.r / (SELECT <mixed>)` over `SELECT l.round AS r` refuses, so the
  test discriminates rather than merely permitting.
* **Origins drop at an arithmetic node that can no longer be INTEGER** — the
  `both` test in `divWalk`'s `+ - * /` arm and `taintWalk`'s. Verified through
  the REAL-partner probes above.
* **The magnitude split is real and correctly two-sided**, measured inside one
  plan: `CASE … THEN 2000000000000000 …` projected out refuses (RENDERED, 1e15);
  the same value only compared inside a derived body answers 10000 = SQLite
  (UNRENDERED, 2^53); `9007199254740993` refuses either way; `999999999999999`
  answers either way.
* **The corner (see B-6): a materialized body's value gets the RENDERED bound
  although its text is never printed.**

### A4. Siblings of the derived-column typing fix — one found, and it is B-4

5a4b04a taught `columnMayBeInt` to type a DERIVED relation's column so
`t.s / (SELECT <mixed>)` over a REAL body column stops being refused. I looked
for the shapes where that new answer is wrong in each direction.

* **Under-arming (would be a wrong answer): none found.** The derived arm answers
  "not INT" only for `SUM`/`AVG` (SwiftQL declares both DOUBLE —
  `logical_plan.cc:86` — so Volcano agrees), for `SUBSTRING` (STRING), for a
  REAL-declared catalog column, for a DOUBLE literal, and for arithmetic with a
  REAL operand. Each of those is REAL in Volcano too. The `COUNT` case correctly
  answers INT and refuses: `FROM (SELECT COUNT(*) AS c FROM laps) t` with
  `t.c / (SELECT <mixed>)` refuses.
  (`SUM(round)/7` diverges from SQLite — 17814.714 against 17814 — in **all four
  modes identically**, which is SwiftQL's own SUM-typing divergence and not this
  seam's; the derived arm is consistent with it.)
* **Over-arming (costs an answer): B-4 below.**

### A5. Conservative really does mean "refuses", never "answers wrongly"

The two conservative arms the fixer named:

* **A correlated body.** `divWalk` never adds a correlated node to `carried`, and
  inside a correlated body's own recursion an outer reference is unresolvable and
  answers INTEGER. Every shape I could construct that reaches that arm is refused
  — and the ones that would have exercised the bare-name fallback in
  `columnMayBeInt` (a body referencing an outer column under a `/`) are refused
  first, by the correlated-inequality rule or by the select-list position rule.
  The one correlated shape that *answers* is the one where the dividend is a REAL
  column of the body's OWN relation (`l2.speed / (SELECT <mixed>)` inside a
  correlated `EXISTS`): 10000 = SQLite, correctly not armed.
* **Depth.** Exhausting the budget returns `true` (INTEGER), which arms, which
  refuses. Measured at derived depths 2 and 3.

Both arms are refusals. I found no shape where the conservative path answers.

---

## B-4 — MEDIUM. The derived-column type question loses the catalog one level
## down, so `MAX_TYPE_DEPTH = 3` is really 1 — and the query it costs has no
## working mode.

`exprMayBeInt` (`subquery_materialization.cc:350-356`) calls `divWalk` with
`catalog = nullptr`:

```cpp
bool exprMayBeInt(const Expr* e, const RangeTable& rt, int depth) {
    std::unordered_set<const SelectStatement*> ignored;
    // The catalog reached this far through the RangeTable already; a body one
    // level down is resolved against `rt`'s own entries, so nothing more is
    // needed here than the depth budget.
    return divWalk(e, rt, ignored, nullptr, depth).may_be_int;
}
```

The comment is the defect. A body one level down is **not** resolved against
`rt`'s entries — it needs its OWN range table, and `derivedColumnMayBeInt` builds
that with `rangeTableOf(body, catalog)`. With `catalog == nullptr` that function
returns an **empty** range table at its first line, and `columnMayBeInt` answers
`true` on `rt.empty()`. So the second derived level always answers INTEGER
regardless of the budget.

```
-- depth 1: t.s is the REAL column laps.speed
SELECT COUNT(*) AS n FROM (SELECT l.speed AS s FROM laps l) t
WHERE t.s / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END) FROM laps l2) > 0
  col-vec 10000 = SQLite                                   <-- 5a4b04a's fix

-- depth 2: the SAME REAL column, one alias further out
SELECT COUNT(*) AS n
FROM (SELECT a.s AS s FROM (SELECT l.speed AS s FROM laps l) a) t
WHERE t.s / (SELECT MAX(CASE WHEN l2.round > 10 THEN 2 ELSE 0.5 END) FROM laps l2) > 0
  col-vec / col-vec --no-optimize -> refused (the type-through-division message)
  both Volcano modes              -> derived-table capability refusal
  SQLite                          -> 10000
```

Depth 3 behaves the same. **No mode answers it** — which is the exact criterion
fix round 4 used to rank E-14 worth fixing ("the derived-table form of that query
then had NO working mode, since Volcano refuses derived tables by capability").
The direction is safe (it refuses, it never answers wrongly), which is why this
is MEDIUM and not higher.

The header comment two paragraphs up names the remaining conservative cases as
"a correlated reference … and a derived relation nested more than
`MAX_TYPE_DEPTH` levels deep". The measured cutoff is **one** level, not three.

**Minimum fix:** give `exprMayBeInt` the catalog and thread it through — one
parameter, three call sites, and the `MAX_TYPE_DEPTH` budget then means what it
says. Delete the comment that says the catalog is not needed.

---

## B-5 — MEDIUM. The pin-discrimination gate now runs three matrices; this seam's
## four defective pins are in none of them, and are unchanged from pass 4.

Re-measured, by executing the harness's own `assert_refusal_pins_discriminate`
over the seam's rejection suites at HEAD.

**The gate.** `main()` calls it three times (`compare_against_sqlite.py:4255`,
`:4262`, `:4271`) on 7 / 17 / 5 pins, which matches the brief. Of the 17, **six**
are the round-4 cut family (`TYPEFIX_CUT_VECTORIZED_REFUSED` 5 +
`TYPEFIX_CUT_DERIVED_STILL_REFUSED` 1) and those belong to this seam — so the
seam's *newest* refusals are now covered, which is the part of pass 4's B-2 that
is genuinely addressed.

**What remains.** The seam's pre-existing rejection suites number **19**
(pass 4 counted 18; `WEEK34_DISTINCT_AGG_REFUSED` is the nineteenth) with **150**
pins. Exactly one of them — `B32_JOIN_KEY_TYPE_REJECTED`, 7 pins — is in a gate
matrix. Not even its own Volcano twin is. Running the function over all 19:

```
==== seam suites: 19   pins: 150   findings: 4
  WEEK33_NESTED_TRIPWIRE_REFUSED: pin 0 ('IN subquery') ALSO matches entry 1's message
  WEEK33_NESTED_TRIPWIRE_REFUSED: pin 1 ('HAVING') ALSO matches entry 0's message
  WEEK34_CORRELATED_SCALAR_REFUSED: pin 0 ('single aggregate') ALSO matches entry 4's
  WEEK34_CORRELATED_SCALAR_REFUSED: pin 4 ('wrapped in constant arithmetic') ALSO matches entry 0's
```

**Identical to pass 4's four.** Both `WEEK33_NESTED_TRIPWIRE_REFUSED` entries
still produce one message ("IN subquery: supported only as a whole top-level
WHERE conjunct (found one in HAVING)"), and `WEEK34_CORRELATED_SCALAR_REFUSED`
entries 0 and 4 still fire the same `refuse` call, so entry 4 still asserts
nothing.

**The pooled matrix**, normalised so the harness's own stderr noise does not
count as a message (150 entries, **29** distinct producer messages), reports four
needle clusters. Two are benign — one producer whose text varies only by the
column or function it names (`B32`'s `'IN / EXISTS subquery: cannot join a STRING
column with a numeric one'`, `WEEK34_DISTINCT_AGG_REFUSED`'s `'DISTINCT is
supported inside COUNT only'`). Two are real and both already known:

```
WEEK33_NESTED_TRIPWIRE_REFUSED  pin 'IN subquery'  -> 3 other producers
    IN subquery: supported only as a whole top-level WHERE conjunct (found one in a non-top-level position)
    IN subquery: the left operand must be a column reference (computed operands are not supported)
    NOT IN subquery: cannot join a STRING column with a numeric one (...)

WEEK35_SUBQUERY_IN_DERIVED_BODY_VOLCANO_REJECTED  pin 'not supported on the Volcano path' -> 3 other producers
    IN subqueries are lowered to a semi-join and are not supported on the Volcano path
    correlated subqueries are decorrelated to a semi-join and are not supported on the Volcano path
    multi-way joins are not supported on the Volcano path
```

**Minimum fix, unchanged from pass 4 and still one loop:** pass the seam's
rejection suites to the function `main()` already calls three times, then fix the
four it reports — the full parenthetical for both `WEEK33_NESTED_TRIPWIRE_REFUSED`
needles (its neighbour `WEEK32_LOWERING_REFUSED` already pins it that way), a
query in `WEEK34_CORRELATED_SCALAR_REFUSED` entry 4 that actually reaches the
non-constant-wrapper branch, and `VOLCANO_DERIVED` for the five `WEEK35` entries.

---

## B-6 — LOW. A materialized body's value is judged by the RENDERED (1e15) bound
## although its text is never printed.

Round 4 split the magnitude bound on a stated ground: "the origin sets of the
plan root's output columns are exactly the origins whose text can be printed;
anything else is `IntNarrowing::UNRENDERED` and judged on the value alone
(2^53)". For a subquery body that ground is false — the body's root output column
crosses the cut as a `Value` inside a `Literal` and is never rendered — so the
relaxation the split bought never applies to a materialized body.

```
-- 2e15: above 1e15, below 2^53; the value is only COMPARED, never printed
SELECT COUNT(*) AS n FROM laps l
WHERE (SELECT MAX(CASE WHEN l2.round > 10 THEN 2000000000000000 ELSE 0.5 END) FROM laps l2) > 0
  col-vec / col-vec --no-optimize -> Error: ... cannot materialize the integer
                                     2000000000000000 ... without changing it
  both Volcano modes / SQLite     -> 10000

-- the SAME value and the same "only compared" use, inside ONE plan
SELECT COUNT(*) AS n FROM
  (SELECT CASE WHEN l.lap_id = 2 THEN 2000000000000000 ELSE 0.5 END AS x FROM laps l) t
WHERE t.x > 0
  col-vec -> 10000 = SQLite        (UNRENDERED, 2^53)
```

Volcano still answers the first one, so it costs an answer rather than every
mode — **except** when the same body sits inside a derived body, where Volcano
refuses by capability and nothing answers:

```
SELECT COUNT(*) AS n FROM
  (SELECT l.lap_id AS k FROM laps l
   WHERE (SELECT MAX(CASE WHEN l2.round > 10 THEN 2000000000000000 ELSE 0.5 END)
          FROM laps l2) > 0) t
  all four modes refuse; SQLite -> 10000
```

The narrow fix is the mirror of B-1's: `materializeSubqueries` already computes
whether a body's value is *divided*; it could as cheaply tell the runner that the
value is *not printed*, and the runner's root columns would take `UNRENDERED`
instead of `RENDERED` unless the body's value reaches the outer output. LOW
because the band is 1e15 … 2^53 and the direction is a refusal.

---

## B-7 — LOW. Pass 3's and pass 4's LOWs are still open, re-confirmed at HEAD.

* **Pass 3 B-2 / pass 4 B-4** — `SuppressNestedCorrelation`'s comment
  (`subquery_decorrelation.cc:73-85`) still justifies its RAII with
  "forEachSubquery descends into nested BODIES", and `forEachSubquery`'s
  `SubqueryExpr` case (`subquery_materialization.cc:43-49`) still visits
  `sq->operand` and returns without touching `sq->subquery`. The construction is
  right; the stated reason is still wrong.
* **Pass 3 B-3** — README's correlated-refusal row still enumerates GROUP BY /
  HAVING / aggregate / LIMIT / DISTINCT / inequality / computed side / more than
  one block out / non-top-level EXISTS, and still does not name "a body whose
  only correlation runs through a nested subquery".
* **Pass 4 B-3** — the join-key refusal's recorded cost still reads as a corner
  case about a hand-written `'16'`; a `SUBSTRING`-derived string key is lost the
  same way.
* **Pass 4 B-4's NOT IN mixed-NULL coverage hole** — still unpinned, and the
  behaviour behind it re-verified correct at HEAD (see below).

---

## Part B — what four passes missed, and what they got right

### NULL semantics across IN / NOT IN / EXISTS / NOT EXISTS — correct

Synthesised with `1 / (x - x)`, which is NULL in both engines (SwiftQL does not
raise on integer division by zero; `WHERE 1 / (d.age - 30) > 0` answers 3 in every
mode = SQLite, so the NULL propagates rather than erroring).

| shape | SwiftQL (both vectorized legs) | SQLite | a wrong impl |
|---|---|---|---|
| `NOT IN` all-NULL body | 0 | 0 | 20 |
| `NOT IN` **mixed** body (some NULL) | 0 | 0 | 16 |
| `NOT IN` over an EMPTY body | 20 | 20 | 0 |
| NULL outer operand, `IN` / `NOT IN` | refused (`the left operand must be a column reference`) | 0 | — |
| NULL correlated key, `NOT EXISTS` | refused (`both sides of a correlated equality must be plain column references`) | 20 | — |

The two refusals are the position/shape rules doing their job: a computed operand
is exactly the case `JoinKey` cannot hold, and both refusals are pinned.

### Cardinality and the documented deliberate divergence — has NOT widened

* `l.speed > (SELECT l2.speed FROM laps l2)` — `scalar subquery returned more
  than one row` in **all four** modes; SQLite answers 1852. Unchanged.
* a one-row body answers 3410 = SQLite; a zero-row body is NULL and answers 0 =
  SQLite.
* a user `LIMIT 3` in a scalar body still raises (the cap is `min(user, 2)`), and
  `LIMIT 1` answers 1852 = SQLite — the one case `deterministicCut` covers.
* Zero rows is NULL, with discriminating predicates rather than `> 0`:
  `(SELECT COUNT(*) … empty) = 0` → 20; `(SELECT 1 + COUNT(*) … empty) = 1` → 20
  (the lifted wrapper); `(SELECT AVG(…) … empty) IS NULL` → 20. All = SQLite in
  all four modes.

### Correlation depth and sideways references — correct

`EXISTS (… EXISTS (… l.driver_id …))` two blocks out reports
`correlated subquery: a reference to a query block more than one level out cannot
be decorrelated here`, in both vectorized modes; both Volcano modes give the
capability refusal. A LATERAL sideways reference is refused by construction (the
body binds against the block's parent scope), unchanged.

### The refusal boundary — enumerated from the code and fired

Every refusal below was executed at HEAD, not read:

| refusal | fires |
|---|---|
| position: subquery in SELECT / ORDER BY / join `ON` | yes, three distinct messages |
| `IN` operand not a plain column | yes |
| `IN` not a whole top-level WHERE conjunct (incl. in HAVING) | yes |
| join-key type (`IN`/`NOT IN`/`EXISTS`/`NOT EXISTS`/correlated scalar) | yes, all five producers |
| cardinality (`> 1 row`) | yes, all four modes |
| correlated: inequality / computed equality side / >1 level out | yes |
| correlated: GROUP BY / HAVING / LIMIT / DISTINCT / aggregate body | yes (pinned suites re-run clean) |
| correlated scalar: select-list shape | yes |
| Volcano capability: derived / IN / correlated / multi-way | yes |
| INT-narrowing magnitude (1e15 rendered, 2^53 unrendered) | yes, both bounds, both sides |
| INT type-through-division across the cut | yes — **except under `SUM`/`AVG` (B-1)** |
| the internal materialization guard | fires where it should NOT (B-2) |

### Fix round 4's own repairs, re-verified

* The pass-4 B-1 witness family now refuses in both vectorized modes and answers
  identically where it should: `THEN 2.0` (no INT branch) 10000 = SQLite,
  `MIN` picking the REAL branch 10000 = SQLite, `(SELECT COUNT(*))` as divisor
  0 = SQLite. The value-driven half survived the fix.
* The correlated twin of the same query still refuses, and the derived-table twin
  still refuses — the three shapes pass 4 named are unchanged.

