# Seam audit — engine divergence (Volcano vs vectorized), PASS 4

Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `b2bc70e` (code identical to
the gated `9da0494`). Predecessors read in full: `-pass-1.md`, `-pass-2.md`, `-pass-3.md`.

Gate state at start, reported by the orchestrator on this exact tree: 874 unit / 97 suites; oracle
1602 passed / 0 failed / 0 errors; regression 335/0/0; TPC-H `PASS (20/22 meaningful)`, baseline md5
unchanged; tracked fingerprint identical before and after; staleness disproved both ways.

All runs below use the tree's own `build/swiftql` (the gate proved it is the product of these
sources — the rebuilt binaries came out bit-identical), four modes: `rv` = row/volcano,
`cv` = columnar/volcano, `vec` = columnar/vectorized optimized, `vnp` = columnar/vectorized
`--no-optimize`.

Status: IN PROGRESS — appended as each item is confirmed.

---

## Part A — verifying fix round 3

### A-0. Both of pass 3's blockers are CLOSED at HEAD. Verified by re-running pass 3's own repros.

**E-8** (`LIMIT` with no `ORDER BY` cutting a plan-dependent probe order). Pass 3's minimal repro
(ii) and its TPC-H repro (iii), re-run:

```sql
SELECT d.team, l.lap_id FROM drivers d JOIN laps l ON d.driver_id = l.driver_id
WHERE l.season = 2022 AND ... (x6) LIMIT 3
```
all four modes: `(AlphaTauri,2) (AlphaTauri,8) (AlphaTauri,47)` — identical. (Pass 3 had
`(AlphaTauri,2)(AlphaTauri,8)(Alpine,9)` vs `(RedBull,95)(RedBull,181)(RedBull,208)`; the new answer
differs from BOTH, which is expected — `deterministicCut` now returns the canonically smallest three
rows rather than either plan's first three.)

```sql
SELECT c.c_name, o.o_orderkey FROM customer c JOIN orders o ON c.c_custkey = o.o_custkey
WHERE o.o_orderstatus = 'F' AND o.o_orderpriority = '1-URGENT' LIMIT 3
```
all four modes: `Customer#000000004/13388, Customer#000000005/1090, Customer#000000005/11941`.

**E-9** (tie-break lexicographic over a permuted schema). Pass 3's 3-way TPC-H repro, re-run:
`vec` and `vnp` now both return `Customer#000000001/282, Customer#000000001/7814,
Customer#000000002/1116`. (Volcano refuses 3-way joins, as before.)

### A-1. E-8's class, re-measured mechanically. 108 generated shapes, 0 divergent.

Pass 3 measured its own class at 2 in 45. I regenerated a wider version of that experiment: four
TPC-H relation pairs (customer/orders, supplier/partsupp, part/partsupp, nation/supplier), every
subset of 2–4 ordinary single-column predicates, and three projection shapes each — a two-column
select, `SELECT *` (whose star expansion inherits the *merged* schema's plan-dependent column
order), and `DISTINCT` — all with `LIMIT` and no `ORDER BY`. 108 queries.

```
gen-join-limit 108 queries, 0 divergent   (all four modes byte-identical on every one)
```

Plus a hand-written battery of 13 shapes chosen where the cut's reasoning is thinnest — `SELECT *`
over 2- and 3-way joins, semi-join (`IN`) and anti-join (`NOT IN`) under a `LIMIT`, a derived table
containing a join, a derived table with its OWN inner `LIMIT` under an outer `LIMIT`, `LEFT JOIN`,
`GROUP BY` over a join, `DISTINCT` over a join, the scalar-subquery transport that carried E-8 into
arithmetic, and two `ORDER BY <constant-over-survivors>` 3-way joins (the E-9 shape): **0 divergent**.

---

## Part B — the seam taken fresh

### B-1 FINDING **E-13 (BLOCKER)** — `AND` short-circuits in ONE engine. Volcano evaluates both
### operands of every `AND` eagerly; the vectorized filter cascades the selection vector and
### evaluates the right conjunct only over the rows the left kept. A `WHERE guard AND
### partial_function(x)` therefore ERRORS on Volcano and RETURNS THE CORRECT ROWS on the
### vectorized path — on the shipped `drivers` table, with no contrivance.

**Run at HEAD, shipped `catalog.json`, `drivers` (20 rows, `age` 21..38):**

```sql
SELECT name, age FROM drivers
WHERE age > 30 AND SUBSTRING(name, age - 30, 3) = 'er_'
```
```
row-volcano          Error: SUBSTRING: start position must be >= 1
columnar-volcano     Error: SUBSTRING: start position must be >= 1
columnar-vectorized  Driver_1 35 | Driver_2 35 | Driver_3 35
columnar-vec-noopt   Driver_1 35 | Driver_2 35 | Driver_3 35
```

The vectorized answer is the correct one: the guard `age > 30` is exactly what makes
`age - 30 >= 1` well-defined, and the three rows it returns are the three whose
`SUBSTRING(name, age-30, 3)` really is `'er_'`. SQLite answers the same three rows (its `substr`
has no such precondition at all). **The declared correctness baseline is the engine that fails.**

**The mechanism, both halves read rather than inferred:**

- `src/execution/evaluator.cc:99-100` — `Value left = evaluate(bin->left…); Value right =
  evaluate(bin->right…);` run *before* the `if (op == "AND" || op == "OR")` tri-state block at
  :111. Both operands are always evaluated. So on every row with `age <= 30` Volcano still
  computes `SUBSTRING(name, age-30, 3)` and `evaluator.cc:61` throws.
- `src/execution/columnar_eval.cc:143-146` — `evalPredicate` special-cases a top-level `AND`:
  `left = evalPredicate(bin->left, …, input_sel); return evalPredicate(bin->right, …, &left);`
  The right conjunct is evaluated over `left`'s survivors only. Its own comment says so:
  *"cascade the selection vector — evaluate the right operand only over rows the left kept"*.

**Three further facts that make this a class rather than one query.**

**(a) A second, independent throwing operator reaches it — integer overflow, no `SUBSTRING`:**
```sql
SELECT lap_id FROM laps WHERE lap_id < 2 AND lap_id * 9223372036854775807 > 0
```
```
row-volcano / columnar-volcano  Error: integer overflow in '*': the result does not fit in a
                                64-bit integer (SwiftQL does not promote to DOUBLE)
vec / vec --no-optimize         lap_id 1
```

**(b) The vectorized path is NOT COMMUTATIVE under `AND`.** Swapping the two conjuncts — a
rewrite SQL says changes nothing — changes the vectorized answer from three rows to an error:
```sql
SELECT name, age FROM drivers WHERE SUBSTRING(name, age - 30, 3) = 'er_' AND age > 30
   -> all four modes: Error: SUBSTRING: start position must be >= 1
```
So the vectorized engine disagrees with ITSELF on two spellings of the same predicate. The `OR`
form throws in all four modes (`sv_union` cannot cascade, by its own comment).

**(c) Chunk pruning adds a THIRD answer, so the two Volcano legs can disagree with each other.**
Raise the guard above the column's zone-map maximum and the columnar leg prunes every chunk, so
the filter never runs and never throws, while the row leg has no zone maps and still evaluates
per row:
```sql
SELECT name FROM drivers WHERE age > 40 AND SUBSTRING(name, age - 40, 2) = 'xx'
```
```
row-volcano       Error: SUBSTRING: start position must be >= 1
columnar-volcano  (0 rows)
vec / vec-noopt   (0 rows)
```
Three modes agree, one errors — and which one errors depends on the DATA relative to the
literal, not on the query.

**Why this was missed, and it is not an oversight of effort.** The project has already reasoned
about exactly this class — for `CASE`, in the opposite direction. README:782 and
development.md:442 both state it: *"`evaluate()` short-circuits; a chunk kernel cannot. Since INT
arithmetic is overflow-checked, an eager kernel raises on rows whose branch is discarded — so the
two evaluators would legitimately disagree."* `compileNode` therefore declines `CASE`
(expression_executor.cc:671), and `evaluate()` short-circuits it (evaluator.cc:210). The same
argument applies verbatim to `AND` **with the polarity reversed** — there it is the SCALAR
evaluator that is eager and the CHUNK path that short-circuits — and that half was never drawn.
The rule is stated as a property of `CASE` rather than as a property of *any construct whose two
implementations disagree about which sub-expressions get evaluated*, so the connective escaped it.

**Ranked BLOCKER, and the reasoning is stated so it can be disagreed with.** It is not a silent
wrong answer, which is what pass 2 and pass 3 reserved BLOCKER for. Against that: it is reachable
on the SHIPPED dataset with two ordinary conjuncts and no literal contrivance (pass 3 ranked E-10
HIGH *specifically* because it needed a 2^53 literal to reach); it makes the CORRECTNESS BASELINE
the failing engine, so "Volcano adjudicates" is not available as a fallback; it puts a legal query
in the state where one mode returns rows and another returns an error, which is the cross-mode
comparison the regression harness exists to make; and (b) is a self-inconsistency inside one
engine that no engine-vs-engine framing even covers.

---

## Part A (cont.)

### A-2 — the tie-break's soundness argument, tested rather than accepted. **It holds, with one
### sentence of the header that is false but not reachable.**

The argument is that `(relation_slot, name)` is a stable, unique name for a column on both legs.
Checked at every producer of a `relation_slot`, by grep over the tree (`relation_slot` is assigned
in exactly six places):

| site | what it stamps | plan-dependent? |
|---|---|---|
| `join_enumeration.cc:257` | leftmost leaf's block -> `order[0]` | slot is the **written-order** index, not the spine position |
| `join_enumeration.cc:316` | relation `r`'s block -> `r` | same |
| `logical_plan.cc:1066` | written-order fold -> `join_slot` = `i+1` | not reordered |
| `logical_plan.cc:495` | `derivedRelationSchema` -> 0 (a leaf's own schema) | re-stamped by the fold above |
| `logical_plan.cc:542` | `blockOutputSchema` -> `i+1` | mirrors the fold; schema-only |
| `subquery_decorrelation.cc:621` | `$scalarN` -> `range_table_size + out.lowered` | unique per lowering, and `hidden` |
| `planner.cc:351` | Volcano's single join -> 1 | Volcano never reorders |

So the SET of `(slot, name)` pairs is the same on both legs, and `decompose`/`rebuild` is the only
thing that permutes the sequence. **Uniqueness** was the part worth attacking, and the two routes
to a duplicate are both closed by an explicit refusal, not by luck:
`derivedRelationSchema` (logical_plan.cc:503-511) throws `column '<x>' is produced twice` on a
derived relation with two same-named outputs, and the catalog refuses duplicate column names.

**The one false sentence.** The header justifies the duplicate case with *"a PROJECTED schema's
order is a function of the SELECT list rather than of the plan."* That is false for `SELECT *`,
whose star expansion (logical_plan.cc:1204-1216, planner.cc:433-445) copies the CHILD schema's
columns in the child's own order — which for a join is the merged, DP-permuted order. It does not
bite, because the star copies whole `ColumnDef`s and so carries `relation_slot` through, and a
duplicate `(slot, name)` pair needs two same-named columns *within one relation*, which the two
refusals above forbid. Recorded below as **E-16 (LOW)**: the sentence is offered as the reason the
fallback is safe, and it is not the reason.

Behaviourally: `SELECT *` over 2-way and 3-way joins under a `LIMIT`, and over a self-join, are
included in the 108-query sweep and the 13-query battery above — all identical in every mode.

### A-3 — the per-node `orderIsPlanStable` table, audited. **The table is right; the one thing it
### cannot see is that it is about ROW ORDER, and one aggregate is order-dependent in its VALUE.**

- SCAN / SORT / JOIN / FILTER / PROJECT / LIMIT / DERIVED — confirmed by reading each operator on
  both paths. The switch is exhaustive over `LogicalNodeType` (9 kinds, 9 cases, no default fall-out
  that is reachable).
- AGGREGATE and DISTINCT: **"both engines emit groups and distinct representatives in
  first-encounter order" is TRUE at HEAD**, re-checked rather than inherited —
  `HashAggregateNode::open` iterates `group_order` (plan_nodes.cc:328, `// first-encounter order,
  not hash order`) against `VecHashAggregateNode::materializeResults`' `group_order_`
  (vec_hash_aggregate_node.cc:248); `DistinctNode`/`VecDistinctNode` both emit first-seen.
- Volcano's local answer (`planner.cc:412`, `order_is_plan_stable = (jc == nullptr) ||
  !stmt.order_by.empty()`) agrees with the logical builder's on **every shape Volcano can build**:
  no join => the tree is SCAN/FILTER/AGG/PROJECT/DISTINCT, all stable; a join with no ORDER BY =>
  JOIN reaches the cut, both insert; a join with ORDER BY => a SORT sits below the projection in
  both, both stop there. Derived tables, multi-way joins and surviving subqueries are refused above,
  so no shape distinguishes the two rules.

**What the table cannot express.** `orderIsPlanStable` reasons about row ORDER. `MIN`/`MAX` are the
one construct whose VALUE is a function of input order — both engines keep the *first* argument
`Value` that compares equal (`if (acc.min_val.isNull() || val < acc.min_val)`, plan_nodes.cc:311 and
vec_hash_aggregate_node.cc:222), so a tie between two values that COMPARE equal but RENDER
differently would be decided by the join's probe order, which no sort above can repair. I looked for
a reachable instance and did not find one: the only way to get two compare-equal, render-different
values into one column is INT vs DOUBLE (`MIN(CASE WHEN … THEN 1 ELSE 1.0 END)`), and `%.15g`
renders `1.0` as `1`, so they render the SAME; the next candidate is INT `2^53+1` against DOUBLE
`2^53`, which the new magnitude refusal now rejects on the vectorized path before it can be
compared. Recorded as a CLEAN result with its reason, not as a finding.

### A-4 FINDING **E-14 (MEDIUM)** — both new INT->DOUBLE refusals fire on queries that are CORRECT
### today, and one of the two over-fires for a reason the code's own comment does not cover.

The brief asks the question directly. Both answers are yes, and they are not the same kind of yes.

**(i) The TYPE refusal (`refuseObservableIntNarrowing`) arms on BOTH operands of `/`
unconditionally, including when the other operand is REAL — where truncation is impossible.**
`taintWalk` (vectorized_plan_builder.cc:184-189) does `if (op == "/") { mergeOrigins(armed, l);
mergeOrigins(armed, r); }` with no test on the operands' types. Run at HEAD:

```sql
SELECT MAX(CASE WHEN lap_id = 1 THEN 7 ELSE 0.5 END) / 2.0 AS y FROM laps
```
```
row-volcano / columnar-volcano   3.5
col-vectorized / vec --no-opt    Error: vectorized execution cannot materialize the integer 7
                                 into a DOUBLE result column that another expression divides …
```
`7 / 2.0` is 3.5 and `7.0 / 2.0` is 3.5 — the stored type CANNOT change this answer, because the
division is never INTEGER/INTEGER. The refusal's own justification ("INTEGER/INTEGER truncates while
INTEGER/REAL does not") does not apply, and this is NOT the residue the comment admits (which is
"when that division happens to be exact"): here no exactness is involved at all. With `/ 2` instead
of `/ 2.0` Volcano answers 3 and the vectorized path would have answered 3.5, so the refusal is
right there — one character apart.

**Where it costs the most: a shape NO engine can now answer.** The same over-arming inside a derived
table leaves the query unanswerable, and the error message says so itself in parentheses:
```sql
SELECT t.x / 2.0 AS y FROM (SELECT CASE WHEN lap_id = 1 THEN 7 ELSE 0.5 END AS x
                            FROM laps WHERE lap_id < 3) t
```
```
row-volcano / columnar-volcano   Error: derived tables … are not supported on the Volcano path
col-vectorized / vec --no-opt    Error: … or re-run with --execution volcano where that path
                                 supports the query (it does not run derived tables).
```
Before fix round 3 this query returned `3.5, 0.25` on the vectorized path, which is correct.

**(ii) The MAGNITUDE refusal (`narrowToDoubleColumn`) fires on the TEXT bound even when the value is
never rendered.** The bound is deliberately the smaller of the value bound (2^53) and the rendering
bound (1e15), and between them the double is EXACT — only `%.15g` differs. So any query that
consumes the column arithmetically or as a predicate rather than printing it is refused for a
difference that cannot occur:
```sql
SELECT MAX(CASE WHEN lap_id = 1 THEN 2000000000000000 ELSE 0.5 END) > 1 AS big FROM laps
```
```
row-volcano / columnar-volcano   1
col-vectorized / vec --no-opt    Error: … cannot materialize the integer 2000000000000000 into a
                                 DOUBLE result column without changing it …
```
`2e15 < 2^53`, so the double IS the integer; the comparison is `1` either way. Volcano answers, the
vectorized path refuses.

Ranked MEDIUM, not higher: both are LOUD, both hand the user a message, and for the single-block
forms the message's advice (`--execution volcano`) actually works. Ranked MEDIUM, not LOW: they are
NEW capability regressions introduced by the fix under audit, on shapes that were correct in all
four modes before it, and the derived-table form has no working mode at all. The corpus cannot see
them — the gate is green — because no corpus query mixes an INT and a REAL branch under a `/` or a
15-digit literal.

### A-5 FINDING **E-15 (MEDIUM)** — the deterministic cut's cost lands on the INJECTED `LIMIT 1`
### that `materializeSubqueries` puts on an uncorrelated `EXISTS` body, where the surviving row's
### identity is provably never read.

This is the case the brief asks to be reported if found: a place the measured 7.6x was not measured.
`runOnce` (subquery_materialization.cc:198) sets `body.limit = 1` for an `EXISTS` body, and that
`stmt.limit` reaches `LogicalPlanBuilder::build`'s `deterministicCut` like any user-written one. If
the body contains a join, a `LogicalSort [canonical row order]` with `row_cap = 1` is inserted — so
the body now READS ITS ENTIRE INPUT and heap-compares every row, to choose which single row proves a
fact that `buildReplacement` reads as `!res.rows.empty()` (subquery_materialization.cc:219). Any row
proves it. Pass 3's own site table says so (`#3 CLOSED — only !res.rows.empty() is read`).

Measured on the tree's `build/swiftql` (a **Debug** build, so the absolute seconds are not the
Release figures — the ratios are the point), `laps` 10,000 rows, `laps ⋈ laps ON driver_id` = 5M rows:

```
EXISTS (SELECT a.lap_id FROM laps a JOIN laps b ON a.driver_id = b.driver_id)   17.25 s
SELECT COUNT(*) FROM laps a JOIN laps b ON a.driver_id = b.driver_id             7.73 s
EXISTS (SELECT lap_id FROM laps)                                                 0.21 s
SELECT a.lap_id FROM laps a JOIN laps b ON … LIMIT 1                            17.56 s
```

The EXISTS pays **the same 17.5 s as the top-level `LIMIT 1`** — the full accepted cost — and is
**2.2x slower than materializing and counting the entire join it is only asking about**. The third
line is the control: the same `EXISTS` over a plan-stable SCAN takes no cut and costs 0.21 s.

The same applies to the injected `LIMIT 2` on a SCALAR body (site #4): the cap exists so that two
rows THROW, and a determined choice between them changes nothing about whether `res.rows.size() > 1`.

Ranked MEDIUM: it is a cost, not a wrong answer — but it is not the accepted cost, because the user
wrote no `LIMIT`, the cut cannot change the answer at either site, and a two-line test in
`deterministicCut`'s caller (skip when the limit was injected) removes it. Nothing measures it: the
TPC-H harness's `EXISTS` bodies (Q4, Q21, Q22) are correlated or single-relation, so none of them
takes this path.

### A-6 — the other cut sites (#5, #7, #8, #9, #10) re-confirmed order-invariant. **CLEAN.**

- **#5** (`res.rows[0][0]`) is guarded by #4's throw, unchanged.
- **#7 DISTINCT**: the dedup key is every output column on both engines
  (`appendGroupKeyField` over the whole row, plan_nodes.cc:460 / vec_distinct_node.cc:41), so tied
  rows are identical in the output. Re-verified behaviourally in the 108-query sweep (a `DISTINCT`
  variant of every generated shape) and in 76 NULL/degenerate queries.
- **#8 semi-join first match**: the output SET is determined (each surviving probe row is emitted
  once); its ORDER feeds a `LIMIT`, which now takes a cut — semi/anti joins are `LogicalJoin`, and
  `orderIsPlanStable` returns false for `JOIN` *without* special-casing them, which is the right
  answer. Verified on `IN` and `NOT IN` under a `LIMIT`.
- **#9 aggregate group order**: see A-3.
- **#10 chunk pruning**: skips whole chunks, so it cannot reorder. But see **E-13(c)** — pruning
  can remove the rows on which the *other* engine throws, which is an observable difference in
  behaviour even though it is not a reordering.

### B-1 (cont.) — **E-13 has a SECOND mechanism, pointing the other way: a `LIMIT` bounds which
### rows Volcano evaluates an expression on, and does not bound the vectorized path's.**

Volcano is pull-based: `LimitNode` asks `ProjectNode` for `n` rows and `ProjectNode` calls
`evaluate()` once per row it is asked for. `VecProjectNode` evaluates its expression over a whole
`DataChunk` (up to 1024 rows) and `VecLimitNode` truncates afterwards. So the same throwing
expression that E-13 reaches through `AND` is reached through `LIMIT` — with the engines swapped.

```sql
SELECT SUBSTRING(name, age - 34, 2) AS s FROM drivers LIMIT 3
```
```
row-volcano / columnar-volcano   Dr | Dr | Dr
col-vectorized / vec --no-opt    Error: SUBSTRING: start position must be >= 1
```
```sql
SELECT lap_id * 9223372036854775807 AS x FROM laps ORDER BY lap_id LIMIT 1
```
```
row-volcano / columnar-volcano   9223372036854775807
col-vectorized / vec --no-opt    Error: integer overflow in '*' …
```

The `LIMIT` is load-bearing, checked: **without** it, `SELECT SUBSTRING(name, age - 34, 2) FROM
drivers` errors in all four modes, which is the agreement the corpus sees. Adding `LIMIT 3` — a
clause that can only ever REMOVE rows — makes two of the four modes start answering.

So the class is not "Volcano is eager"; it is **the two engines evaluate expressions on different
row sets, in both directions**: the scalar evaluator is eager where the chunk path short-circuits
(`AND`), and the chunk path is eager where the scalar pipeline is lazy (`LIMIT`, and any
row-at-a-time consumer above a projection). Every partial function in the engine — `SUBSTRING`'s
two preconditions (evaluator.cc:61,63), `checkedAdd/Sub/Mul/Div/Negate`, `LIKE`/`SUBSTRING`'s
STRING-operand checks — sits on that difference.

**Corpus coverage of the class: measured 0.** Harvested every `SELECT` string literal from
`compare_against_sqlite.py`, `test_new_queries.py` and `tpch_queries.py` — 592 distinct. 11 contain
`SUBSTRING`; 5 of those have both a `WHERE` and an `AND`; **all 5 use a CONSTANT start position
(`SUBSTRING(c_phone, 1, 2)`, `SUBSTRING(o_orderdate, 1, 4)`, `SUBSTRING(l_shipdate, 1, 4)`)**, so no
corpus query has a partial function whose precondition depends on a guarded column. 6 literals carry
a >= 15-digit constant and none of them multiplies. The gate is green on a corpus with no instance
of the class — the same sentence pass 2 and pass 3 each had to write.
