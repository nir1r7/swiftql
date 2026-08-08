# Seam audit — engine divergence (Volcano vs vectorized), PASS 5 — FINAL

Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `b14d086`. Predecessors read in
full: `-pass-1.md` … `-pass-4.md`.

Gate state at start, reported by the orchestrator on this exact tree: 910 unit / 98 suites; oracle
1722 passed / 0 failed / 0 errors; regression 340/0/0; TPC-H `PASS (20/22 meaningful)`, baseline md5
unchanged; tracked fingerprint identical before and after; staleness disproved both ways (rebuilt
binaries bit-identical); three refusal-pin matrices clean (7/17/5 pins); rejection sweep 35 suites /
257 entries.

All runs below use the tree's own `build/swiftql`, in the **six meaningful mode combinations** —
`--storage row|columnar` x `--execution volcano|vectorized` x `--optimize|--no-optimize`, minus the
two `row`+`vectorized` cells the CLI refuses outright:

```
rv    row/volcano             rvn   row/volcano --no-optimize
cv    columnar/volcano        cvn   columnar/volcano --no-optimize
vec   columnar/vectorized     vecn  columnar/vectorized --no-optimize
```

Status: COMPLETE. Two findings are OPEN at the cap: E-19 (BLOCKER) and E-20 (HIGH).

---

## Part B — the seam taken fresh (written first, because it found the blocker)

### B-1 FINDING **E-19 (BLOCKER)** — a SILENT WRONG ANSWER, no error, no contrivance beyond one
### ordinary multiplication. `MAX`/`MIN` over a mixed-type `CASE` hands Volcano an **INT Value** and
### the vectorized engine a **DOUBLE cell**, and the first `+`, `-` or `*` above it carries that
### difference past the `%.15g` cliff, where the two engines print different numbers. SQLite
### adjudicates: **Volcano is right, the vectorized engine is wrong.**

Run at HEAD, shipped `catalog.json`, `laps` (10,000 rows). No derived table, no `--no-optimize`, no
`LIMIT`, no 15-digit literal — every constant is nine digits:

```sql
SELECT MAX(CASE WHEN lap_id = 1 THEN 123456789 ELSE 0.5 END) * 987654321 AS y FROM laps
```
```
row/volcano                y = 121932631112635269
row/volcano/noopt          y = 121932631112635269
columnar/volcano           y = 121932631112635269
columnar/volcano/noopt     y = 121932631112635269
columnar/vectorized        y = 1.21932631112635e+17     <-- WRONG
columnar/vectorized/noopt  y = 1.21932631112635e+17     <-- WRONG
```

**The oracle agrees with Volcano.** `sqlite3` on the same shape:
```
SELECT MAX(CASE WHEN lap_id = 1 THEN 123456789 ELSE 0.5 END) * 987654321  ->  121932631112635269
SELECT typeof(MAX(CASE WHEN lap_id = 1 THEN 123456789 ELSE 0.5 END))      ->  'integer'
```

**The control that isolates it to the materialization**, same product, no `CASE`:
```sql
SELECT 123456789 * 987654321 AS y FROM laps LIMIT 1
   all six modes: 121932631112635269
```
So the difference is not the arithmetic, not the printer and not the storage. It is one cell.

**The mechanism, both halves read rather than inferred.**

- Volcano: `HashAggregateNode`'s MIN/MAX keeps the argument `Value` verbatim (plan_nodes.cc:311),
  so `MAX` returns the **INT** `123456789`. `evaluate()`'s `*` then takes the INT/INT branch
  (evaluator.cc:140-148) and `checkedMul` produces the exact `121932631112635269`, which
  `Value::toString` prints in full because it is an INT.
- Vectorized: `VecHashAggregateNode::fillChunk` appends that same INT `Value` into a column whose
  type was declared **before the value existed** — `inferExprType` types a mixed `CASE` as DOUBLE —
  so `appendColumnValue` runs it through both refusals and **both pass**: `int_narrowing` is
  `RENDERED` (the column IS in the root's output sets) and `narrowToDoubleColumn` compares the
  **stored value** `123456789` against `1e15`. The cell becomes the double `123456789.0`. The
  projection above then does a DOUBLE `*`, giving `1.2193263111263526e17`, and `%.15g` prints
  fifteen significant digits.

**The false premise, in the code's own words** (`vectorized_plan_builder.cc:84-86`, the paragraph
titled *"Why `/` alone and not 'consumed by any expression'"*):

> `+ - *` on INT/INT give an INT whose `%.15g` rendering is identical to the DOUBLE (`x+1` is "8"
> either way), so `WHERE x+1 > 3` and `SELECT x*2` agree across engines.

That is a statement about the **operand**, and the rendering cliff applies to the **result**. A
single `*` moves a value from `1.2e8` to `1.2e17` — past `2^53`, where the double is no longer the
integer and `%.15g` is no longer the integer's text. `narrowToDoubleColumn` guards the operand and
nothing guards the result, because by then the vectorized value is a **genuine DOUBLE**, and both
refusals are `if (v.type() != TypeId::INT) return` by construction. `refuseObservableIntNarrowing`
arms `/` only, deliberately — so the one operator family that is armed is the one that CANNOT
produce this, and the three that are not armed all can.

**Three more routes, all run, all the same split (Volcano/SQLite exact, vectorized `%.15g`):**

| shape | Volcano (4 modes) | vectorized (2 modes) |
|---|---|---|
| `MAX(CASE WHEN lap_id=1 THEN 999999999999999 ELSE 0.5 END) + 9000000000000000` | `9999999999999999` | `1e+16` |
| `MAX(CASE WHEN lap_id=1 THEN 999999999999999 ELSE 0.5 END) - 9000000000000000` | `-8000000000000001` | `-8e+15` |
| `MIN(CASE WHEN lap_id=1 THEN 123456789 ELSE 999999999.5 END) * 987654321` | `121932631112635269` | `1.21932631112635e+17` |

**And a fourth face of the same defect, this one an ERROR/ANSWER split rather than a wrong answer** —
SwiftQL's INT arithmetic is overflow-checked and its DOUBLE arithmetic is not, so the same one-cell
type difference decides whether the query throws:

```sql
SELECT MAX(CASE WHEN lap_id = 1 THEN 7 ELSE 0.5 END) * 9223372036854775807 AS y FROM laps
   4 Volcano modes  Error: integer overflow in '*': the result does not fit in a 64-bit integer
   2 vec modes      y = 6.45636042579834e+19
```
It fires at magnitude **7** — the stored value is irrelevant, only the multiplier is — and with an
ordinary-looking multiplier too (`MAX(CASE … THEN 4000000000 ELSE 0.5 END) * 4000000000` -> Volcano
errors, vectorized answers `1.6e+19`). The `+` form errors the same way. Here SQLite sides with the
*vectorized* engine (`6.456360425798343e+19`, it promotes), so this face is a three-way split:
SwiftQL-Volcano throws, SwiftQL-vectorized and SQLite answer.

**This route is left open ON PURPOSE, and its stated compensating control does not compensate.**
The same comment continues:

> The one route left open on purpose is checked INT overflow: `x * <huge>` throws in Volcano and
> yields a double here. **That is a magnitude story like `narrowToDoubleColumn`'s**, not this type
> story, and arming for it would refuse `SELECT x*2`, which is right today.

`narrowToDoubleColumn` bounds the **stored** value. Overflow is a property of the **product**. The
two are independent: the wrong answer above stores `123456789` and the overflow above stores `7`,
both four to fourteen orders of magnitude below either bound. So the magnitude rule was never
capable of covering the route the type rule delegated to it, at any bound — which is why this is a
defect and not the accepted residue the sentence describes.

**Ranked BLOCKER**, on the criterion passes 2 and 3 set and pass 4 had to argue around: this one IS
a silent wrong answer. Both engines return a row, neither warns, the two rows differ, and the
declared oracle picks Volcano. It needs no derived table (which Volcano refuses), no `--no-optimize`,
no `LIMIT`, no plan-order accident and no near-`2^53` literal — the largest constant in the minimal
repro is `987654321`. It is reachable on the shipped catalog in the first shape anyone writes when
mixing an integer and a real branch under `MIN`/`MAX`.

**Corpus coverage: measured 0.** Harvested every `SELECT` string literal from
`compare_against_sqlite.py`, `test_new_queries.py` and `tpch_queries.py` and matched
`(MIN|MAX)\s*\(\s*CASE`: **9 literals, and not one of them has a `+`, `-` or `*` above the
aggregate.** All nine consume it through a comparison (`> 150`, `> 300`, `> 3`, `> 1`), through
`/ 2.0` (the arm that IS screened), or bare (`AS m`). The pinned `9223372036854775807` literals
(compare_against_sqlite.py:2481/2505, test_new_queries.py:1445) are all `lap_id * <huge>` inside a
`WHERE`, i.e. pass 4's ROW-SET class, which is a different mechanism reached through a different
node. `tests/test_int_double_materialization.cc` and
`tests/test_int_double_type_through_division.cc` pin the `/` arm and both magnitude bounds and
contain the string "overflow" zero times.

**A FIFTH face, and the one that leaks a C++ internal to the user.** `AND`/`OR` call `asInt()` on
each operand, and `asInt()` on a DOUBLE is `std::bad_variant_access`. `expr_totality.h:185-189`
documents that consumer exactly — *"asInt() on a non-INT operand raises std::bad_variant_access
(evaluator.cc:112-113, value.cc:28)"* — so the totality screen knows about it and the arming pass
does not:

```sql
SELECT MAX(CASE WHEN lap_id = 1 THEN 1 ELSE 0.5 END) AND 1 AS y FROM laps
   4 Volcano modes  y = 1                                    (SQLite: 1)
   2 vec modes      Error: std::get: wrong index for variant
```
The control that separates the ENGINE from the DIALECT: with the aggregate written bare in a
`HAVING` — where Volcano also holds a DOUBLE, because the value came from the storage and not from a
mixed `CASE` — all six modes give the same `std::get` error. It is only the materialization that
splits them. (That both engines surface a raw `std::bad_variant_access` text rather than a SwiftQL
diagnostic is a separate, smaller matter; it is recorded here because it is what the user of the
diverging query sees.)

**Every consumer, enumerated, so the finding is a class and not five queries.** The materialized cell
differs from Volcano's `Value` in exactly one bit — INT versus DOUBLE — and the dialect has five
places that read it:

| consumer | armed by `taintWalk`? | diverges? |
|---|---|---|
| `/` with an INT partner | **yes** (`OBSERVABLE`) | no — refused, loudly |
| `/` with a REAL partner | no, correctly | no — INT/REAL and REAL/REAL agree |
| `+ - *` result past `2^53` | no | **YES — silent wrong answer** |
| `+ - *` result past INT64 | no | **YES — Volcano throws, vectorized answers** |
| `AND` / `OR` (`asInt()`) | no | **YES — Volcano answers, vectorized `std::get` error** |
| `=`,`<`,`IN`,`ORDER BY`, group/DISTINCT keys | no, correctly | no — all coerce or normalize (re-run, clean) |

So the arming rule covers one of the three consumers that can see the difference, and the two it
misses are the two that were argued away in prose rather than measured.

**Measured as a class, not as five queries.** A generated matrix — 4 `CASE` shapes (mixed
INT/REAL, INT/INT, REAL/REAL, and a 15-digit mixed one) x 12 consumers (`bare`, `+1`, `-1`, `*3`,
`*987654321`, `/2.0`, `>1`, `=7`, `IN (7)`, unary `-`, `+9e15`, `*4e9`) x 2 shapes (scalar
aggregate and `GROUP BY`), plus 47 empty/degenerate queries and 5 `DISTINCT` ones — **147 queries,
12 divergent, and all 12 are this class.** The INT/INT and REAL/REAL `CASE` rows are clean in every
consumer, which is the control that the mixing is the cause.

The matrix also finds the **smallest possible instance — adding `1`:**
```sql
SELECT MAX(CASE WHEN lap_id = 1 THEN 999999999999999 ELSE 0.5 END) + 1 AS y FROM laps
   4 Volcano modes  y = 1000000000000000      (SQLite: 1000000000000000)
   2 vec modes      y = 1e+15
```
`999999999999999` is exactly one below `MAX_LOSSLESS_INT_IN_DOUBLE_COLUMN`, so it stores; `+ 1`
crosses the bound the store was checked against. **The three sub-classes have crisp boundaries**,
which is what makes this a rule and not an anecdote:

| result magnitude | what differs | example |
|---|---|---|
| `>= 1e15` (the `%.15g` cliff) | the **text** only — same number, different rendering | `999999999999999 + 1` -> `1000000000000000` vs `1e+15` |
| `> 2^53` | the **value** — the double is no longer the integer | `123456789 * 987654321` |
| `> INT64_MAX` | Volcano **throws**, vectorized answers | `7 * 9223372036854775807` |

**`--no-optimize` cannot turn this off, and that is structural.** `main.cc:574` gates exactly three
passes — `PredicatePushdown`, `JoinEnumeration`, `CardinalityEstimator`. `collectIntOrigins`, the
pass that decides the arming, runs inside `VectorizedPlanBuilder::build`, **outside** the gate. So
both vectorized legs give the identical wrong answer and `compare_against_sqlite.py`'s fourth mode
(`run_optimizer_invariant`, "optimized == `--no-optimize`") is blind to it by construction — the
same blindness pass 4 recorded for its own class, arriving by a different route.

**Amplified where no cross-engine check exists at all.** Inside a derived table the same defect runs
with Volcano refusing the shape by capability, so the only oracle is SQLite:
```sql
SELECT t.x + 1 AS y FROM (SELECT CASE WHEN lap_id = 1 THEN 999999999999999 ELSE 0.5 END AS x
                          FROM laps WHERE lap_id < 2) t
   4 Volcano modes  Error: derived tables … are not supported on the Volcano path
   2 vec modes      y = 1e+15          (SQLite: 1000000000000000)
```

### B-2 FINDING **E-20 (HIGH)** — the round-4 chunk-pruner rule is REOPENED by the bare-name
### fallback in `staticTypeOf`. A conjunct naming ANOTHER relation is screened against the SCANNING
### relation's schema, and when the two share a column name the screen types the wrong column,
### answers "cannot raise", and lets a later conjunct prune away the rows the raise was owed.

Fix round 4 added the chunk-pruner half of the rule and stated its own soundness argument
(`chunk_pruner.h`):

> The screen … answers "may raise" for a conjunct this scan's schema cannot type — **which is every
> conjunct naming another relation**, so an un-pushed WHERE handed to the FROM-side scan of a join
> stops contributing hints at its first cross-relation conjunct.

That is false whenever the other relation has a column of the same NAME. `staticTypeOf`'s
`ColumnRef` arm (`expr_totality.h:68-77`) resolves **slot-first with a bare-name fallback**: the
slot lookup misses (the conjunct's slot is not this scan's), the bare-name lookup then **hits the
scanning relation's own column of that name**, and the conjunct is typed against a column it does
not refer to.

**(i) A row-vs-columnar split, no derived table.** Two tables sharing a column name with different
types — `laps.season` INT and `alt.season` STRING (catalog at
`/tmp/seam5-e13-priv/cat2.json`; `alt` is `data/drivers.csv` with its fifth column declared STRING):

```sql
SELECT COUNT(*) AS n FROM laps l JOIN alt a ON l.driver_id = a.driver_id
WHERE a.season = 5 AND l.lap_id > 999999
```
```
row/volcano        Error: Type mismatch in Value comparison
row/volcano/noopt  Error: Type mismatch in Value comparison
columnar/volcano   n = 0            <-- the zone map skipped every chunk
columnar/volcano/noopt   n = 0
columnar/vectorized      n = 0
columnar/vectorized/noopt n = 0
```
The raising conjunct is written **FIRST**, which is exactly the case the rule's own proof calls
unsound (*"j < k — C_j IS evaluated on rows of that chunk, and skipping removes them. Unsound; this
walker stops before reaching C_k"*). It does not stop, because it believes `a.season` is
`laps.season`, an INT, compared against an INT.

**(ii) The SHIPPED catalog reaches it too, through a derived-table alias** — no custom schema, both
vectorized legs, and no control in the project can see it:
```sql
SELECT COUNT(*) AS n FROM laps l
JOIN (SELECT d.driver_id AS driver_id, d.name AS season FROM drivers d) x
  ON l.driver_id = x.driver_id
WHERE x.season = 5                            -> Error: Type mismatch in Value comparison
WHERE x.season = 5 AND l.lap_id > 999999      -> n = 0
WHERE l.lap_id > 999999 AND x.season = 5      -> n = 0
```
Adding a conjunct that can only **remove** rows turns the error into an answer, in **both**
orders — including the order the rule explicitly forbids. Volcano refuses derived tables, so there
is no cross-engine check; both vectorized legs agree, so the optimizer-invariant check is blind; and
SQLite answers `0` for all three (it is dynamically typed and never raises), so the SQLite oracle
flags the *control* rather than the defect.

**Why the shipped catalog alone does not reach the row-vs-columnar form**, checked rather than
assumed: `laps` and `drivers` collide on `driver_id` (INT/INT) and `team` (STRING/STRING) — same
types both times, so the mistyping is harmless. Every other cross-relation reference (`d.name`,
`d.age`, `d.nationality`, `l.speed`, `l.season`) is absent from the other schema, `staticTypeOf`
fails, and the screen correctly answers "may raise". The hole is precisely *a same-named column of a
different type*, which needs two tables written without a per-table column prefix — ordinary
practice, and the reason TPC-H (`l_`, `o_`, `c_`) never hits it.

**Ranked HIGH, not BLOCKER, and the reasoning is stated so it can be disagreed with.** Against
BLOCKER: no silent wrong answer — every leg either errors or returns the SQL-correct row count for
the rows that survive; and the cross-*engine* form needs a schema the shipped catalog does not have.
Against MEDIUM: it reopens, in the same release, the exact class the fix under audit was written to
close, with the same repro shape (`<raiser> AND <prunable>`) the fix's own comment uses; the
soundness argument that carries it is stated in the code and is false; and form (ii) is on shipped
data with **no** control able to detect it. It is a defect of the screen, not of the pruner: one
`staticTypeOf` that refuses the bare-name fallback when the `ColumnRef` carries a resolved non-local
slot would close both forms.

---

## Part A — verifying fix round 4

### A-0. **E-13 is CLOSED, in every one of its faces, and closed as a CLASS.** Re-run at HEAD.

Pass 4's own reproductions, all six modes each:

| pass-4 repro | pass 4 | HEAD |
|---|---|---|
| `WHERE age > 30 AND SUBSTRING(name, age-30, 3) = 'er_'` | Volcano **errors**, vec returns 3 rows | **all six: the 3 rows** |
| the same, conjuncts swapped | all four errored | **all six: error** (the defined order says so) |
| (a) `WHERE lap_id < 2 AND lap_id * 9223372036854775807 > 0` | Volcano errors, vec `lap_id 1` | **all six: `lap_id 1`** |
| (a) reversed | — | **all six: error** |
| (c) `WHERE age > 40 AND SUBSTRING(name, age-40, 2) = 'xx'` | 3 modes `0 rows`, row-volcano **errors** | **all six: `0 rows`** |
| pruner, raiser first: `WHERE lap_id * <huge> > 0 AND lap_id > 999999` | row errors / columnar `0 rows` | **all six: error** |
| pruner, prunable first | — | **all six: `0 rows`** |
| mech. 2 `SELECT SUBSTRING(name, age-34, 2) FROM drivers LIMIT 3` | Volcano `Dr/Dr/Dr`, vec **errors** | **all six: `Dr/Dr/Dr`** |

The cascade is not only the top-level `AND` spine. Re-run and identical in all six: `guard AND
(raiser OR const)`, `(guard AND raiser) OR const`, three conjuncts with the guard in the middle,
three with the raiser first, `HAVING guard AND raiser` and its reverse, and `LIMIT` combined with
`ORDER BY` (asc and desc), `DISTINCT`, `WHERE`, `LIMIT 0` and a `LIMIT` past the end of the table.

### A-1. **Every path that evaluates a filter, enumerated from the code, not sampled.**

`grep` for `evaluate(` across `src/execution` and `src/planner` returns eleven live call sites. Each
classified:

| site | role | engine twin | verdict |
|---|---|---|---|
| `plan_nodes.cc:142` `FilterNode` | WHERE | `vec_filter_node.cc:30` `evalPredicate` | **both cascade** |
| `plan_nodes.cc:429` `HavingNode` | HAVING | HAVING is a `LogicalFilter` -> the same `VecFilterNode` | **both cascade** |
| `plan_nodes.cc:742` / `vec_hash_join_node.cc:210` | outer-join `ON` residual | each other | **both eager, and equal** |
| `plan_nodes.cc:189` / `vec_project_node.cc:136` | projection | each other | eager both; `applyLimit` fixes the row set |
| `plan_nodes.cc:250,279` / `vec_hash_aggregate_node.cc:157,200` | group key, aggregate arg | each other | eager both |
| `columnar_eval.cc:123` | the vectorized scalar fallback | — | runs **inside** the cascade, over `input_sel` |
| `constant_folding.cc:91,110` | plan time, empty row | — | not per-row |

So the answer to the brief's question is **yes for filters and no for `ON` residuals — and the `ON`
residual is nevertheless CONSISTENT**, because both engines are eager there. An inner join's
residuals are folded into the `WHERE` conjunction in **the same position by both planners** —
residuals first, then the user's `WHERE` (`logical_plan.cc:1144-1146` and `planner.cc:173-176`) — so
the cascade sees the same written order on both legs. An outer join's residual stays on the join and
is evaluated eagerly on every key-matched pair in both. Run: `LEFT JOIN … ON k AND age > 30 AND
SUBSTRING(name, age-30, 3) = 'er_'` errors in all six, where the same two conjuncts in a `WHERE`
answer in all six. **That is a deviation from the stated rule** — `ON` conjuncts are "every other
expression", not "a conjunct of a filter" — but it is a deviation both engines make identically, so
it is a dialect fact, not a seam defect. It is worth one sentence in `expr_totality.h`, which
currently says only "a conjunct of a filter" and leaves a reader to work out that an `ON` clause is
not one.

Semi-join probe predicates and correlated bodies: **there is no Volcano twin to disagree with.**
`IN (SELECT …)`, `EXISTS` and correlated scalars are all refused on the Volcano path by capability.
Inside the vectorized body the cascade holds — `IN (SELECT driver_id FROM laps WHERE lap_id < 2 AND
lap_id * <huge> > 0)` answers `Driver_16`; with the conjuncts swapped it errors.

### A-2. **The `NULL AND FALSE` inexactness is UNOBSERVABLE, proved twice.**

*By construction.* `evaluatePredicate` has exactly two callers (`FilterNode`, `HavingNode`), both
`result == 1`. Order the answers `FALSE < UNKNOWN < TRUE`; the function over-approximates **upward**
(it returns UNKNOWN where 3VL says FALSE, never the reverse), and both connectives are monotone in
that order, so a sub-answer that is too high can never make the root TRUE where 3VL says it is not.
Cascading on TRUE (rather than on non-FALSE) is what makes the TRUE case exact, and TRUE is the only
case a caller reads.

*By measurement.* `evaluate()` is untouched and still exact where it is observable — NULLs
manufactured by integer division by zero:
```sql
SELECT (lap_id/0 = 1) AND (1 = 0) AS a,  (1 = 0) AND (lap_id/0 = 1) AS b,
       (lap_id/0 = 1) AND (1 = 1) AS c,  (lap_id/0 = 1) OR (1 = 1)  AS d
FROM laps LIMIT 1
   all six modes:  a = 0   b = 0   c = NULL   d = 1
```
`a` and `b` are the exact 3VL `FALSE`, not `NULL`, in both orders. The same two predicates as a
`WHERE` and as a `HAVING` return `0` rows in all six, which is the "both reject" the contract needs.

### A-3. **The three disputed pass-4 findings, adjudicated. The fixer is right on all three.**

**E-17 — WITHDRAWN.** Re-run: `SELECT SUBSTRING(d.name, d.age-34, 2) FROM drivers d JOIN laps l ON …
LIMIT 1` errors in **all six** modes; the join-free form answers `Dr` in all six. There is no engine
divergence, and under the rule the plan is honest: `deterministicCut`'s sort is a pipeline breaker,
the projection beneath it genuinely is evaluated on every row, and `applyLimit`'s comment says so in
advance rather than being caught saying it afterwards. Pass 4's framing — "adding a `JOIN` turns a
correct answer into an error" — describes a real capability change but not a seam defect, and SQL
gives `LIMIT` no power to bound evaluation. **My own pass-4 self was wrong to rank it MEDIUM.**

**E-13(b) — MISNAMED, and the fixer's correction is the better statement.** "The vectorized engine
disagrees with itself on a commutative operator" is not what was happening: the vectorized engine
consistently cascaded left-to-right, which is an evaluation *order*, not a disagreement. `AND` is
not commutative for error behaviour under any short-circuit order — C's `&&`, SQLite's, and now
SwiftQL's. The real defect was the one named in E-13's headline, the two **engines** disagreeing,
and it is fixed. I withdraw (b) as a separate defect; the two runs above (`p AND q` answers, `q AND
p` errors, both in all six modes) are now the *definition*, agreed on by every mode.

**E-14 — FIXED, both halves, and the fix is exact rather than blanket.** Verified at HEAD:

| pass-4 shape | pass 4 | HEAD |
|---|---|---|
| `MAX(CASE … 7 ELSE 0.5 END) / 2.0` | Volcano `3.5`, vec **refuses** | **all six `3.5`** |
| its derived-table form (no working mode in pass 4) | **no mode answered** | vec answers `0.25`, `3.5` |
| `MAX(CASE … 2000000000000000 ELSE 0.5 END) > 1` | Volcano `1`, vec **refuses** | **all six `1`** |
| control `MAX(CASE … 7 ELSE 0.5 END) / 2` | Volcano `3`, vec refuses | **still refuses** — correctly, `3` vs `3.5` |
| control `MAX(CASE … 2000000000000000 ELSE 0.5 END) AS m` (printed) | — | **still refuses** — correctly, `2000000000000000` vs `2e+15` |

The `UNRENDERED` relaxation was attacked rather than accepted, since an incomplete origin walk now
*weakens* a refusal instead of only under-arming the `/` rule. Two probes at values inside the newly
opened window `(1e15, 2^53]`, both reached by `taintWalk`'s `if (both)` arm dropping the origin at a
REAL partner: `MAX(CASE … 2000000000000000 …) + 0.5` -> all six `2e+15`; `MAX(CASE …
9007199254740992 …) + 0.0` -> all six `9.00719925474099e+15`. Sound, and for the right reason —
inside that window `(double)i == i` exactly, so Volcano's own INT->double conversion at the REAL
operand produces the identical bits. **E-19 is not a counter-example to this**: it needs the origin
to stay `RENDERED` and to cross the bound *after* the store, which no bound on the stored value can
see.

### A-4 FINDING **E-21 (LOW)** — the round-4 rule is a USER-VISIBLE dialect semantic and it is
### documented only inside two source headers. The two doc rows the fix did update state the
### *engineering* rule and not the consequence a user can trip over.

`WHERE p AND q` and `WHERE q AND p` are now different programs in SwiftQL: one can error where the
other returns rows. That is a deliberate, defensible choice — it is what C and SQLite do — but it is
a divergence from standard SQL, where `AND` has no specified evaluation order, and this project has
a table for exactly that kind of thing (README's *Syntax Deliberately Not Supported* and the
divergence rows beside it, which already carry finer points such as `LIKE` having no `ESCAPE` and a
multi-row scalar subquery being an error rather than SQLite's first row).

`grep` over `README.md` and `development.md` for "written before it", "evaluation order" and
"commutative" (the two "not commutative" hits are about join enumeration and outer joins): **the
rule appears nowhere.** Round 4 did edit README:782 and development.md:442, and both edits state the
*implementation* rule — *"any construct whose two implementations disagree about which
sub-expressions get evaluated decides whether a query errors"* — with a pointer to
`parser/expr_totality.h`. Neither says the sentence a user needs: *a conjunct is evaluated only on
the rows every conjunct written before it kept, so reordering your `WHERE` can change whether the
query errors.*

Same class as pass 4's E-16 and pass 3's E-12, one step out: not a comment that fails a check, but a
rule that never left the source tree. Ranked LOW — no query returns a wrong answer because of it,
and the rule itself is right.

---

## What came back CLEAN — results, not gaps

- **E-13 is closed in all five of its faces** (the `AND` cascade in both orders, the compiled-kernel
  overflow variant, the chunk-pruning variant, the `LIMIT`-over-projection variant, and the pruner's
  written-order rule in both conjunct orders) — see A-0's table. Four passes have now each closed
  the previous pass's blockers; this is the first round to close one as a *definition* obeyed by
  four independent mechanisms (two engines, the optimizer, the pruner) rather than as a patch.
- **92 NULL-bearing queries, 0 divergent.** NULLs manufactured the only two ways the loader allows —
  LEFT-JOIN null-extension against a never-matching `ON`, and integer division by zero — across 20
  expression shapes, 19 predicate shapes and 11 aggregates, each in `SELECT`, `DISTINCT`, `WHERE`,
  `GROUP BY` and `HAVING` position. Includes `NOT IN`, `NOT LIKE`, `BETWEEN`, `IS [NOT] NULL`,
  `COUNT(col)` vs `COUNT(*)` over an all-NULL column, and `MIN`/`MAX`/`SUM`/`AVG` over one.
- **47 empty and degenerate queries, 0 divergent**: every aggregate over a zero-row input (scalar
  and grouped), `HAVING` over no groups, `DISTINCT` over nothing, `SELECT *` over nothing,
  `ORDER BY … LIMIT` over nothing, `LIMIT 0` with and without a matching `WHERE`, an inner join with
  an empty result, a LEFT join whose right side never matches, and a join under `WHERE 1 = 0`.
- **`DISTINCT` is clean** on doubles, on multi-column keys, on a mixed-type `CASE` key, and over an
  aggregate — 5 queries plus the `DISTINCT` variant of every expression in the NULL battery.
- **The `applyLimit` rewrite is behaviour-preserving on every shape it can reach**: `ORDER BY` asc
  and desc, `DISTINCT`, `WHERE`, `LIMIT 0`, and a `LIMIT` larger than the table — all six modes
  identical on each, and the `LIMIT 0` case now costs nothing in either engine.
- **The type-observable consumers that are NOT armed and are nevertheless safe** were measured, not
  assumed: `=`, `<`, `IN`, `ORDER BY`, group keys and `DISTINCT` keys over a mixed-`CASE` aggregate
  are identical in all six modes (`key_encoding.h`'s `keyFieldText` normalizes an integral double
  back to INT text, and `Value`'s comparisons coerce). The `INT/INT` and `REAL/REAL` `CASE` rows of
  the 147-query matrix are clean under all twelve consumers.
- **`SUBSTRING` with a computed start is screened, not argued about**: `exprMayRaise` answers TRUE
  for any non-`Literal` start, so no mis-resolution can make one look total — the one path E-20's
  bare-name fallback cannot reach.
- **`evaluate()`'s eager `AND` is still exactly three-valued** where a `SELECT` list observes it
  (A-2), and the two callers of the new `evaluatePredicate` cannot see its one inexactness.

---

## SUMMARY

```
BLOCKER   1   E-19
HIGH      1   E-20
MEDIUM    0
LOW       1   E-21

WITHDRAWN (pass 4's, adjudicated in the fixer's favour)   E-17, E-13(b)
CLOSED    (pass 4's, verified at HEAD)                    E-13 (all faces), E-14
```

| # | Rank | Finding | Concrete failing shape? |
|---|---|---|---|
| **E-19** | **BLOCKER** | `MIN`/`MAX` over a type-mixed `CASE` hands Volcano an **INT `Value`** and the vectorized engine a **DOUBLE cell**. Both narrowing refusals inspect the **stored** value; the first `+`, `-` or `*` above the aggregate carries the result past `1e15` (text differs), past `2^53` (value differs) or past `INT64_MAX` (Volcano throws, vectorized answers), and `AND`/`OR`'s `asInt()` on the DOUBLE raises a raw `std::bad_variant_access`. `taintWalk` arms `/` only — the one operator family that cannot produce any of it. | Yes — run, and adjudicated by SQLite. `SELECT MAX(CASE WHEN lap_id=1 THEN 123456789 ELSE 0.5 END) * 987654321 FROM laps`: Volcano and SQLite `121932631112635269`, vectorized `1.21932631112635e+17` — **a silent wrong answer, no error, on the shipped catalog**. Minimal form: `… THEN 999999999999999 …) + 1` -> `1000000000000000` vs `1e+15`. Measured as a class: 147-query matrix, 12 divergent, all this class, with `INT/INT` and `REAL/REAL` `CASE` rows clean. |
| **E-20** | HIGH | The round-4 chunk-pruner rule is reopened by `staticTypeOf`'s **bare-name fallback**: a conjunct naming another relation is typed against the *scanning* relation's same-named column, answers "cannot raise", and lets a later conjunct prune away the rows the raise was owed. The pruner's own soundness sentence — *"which is every conjunct naming another relation"* — is what is false. | Yes — run, both forms. Custom two-table catalog with a name collision: `WHERE a.season = 5 AND l.lap_id > 999999` -> row storage **errors**, all four columnar modes `n = 0`, with the raiser written FIRST. Shipped catalog via a derived-table alias: `WHERE x.season = 5` errors, `… AND l.lap_id > 999999` answers `0` — **in both conjunct orders**, in the only engine that can run the shape. |
| **E-21** | LOW | The rule that decides whether a legal query errors — *a conjunct is evaluated only on the rows every conjunct written before it kept*, hence `p AND q` may error where `q AND p` does not — exists only in `parser/expr_totality.h` and `evaluator.cc`. The two doc rows round 4 edited state the engineering rule and not the user-visible consequence, and the project has a divergence table for exactly this. | No — documentation, checked by `grep` over README.md and development.md for "written before it" / "evaluation order" / "commutative": zero hits on the rule. |

### Verdict

**Fix round 4 is the strongest of the four, and the seam is still not clean.**

What round 4 did, verified rather than accepted: it took pass 4's E-13 — a defect the previous three
rounds would have patched at one call site — and answered it with a **definition**, then made four
independent mechanisms obey it (`evaluatePredicate` on the Volcano side, `evalPredicate` on the
vectorized side, `PredicatePushdown`'s frozen suffix, `ChunkPruner`'s stop-at-first-raiser), with one
shared screen. Every face of E-13 is closed, in both conjunct orders, including the two the fixer
had to find for itself (the pruner and the `LIMIT`-over-projection form). The one deliberate
inexactness it left is provably unobservable, by monotonicity and by measurement. Two of the three
findings it disputed were right to dispute and I withdraw them, one of them my own pass-4 self's.

What it did not reach is one AST node deeper than where it looked. The rule it wrote governs **which
rows** an expression is evaluated on. **E-19 is about which TYPE the value has when it gets there** —
a mixed-`CASE` aggregate is an INT in one engine and a double in the other, and the guard that exists
for exactly that (`narrowToDoubleColumn`) checks the value at the moment it is *stored* while the
divergence appears at the moment it is *used*. Everything about it is one step past a boundary
someone drew carefully: the arming covers `/`, the three operators it skips are the ones that carry
the value across the bound; the magnitude rule covers the operand, the cliff applies to the result;
the comment that delegates the overflow route to "a magnitude story like `narrowToDoubleColumn`'s"
delegates it to a rule that cannot see it. And it is a **silent wrong answer** with SQLite as the
adjudicator, which is the bar passes 2 and 3 set for BLOCKER and which pass 4 had to argue around.

**E-20 is the same shape of miss in the fix's own new code**: the screen it built is precise, and
the one place it is *im*precise — a `ColumnRef` whose slot does not belong to the schema it is being
typed against — is the exact input the chunk pruner feeds it. Two seams reached that bare-name
fallback independently this round.

The gate being green is consistent with all of this and is not evidence against it, measured rather
than assumed for the third pass running: of the corpus's `MIN`/`MAX`-over-`CASE` literals, **nine
exist and not one has a `+`, `-` or `*` above it**; the arming pass runs **outside** the
`--no-optimize` gate, so the optimizer-invariant mode is blind to E-19 by construction; and E-20's
shipped-catalog form runs only where Volcano refuses the shape, so no cross-engine control exists to
see it either.

**Ending with an open blocker is the honest outcome, and the audit ends here at the cap.** Pass 5
found what the four before it did because the seam keeps producing the same kind of defect: a rule
stated as a property of the construct it was discovered on rather than of the property that makes it
true. Round 4 fixed that for evaluation *order* and left it standing for evaluation *type*. The next
round has a one-line direction for each: arm `+ - *` and `AND`/`OR` the way `/` is armed (accepting
that it refuses `SELECT x*2` on a mixed-`CASE` column, which is the cost the comment already prices),
or move the test to the arithmetic site where the result is known; and make `staticTypeOf` decline
the bare-name fallback when the `ColumnRef` carries a resolved slot the schema does not hold.

**This pass ends the audit of this seam, with E-19 and E-20 recorded as OPEN.**
