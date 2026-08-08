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

