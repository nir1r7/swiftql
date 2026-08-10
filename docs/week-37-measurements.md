# Week 37 — measurement log

Every measurement taken this week, including the ones that are not results.
Latency comparisons live in
[week-37-engine-comparison.md](week-37-engine-comparison.md); this file is the
record of what was run, on what, and what came back.

Machine: Apple silicon, 14 cores, 24 GB, macOS. All SwiftQL figures are from a
Release build (`-O3 -DNDEBUG`) unless a row says otherwise.

## 1. Correctness — SwiftQL vs SQLite

`python_tools/run_tpch.py`. "Meaningful" means matched SQLite **and** survived
the mutation check (neutering the query's characteristic predicate changes its
answer). Vacuous means matched while asserting nothing.

| dataset | modes | result | vacuous | wall |
|---|---|---|---|---|
| `sf0.01` (seeded generator, gated) | 4 | **21/22 meaningful** — 5 in all four modes, 16 vectorized-only | q18 EMPTY | ~3 min |
| `dbgen-sf0.01` | 4 | 20/22 meaningful | q16 INERT, q17 ALL_NULL | 3 min 15 s |
| `dbgen-sf0.1` | 2 (col-vec, col-vec-noopt) | **22/22 meaningful, 0 vacuous** | none | **177 s** |
| `dbgen-sf1` | 2 | **22/22 meaningful, 0 vacuous** | none | 19 min |

### Three findings from that table

**Real dbgen data closes q18's vacuity, which the synthetic generator could not.**
q18's `SUM(l_quantity) > 300` is unreachable on the seeded generator's data (max
per-order sum 295.0), and the README argues at length that lowering the
threshold would invent a value the spec does not contain. On dbgen data the
spec's own 300 discriminates: 2 rows → 100 under mutation.

**q16 and q17's vacuity at `dbgen-sf0.01` is a SCALE artifact, not a parameter
one.** Both already use spec-literal parameters. At SF=0.01 there are 100
suppliers and 2 000 parts, too few for `%Customer%Complaints%` and
`Brand#23 / MED BOX` to bite. **At SF=0.1 both recover**, which is what takes the
figure to 22/22.

**22/22 meaningful with 0 vacuous at SF=0.1 AND SF=1 is the strongest
correctness result this project has.** It is two modes, not four — `Planner::plan` builds exactly
one join, so 17 of the 22 refuse on the Volcano paths by design — and the
verdict line says so rather than letting 22/22 read as full coverage.

## 2. The `-O0` build

`CMakeLists.txt` set no default `CMAKE_BUILD_TYPE`, so CMake passed no `-O` flag
and every benchmark this project published before Week 37 was taken on an
unoptimized binary. Same commit, same data, `dbgen-sf0.01`:

| query | no `-O` | `-O3 -DNDEBUG` | ratio |
|---|---|---|---|
| q1 | 259.8 ms | 9.2 ms | 28.2× |
| q9 | 506.6 ms | 24.7 ms | 20.5× |
| q18 | 1568.4 ms | 72.6 ms | 21.6× |
| q21 | 1314.7 ms | 59.5 ms | 22.1× |

Verified by reading the compile line out of each tree's
`compile_commands.json`, not inferred. Both binaries return byte-identical
answers. Release timings are repeatable within ~3–7% over 5 runs.

**Fixed**: `CMakeLists.txt` now defaults to Release, and `compare_engines.py`
refuses to run against a binary that is not an optimized build.

## 3. Operator optimizations

Three agents, disjoint files, each required to keep all 22 answers
byte-identical. Measured at `dbgen-sf0.01`.

| change | before → after |
|---|---|
| Hash join: `unordered_map<string, vector<Row>>` → column-wise row store + chained index, late-materialized output | q18 63.9 → 19.7 ms; q21 58.7 → 18.3 ms; q9 23.2 → 7.7 ms; q7 17.4 → 7.1 ms |
| Aggregate: key serialized from the column into a reused buffer; `group_order_` folded away; function dispatch hoisted | isolated grouping 4.4 → 1.5 ms / 60 k rows (74 → 25 ns/row); q1 9.1 → 5.8 ms |
| Sort: ORDER BY keys evaluated once per row instead of per comparison | `VecSort` self-time q3 908 → 107 µs; q10 3459 → 370 µs |

Post-change gates, all green: 973 unit tests, 1796 SQLite-oracle queries, 345
regression queries, `GATE tpch: PASS (21/22 meaningful, 0 unported)`.

ASan found one real bug the 973 tests did not: `build_cols_` indexed while empty
when a LEFT join's build side contributed no rows. Harmless in practice,
undefined behaviour, silent in Release.

## 4. Profiler instrumentation

README:537 documents `--explain-analyze` as per-node **exclusive** self-time.
Three nodes violated it — `VecSortNode`, `VecDistinctNode`, `VecDerivedNode` —
by starting their timer before draining the child, charging their whole subtree
to themselves. `VecHashJoinNode`, `VecHashAggregateNode` and
`VecSimdLoopJoinNode` were already correct.

Fixed. On q18 the `VecSort` line went **42.4 ms (64.8%) → 3.0 µs (0.0%)**, which
is what sorting 2 rows costs. Before the fix the profile named the sort as the
hot node on every ORDER BY query and hid the join beneath it.

## 5. Oracle scalability — the SF≥0.1 blocker, and its fix

The SQLite mirror in `compare_against_sqlite.py` was built with **no indexes and
no statistics**. Consequences, all measured:

| symptom | cause | fix |
|---|---|---|
| q21 took 19.7 s at SF=0.01 | correlated `NOT EXISTS` as a nested scan | index every `*key` / `*id` column → whole q21 run 2.3 s |
| SF=0.1 correctness killed after 55 min, still inside q21 | same | same; SF=0.1 now completes in 177 s |
| SF=1 q5 ran 28 min without finishing | indexes present but no `sqlite_stat1`, so a six-table join order chosen from static heuristics | `ANALYZE` after load → **q5 3.1 s** |

Load + index + ANALYZE at SF=1 is a 36.2 s one-off. Neither an index nor
`ANALYZE` can change an answer — they change which plan produces it.

**The ceiling was the oracle, not the engine, at every step.** SwiftQL answers
q21 at SF=0.01 in 18.6 ms against SQLite's 19.7 s.

## 6. Indexing the competitors, and what it cost the headline

The latency harness originally built SQLite, Postgres and DuckDB with **no
indexes**, on the reasoning that SwiftQL has none either. That is defensible as
a like-for-like statement about capability and indefensible as a benchmark: no
one runs a row store that way, and a reviewer will assume the competitor was
configured properly.

**Index set: exactly the PRIMARY and FOREIGN keys `dbgen/dss.ri` declares**,
transcribed from the official V3.0.1 kit. Not chosen by reading the 22 queries --
that would be tuning, and tuning one engine and not the others voids the
comparison. The spec's set is query-blind (fixed before we picked a query),
authored by the benchmark's own authors, and is what a DBA creates first.
Composite keys keep the spec's column order. SQLite and Postgres get them plus
`ANALYZE`; DuckDB and SwiftQL do not, because both prune with automatic min/max
zone maps rather than user-declared indexes.

The effect at `dbgen-sf0.01`, geometric mean against SwiftQL:

| | unindexed | indexed |
|---|---|---|
| vs SQLite | 0.30x (SwiftQL 3.3x faster), 19/22 wins | **1.07x -- a tie**, 9/22 wins |
| vs Postgres | 0.31x (3.2x faster), 19/22 | **0.47x (2.1x faster)**, 17/22 |
| vs DuckDB | 1.67x slower | 1.75x slower |

**The single largest change is q21**: SQLite goes from 19.7 s to 0.8 ms, because
the correlated `NOT EXISTS` stops being a nested scan. Almost the whole
"SwiftQL beats SQLite" result at SF=0.01 was that one query, and it was an
artifact of configuration rather than of engine design.

Read this as the honest baseline: **at 60 000 rows, index point-lookups beat
columnar scans, and SwiftQL ties SQLite.** Whether a scan-based engine pulls
ahead as the data grows is what SF=0.1 and SF=1 exist to answer, and it is now a
real question rather than a formality.

### Optimizer impact (same run, `swiftql` vs `swiftql-noopt`)

**1.88x geometric mean.** Largest on q8, q21, q2, q9 — the queries whose shape
the planner rewrites (derived table, correlated `NOT EXISTS` → anti-join,
correlated scalar). q21 is 170.2 ms unoptimized against 18.1 ms optimized.
Several queries (q13, q18, q19, q20, q22) show no measurable difference.

## 7. Latency across three scale factors — the trend is the result

Five engines, indexed, `python_tools/compare_engines.py`. Geometric mean of
SwiftQL against each; below 1.0 means SwiftQL is faster.

| | SF=0.01 | SF=0.1 | SF=1 |
|---|---|---|---|
| vs **SQLite** | 1.07x (tie) | 0.79x | **0.66x — 1.5x faster** |
| vs **Postgres** | **0.47x — 2.1x faster** | 1.16x | 1.70x — 1.7x slower |
| vs **DuckDB** | 1.75x slower | 8.26x slower | **29.9x slower** |
| optimizer (`noopt`/`opt`) | 1.88x | 2.00x | **2.08x** |
| queries won vs SQLite | 9/22 | 11/21 | 11/21 |

**Three trends, and only one of them was predicted.**

1. **SwiftQL gains on SQLite as scale grows** (1.07 -> 0.79 -> 0.66). Predicted,
   and the mechanism is visible per query: scan-and-hash is linear in rows while
   SQLite's per-row index traversal degrades on large joins. At SF=1 the biggest
   wins are q14 **69.6x**, q15 **67.0x**, q5 17.7x, q7 16.6x, q8 10.9x.
2. **SwiftQL LOSES ground to Postgres** (0.47 -> 1.16 -> 1.70). Not predicted.
   Postgres was 2.1x behind at SF=0.01 and is 1.7x ahead at SF=1. Its planner and
   buffer manager scale better than a single-threaded scan does.
3. **The DuckDB gap widens by ~17x across two orders of magnitude** (1.75 ->
   29.9), and SwiftQL wins zero queries at SF>=0.1. This is parallelism: DuckDB
   uses 14 cores, SwiftQL uses one. It is the clearest argument that parallel
   scan and aggregation is the highest-value extension remaining.

**The optimizer's contribution GROWS with scale** — 1.88x, 2.00x, 2.08x — and it
is the part of the system that is unambiguously working. At SF=1 the largest
gains are q8 **13.7x**, q21 **7.3x**, q10 4.5x, q14 3.8x, q2 3.6x: the
decorrelation and derived-table rewrites, paying off exactly where a bad plan
costs most.

### Where SwiftQL loses at SF=1, by cause

| queries | cause |
|---|---|
| q13, q18 | Planner-level. q13 hits the **forced outer-join build side** (the preserved side must probe, so the null-supplying relation is hashed however large it is — Week 29 deferred this pending a measurement; this is that measurement). q18 carries columns nothing downstream reads — **no projection pushdown through the join output**. Optimizer impact on both is nil (0.99x, 1.02x), consistent with join ordering being disabled for outer joins |
| q17, q4, q11, q20 | Access path. A selective predicate on a key column is a B-tree point lookup for SQLite/Postgres and a full scan for SwiftQL, which has no index of any kind — only `ChunkPruner`'s min/max zone maps over 8192-row chunks |
| q9 | Large multi-way join; the remaining hash-join materialization cost |

### A correctness divergence the latency harness found

**q15 returns 1 row on SwiftQL, SQLite and DuckDB, and 0 rows on Postgres**, at
both SF=0.1 and SF=1. Its `WHERE total_revenue = (SELECT MAX(total_revenue))` is
an exact equality on a **computed DOUBLE**: `SUM(l_extendedprice * (1 -
l_discount))` accumulates in a different order under Postgres, so the row that
should equal the maximum differs in its last bits. The other three agree by
coincidence of summation order, not by contract.

q15 is **excluded from every geometric mean above** — comparing latencies for
different answers is meaningless. It is a property of the query, not a defect in
any engine.

## 8. Bloom filter join pushdown — a modest win, honestly measured

Built because profiling the 10 queries losing to Postgres at SF=0.1 showed the
join family is the top node in 8 of 10. The join builds a Bloom filter from its
build-side keys and pushes it into the probe side's scan, so rows that cannot
match are dropped before reaching the join.

**Applied only to INNER and SEMI joins.** A Bloom filter has false positives but
no false negatives, so "reject" means "certainly no match" -- safe only where a
non-matching probe row produces no output. LEFT joins must still emit
null-extended rows and ANTI joins return exactly the non-matching rows. With the
gate deliberately forced open, q13 returns 38 rows instead of 39 and q22 returns
7 instead of 9. Three unit tests pin each excluded semantics.

### Result at SF=0.1 (independent re-measurement, median of 5)

| improved | | regressed | |
|---|---|---|---|
| q8 | **-55.1%** | q11 | +2.9% |
| q3 | **-35.3%** | q7 | +2.5% |
| q5 | -22.5% | q19 | +2.3% |
| q20 | -20.5% | q9 | +1.9% |
| q2 | -10.5% | q22 | +1.6% |
| q10 | -9.7% | q21 | +1.3% |
| q16 | -7.7% | q17 | +1.2% |
| q12 | -6.9% | q13 | +1.0% |

**Geometric mean 0.913x (8.7% faster). Sum of all 22: 1158.0 -> 1138.2 ms, only
-1.7%.** The two figures disagree because the slowest queries -- q18, q21, q17,
q13 -- are not join-selectivity-bound and do not benefit. Eight queries regress
by 1-3%: the cost of testing the filter on scans where it rejects little.

**This did NOT close the Postgres gap.** The six queries named as the worst
Postgres losses were q4, q9, q18, q19, q20, q21, and only q20 moved. The real
wins landed on queries SwiftQL was already winning. Recorded as-is rather than
re-framed: the prediction that this would target the Postgres losses was wrong.

### Three failures on the way, each fixed by measurement

1. **The first version was a uniform loss** -- 10 of 22 slower, q3 +22%. Per-probe-row
   key serialization cost ~13 ns, so q3's scan grew 2.6 -> 10.4 ms to save 8.2 ms
   below it. Fixed with an INT64 key mode for single-INT-column joins.
2. **Scans rejecting nothing were pure overhead.** Now the filter samples its first
   8192 rows and abandons itself below 1/8 rejection; `--explain-analyze` prints
   `bloom=gave_up`. q9 went +17% -> +2%.
3. **A never-taken branch cost 5-8 ms of a 36 ms query** (q4). Moved out of the
   semi-join build loop.

### A correctness bug the SQLite oracle caught

Forwarding the filter through `VecFilterNode` unconditionally turned an
`integer overflow in '*'` error into a 0-row answer: the Bloom filter rejected
every row before the overflow conjunct was evaluated. Fixed by forwarding only
when the predicate cannot raise -- the same `conjunctMayRaise` rule
`ChunkPruner::shouldSkip` already uses. `VecLimitNode` deliberately does not
forward at all, since dropping rows below a LIMIT changes which rows survive it.

Gates after the change: **992 unit tests** (973 + 19 new), 1796 oracle, 345
regression, `GATE tpch: PASS (21/22 meaningful, 0 unported)`.

## 9. Two controls that change what the Postgres comparison MEANS

The headline "SwiftQL is 1.52x slower than Postgres at SF=1" turned out to be
measuring two things that are not engine quality. Both were isolated.

### 9a. Postgres was using three processes; SwiftQL uses one

`compare_engines.py` set no Postgres GUCs, so Postgres ran at its defaults:
`max_parallel_workers_per_gather = 2` and `min_parallel_table_scan_size = 8 MB`.
At SF=1 five of the eight TPC-H tables exceed 8 MB, so Postgres was very likely
running the large queries on a leader plus two workers.

Re-run with `SET max_parallel_workers_per_gather = 0` (applied per connection,
BEFORE the timer, since it configures the engine rather than doing the query's
work):

| SF=1, 21 queries | geomean | SwiftQL wins |
|---|---|---|
| vs Postgres **default** (parallel) | 1.52x -- SwiftQL slower | 9/21 |
| vs Postgres **single-threaded** | **0.88x -- SwiftQL 1.14x FASTER** | 10/21 |

**Postgres's own parallel speedup is 1.73x geomean** (q10 3.6x, q12 3.0x,
q1 2.9x, q14 2.7x, q6 2.7x, q4 2.5x). So essentially the entire gap previously
attributed to "Postgres's planner and buffer manager scale better" is **core
count**.

**Both numbers belong in the paper.** Single-threaded is the fair
engine-against-engine comparison; default is the configuration anyone actually
runs. Quoting either alone is cherry-picking, in one direction or the other.

### 9b. Without indexes, the row stores cannot finish three of these queries

Same data, `--no-index` (no PK/FK indexes, no ANALYZE), one query per
invocation, each capped at 600 s covering 1 warmup + 3 repetitions:

| query | SwiftQL | SQLite no-index | Postgres no-index | Postgres indexed |
|---|---|---|---|---|
| q17 | 1367 ms | **did not finish** | **did not finish** | 49 ms |
| q21 | 2566 ms | **did not finish** | **did not finish** | 666 ms |
| q22 | 107 ms | **413 273 ms** (one execution) | 283 ms | 24 ms |
| q19 | 379 ms | 3827 ms | 214 ms | 13.6 ms |
| q18 | 2718 ms | 1998 ms | 1315 ms | 802 ms |
| q20 | 45 ms | 49 ms | 27 ms | 37 ms |
| q13 | 1739 ms | 4223 ms | 252 ms | 319 ms |

**Precision about "did not finish":** that cap covered a warmup plus three
repetitions, i.e. up to four executions. It proved *four executions exceed
600 s*, not that one does.

**Re-run at `--reps 1 --warmups 0` with a 3600 s cap, and the refinement matters
— the two engines are NOT alike here.** On q22, one execution measures
**SQLite 413 273 ms (6.9 minutes)** against **PostgreSQL 282.6 ms**. So the
earlier "neither finished" was an artifact of timing four executions of a
7-minute query: PostgreSQL without indexes handles q22 perfectly well, and
SQLite without indexes does not. Against SwiftQL's 107 ms that is a **3 860x**
gap to SQLite and a 2.6x gap to PostgreSQL.

The earlier row is corrected accordingly. This is exactly why the
single-execution bound was worth running rather than quoting the cap.

Three readings, and the third is the one that keeps this honest:

- **q17, q21, q22 are queries SwiftQL "loses" that the row stores can only win
  BECAUSE of an index.** Remove the index and they do not produce an answer at
  all in the time SwiftQL takes 0.1-2.6 s. The decorrelation-to-anti-join and
  semi-join rewrites are doing work a nested scan cannot.
- **q19 confirms the diagnosis independently.** Postgres indexed 13.6 ms,
  unindexed 214 ms -- a **16x** difference. Its win is the index, and SwiftQL at
  379 ms is within 1.8x of unindexed Postgres.
- **q18 is the counterexample and should be published as one.** Unindexed
  Postgres still beats SwiftQL, 1315 ms against 2718 ms. That loss is genuine
  engine quality, not access-path asymmetry.

### What the three controls, together, license

| claim | basis |
|---|---|
| SwiftQL is **1.7x faster than SQLite** at SF=1 | indexed, 21 queries, geomean 0.59x |
| SwiftQL is **1.14x faster than single-threaded Postgres** at SF=1 | geomean 0.88x |
| SwiftQL is **1.52x slower than default Postgres** at SF=1 | Postgres uses ~3 cores |
| SwiftQL is **27x slower than DuckDB** | DuckDB uses 14 cores |
| Against unindexed row stores SwiftQL answers 3 queries they cannot finish | 600 s cap |
| q18 is a genuine engine-quality loss | holds with and without indexes |

## 10. Six optimizations, and the result they produced

Two agents, disjoint files, each change measured separately and required to keep
all 22 answers byte-identical.

| # | change | effect at SF=0.1 |
|---|---|---|
| 1 | **Narrow scan schemas for subquery statements.** One line (`if (stmt.has_subquery) return full_schema;`) disabled column pruning for every scan in the block. q21's scans went 7/16/9/4 columns to 3/4/2/2 | q18 2.55x, q21 2.00x, q20 1.78x |
| 2 | **Semi/anti join as a spine boundary.** One semi-join disabled join ordering for the whole query; now the inner spine below it is enumerated. q21's largest estimated intermediate: 300286 to 12011 rows | q21 1.47x |
| 3 | **Per-relation restrictions from OR.** q19's whole WHERE is one OR conjunct, so nothing was pushed. Deriving the sound weaker single-relation filter cuts lineitem into the join from 600572 to 25402 rows, and the plan flips from a hash join to a SIMD loop join | **q19 7.13x** |
| 4 | **Share `ColumnarTable` instead of deep-copying per scan** | plan time only; -102 ms over 22 queries |
| 5 | **Semi/anti join keys into the Week-37 arena + chained index**, replacing `unordered_set<std::string>` | -27 ms over 22 |
| 6 | **INT64 key path in the join**, replacing decimal-text keys for single-INT joins | -91 ms over 22; q18 -23.6, q21 -18.7, q4 -14.8, q9 -10.7 |

Gates after all six: **1005 unit tests**, 1796 oracle, 345 regression,
`GATE tpch: PASS (21/22 meaningful, 0 unported)`.

### A design I specified that lost, and was correctly overruled

I specified an inline `{uint64 key, int32 row}` bucket array for change 6. It was
built and **measured slower**: on q9's join (600572 build rows against a
1075-row probe) it cost 60.3 ms against the chained index's 44.1 ms, because
16-byte slots at a 0.5 load factor make the table 4x larger and the build pays
for that in full. Resizing to 1.5x recovered only 4 ms. Reusing the existing
chain over the key column in place gives 38.7 ms — better than both. That is
what shipped, and the agent reported the rejection rather than delivering the
specified design.

### Final result

| geomean (SwiftQL / engine) | SF=0.01 | SF=0.1 | SF=1 |
|---|---|---|---|
| vs SQLite (indexed) | 0.74x | 0.37x | **0.33x — 3.0x faster** |
| vs Postgres (indexed, parallel) | 0.29x | 0.65x | **0.87x — 1.15x faster** |
| vs Postgres (indexed, 1 thread) | — | — | **0.49x — 2.0x faster** |
| vs DuckDB | 1.20x | 4.70x | 15.37x |
| optimizer | 1.69x | 1.92x | 2.22x |

At SF=1 the Postgres comparison moved from **1.52x slower to 1.15x faster**, and
the SQLite comparison from 0.59x to 0.33x. Per-query detail:
[week-37-per-query.md](week-37-per-query.md).

### Reproducibility note

Every results JSON now records its own conditions — index configuration,
Postgres parallelism setting, and which SwiftQL binary produced it. The same 22
latencies mean different things under different conditions (measured: 1.52x vs
0.87x against Postgres at SF=1), so a file that does not state them cannot be
cited. The four files published before this change were stamped retroactively
from their run logs and are marked `stamped_retroactively`.

## 11. Cardinality-estimation accuracy (q-error)

`python_tools/qerror_tpch.py`. q-error is `max(est, actual) / min(est, actual)`
per plan node — symmetric, so a 10x under-count and a 10x over-count score alike.
The geometric mean and the max are reported; an arithmetic mean over ratios
spanning five orders of magnitude describes no individual node.

| | SF=0.1 | SF=1 |
|---|---|---|
| nodes measured | 276 | 276 |
| **geometric mean** | **4.52** | **6.59** |
| median | 1.59 | 1.86 |
| 90th percentile | 100.0 | 533.0 |
| max | 120 114 | 118 274 |
| within 2x / 10x / 100x | 52% / 77% / 92% | 51% / 75% / 83% |

**Half of all estimates are within 1.6-1.9x**, which is respectable for a
histogram-free estimator on uniform data. **The tail is where it fails, and it
degrades with scale**: the 90th percentile moves 100x -> 533x and the fraction
within 100x drops 92% -> 83%. That is the independence assumption compounding
along a join spine, exactly as the README's Limitations bullet predicts.

**One systematic, nameable defect.** Semi/anti joins estimate **1 row**: q16's
`VecAntiHashJoin` estimates 1 against an actual 11 635, and q21's estimates 1
against 465. The README already records the cause — the semi/anti rule falls back
to `frac = 1.0` because the body plan's `LogicalProject` empties its
`StatsContext` and the right-side NDV lookup misses — and predicted it would show
up as a flat error on exactly these queries. It does. Worst per-query geometric
means at SF=0.1: q18 120.4, q16 83.7, q15 20.7.

**What this does NOT show.** A node can be 100x out and still produce the right
plan if the ordering is unaffected. This measures estimate accuracy, not plan
quality; the optimizer's measured 1.69-2.22x gain is the evidence that ranking
still works.

## 12. Scale and memory, re-measured

`SELECT COUNT(*) FROM lineitem`, peak RSS of the whole process via
`/usr/bin/time -l`, which is per-process. (An earlier attempt used
`resource.getrusage(RUSAGE_CHILDREN)`, a running maximum over every child ever
reaped, which cannot isolate one run. Its numbers were wrong and are not
reported.)

| dataset | `lineitem` rows | `--storage row` | `--storage columnar` |
|---|---|---|---|
| `dbgen-sf0.01` | 60 175 | 0.1 s / 51 MB | 0.1 s / 72 MB |
| `dbgen-sf0.1` | 600 572 | 1.2 s / 475 MB | 1.7 s / 726 MB |
| `dbgen-sf1` | 6 001 215 | 14.7 s / **4 521 MB** | 20.3 s / **5 450 MB** |

**Columnar still peaks higher than row**, for the reason the README already
identified: `main.cc` clears `table_rows` only after every table is converted, so
peak is the row image plus the columnar image, not the columnar size. The ratio
narrowed at SF=1 (1.21x against 1.53x at SF=0.1) but the shape is unchanged.

**SF=1 is the largest scale exercised**, at 5.4 GB peak on a 24 GB machine. SF=10
would need roughly 55 GB and is not attempted; that figure is an extrapolation
and is labelled as one.

## Not yet measured

- A single-execution lower bound for the three unindexed timeouts (q17, q21,
  q22). The 600 s cap covered a warmup plus three repetitions, so it bounds four
  executions, not one.
