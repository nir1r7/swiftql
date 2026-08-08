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
