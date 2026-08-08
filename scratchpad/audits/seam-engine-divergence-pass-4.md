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
