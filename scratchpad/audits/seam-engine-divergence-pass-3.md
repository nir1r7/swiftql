# Seam audit — engine divergence (Volcano vs vectorized), PASS 3

Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `922ca15`.
Predecessors read in full: `seam-engine-divergence-pass-1.md`, `seam-engine-divergence-pass-2.md`.

Status: IN PROGRESS — written incrementally, appended as each item is confirmed.

Gate state at start (reported by the orchestrator, on this exact tree): build 56 TUs / 0 warnings;
unit 823/823; SQLite oracle 1496 passed / 0 failed / 0 errors; regression 318 incl. 119
`optimized == --no-optimize` checks, 0 divergences; TPC-H 20/22 meaningful, baseline md5 unchanged.

---

## Part A — verifying pass 2's fixes

### A0. What the two commits actually changed

- `7b84952` (record-only; content landed in `733596e`/`119bf75`) — `src/execution/sort_comparator.h`,
  called by `SortNode` (plan_nodes.cc:528) and `VecSortNode` (vec_sort_node.cc:48) and by nothing
  else. When every declared `ORDER BY` key ties, `rowLess` compares the whole row column by column
  in schema order, ascending, over `min(schema.size(), a.size(), b.size())` columns, with
  `compareForTieBreak` (a widened `compareForSort` that orders any number before any string instead
  of throwing).
- `70570dc` — `Planner::plan` now applies `narrowRows` on the row-storage legs so both storage legs
  hand `SeqScanNode` the same `buildScanSchema` result. Plus two comment sweeps.

Operator order is identical in both builders — `planner.cc:409-455` and `logical_plan.cc:1098-1134`
both produce `... -> Sort -> Project -> Distinct -> Limit`.

---

### A-1 FINDING **E-8 (BLOCKER)** — the fix is in the sort comparator, so it does not fire when
### there is no sort. **Delete the `ORDER BY` from pass 2's own repro and E-1 and E-1b both come
### straight back, at HEAD, with the gate green.**

Pass 2's E-1/E-1b repro carried `ORDER BY MIN(l.season)`. That clause does no work in the query —
every surviving group has `MIN(season) = 2022`, which is exactly why it was a *tie* at the cut.
Removing it removes the only node that calls `sort_comparator::rowLess`, and the cut then lands
directly on `HashAggregateNode`/`VecHashAggregateNode` first-encounter order, which is the join's
probe order, which is the build-side choice the two engines still make by different rules
(`planner.cc:392` raw counts vs `vectorized_plan_builder.cc:527` post-pushdown estimates).

**Run at HEAD `922ca15`:**

```sql
SELECT d.team, MIN(l.season)
FROM drivers d JOIN laps l ON d.driver_id = l.driver_id
WHERE l.season = 2022 AND l.season = 2022 AND l.season = 2022
  AND l.season = 2022 AND l.season = 2022 AND l.season = 2022
GROUP BY d.team LIMIT 3
```

```
row-volcano          AlphaTauri 2022 | Alpine     2022 | McLaren 2022
columnar-volcano     AlphaTauri 2022 | Alpine     2022 | McLaren 2022
columnar-vectorized  RedBull    2022 | AlphaTauri 2022 | McLaren 2022   <-- different row SET
columnar-vec-noopt   AlphaTauri 2022 | Alpine     2022 | McLaren 2022
```

`{AlphaTauri, Alpine, McLaren}` vs `{RedBull, AlphaTauri, McLaren}` — byte-for-byte pass 2's E-1,
unfixed.

And the E-1b transport is also intact — the same body as a scalar subquery, `LIMIT 1`, no `ORDER BY`:

```sql
SELECT COUNT(*) FROM laps
WHERE team = (SELECT d.team FROM drivers d JOIN laps l ON d.driver_id = l.driver_id
              WHERE l.season = 2022 AND ... (x6)
              GROUP BY d.team LIMIT 1)
```

```
row-volcano          COUNT(*)  977
columnar-volcano     COUNT(*)  977
columnar-vectorized  COUNT(*) 1536      <-- inner scalar resolved to 'RedBull', not 'AlphaTauri'
columnar-vec-noopt   COUNT(*)  977
SQLite                          977
```

**Why this is a BLOCKER and not a re-report.** It is a *different* failing shape (no `ORDER BY`
anywhere in the query) reaching the *same* consequence, and it is a direct violation of
`optimized == --no-optimize` — the invariant this project gates on with 119 entries in
`run_optimizer_invariant` — expressed as a single scalar differing by 559, not as a row order SQL
leaves unspecified. `normalize()` sorts, so a set difference of this kind IS visible to the
existing harness; it is missed only because no query of this shape is in any list.

**Why the fix missed it.** The fix was chosen at the sort site because that is where pass 2's repro
exhibited it. But `sort_comparator.h`'s own header states the general defect correctly —
"`std::stable_sort` propagates its INPUT order, and input order is a function of the PLAN" — and
then closes only the stable_sort instance. The cut is `LimitNode`/`VecLimitNode`, and nothing
requires a `SortNode` to sit under it. `LIMIT` without `ORDER BY` is a first-class SQL shape.

#### E-8 is much wider than pass 2's repro: no `GROUP BY` and no contrivance are needed either

The aggregate is not load-bearing. Neither is the repeated-conjunct trick. Three more shapes, all
run at HEAD:

**(i) `DISTINCT` + `LIMIT`, no `ORDER BY`** (f1 data, same 6x predicate):

```sql
SELECT DISTINCT d.team FROM drivers d JOIN laps l ON d.driver_id = l.driver_id WHERE ... LIMIT 3
```
```
row-volcano / col-volcano / vec-noopt : AlphaTauri, Alpine,     McLaren
vec optimized                          : RedBull,    AlphaTauri, McLaren
```

**(ii) A bare projection over a join + `LIMIT`. No aggregate, no DISTINCT, no ORDER BY** — the
minimal expression of the defect:

```sql
SELECT d.team, l.lap_id FROM drivers d JOIN laps l ON d.driver_id = l.driver_id WHERE ... LIMIT 3
```
```
row-volcano / col-volcano / vec-noopt : (AlphaTauri,2) (AlphaTauri,8) (Alpine,9)
vec optimized                          : (RedBull,95)   (RedBull,181)  (RedBull,208)
```
Disjoint row sets.

**(iii) NO CONTRIVANCE AT ALL — an ordinary TPC-H query, two plain equality predicates**
(`--catalog data/tpch/sf0.01/catalog.json`):

```sql
SELECT c.c_name, o.o_orderkey
FROM customer c JOIN orders o ON c.c_custkey = o.o_custkey
WHERE o.o_orderstatus = 'F' AND o.o_orderpriority = '1-URGENT'
LIMIT 3
```
```
row-volcano       Customer#000000404 2 | Customer#000000752 36 | Customer#000000607 45
columnar-volcano  Customer#000000404 2 | Customer#000000752 36 | Customer#000000607 45
col-vectorized    Customer#000000004 13388 | Customer#000000005 1090 | Customer#000000005 11941
col-vec-noopt     Customer#000000404 2 | Customer#000000752 36 | Customer#000000607 45
```
Disjoint row sets, on a query no one would call pathological. `--explain` names the mechanism
exactly:
```
optimized     VecHashJoin [c_custkey = o_custkey] build=orders    <- probe = customer
                VecScan [customer]                    est=1500
                VecFilter [...] / VecScan [orders]    est=1000    (15000 / 3 / 5)
--no-optimize VecHashJoin [o_custkey = c_custkey]                 <- probe = orders
```
Two ordinary conjuncts on `orders` drop its estimate (`15000 * 1/3 * 1/5 = 1000`) below
`customer` (1500), the build side flips, the probe order reverses, and the `LIMIT 3` cut lands on
a different three rows. Volcano's raw-count rule (`1500 < 15000`) never flips.

**And it transports into a value on the same query**, so it is arithmetic and not row order:

```sql
SELECT COUNT(*) FROM orders WHERE o_custkey =
  (SELECT c.c_custkey FROM customer c JOIN orders o ON c.c_custkey = o.o_custkey
   WHERE o.o_orderstatus = 'F' AND o.o_orderpriority = '1-URGENT' LIMIT 1)
```
```
row-volcano 11 | columnar-volcano 11 | col-vectorized 10 | col-vec-noopt 11
```

So E-8 is not a corner of pass 2's contrived predicate. `<any join> ... LIMIT n` where pushdown
moves one side's estimate across the other's raw count is the general shape, and TPC-H-scale
tables (1500 vs 15000, two ordinary equality filters) reach it without help.


---

### A-1 (cont.) FINDING **E-9 (BLOCKER)** — the tie-break is a function of the row's values **and
### of the schema's column ORDER**, and `JoinEnumeration` permutes that order. Two legs that both
### run the tie-break, correctly, still cut differently.

`sort_comparator.h` states the property it relies on:

> It is a function of the row's VALUES alone. It cannot consult arrival order, hash bucket layout,
> chunk boundaries, or which side built the hash table — precisely the things that differ between
> the legs.

That list is incomplete. `rowLess` walks `for (int i = 0; i < n; ++i) compareForTieBreak(a[i], b[i])`
— a **lexicographic** order over the row **in schema index order**. Lexicographic order over a
permuted tuple is a *different total order*. And the schema handed to the sort is the join's merged
schema, whose column order is built from the **chosen join order**:

`join_enumeration.cc:225-270`, `rebuild()`:
```
std::vector<ColumnDef> merged = node->output_schema.columns();   // order[0]'s columns
for (ColumnDef& c : merged) c.relation_slot = order[0];
for (k = 1 ...) { ... merged schema: [left block] ++ [this relation's columns, stamped r]; }
```
with the comment `// a slot-sorted "canonical" order is not available (invariant 1)`. Confirmed by
`--explain`: `LogicalJoin ... order=customer@1,nation@2,orders@0` under the optimizer vs the written
`orders, customer, nation` under `--no-optimize` — same relations, same rows, **columns in a
different order**. The projection above the sort resolves by name/slot so the *printed* column order
is unaffected; the *tie-break's* column order is not.

**Concrete failing shape, run at HEAD** (`--catalog data/tpch/sf0.01/catalog.json`):

```sql
SELECT c.c_name, o.o_orderkey
FROM orders o JOIN customer c ON c.c_custkey = o.o_custkey
              JOIN nation   n ON n.n_nationkey = c.c_nationkey
WHERE o.o_orderstatus = 'F'
ORDER BY o.o_orderstatus
LIMIT 3
```

```
vec optimized      Customer#000000001  282 | Customer#000000001 7814 | Customer#000000002 1116
vec --no-optimize  Customer#000000404    2 | Customer#000000830    3 | Customer#000001471    6
```

Disjoint row sets — a direct `optimized == --no-optimize` violation.

**The mechanism is unambiguous here, in a way E-8's is not.** `o_orderstatus` is `'F'` for every
surviving row, so the single declared key ties on *every* pair and the tie-break decides the entire
order, in both legs. Both legs run it. Both are deterministic. They still disagree, because:

- optimized leads with `customer` -> merged `[c_custkey, c_name, ...][n_...][o_...]` -> lexicographic
  by `c_custkey` -> the three lowest customers.
- `--no-optimize` keeps the written order -> merged `[o_orderkey, o_custkey, ...][c_...][n_...]` ->
  lexicographic by `o_orderkey` -> order keys 2, 3, 6.

Both answers are what the comparator promises; the comparator's promise is just not plan-independent.

This is the *same* class 70570dc closed on the storage axis (two legs tie-breaking over different
column SETS) reappearing on the optimizer axis (two legs tie-breaking over the same column set in a
different ORDER). 70570dc's commit message says the asymmetry "was benign only because the first
discriminating column happened to be `driver_id` in both legs. That is luck, and it changes with the
data." The identical sentence applies here and no one checked the second axis.

Volcano refuses three-way joins, so this is not observable as a Volcano-vs-vec difference — it is
observable as the invariant the project actually gates on.


---

### A-1 (cont.) — is the "identical rows may stay tied" argument true? **Yes, and it is the one
### part of the tie-break's reasoning that holds.**

The argument is that rows equal in every compared column project identically, so whichever survives
the cut the answer is the same. I looked for the counterexample the brief asks about — duplicate
rows with different *provenance* — and it does not exist, for a structural reason worth stating:

**everything that could distinguish two rows sits BELOW the sort, never above it.** Both builders
place the sort at `... -> Sort -> Project -> Distinct -> Limit` (planner.cc:409, logical_plan.cc:1098),
so the only consumers of a sorted stream are:

- `ProjectNode`/`VecProjectNode` — evaluates against the sort's own schema, so it cannot read a
  column the tie-break did not compare;
- `DistinctNode`/`VecDistinctNode` — keys on every *output* column, so identical rows collapse
  regardless of which came first;
- `LimitNode`/`VecLimitNode` — a prefix.

A join that multiplies rows, a `GROUP BY` that puts them in different groups, an aggregate over
them: all of those are *inputs* to the sort. A block's sorted output escapes upward only as
(a) the printed result, (b) a scalar subquery's `res.rows[0][0]`, (c) an `EXISTS`'s row count,
(d) a derived relation. In every one of those, two rows equal in every column of the sort's schema
are interchangeable.

The remaining worry — the `min(schema.size(), a.size(), b.size())` truncation silently hiding
columns — I could not reach. Every row-producing site builds exactly `schema.size()` values
(`SeqScanNode` columnar loop, `VecSortNode::consumeAndSort`, join concatenation over
`merged_schema`, `buildAggregateSchema`), and `narrowRows` (70570dc) closed the one site where a
row was wider than its schema. The `min` is defensive, not load-bearing, today.

So: **the tie-break is total on distinguishable rows in the sense it claims. Its defect is not
incompleteness within one plan — it is that "distinguishable" is defined by a column ORDER the
optimizer is free to permute (E-9), and that it is not consulted at all when there is no sort
(E-8).**

---

### A-2 — "ascending regardless of `desc`": nothing downstream assumes tie-break direction.
### **CLEAN.**

Checked by exhaustion rather than argument. Grepped the tree for anything that could reverse,
truncate-from-the-end, or otherwise read the sort's *direction*:

- no `std::reverse` / `rbegin` / `partial_sort` / `nth_element` anywhere in `src/execution/` or
  `src/planner/` except `join_enumeration.cc:374`, which reverses a DP *back-pointer chain* and
  never touches rows;
- no top-N / limit-into-sort pushdown exists — `VecSortNode` has no `limit_` member and the limit
  is a separate node in both builders;
- `CardinalityEstimator` reads `lim.limit` only to bound `estimated_rows` (:570), never to reshape;
- the three consumers of a sorted stream (Project, Distinct, Limit) all read a *prefix in order*.

The visible consequence, stated so it is not mistaken for a bug later: with a tie at the cut,
`ORDER BY k DESC LIMIT n` and `ORDER BY k ASC LIMIT n` both keep the lexicographically SMALLEST full
rows of their respective boundary tie-groups. That is surprising, but it is applied identically by
both engines, so it is a dialect choice and not a seam divergence.

---

### A-3 — does the tie-break fire everywhere it must? **The site enumeration, and the result at
### each.**

| # | Site where a non-total order meets a cut | Status |
|---|---|---|
| 1 | `LimitNode`/`VecLimitNode` **with** a sort below | comparator fires; closed on the *values* axis, **OPEN on the schema-order axis — E-9** |
| 2 | `LimitNode`/`VecLimitNode` **with no sort at all** (`LIMIT` without `ORDER BY`) | **OPEN — E-8.** Comparator cannot fire; the cut lands on raw plan order |
| 3 | `runOnce`'s injected `LIMIT 1` on an `EXISTS` body (subquery_materialization.cc:198) | CLOSED — only `!res.rows.empty()` is read, so any row proves existence |
| 4 | `runOnce`'s injected `LIMIT 2` on a `SCALAR` body (:201) | CLOSED **by design** — the cap is 2 precisely so `res.rows.size() > 1` THROWS (:229) instead of silently picking. It never cuts |
| 5 | `buildReplacement` SCALAR `res.rows[0][0]` (:242) | CLOSED — guarded by #4's throw |
| 6 | a **user-written** `LIMIT 1` in a scalar body | **OPEN — this is E-8/E-9's transport into arithmetic** |
| 7 | `DistinctNode`/`VecDistinctNode` representative choice | CLOSED — the dedup key is every output column, so tied rows are identical in the output |
| 8 | semi/anti join "first match" | CLOSED as a cut — each surviving probe row is emitted exactly once and the output SET is determined. Its *order* is probe order, which feeds #2 |
| 9 | `HashAggregateNode`/`VecHashAggregateNode` group emission order | CLOSED as a cut (the group SET is determined); its order feeds #2 |
| 10 | `ChunkPruner::shouldSkip` | CLOSED — skips whole chunks, cannot reorder |
| 11 | `MIN`/`MAX` keeping the *argument* `Value` on a tie | see E-10 below — reachable, but the observable difference is the one E-10 names |

One shared-dialect note from #4, not a seam finding: a user-written `LIMIT 5` on a scalar body
becomes `LIMIT 2` and then raises "scalar subquery returned more than one row", where SQLite would
silently take the first row. Both engines do this identically.

---

### A-4 — did any other consumer rely on the wide scan schema? **No. CLEAN.**

Enumerated every consumer of the FROM/JOIN scan's schema and rows in `Planner::plan`:
`merged_cols` for the join (planner.cc:349 — the row leg's merged schema is now as narrow as the
columnar leg's, which is the *reduction* of a difference), `preserved_slots` /
`pruningHintForPreservedSide` (already built from `scan_schema` before the fix, unchanged),
`inferExprType` on WHERE / HAVING / ORDER BY, `buildAggregateSchema`, `HashJoinNode`'s bare-name key
resolution, and the `select_star` expansion (which never narrows — `buildScanSchema` returns
`full_schema` for `select_star` and for `has_subquery`).

`buildScanSchema` (logical_plan.cc:294-335) collects from select list, WHERE, GROUP BY (including
expression keys), HAVING, ORDER BY **and every `stmt.joins[].condition`** — and the ON walk matters
here, because for a LEFT join the residual is deliberately *not* folded into `stmt.where`
(planner.cc:167-178), so it would otherwise be uncollected. Order is right too: the ON
decomposition runs before `buildScanSchema`, and `classifyJoinCondition` only reads (residuals are
`cloneExpr`'d), so `stmt.joins[0].condition` is intact when the collector walks it.

`narrowRows` itself is correct: positional (`full.indexOf(c.name)`), because the row path returns
`&rows_[cursor_]` by position while the columnar path reconstructs by NAME
(`columnar_table_.getValue(schema_.column(c).name, ...)`, plan_nodes.cc:76); it throws by name on a
miss; and the `narrowed.size() == full.size()` early-out is sound because `narrowSchema` returns a
subsequence, so equal size implies identity. Self-join is right too — the pre-scan copy is narrowed
with `right_scan_schema`, and for a self-join both schemas are the same `buildScanSchema` result.

Spot-checked with a six-query battery on the shapes that stress narrowing (LEFT JOIN with an ON
residual and a null-extended `l.speed` in ORDER BY; ORDER BY on columns absent from the select list;
GROUP BY + HAVING; DISTINCT over a join; `SELECT *` over a join; a LEFT join with a WHERE on the
preserved side). All four modes byte-identical on all six.


---

## Part B — the seam taken fresh

### B-1 FINDING **E-10 (HIGH)** — the vectorized path coerces every value it re-materializes to
### its schema-declared type; Volcano does not. The widening `appendColumnValue`'s own comment
### calls "lossless" **is not**, and it changes a VALUE and a ROW COUNT.

`vec_types.h:100-106`:

> Append one cell, NULL-aware. `cv.type` decides the storage type. **An INT Value widens into a
> DOUBLE column (lossless**, and the same promotion evaluate() does via toNumeric() — reachable when
> a schema declares DOUBLE for a type-preserving MIN/MAX over an INT column). Every other type
> disagreement is a planner/schema bug…

INT -> DOUBLE is lossless only below 2^53. Above it, two distinct `int64_t` collapse onto one
`double`. There are **seven** such coercion sites, one in every vectorized operator that rebuilds a
`Row` into a typed `ColumnVector`: `vec_project_node.cc:86,97`, `vec_sort_node.cc:62`,
`vec_distinct_node.cc:66`, `vec_hash_aggregate_node.cc:295`, `vec_hash_join_node.cc:156`,
`vec_simd_loop_join_node.cc:120`. **Volcano has no equivalent site at all** — `ProjectNode` emits the
`Value` the evaluator produced, untouched by the schema. That asymmetry is the seam.

It is reachable whenever an expression's *inferred* type is DOUBLE while its *runtime* Value is INT.
`CaseExpr` with one numeric branch of each type is the general route (`inferExprType` unifies to
DOUBLE; `evaluate()` returns the taken branch verbatim).

**(a) A different value. Run at HEAD:**

```sql
SELECT CASE WHEN round > 10 THEN 9007199254740993 ELSE 0.5 END AS c FROM laps LIMIT 2
```
```
row-volcano       9007199254740993
columnar-volcano  9007199254740993
col-vectorized    9.00719925474099e+15     <-- 9007199254740992: a DIFFERENT integer
SQLite            9007199254740993
```
Not a formatting artefact: 2^53+1 is not representable as a double, so the vectorized path returns
an integer the query never mentions. Volcano and SQLite agree; the vectorized path is alone.

(The *formatting* half of the divergence starts lower, at 1e15, where `%.15g` switches to exponent
form while `std::to_string(int64_t)` does not — so `9007199254740992` prints as
`9.00719925474099e+15` on the vectorized path even where the value survives. The harness compares
text, so that alone would be a red diff.)

**(b) A different ROW COUNT — the stronger form.** `VecDistinctNode` sits above `VecProjectNode`,
so it dedups values that have already been coerced:

```sql
SELECT DISTINCT CASE WHEN lap_id = 2 THEN 9007199254740993
                     WHEN lap_id = 8 THEN 9007199254740992
                     ELSE 0.5 END AS c
FROM laps WHERE lap_id < 10
```
```
row-volcano       9007199254740993 | 9007199254740992 | 0.5     (3 rows)
columnar-volcano  9007199254740993 | 9007199254740992 | 0.5     (3 rows)
col-vectorized    9.00719925474099e+15 | 0.5                    (2 rows)
col-vec-noopt     9.00719925474099e+15 | 0.5                    (2 rows)
SQLite            3 rows
```

Two distinct inputs become one output row. And the vectorized path contradicts **itself** on the
same expression — `COUNT(DISTINCT <same CASE>)` returns **3** in all four modes, because
`VecHashAggregateNode` builds its distinct key from the pre-coercion `Value` via
`appendGroupKeyField`. So within one engine, `SELECT DISTINCT e` says two groups and
`COUNT(DISTINCT e)` says three.

**Why passes 1 and 2 did not see it.** Pass 2's B3 checked type/precision by comparing the two
*evaluators* — `evaluator.cc` against `expression_executor.cc`/`columnar_eval.cc` — and correctly
found them aligned, including the architectural reason (the compiler returns `nullptr` and falls
back to `evaluate()` for anything it cannot reproduce). The coercion is not in either evaluator. It
is in the **materialization** step that only one engine has, one layer above where both passes
looked. `CaseExpr` in particular is a shape the vectorized compiler *declines* — so the value is
produced by the shared `evaluate()`, identically in both engines, and then changed on the way into
the chunk.

Ranked HIGH rather than BLOCKER for one reason only: no shipped dataset has an INT column at 1e15
or above, so today it takes a literal to reach. The `checked_arith.h` guard means arithmetic cannot
manufacture one either — it throws on overflow rather than producing a large magnitude silently.
Nothing in the loader prevents such a column: `TypeId::INT` is `int64_t`.


---

### B-2 — how much of the corpus can express E-8 at all? **Measured: none of it.**

Same method as pass 2's B8, applied to the new class. Harvested every `SELECT` string literal from
`compare_against_sqlite.py` and `test_new_queries.py`:

```
distinct SELECT literals harvested : 211
  with LIMIT                       :  28
  LIMIT and NO ORDER BY            :   6
  ... and containing a JOIN        :   1
```

The single one is
`SELECT season, speed FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id LIMIT 5` — **no
WHERE clause**, so pushdown moves no estimate and the two build-side rules provably coincide. It is
exactly the blind spot pass 2 named in `ENGINE_AGREEMENT_QUERIES` ("both queries have no WHERE
clause, so the join-side rule provably coincides across all four modes"), reproduced in the only
corpus query that could otherwise have caught E-8. Verified: all four modes byte-identical on it.

So the corpus contains **zero** queries able to express E-8, and green on 1496 + 318 + 823 is a true
statement about a corpus with no instance of the class — the same finding pass 2 made about its
predecessor, one class along.

### B-3 — how PREVALENT is E-8? **Measured: 2 of 45 ordinary generated queries. ~4%.**

Not reasoned. Generated every `SELECT <two cols> FROM a JOIN b ON <key> WHERE <subset of ordinary
single-column predicates> LIMIT 3` over three TPC-H relation pairs (customer/orders,
supplier/partsupp, part/partsupp) — 45 queries that all four modes execute. Compared optimized
against `--no-optimize`, and row-Volcano against optimized vectorized:

```
join+WHERE+LIMIT3 queries run : 45
  optimized != --no-optimize  :  2
  volcano   != optimized vec  :  2
```

The two:
```
SELECT c.c_name, o.o_orderkey FROM customer c JOIN orders o ON c.c_custkey = o.o_custkey
  WHERE o.o_orderstatus = 'F' AND o.o_orderpriority = '1-URGENT' [AND o.o_shippriority = 0] LIMIT 3
  opt   : Customer#000000004 13388 | Customer#000000005  1090 | Customer#000000005 11941
  noopt : Customer#000000404     2 | Customer#000000752    36 | Customer#000000607    45
```

Nothing in either is contrived. This is not a class that needs a pathological estimator input; it
needs a WHERE clause selective enough to move one side's estimate across the other side's raw count,
which two ordinary equality predicates on a 10x-larger table achieve.

### B-4 — is there a SHIPPED instance? TPC-H swept: **no. 21 of 22 agree, 1 refused.**

Rendered all 22 TPC-H templates and ran each optimized against `--no-optimize` at sf0.01:
21 agree byte-for-byte, q21 refused by both. **0 divergent.** So E-8 and E-9 have no instance in the
shipped benchmark, which is why the gate is green and why the TPC-H baseline md5 is unchanged.
TPC-H's `ORDER BY` clauses are deliberately near-total and its `LIMIT`s always follow one — the same
structural reason pass 2 gave for finding no TPC-H instance of E-2.

Stated plainly: **the gate being green is consistent with both blockers. It is not evidence against
them.**

### B-5 — the vec-only families, re-hunted against SQLite directly. **CLEAN — 42 of 45.**

Pass 2's E-2 is that the four Volcano-refused families rest on SQLite as a single oracle. I ran that
oracle myself rather than trusting the harness's selection, on 45 hand-written queries chosen to be
adversarial exactly where the harness is thin — NULL-bearing data manufactured the only two ways the
loader allows (outer-join null-extension and division producing NULL), inside shapes only the
vectorized path executes. Both vectorized modes, diffed against an in-process SQLite over the same
CSVs, sorted comparison.

Covered: `IN`/`NOT IN` where the subquery result CONTAINS NULLs (`SELECT l.driver_id / 0`) and where
it is EMPTY; `NOT IN` over a null-extended outer-join column; correlated `EXISTS`/`NOT EXISTS`;
derived tables that are empty, that aggregate, that carry `HAVING`, that are `DISTINCT`, and that sit
on the null-supplying side of a LEFT join; `COUNT`/`SUM`/`AVG`/`MIN`/`MAX`/`COUNT(DISTINCT)` over an
all-NULL column; `GROUP BY` on a nullable column; three-relation joins mixing INNER and LEFT;
`IN` and `NOT IN` conjoined in one WHERE; nested derived-inside-`NOT IN`.

**Result: 42 clean, 3 flagged — and all 3 are declared refusals, not wrong answers:**
- `SELECT`-list subqueries (2 queries) — "subqueries are supported in WHERE and HAVING only";
- a correlated INEQUALITY (1) — refused with a full explanation of why it has no equi-join to lower to.

No NULL, type, DISTINCT, empty-input or three-valued-logic divergence found in the vec-only families.
This confirms pass 2's B2/B3/B4 against an independent oracle and an independent query set, and it is
a result rather than an absence of effort: pass 2 reached the same conclusion by reading the two
evaluators, and E-10 is the one thing that method could not see, because it lives above both.


### B-6 — `VecSimdLoopJoinNode` is NOT a third source of order. **CLEAN, verified by run.**

Pass 1 asserted from the source that the SIMD loop join emits probe-major/build-index like the hash
join. Confirmed behaviourally: a query where the optimizer picks `VecSimdLoopJoin build=nation` and
`--no-optimize` picks `VecHashJoin` on the *same* sides
(`SELECT c.c_name, n.n_name FROM customer c JOIN nation n ON c.c_nationkey = n.n_nationkey LIMIT 4`)
returns identical rows in all four modes. So the algorithm choice — which IS optimizer-gated — adds
nothing to E-8. The two roots are the **build side** and the **join order**, and nothing else.

### B-7 (LOW) **E-11** — `tests/test_sort_tiebreak.cc` cannot see E-9 by construction

13 tests, and they are good ones — `VolcanoSortIsPermutationInvariant`,
`VectorizedSortIsPermutationInvariantAndChunkInvariant`, `TheTwoENGINESAgreeOnTheSameInputMultiset`,
`LimitCutSurvivesTheSameRowsOnBothEngines`, `EveryPermutationOfATiedInputGivesTheSameCut`. Every one
permutes the **rows**. None permutes the **columns**, because each builds a single fixed `Schema`.
The file's header already concedes this shape of blindness for the scan-schema case ("it cannot see
this one, because it builds one schema"); the same sentence covers E-9, and nobody drew the second
conclusion. A `SchemaColumnOrderDoesNotChangeTheCut` test — same rows, columns permuted, assert the
same cut — is four lines and would have caught E-9 before it shipped.

### B-8 (LOW) **E-12** — `sort_comparator.h` states a checkable precondition, and it is false

Two claims in the header are now known-wrong, and both were written by the fix this pass audits:

1. > It is a function of the row's VALUES alone. It cannot consult arrival order, hash bucket layout,
   > chunk boundaries, or which side built the hash table — precisely the things that differ between
   > the legs.

   It also consults **schema column order**, which is a plan property and differs between the legs
   (E-9). The list is presented as exhaustive and is not.

2. > THE PRECONDITION IT RESTS ON, STATED SO IT CAN BE CHECKED: the sort's INPUT row must be the same
   > in every mode. It is, for every shape this can decide, because ORDER BY is planned directly above
   > the aggregate/filter/join and below the projection in both builders, and an aggregate's output
   > schema (`buildAggregateSchema`) is the same in all four modes.

   The justification covers only the aggregate case. For a join it is false: the sort's input row IS
   the join's merged row, and `JoinEnumeration::rebuild` builds that schema from the chosen order.

This is precisely the class 70570dc's own commit message names — "a comment asserting a property the
code no longer has" — introduced by the commit two before it. Recording it separately from E-9
because the header explicitly invites being checked, and the check fails.

### B-9 — pass 2's E-5 persists, unchanged

`plan_nodes.cc:332` is still `auto& group_accs = accumulators[key_str];` — `operator[]`, not `.at()`.
Still unreachable (nothing erases from `accumulators`), still silent UB if it ever becomes reachable.
Pass 2's issue-2 from pass 1 (a semi/anti join's `on_residual` silently dropped by the builder) **is**
fixed — `vectorized_plan_builder.cc:524-529` now forwards `std::move(join->on_residual)` so the
operator's own guard fires. Pass 2's E-4 (the tie-precondition self-check) is fixed too:
`compare_against_sqlite.py:2929-2985` now asserts, per entry, that the probe exposes its key, that
rows exist beyond the cut, that a tie spans the cut, and that the tie is MATERIAL.

---

## SUMMARY

```
BLOCKER   2   E-8, E-9
HIGH      1   E-10
LOW       2   E-11, E-12
```

| # | Rank | Finding | Concrete failing shape? |
|---|---|---|---|
| **E-8** | **BLOCKER** | The tie-break lives in the sort comparator, so it does not fire when there is no sort. `LIMIT` without `ORDER BY` still cuts on plan order, and the two engines still pick the join build side by different rules. Pass 2's E-1 and E-1b both reproduce at HEAD with the `ORDER BY` deleted, and the class reaches a bare `SELECT a, b FROM x JOIN y ... LIMIT n`. | Yes — run. `{AlphaTauri,Alpine,McLaren}` vs `{RedBull,AlphaTauri,McLaren}`; `COUNT(*)` 977 vs 1536; and a plain TPC-H `customer JOIN orders WHERE o_orderstatus='F' AND o_orderpriority='1-URGENT' LIMIT 3` giving disjoint row sets, `COUNT(*)` 11 vs 10 |
| **E-9** | **BLOCKER** | The tie-break is lexicographic over the row **in schema index order**, and `JoinEnumeration::rebuild` builds the merged schema from the *chosen join order*. Two legs that both run the tie-break correctly still cut differently. The comparator's stated precondition ("the sort's INPUT row must be the same in every mode") is false for any multi-way join. | Yes — run. 3-way TPC-H join, `ORDER BY o_orderstatus` (constant over the survivors, so the tie-break decides everything) `LIMIT 3`: `Customer#000000001/282…` vs `Customer#000000404/2…`, disjoint; transported into a scalar, `COUNT(*)` 281 vs 1 |
| **E-10** | HIGH | The vectorized path coerces every value it re-materializes to its schema type at seven sites; Volcano has no such site. `appendColumnValue`'s comment calls the INT->DOUBLE widening "lossless" — it is not above 2^53. Changes a VALUE, and changes a ROW COUNT under `DISTINCT`; the vectorized path also contradicts itself (`SELECT DISTINCT e` = 2 rows, `COUNT(DISTINCT e)` = 3). | Yes — run. `CASE ... THEN 9007199254740993 ELSE 0.5 END`: Volcano and SQLite `9007199254740993`, vectorized `9.00719925474099e+15`. `SELECT DISTINCT` of the same: 3 rows vs 2 |
| **E-11** | LOW | `tests/test_sort_tiebreak.cc` permutes rows in five tests and columns in none — it cannot see E-9 by construction, for the same reason its header already concedes about the scan-schema case. | No — a coverage gap |
| **E-12** | LOW | `sort_comparator.h` presents an exhaustive list of what the tie-break cannot consult (it can consult schema order) and a "stated so it can be checked" precondition that is false for joins. Written by the fix under audit. | N/A (documentation) |

### What came back CLEAN — results, not gaps

- **The tie-break's "identical rows may stay tied" argument is TRUE** (A-1). Everything that could
  distinguish two rows sits below the sort; the only consumers of a sorted stream are Project,
  Distinct and Limit, and a block's sorted output escapes upward only as forms in which
  column-identical rows are interchangeable.
- **Ascending-regardless-of-`desc` is safe** (A-2). No top-N, no limit-into-sort pushdown, no
  `reverse`/`partial_sort`/`nth_element` anywhere; nothing downstream reads the tie-break's direction.
- **The scan-schema fix has no other consumer** (A-4). All seven consumers enumerated;
  `buildScanSchema` collects from every clause *including* `stmt.joins[].condition`, which is what a
  LEFT join's un-folded residual needs; `narrowRows` is positional, throws by name, and its early-out
  is sound. Six-query battery on the shapes that stress it: four modes byte-identical on all six.
- **The other nine cut sites are closed** (A-3), including the two injected limits in
  `runOnce` — the SCALAR cap of 2 exists so a multi-row body throws rather than silently picking, and
  it does.
- **The vec-only families, re-hunted against an independent SQLite oracle on 45 adversarial
  NULL-bearing and degenerate queries: 42 clean, 3 declared refusals** (B-5). No NULL, three-valued
  logic, type, DISTINCT or empty-input divergence found there.
- **`VecSimdLoopJoinNode` adds no third ordering** (B-6), verified by run and not only by reading.
- **TPC-H is clean on the optimizer axis**: 21 of 22 templates agree optimized vs `--no-optimize`,
  1 refused, 0 divergent (B-4).

### Verdict

**The seam is not clean, and the fix under audit closed an instance rather than the class — for the
second time in a row.** Pass 1 fixed a container's iteration order and left a plan decision (pass 2's
E-1). Pass 2 fixed the sort comparator and left (a) every query that reaches a `LIMIT` without a sort
(E-8) and (b) every sort whose input schema the optimizer permutes (E-9). Both blockers are the
*same defect* pass 2 identified, correctly, in one sentence — "`std::stable_sort` propagates its
INPUT order, and input order is a function of the PLAN, not of the query" — and then closed only at
`std::stable_sort`. The cut is `LimitNode`/`VecLimitNode`, not `SortNode`, and nothing requires a
sort beneath it.

Both are demonstrated on run queries with both legs' output side by side, both violate
`optimized == --no-optimize`, and both transport through `materializeSubqueries` into a differing
scalar so they are arithmetic and not merely row order SQL leaves unspecified. E-8 additionally
reproduces on an ordinary TPC-H two-relation query with two plain equality predicates, at a measured
rate of **2 in 45** mechanically-generated join-plus-`LIMIT` queries — it does not need a
pathological estimator input.

The gate is green and that is consistent with all of this: the corpus contains **6** queries with
`LIMIT` and no `ORDER BY`, only **1** with a join, and that one has no WHERE clause — so pushdown
moves no estimate and the two build-side rules provably coincide. Measured, not assumed. Green here
means the corpus has no instance of the class, exactly as it did before pass 2.

E-10 is separate and structural: only one engine materializes `Value`s into typed columns, and that
step is not lossless. Pass 2's type-and-precision sweep could not have found it, because it compared
the two *evaluators* and the coercion is one layer above both.

**This pass does not end the audit.**
