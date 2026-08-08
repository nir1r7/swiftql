# Seam audit — storage — pass 5 (final)

Branch `claude/phase5-week26-qomtkb`, HEAD `b14d086`. Written incrementally; sections
appear in the order they were established, and the file was committed and pushed
before it was finished.

Prior passes: `-1.md` (S-0 headline), `-2.md` (refuted S-0; 0/0/0/2), `-3.md`
(0/0/1/3, S-9), `-4.md` (0/0/2/5, S-9 + S-12 MEDIUM; S-10, S-11, L-1..L-3 LOW).

**Binaries.** Every measurement below uses a **Release** build (`-O3 -DNDEBUG`)
configured from a private detached worktree of `b14d086`
(`scratchpad/st5-wt`, build dir `scratchpad/st5/rel`), compiled under
`flock -w 1800 /tmp/swiftql-build.lock`. No build directory in the working tree was
touched and no source file was modified. A second Release build of `364a2d3` — the
parent of `33bb7ea`, the commit that introduced the pruner's written-order walk — was
made the same way for the before/after in Part A.3; it is a private throwaway
checkout, not an edit to the repo.

The three real cells, restated:

    A  --storage row      --execution volcano
    B  --storage columnar --execution volcano
    C  --storage columnar --execution vectorized
    (D = the same three with --no-optimize, an optimizer flag, not a fourth cell)

`--storage row --execution vectorized` is still refused for every query
(`main.cc:548`), so there are still three cells and the storage oracle is still
**A vs B**.

---

## Part A.1 — the two shapes the fix round closed: **confirmed closed**

Run at HEAD on the shipped `catalog.json`, all six combinations:

    SELECT lap_id FROM laps WHERE lap_id * 9223372036854775807 > 0 AND lap_id > 999999

| cell | result |
|---|---|
| A / A-noopt | `Error: integer overflow in '*'` |
| B / B-noopt | `Error: integer overflow in '*'` |
| C / C-noopt | `Error: integer overflow in '*'` |

    SELECT lap_id FROM laps WHERE speed AND lap_id > 999999      -- the bare DOUBLE conjunct

| cell | result |
|---|---|
| A / A-noopt | `Error: std::get: wrong index for variant` |
| B / B-noopt | `Error: std::get: wrong index for variant` |
| C / C-noopt | `Error: std::get: wrong index for variant` |

Both were storage-mode divergences before the fix (row: Error; columnar: 0 rows). Both
now agree in all six. The written-order rule is also confirmed to keep the pruning it
is supposed to keep: with the order reversed
(`WHERE lap_id > 999999 AND lap_id * 92233…> 0`) every cell returns 0 rows and
`chunks_skipped=2/2` is still observed, i.e. a conjunct written ahead of every raiser
still prunes.

**Pass 4 recorded the bare-`DOUBLE`-predicate row as "unbroken, not proven safe". It
was broken.** That correction is accepted and is the reason Part A.2 below exists: the
class produces a **storage-mode** divergence (A vs B), not a leg divergence
(optimized vs `--no-optimize`), and four passes looked for it in the wrong axis.

---

## Part A.2 — **FINDING S-13, HIGH: the same raise-screen is still bypassed, by a cross-relation conjunct whose column name also exists in the scanned table**

This is the blind spot Part A told me to go looking for, and it is still open at HEAD.
It is the *same* defect the fix round closed, reached by a different door.

### The mechanism

`ChunkPruner::collectSimplePredicates` now screens each conjunct with
`conjunctMayRaise(expr, schema)`, where `schema` is **the scanning node's own output
schema**. The screen bottoms out in `staticTypeOf` (`expr_totality.h`), whose
`ColumnRef` branch is:

```
int idx = (col->id.isResolved() && col->id.isLocal())
    ? schema.indexOf(col->column_name, col->id.localSlot("staticTypeOf"))
    : -1;
if (idx < 0) idx = schema.indexOf(col->column_name);   // <-- bare-name fallback
```

For a conjunct naming **another relation**, the slot lookup fails against a
single-relation scan schema — and then the **bare-name fallback resolves the name
against the scanned table anyway**. If the two tables happen to share a column *name*
with a *different type*, the screen types the conjunct off the wrong column, concludes
it is total, and lets the walk continue to the next conjunct, which prunes.

The collector's own guard (`col->id.isLocal() && localSlot(...) < 1`) correctly stops
the cross-relation conjunct from being *collected* as a hint. It does nothing about the
*screen*, and the screen is what decides whether everything written after it may prune.

Two consumers of one screen therefore answer differently about the same conjunct:
`PredicatePushdown` types it in the filter's own (join-output) schema, gets
"may raise", and freezes the whole WHERE — visible in the plan, the filter never moves.
`ChunkPruner` types it in the scan's schema, gets "total", and prunes on the conjunct
behind it.

### The failing shapes, run across the three real cells

Synthetic catalog, two tables sharing the column name `x` with different declared
types — `big(k INT, x INT, j INT)` 20 000 rows (3 chunks, `k` ascending 0..19999),
`small(j INT, x DOUBLE, s STRING)` and `small2(j INT, x STRING, s STRING)`, 10 rows each,
joining 1:many on `j`.

**S-13a — the truth-value form** (`asInt()` on a DOUBLE, `value.cc:28`):

    SELECT b.k FROM big b JOIN small s ON b.j = s.j WHERE s.x AND b.k > 999999

| cell | rc | output |
|---|---|---|
| A row-volcano | 1 | `Error: std::get: wrong index for variant` |
| A row-volcano `--no-optimize` | 1 | `Error: std::get: wrong index for variant` |
| **B col-volcano** | **0** | **0 rows** |
| **B col-volcano `--no-optimize`** | **0** | **0 rows** |
| **C col-vectorized** | **0** | **0 rows** |
| **C col-vectorized `--no-optimize`** | **0** | **0 rows** |

**S-13b — the STRING-boundary comparison form** (`Type mismatch in Value comparison`,
`value.cc`'s `NUMERIC_COERCE`; this is pass 4's P4-1 error, reached through the screen
instead of through `canSkipChunk`):

    SELECT b.k FROM big b JOIN small2 s ON b.j = s.j WHERE s.x > 5 AND b.k > 999999

| cell | rc | output |
|---|---|---|
| A row-volcano | 1 | `Error: Type mismatch in Value comparison` |
| A row-volcano `--no-optimize` | 1 | `Error: Type mismatch in Value comparison` |
| **B col-volcano** | **0** | **0 rows** |
| **B col-volcano `--no-optimize`** | **0** | **0 rows** |
| **C col-vectorized** | **0** | **0 rows** |
| **C col-vectorized `--no-optimize`** | **0** | **0 rows** |

### It is the pruner, confirmed from the plan rather than inferred

`--explain-analyze`, S-13a, columnar/volcano, **optimizer ON**:

    Project [k]                                       rows_in=0  rows_out=0
      Filter [(s.x AND (b.k > 999999))]               rows_in=0  rows_out=0
        HashJoin [j = j]                              rows_in=0  rows_out=0
          SeqScan [big, 3 columns] chunks_skipped=3/3 rows_in=0  rows_out=0
          SeqScan [small, 2 columns]                  rows_in=0  rows_out=10

`chunks_skipped=3/3`. The filter is still sitting above the join with the whole WHERE
intact — pushdown froze it, exactly as it should — and the scan pruned itself to
nothing underneath it anyway. Identical shape with `--no-optimize`, and on the
vectorized builder (`VecScan [big] chunks_skipped=3/3`).

### Controls, so the claim is not larger than the evidence

- **Written order decides it.** `WHERE b.k > 999999 AND s.x > 5` — the prunable
  conjunct first — returns 0 rows in **all six**, and that is correct: the cascade
  never evaluates `s.x` on any row.
- **Total pruning is required.** `WHERE s.x AND b.k > 15000` (chunk 1 survives) and
  `WHERE s.x AND b.k > 5` (nothing pruned) both give
  `Error: std::get: wrong index for variant` in **all six**. The divergence needs the
  hint to empty the scan, which is what makes it a *storage* divergence rather than a
  general one.
- **Not reachable on the shipped catalog.** `laps` and `drivers` share `driver_id`
  (INT/INT) and `team` (STRING/STRING) — same types both times, so the bare-name
  fallback happens to land on a column of the correct type and the screen's answer is
  accidentally right. The shape needs a same-named column with a *different* type in
  the two joined relations, which the shipped catalog and TPC-H (uniformly prefixed
  column names) do not have. That bounds the blast radius; it does not make the guard
  sound, because a catalog is user input.

### Severity: **HIGH**

A silently masked runtime error is a wrong answer, it is decided by which of the two
storage formats you chose, and the guard that was supposed to close this exact class
one commit ago does not cover it. It is not a BLOCKER only because it needs a catalog
in which two joined relations declare the same column name with different types — the
shipped catalog, TPC-H, and every catalog in `tests/` are all outside it, so no gate
can currently see it.

### The fix, and it is the same fix as Part A.3's

Two options, and the second one buys both:

1. **Local, ~6 lines, in `chunk_pruner.h`.** Before screening a conjunct, require that
   every `ColumnRef` in it is local *and* resolves at its own slot in `schema`
   (`schema.indexOf(name, slot) >= 0`); if not, treat it as may-raise and stop. This is
   the guard the collector already applies, lifted from the collection site to the
   screening site. Cost: nothing measurable — the conjuncts it newly stops the walk at
   are exactly the ones that already stopped it whenever the name did *not* collide.
2. **The one the file itself proposes**: thread the filter's child schema alongside the
   hint through `pruningHintForPreservedSide`, so this walker types every conjunct in
   the schema it was written against. That makes `s.x` type as DOUBLE and answer
   may-raise correctly, **and** it recovers the pruning Part A.3 measures as lost. One
   fix, two findings. Sized in A.3.

Do not fix this by removing the bare-name fallback from `staticTypeOf`: it is shared
with `evaluator.cc`'s resolution rule and with two other consumers where it is
load-bearing. The defect is applying the screen in a schema the ref does not belong to.

---
## Part A.3 — the pruning loss: **confirmed, re-sized, and its scope corrected in two ways**

The fixer recorded: *"the OPTIMIZED leg is unchanged at `chunks_skipped=1/2` … the
`--no-optimize` leg drops to `chunks_skipped=0/2` and 42.5 ms becomes 51.5 ms, +21%."*
The effect is real. The scope is wrong, and the ratio is the smallest one available.

### It is not a `--no-optimize` phenomenon; it is a **written-order** phenomenon, and it hits Volcano's optimized leg too

The controlled experiment is the same query with the two conjuncts swapped — identical
plan otherwise, identical answer (398 rows), same binary, so nothing but the order of
the WHERE differs. Shipped `catalog.json`, Release, `Execution:` from
`--explain-analyze --no-cache`, **median of 11**:

    cross-first   WHERE d.nationality = 'British' AND l.season = 2024
    local-first   WHERE l.season = 2024 AND d.nationality = 'British'

| binary | leg | cross-first | local-first | delta | `chunks_skipped` |
|---|---|---|---|---|---|
| **HEAD `b14d086`** | col-volcano **optimized** | 7.421 ms | 6.115 ms | **+21.4 %** | **0/2** vs 1/2 |
| HEAD | col-volcano `--no-optimize` | 7.470 ms | 6.098 ms | **+22.5 %** | **0/2** vs 1/2 |
| HEAD | col-vectorized `--no-optimize` | 2.317 ms | 1.786 ms | **+29.7 %** | **0/2** vs 1/2 |
| HEAD | col-vectorized optimized | 0.197 ms | 0.183 ms | +7.7 % (noise) | 1/2 both |
| **PRE `364a2d3`** | col-volcano optimized | 7.022 ms | 7.067 ms | −0.6 % | **1/2 both** |
| PRE | col-volcano `--no-optimize` | 7.010 ms | 7.095 ms | −1.2 % | **1/2 both** |
| PRE | col-vectorized `--no-optimize` | 1.796 ms | 1.751 ms | +2.6 % | **1/2 both** |
| PRE | col-vectorized optimized | 0.174 ms | 0.186 ms | −6.5 % | 1/2 both |

`364a2d3` is the parent of `33bb7ea`, the commit that added the schema parameter and the
written-order stop. On that binary the conjunct order makes no difference at all and the
chunk is always skipped, which is the control that proves the HEAD delta is caused by
the pruner change and not by the query text. **Volcano's optimized leg lost pruning as
well**, because `Planner::plan:284` hands the raw `stmt.where` to the FROM scan as the
hint whether or not the optimizer ran — pushdown's re-stamped, scan-local hint is a
*vectorized-builder* property, not an optimizer property. The fixer measured the one
cell that kept it and generalized from it.

### The ratio, on a denominator that is not a 2-chunk table

`laps` is 10 000 rows in 2 chunks, so the lost skip is 18 % of one scan. TPC-H sf0.01
(`lineitem` 60 144 rows, **8 chunks**, `l_orderkey` clustered; `l_orderkey > 14295`
prunes 7 of 8), same swap, same binary, median of 7:

| leg | cross-first | local-first | delta | `chunks_skipped` |
|---|---|---|---|---|
| col-volcano **optimized** | 65.56 ms | 23.44 ms | **+180 % (2.80×)** | **0/8** vs 7/8 |
| col-volcano `--no-optimize` | 63.60 ms | 22.57 ms | **+182 % (2.82×)** | **0/8** vs 7/8 |
| col-vectorized `--no-optimize` | 32.35 ms | 17.58 ms | **+84 % (1.84×)** | **0/8** vs 7/8 |
| col-vectorized optimized | 13.40 ms | 12.22 ms | +9.7 % (noise) | 7/8 both |

Same answer in every row (945 rows). **Method and denominator, stated because this is a
performance claim:** the engine's own `Execution:` line on a Release build, median of
11 (shipped catalog) or 7 (TPC-H) runs with `--no-cache`, quiet box; the denominator is
the *same query's* execution time with the conjuncts written in the other order, so
loading, parsing and planning are outside it entirely. The percentage is not comparable
to S-9's, which is quoted against a whole process.

The PRE binary could not be used for the TPC-H half — it fails to load `.tbl` files
(`Error: Column count mismatch in CSV row`; the trailing-delimiter handling changed
after `364a2d3`), so the before/after control exists only on the shipped catalog. The
within-binary swap at HEAD stands on its own for the TPC-H numbers.

### **FINDING S-14, MEDIUM** — restated

A cross-relation conjunct written *before* a scan-local one costs the scan all of its
zone-map pruning: **+21 % on a 2-chunk table and 2.8× on an 8-chunk one**, in
**col-volcano both optimizer settings** and **col-vectorized `--no-optimize`**. It is a
regression against `364a2d3`, it is invisible to `benchmark.py` only in the sense that
nobody has pointed `benchmark.py` at it — the cost is in `Execution:`, so the instrument
would see it.

**The fix is the same one S-13 needs**, and that is the reason to do it rather than the
6-line local guard: thread the filter's child schema alongside the hint through
`pruningHintForPreservedSide` (`predicate_pushdown.h`), so `collectSimplePredicates`
types every conjunct in the schema it was written against. Then `d.nationality = 'British'`
types as STRING = STRING, answers "cannot raise", the walk continues, and
`l.season = 2024` prunes — while `s.x` (S-13) types as DOUBLE, answers "may raise"
correctly, and stops the walk. One change closes a HIGH correctness finding and a
MEDIUM performance regression. Sizing: one extra argument on
`pruningHintForPreservedSide` and on `ChunkPruner::shouldSkip`/`collectSimplePredicates`,
one `Schema` member on `SeqScanNode` and `VecScanNode`, and the two call sites that
already compute the child schema (`planner.cc:284`, `vectorized_plan_builder.cc:914`) —
**~20 lines**, not the "one line in each of the two builders" the comment claims, because
the schema has to survive to scan time and the two scan nodes currently store only their
own.

---
## Part A.4 — S-9 / S-10 / S-11 / S-12 re-measured at HEAD

All four are **unchanged in code**, verified line by line rather than assumed:

- `narrowRows` is still a plan-time lambda inside `Planner::plan`
  (`planner.cc:217`), still applied at the same three ROW-storage sites
  (`:293`, `:319`, `:326`).
- `SeqScanNode` still has no `keep_` (`plan_nodes.h:14-17`; the row constructor still
  takes a finished `std::vector<Row>`).
- `benchmark.py::run_once` still greps `Execution:` and nothing else
  (`python_tools/benchmark.py:81`); `Plan:` is never parsed.
- `row_cap` is still assigned in exactly two places, both the deterministic cut
  (`logical_plan.cc:1004`, `planner.cc:479`).

### S-9 — re-measured, TPC-H sf0.1 (`lineitem` 600 865 × 16), Release, `--execution volcano`

`Plan:`/`Execution:` are medians of 5 `--explain-analyze --no-cache` runs; wall is the
median of 7 whole-process runs with a wall clock around `subprocess.run`.

| query | ROW Plan | ROW Exec | ROW wall | COL Plan | COL Exec |
|---|---|---|---|---|---|
| `SELECT * FROM lineitem LIMIT 1` (early-returns) | **44.1 µs** | 19.1 µs | 2 699.0 ms | 40.3 µs | 23.2 µs |
| `SELECT l_quantity FROM lineitem LIMIT 1` (1 of 16) | **110 926.8 µs** | 14.2 µs | 2 820.3 ms | 31.3 µs | 6.1 µs |
| `… l_quantity, l_discount, l_tax, l_extendedprice … LIMIT 1` (4 of 16) | **124 364.5 µs** | 16.6 µs | 2 885.0 ms | 46.0 µs | 8.2 µs |
| `SELECT COUNT(*) FROM lineitem WHERE l_quantity > 30` | **110 250.0 µs** | 186 092.8 µs | 3 062.6 ms | 40.2 µs | 186 516.6 µs |

**Both denominators, again, and one correction to pass 4's.** Over the two numbers the
engine prints: **2 515×** on `Plan:` (110 926.8 / 44.1), and **2 500-2 800×** against the
columnar leg's plan time for the same query. Over the whole process: +121.3 ms on
2 699.0 ms = **+4.5 %** for 1 kept column, +186.0 ms = **+6.9 %** for 4 — because ~2.7 s
of every run is `CSVLoader::load`, which the engine does not time.

The correction: pass 4 read "keeping 4 columns costs twice what keeping 1 costs" off the
wall deltas and called it the signature of pure overhead. The direct timer disagrees
about the factor — `Plan:` goes 110 926.8 → 124 364.5 µs, **+12 %** for 4× the kept
width, while the wall delta goes 121.3 → 186.0 ms (1.53×). So the cost is dominated by
the per-row rebuild, not by the width; it is O(rows) with a weak width term, not
O(rows × width). That makes it *worse* as a claim, not better — the price is paid in
full for keeping a single column — but pass 4's stated shape was not what the
instrument says.

The measurement-integrity half is unchanged and now shown rather than argued: for
`SELECT l_quantity FROM lineitem LIMIT 1` the row leg's `Execution:` is **14.2 µs versus
19.1 µs** for `SELECT *`. `benchmark.py` reads only that line, so it reports the query
that got 110 ms slower as **25 % faster**. **S-9: OPEN, unchanged.**

### S-12 — re-measured, TPC-H sf0.1

| query | cell | Sort node | Execution |
|---|---|---|---|
| `SELECT l_orderkey FROM lineitem ORDER BY l_orderkey LIMIT 5` | A row-volc | `Sort [l_orderkey] rows_in=600865 rows_out=5` **930 686 µs** | 1 001.1 ms |
| `SELECT l_orderkey FROM lineitem ORDER BY l_orderkey` | A row-volc | `rows_in=600865 rows_out=600865` 877 599 µs | 1 032.5 ms |
| `… ORDER BY l_orderkey LIMIT 5` | C col-vec | `VecSort [l_orderkey] rows_in=600865 **rows_out=600865**` | 1 061.7 ms |
| `… ORDER BY l_orderkey` | C col-vec | `VecSort rows_in=600865 rows_out=600865` | 1 098.5 ms |

Adding the `LIMIT` still changes the sort's cost by nothing, and the vectorized node
still prints `rows_out=600865` under a `LIMIT 5`. **S-12: OPEN, unchanged.**

A second, incidental corroboration turned up in the S-11 plans below: on the *same*
5 000-row join, the declared `ORDER BY b.id` sort (`row_cap=0`) costs **5 547 µs** and
the cut's keyless sort (`row_cap=1`) costs **237.5 µs** — 23×. Confounded (different
comparator, different input width), so pass 4's controlled 8.3× on one comparator and
one input remains the number to quote; this only shows the same miss from a second
angle.

### S-10 — re-measured, TPC-H sf0.01 (`lineitem ⨝ orders`, 60 144 rows)

| n | cell | Sort node | Execution |
|---|---|---|---|
| **0** | A row-volc | `rows_in=60144 rows_out=0` **20 133 µs** | 68.04 ms |
| **0** | B col-volc | `rows_in=60144 rows_out=0` **23 778 µs** | 86.29 ms |
| 1 | A row-volc | `rows_out=1` 2 792 µs | 48.78 ms |
| 1 | B col-volc | `rows_out=1` 2 781 µs | 60.69 ms |
| 5 | A row-volc | `rows_out=5` 2 842 µs | 51.13 ms |
| 5 | B col-volc | `rows_out=5` 2 756 µs | 59.79 ms |

Returning zero rows still costs **7.2× (row) / 8.5× (columnar)** more on the sort than
returning one. On the shipped catalog, `drivers ⨝ laps … LIMIT 0`:
**A 11.626 ms / B 11.993 ms / C 0.008 ms** — ~1 450×, same answer (0 rows) in all three.
**S-10: OPEN, unchanged.**

### S-11 — re-measured, plan shapes and the answerability flip

Plan shapes, same query with and without a redundant `ORDER BY`, row-volcano, 5 000-row
join:

    -- declared ORDER BY: Sort BELOW the projection
    Limit [1]  rows_in=1
      Project [v]   rows_in=1    rows_out=1
        Sort [b.id] rows_in=5000 rows_out=1   time=5547µs
          HashJoin  rows_in=5000 rows_out=5000

    -- deterministic cut: Sort ABOVE the projection
    Limit [1]  rows_in=1
      Sort [canonical row order] rows_in=5000 rows_out=1    time=237.5µs
        Project [v]              rows_in=5000 rows_out=5000
          HashJoin               rows_in=5000 rows_out=5000

and the answerability flip, `v = CASE WHEN b.c = 1 THEN b.big ELSE b.dd END`,
`b.big = 1000000000000007`, only the last of 5 000 rows taking the INT branch:

| query | A | A-noopt | B | B-noopt | C | C-noopt |
|---|---|---|---|---|---|---|
| `… ORDER BY b.id LIMIT 1` | `0` | `0` | `0` | `0` | `0` | `0` |
| `… LIMIT 1` | `0` | `0` | `0` | `0` | **REFUSED** | **REFUSED** |

Refusal text unchanged (*"vectorized execution cannot materialize the integer
1000000000000007 into a DOUBLE result column without changing it…"*). A and B still
agree in both storage formats either way, so the storage oracle is intact.
**S-11: OPEN, unchanged.**

---

## Part B — hunting what four passes missed

### B.1 The raiser × prunable battery — 144 queries on `laps`, **0 divergences**

Every combination of 18 leading conjuncts × 4 all-chunk-pruning conjuncts × both
written orders, `SELECT COUNT(*) FROM laps WHERE <R> AND <P>` and `<P> AND <R>`, across
all six invocations, compared against cell A:

    raisers   lap_id * 92233…807 > 0    lap_id + 92233…807 > 0   -lap_id * 92233…807 > 0
              speed                     sector_1                 speed * lap_id
              team > 5   team = 5   team < 5
              CASE WHEN speed THEN 1 ELSE 0 END = 1
              SUBSTRING(team, lap_id, 2) = 'x'
              lap_id * lap_id * lap_id * lap_id * lap_id > 0
    safe      lap_id > 1  team = 'Ferrari'  speed > 100.0  lap_id IN (1,2)
              team LIKE 'A%'  lap_id IS NULL
    pruners   lap_id > 999999   lap_id < -5   season > 2999   round = 99999

**144 queries × 6 invocations, 0 divergences.** The written-order rule holds in both
directions: a raiser first suppresses the skip everywhere (Error in all six), a pruner
first still prunes everywhere (0 rows / count 0 in all six).

Two of these are worth stating individually because they are the cases the walker's
structure could plausibly get wrong:

- `WHERE (speed > 1 OR team = 5) AND lap_id > 999999` — a raiser **inside a
  disjunction** correctly stops the walk: `Error: Type mismatch in Value comparison` in
  all six. `exprMayRaise` recurses into both arms of the OR.
- `WHERE (lap_id > 1 OR lap_id > 2) AND lap_id > 999999` — a *total* disjunction does
  **not** stop it: count 0 in all six with the skip still taken. So the OR handling is
  precise rather than uniformly conservative.
- `NOT (…)` is a parse error in this dialect (`NOT is supported only as NOT BETWEEN,
  NOT LIKE, NOT IN or IS NOT NULL`), so the `UnaryExpr`-wrapping-an-`AND` shape is
  unreachable.

### B.2 The join battery — 64 queries, **6 divergences, all of them S-13**

Same construction over the synthetic 3-chunk `big`, with the leading conjunct drawn from
three groups: scan-local raisers, cross-relation conjuncts on a **same-typed** shared
name (`small3.k`, `small3.j` — both INT, as in `big`), and cross-relation conjuncts on a
**differently-typed** shared name (`small2.x` STRING vs `big.x` INT).

| leading conjunct group | queries | divergences |
|---|---|---|
| scan-local raiser (`b.k * 92233…807 > 0`, …) | 24 | **0** |
| cross-relation, same-typed name (`s.k > 5`, `s.j = 3`, `s.k = 50001`) | 24 | **0** |
| cross-relation, differently-typed name (`s2.x`, `s2.x > 5`) | 16 | **6** |

The six are exactly S-13, and they reproduce over three different all-chunk-pruning
conjuncts (`b.k > 999999`, `b.k < -1`, `b.x > 99`). **An aggregate does not contain the
damage — it converts it into a number:**
`SELECT COUNT(*) FROM big b JOIN small2 s2 ON b.j = s2.j WHERE s2.x AND b.k > 999999`
gives `Error` in A and **`COUNT(*) = 0`** in B and C. `GROUP BY` likewise returns an
empty grouped result where row storage errors. That is the sharpest statement of S-13's
severity: it is not only a suppressed error, it is a printed wrong value.

### B.3 S-13's boundary — an OUTER join does **not** have it, and that pins the fix

    SELECT COUNT(*) FROM big b LEFT JOIN small2 s2 ON b.j = s2.j WHERE s2.x AND b.k > 999999
    SELECT COUNT(*) FROM small2 s2 LEFT JOIN big b ON b.j = s2.j WHERE s2.x AND b.k > 999999

Both give `Error: std::get: wrong index for variant` in **all six**. Week 29's
`pruningHintForPreservedSide` withholds the hint outright when the join is not INNER and
the hint mentions an unpreserved slot, so the cross-relation conjunct never reaches the
pruner at all. S-13 lives entirely in that function's first line —
`if (!hint || join_type == JoinType::INNER) return hint;` — which hands an INNER join's
hint through **without looking at slots**. Tightening *that* line is not the fix
(withholding the whole hint whenever any conjunct is cross-relation would cost more
pruning than S-14 already does); the fix is still to give the pruner the schema, so it
can tell a cross-relation conjunct apart from a scan-local one instead of guessing from
a name.

### B.4 Invariant 12 — the wrong relation's zone maps, re-tested with a construction that would collapse the answer

`small3(j INT, k INT)` shares the name `k` with `big` and its values (50 000-50 009) sit
entirely **outside** `big.k`'s zone maps (0-19 999). If the collector's
`isLocal() && localSlot(...) < 1` guard leaked, `WHERE s.k = 50001` would be matched
against `big`'s zone maps by name and skip every chunk, collapsing the count to 0:

| query | A | A-noopt | B | B-noopt | C | C-noopt |
|---|---|---|---|---|---|---|
| `SELECT COUNT(*) FROM big b JOIN small3 s ON b.j = s.j WHERE s.k = 50001` | 2000 | 2000 | 2000 | 2000 | 2000 | 2000 |
| `… WHERE s.k > 49999` | 20000 | 20000 | 20000 | 20000 | 20000 | 20000 |

**Guard holds.** The slot test still does its job; what S-13 shows is that the *screen*
in the same function never got the same guard.

### B.5 Correlated refs cannot reach the pruner at all

The Week-33 decline is now moot for this seam on the Volcano side and refused on the
vectorized side:

    SELECT s.j FROM small s WHERE (SELECT COUNT(*) FROM big b WHERE s.x AND b.k > 999999) = 0
      A, B  Error: correlated subqueries are decorrelated to a semi-join and are not
            supported on the Volcano path; use --execution vectorized
      C     Error: correlated subquery: only an equality between two columns can become
            a join key …

So the correlated door onto S-13 is shut by two independent refusals, not by the
pruner's decline. Recorded so a future round that opens either refusal knows it inherits
S-13.

### B.6 Value fidelity, scan order, loading — re-confirmed byte-exactly

- **Full-width fidelity.** `SELECT * FROM laps ORDER BY lap_id, driver_id, speed`
  (10 000 rows × 9 columns, INT/DOUBLE/STRING, RLE on `season` and dictionary on `team`)
  gives **md5 `9b1a8d4d614ff1abea5e49ebedffdba3` in all three cells**. `SELECT * FROM
  drivers` likewise identical.
- **Scan order.** The same query with **no** `ORDER BY`, all 10 000 rows, `diff`s clean
  between A and B. Order is identical, not merely a permutation.
- **Loading.** Unchanged and still structurally single-sourced: `CSVToColumnar::convert`
  is the only `ColumnarTable` producer and `parseField` still has no NULL production, so
  NULL *representation* remains untestable across storage — established in pass 2 B.2 /
  C.3 and not re-derived here.

### B.7 The refusals still widen the oracle rather than escaping it

`--storage row --execution vectorized` is still refused for every query
(`Error: --execution vectorized requires --storage columnar`), so there are still three
cells. S-11's shape is the live confirmation of pass 4's A.3.1: cell C refuses
`SELECT v … LIMIT 1` while A and B both answer `0`. Every shape the round-4 refusals
push off the vectorized path lands in Volcano, which is the one engine that runs both
storage formats — so the refusals hand this seam *more* jurisdiction, not less.
**Confirmed, still holds.**

### B.8 L-1, L-2, L-3 — re-verified at HEAD

- **L-1** live: a duplicate column name is still refused —
  `catalog: table 'laps': column 'lap_id' is declared twice; give one of them a distinct
  name` — in both storage modes, and it is still the only thing keeping `narrowRows`'
  `full.indexOf(c.name)` from putting one index into `keep` twice.
- **L-2** live: a catalog with two tables named `laps` (the second pointing at
  `drivers.csv`) silently keeps the first — `COUNT(*) = 10000` in **all three cells**.
  Catalog input validation, identical across storage, not a divergence.
- **L-3** live: `narrowRows`' `if (narrowed.size() == full.size()) return rows;` is
  still correct only because `narrowSchema` builds an order-preserving subset, and
  nothing states or tests that coupling.

---
