# Week 37 — TPC-H Benchmarks + Final Documentation

**Checkpoint:** TPC-H results are reproducible and the full project story is
documented. ✅

The measurements themselves are in
[week-37-measurements.md](week-37-measurements.md) (every run, including the
ones that are not results), [week-37-per-query.md](week-37-per-query.md) (all 22
queries × 5 engines × 3 scale factors) and
[week-37-engine-comparison.md](week-37-engine-comparison.md) (methodology). This
file is the week's own account: what was planned, what actually happened, and
which of the two was wrong.

---

## The week in one paragraph

The plan was to measure. Measuring first found that **every benchmark this
project had ever published was taken on an unoptimized binary**, and that
**three of the six blocking operators were mis-reporting their own time**, so
neither the numbers nor the profiler could be trusted. Both were fixed before
anything was measured for real. The measurements then said SwiftQL was 1.52×
slower than PostgreSQL at SF=1 — which turned out to be measuring PostgreSQL's
core count rather than its engine. Two controls separated that, six
optimizations closed the rest, and the final position is **faster than SQLite
and PostgreSQL at every scale factor tested**, with DuckDB still 15× ahead on 14
cores.

---

## 1. Two defects that invalidated the deliverable before it started

### `CMakeLists.txt` set no default build type

CMake passes **no `-O` flag at all** when `CMAKE_BUILD_TYPE` is empty, and it was
empty. Same commit, same data, `dbgen-sf0.01`:

| query | no `-O` | `-O3 -DNDEBUG` | ratio |
|---|---|---|---|
| q1 | 259.8 ms | 9.2 ms | 28.2× |
| q9 | 506.6 ms | 24.7 ms | 20.5× |
| q18 | 1568.4 ms | 72.6 ms | 21.6× |
| q21 | 1314.7 ms | 59.5 ms | 22.1× |

A ~21× factor with no source change behind it. It inverted the headline: at `-O0`
SwiftQL measured 6.31× slower than SQLite; the same code at `-O3` is 1.9× faster.

Nothing about this was visible. `cmake ..` succeeds, the tests pass, and the only
evidence is an absent flag in `compile_commands.json`. **A default that must be
remembered is a default that will be forgotten**, so `CMakeLists.txt` now sets
Release explicitly and `compare_engines.py` refuses to run against a binary that
is not an optimized build.

### `--explain-analyze` was not reporting exclusive time

README:537 documents per-node **exclusive** self-time. `VecSortNode`,
`VecDistinctNode` and `VecDerivedNode` all started their timer before draining
the child, charging their whole subtree to themselves. `VecHashJoinNode`,
`VecHashAggregateNode` and `VecSimdLoopJoinNode` were already correct.

On q18 the `VecSort` line read **42.4 ms (64.8% of execution)** and now reads
**3.0 µs (0.0%)** — which is what sorting 2 rows costs. Before the fix the
profile named the sort as the hot node on **every** `ORDER BY` query and hid the
join beneath it.

**This week's deliverable is per-node profiling.** Fixing the instrument had to
come before using it, and every profile taken before this point is wrong.

---

## 2. What the correctness figure actually is

| dataset | modes | result |
|---|---|---|
| `sf0.01` (seeded generator, the gated set) | 4 | 21/22 meaningful, q18 vacuous |
| `dbgen-sf0.01` | 4 | 20/22 meaningful, q16 + q17 vacuous |
| **`dbgen-sf0.1`** | 2 | **22/22 meaningful, 0 vacuous** |
| **`dbgen-sf1`** | 2 | **22/22 meaningful, 0 vacuous** |

Three things worth stating plainly:

**Real `dbgen` data closed q18's vacuity, which the seeded generator could not.**
The README argues at length that lowering `SUM(l_quantity) > 300` would invent a
value the spec does not contain, and it was right to refuse. On `dbgen` data the
spec's own 300 discriminates.

**q16 and q17's vacuity at SF=0.01 is a scale artifact, not a parameter one.**
Both already use spec-literal parameters; 100 suppliers and 2 000 parts are
simply too few for `%Customer%Complaints%` and `Brand#23 / MED BOX` to bite. Both
recover at SF=0.1.

**22/22 is two modes, not four**, and the verdict line says so. `Planner::plan`
builds exactly one join, so 17 of the 22 refuse on the Volcano paths by design.
A count without that split reads as coverage the engine does not have.

---

## 3. The comparison, and the two controls that changed what it means

Geometric mean of SwiftQL ÷ engine; **below 1.0 means SwiftQL is faster.**

| | SF=0.01 | SF=0.1 | SF=1 |
|---|---|---|---|
| vs **SQLite** | 0.74× | 0.37× | **0.33× — 3.0× faster** |
| vs **PostgreSQL** (default) | 0.29× | 0.65× | **0.87× — 1.15× faster** |
| vs **PostgreSQL** (single-threaded) | — | — | **0.49× — 2.0× faster** |
| vs **DuckDB** | 1.20× | 4.70× | 15.37× |
| optimizer | 1.69× | 1.92× | **2.22×** |

SQLite and PostgreSQL carry the TPC-H specification's own PK/FK indexes (from
`dbgen/dss.ri`) plus `ANALYZE`. That index set was chosen because it is
**query-blind** — fixed by the benchmark's authors before we picked a query —
where an index set chosen by reading the 22 queries would be tuning, and tuning
one engine and not the others voids the comparison.

### Control 1 — PostgreSQL was using three processes

The first SF=1 result was **1.52× slower than PostgreSQL**, and the write-up
attributed it to "PostgreSQL's planner and buffer manager scale better". That was
not a safe claim: the harness set no GUCs, PostgreSQL defaults to
`max_parallel_workers_per_gather = 2`, and every TPC-H table over 8 MB qualifies.

Re-run with parallelism disabled, **PostgreSQL's own parallel speedup is 1.73×**
— which is essentially the whole gap. Both numbers are published: single-threaded
is the engine-against-engine comparison, default is what anyone actually runs,
and quoting either alone is cherry-picking.

### Control 2 — without indexes the row stores fall apart, unevenly

| query | SwiftQL | SQLite no-index | PostgreSQL no-index | PostgreSQL indexed |
|---|---|---|---|---|
| q22 | 107 ms | **413 273 ms** | 283 ms | 24 ms |
| q19 | 379 ms | 3 827 ms | 214 ms | 13.6 ms |
| q18 | 2 718 ms | 1 998 ms | 1 315 ms | 802 ms |

The first pass reported "neither engine finished q17, q21 or q22 in 600 s" — but
that cap covered a warmup plus three repetitions, so it bounded *four*
executions, not one. Re-measured at one execution, the two engines turn out to be
nothing alike: **SQLite takes 6.9 minutes on q22 where PostgreSQL takes 283 ms**.
The coarse claim would have been directionally right and specifically wrong,
which is why the single-execution bound was worth running.

**q18 is the counterexample and is published as one.** Unindexed PostgreSQL still
beats SwiftQL, 1 315 ms against 2 718 ms. That loss is engine quality, not
access-path asymmetry.

---

## 4. Six optimizations

Each measured separately; all 22 answers byte-identical throughout.

| change | effect |
|---|---|
| **Narrow scan schemas under a subquery.** `buildScanSchema` returned the full schema for any statement containing one — a single line disabling column pruning for every scan in the block. Now narrows to the block's columns unioned with the outer columns nested bodies correlate on | q18 2.55×, q21 2.00×, q20 1.78×. q21's scans go 7/16/9/4 columns → 3/4/2/2 |
| **Enumerate the spine below a semi/anti join.** One semi-join turned join ordering off for the entire query; it is now a spine boundary rather than a global veto | q21 1.47×; largest estimated intermediate 300 286 → 12 011 rows |
| **Derive single-relation restrictions from an OR.** q19's whole `WHERE` is one OR conjunct, so nothing was pushed — `opt = 1.00×` at every scale was the fingerprint | **q19 7.1×**; lineitem into the join 600 572 → 25 402 rows |
| **Share `ColumnarTable` instead of copying per scan** | plan time only; q21 paid 344 ms at SF=1 for two copies |
| **Semi/anti join keys into the arena + chained index**, replacing `unordered_set<std::string>` | −27 ms over 22 queries |
| **INT64 join key path**, replacing decimal-text keys for single-INT joins | −91 ms over 22; q18 −23.6, q21 −18.7 |

Together these moved SF=1 from **1.52× slower than PostgreSQL to 1.15× faster**.

---

## 5. Three things that were wrong, kept in the record

**A design specified in this week's own brief was measured and rejected.** An
inline `{uint64 key, int32 row}` bucket array for the join cost 60.3 ms against
the chained index's 44.1 ms on q9 — 16-byte slots at a 0.5 load factor make the
table 4× larger and the build pays for that in full. Reusing the existing chain
gives 38.7 ms, better than both.

**Bloom filter pushdown missed the queries it was chosen for.** It was picked
because the join family is the top node in 8 of the 10 queries losing to
PostgreSQL. Of the six named targets only q20 moved; the real wins were q8
(−55%) and q3 (−35%), queries SwiftQL was already winning. The prediction was
wrong and is recorded as wrong rather than re-framed around what it did help.

**Clustering was measured and rejected, not deferred.** The obvious columnar
answer to "no indexes" is to sort at load so zone maps prune. Measured: every
8192-row chunk's `l_shipdate` zone map spans the **entire 7-year corpus**, so
`ChunkPruner` skips zero chunks on every date, quantity, discount, brand and
shipmode predicate. Sorting would help q6, q12, q14 and q1 — every one of which
SwiftQL already wins — and would cost the `l_orderkey` clustering. `CHUNK_SIZE`
is irrelevant for the same reason: no chunk size prunes a chunk whose min/max is
the whole domain.

---

## 6. Estimate accuracy

276 plan nodes, `python_tools/qerror_tpch.py`:

| | SF=0.1 | SF=1 |
|---|---|---|
| geometric mean | 4.52 | 6.59 |
| median | 1.59 | 1.86 |
| within 2× / 10× / 100× | 52% / 77% / 92% | 51% / 75% / 83% |

Half of all estimates land within 1.6–1.9×, which is respectable for a
histogram-free estimator. **The tail is where it fails and it degrades with
scale** — the 90th percentile moves 100× → 533× — which is the independence
assumption compounding along a join spine, exactly as Limitations predicts.

One systematic defect, already named in the README's Week 37 starting note and
now measured: **semi/anti joins estimate 1 row.** q16 estimates 1 against an
actual 11 635, q21 estimates 1 against 465. The cause is recorded — the body
plan's `LogicalProject` empties its `StatsContext` and the right-side NDV lookup
misses.

This measures estimate accuracy, not plan quality. A node can be 100× out and
still produce the right plan; the optimizer's 1.69–2.22× measured gain is the
evidence that ranking still works.

---

## 7. Carried forward

**The remaining gap to PostgreSQL is an access-path gap, and closing it means
admitting an index.** Three read-only analyses converged on the same arithmetic:
sharing tables, narrowing scans and the OR derivation together reach roughly
parity, and the residual (q17, q19, q13, q9) is lost to one thing — PostgreSQL
reaches a small number of rows through an index while SwiftQL reaches them by
touching all six million. Min/max metadata cannot close that, because it prunes
only contiguous ranges and the qualifying rows are scattered. A sorted
key-to-row permutation built once at load would close it and is far cheaper than
a B-tree, but it *is* an index, and whether that is inside or outside the
project's stated position is a decision rather than an optimization.

**Parallelism is the whole DuckDB gap.** 1.20× → 4.70× → 15.37× across two orders
of magnitude, with SwiftQL winning 9 of 22 at SF=0.01 and none above it. 14 cores
against one. It remains a Possible Extension.

**Three key encoders now exist where there was one**, each added for a measured
reason and each required to stay byte-identical.
`tests/test_key_encoding_parity.cc` pins that, mutation-verified — but the
duplication itself is standing risk, and this codebase's most productive bug
class is code trusting a rule that has since changed elsewhere.

**Owed and not done:** a single-execution bound for q17 and q21 unindexed (q22's
landed and is above), and the Week 12/15 F1 phase-comparison tables, which were
never measured and are queued rather than filled.
