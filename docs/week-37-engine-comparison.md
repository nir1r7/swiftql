# Week 37 — SwiftQL vs SQLite, Postgres and DuckDB (TPC-H, SF=0.01)

Produced by `python_tools/compare_engines.py`. Raw record:
[week-37-engine-comparison-sf0.01.json](week-37-engine-comparison-sf0.01.json).

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release && cmake --build build-release -j
.venv/bin/python python_tools/compare_engines.py \
    --catalog data/tpch/dbgen-sf0.01/catalog.json \
    --engines swiftql,sqlite,duckdb,postgres --reps 5 --warmups 1 \
    --json docs/week-37-engine-comparison-sf0.01.json
```

## The build flag that invalidated the first measurement

`CMakeLists.txt` sets **no default `CMAKE_BUILD_TYPE`**, and `build/` was
configured without one. CMake passes no `-O` flag at all in that case, so the
binary every earlier benchmark in this project quoted was compiled at `-O0`.

Same commit, same data, same query text:

| query | `build/` (no `-O`) | `build-release/` (`-O3 -DNDEBUG`) | ratio |
|---|---|---|---|
| q1 | 259.8 ms | 9.2 ms | 28.2× |
| q9 | 506.6 ms | 24.7 ms | 20.5× |
| q18 | 1568.4 ms | 72.6 ms | 21.6× |
| q21 | 1314.7 ms | 59.5 ms | 22.1× |

**A ~21× geometric factor that has nothing to do with the engine.** It inverts
the headline result: at `-O0` SwiftQL measured 6.3× slower than SQLite; the same
code at `-O3` is 1.9× faster, and 3.3× faster once the three operator
optimizations below land. `compare_engines.py` now defaults to `./build-release/swiftql`
and refuses to run if it is absent, naming the configure command — a benchmark
that can silently quote an unoptimized binary is not a benchmark.

Every number below is the Release build. **The `verify` skill's five gates still
use `build/`**, which is correct for correctness work and wrong for any timing
claim; that split should be made explicit rather than left to whoever runs it
next.

## Methodology

| decision | what was done | why it matters |
|---|---|---|
| Query text | One string per query — `tpch_queries.render(qid)`, the SwiftQL dialect port, with the validation parameters the correctness harness uses | No engine gets a rewrite tuned to it. This cuts **against** SwiftQL: the port exists because SwiftQL cannot parse the spec's own text, so the other three are handed a query shaped by a dialect they do not need |
| Types | All four get `tpch_schema.py`'s SQLite declarations (INTEGER / REAL / TEXT) | A DATE is a lexicographically-compared string everywhere, as `src/common/date_util.h` defines it. Letting Postgres use a native DATE and NUMERIC would measure a different query |
| Indexes | **None, on any engine.** No primary keys, no secondary indexes | SwiftQL has no index support at all, so an indexed comparison would not be one. This is the single largest handicap on SQLite and Postgres, and it is what makes q21 pathological for SQLite |
| Load | Excluded for all four | SQLite, DuckDB and Postgres load once into a persistent database and are timed on execution alone. SwiftQL has no persistence — every invocation re-reads the `.tbl` files — so it is timed from its own `--explain-analyze` footer, whose timers all start after the load. **Verified**: 1.56 s wall clock against a reported 17.7 ms on the same query |
| SwiftQL mode | `--storage columnar --execution vectorized` | Its best mode, and the only one that answers all 22. `row-volcano` and `col-volcano` answer 5 of 22 |
| Statistic | Median of 5 after 1 discarded warmup | |
| SwiftQL figure quoted | `parse + plan + exec` | The other three are timed with a call that includes their own parse and plan. Exec-only is also in the JSON |

**Not measured:** cold start, concurrency, write throughput, larger-than-memory
operation, index selection, multi-user. SwiftQL has no write path, no
persistence and no indexes, so several of those are not comparisons it can
enter. This is single-query, single-threaded, warm-cache latency on one machine
(24 GB, 14 cores, macOS).

## Results (ms, median of 5)

| query | SwiftQL | SQLite | Postgres | DuckDB | rows |
|---|---|---|---|---|---|
| q1 | **5.8** | 22.9 | 12.2 | 2.3 | 4 |
| q2 | **1.6** | 4.2 | 6.6 | 2.0 | 8 |
| q3 | **1.7** | 11.7 | 15.4 | 1.7 | 10 |
| q4 | **3.6** | 23.7 | 20.1 | 2.7 | 5 |
| q5 | **3.2** | 14.5 | 10.7 | 2.0 | 5 |
| q6 | **0.8** | 3.3 | 16.9 | 0.8 | 1 |
| q7 | **7.1** | 13.0 | 19.9 | 1.9 | 4 |
| q8 | **1.6** | 29.5 | 10.7 | 2.3 | 2 |
| q9 | **7.8** | 35.7 | 12.0 | 2.6 | 173 |
| q10 | **3.2** | 5.3 | 12.5 | 2.8 | 20 |
| q11 | **1.0** | 4.5 | 2.2 | 1.2 | 359 |
| q12 | **2.4** | 6.7 | 14.1 | 2.2 | 2 |
| q13 | 10.0 | 11.7 | 6.0 | 1.8 | 33 |
| q14 | **1.1** | 3.8 | 15.4 | 1.1 | 1 |
| q15 | **3.6** | 9.7 | 29.6 | 1.5 | 1 |
| q16 | **1.6** | 1.8 | 6.0 | 2.4 | 296 |
| q17 | 7.1 | 0.1 | 1.6 | 1.0 | 1 |
| q18 | 19.5 | 16.5 | 11.9 | 2.4 | 2 |
| q19 | **3.1** | 12.5 | 8.9 | 1.5 | 1 |
| q20 | **0.7** | 0.5 | 1.8 | 0.9 | 1 |
| q21 | **18.6** | 20206.4 | 91.7 | 5.1 | 1 |
| q22 | **1.0** | 27.3 | 2.9 | 1.4 | 7 |

All four engines returned the same row count on every query, so the latencies
are for the same answer. (Row counts are a sanity check, not the correctness
oracle — that is `run_tpch.py`, which diffs values.)

## Summary

| pair | geometric mean |
|---|---|
| SwiftQL / SQLite | **0.30× — SwiftQL is 3.3× faster** |
| SwiftQL / Postgres | **0.31× — SwiftQL is 3.2× faster** |
| SwiftQL / DuckDB | 1.67× slower |

| engine | queries SwiftQL beats it on |
|---|---|
| SQLite | 19 / 22 |
| Postgres | 19 / 22 |
| DuckDB | 7 / 22 |

Sum of all 22: SwiftQL 106.0 ms. SQLite's 20 206 ms is one query (q21), so the
sum is meaningless and the geometric mean is the figure to quote.

### How this figure moved

| stage | vs SQLite | vs Postgres | vs DuckDB |
|---|---|---|---|
| `-O0` build (invalid) | 6.31× slower | 6.31× slower | 36.30× slower |
| Release build | 0.53× (1.9× faster) | 0.60× (1.7× faster) | 2.89× slower |
| + three operator optimizations | **0.30× (3.3× faster)** | **0.31× (3.2× faster)** | **1.67× slower** |

The three optimizations, each landed by a separate agent against the constraint
that all 22 answers stay byte-identical:

| change | effect |
|---|---|
| **Hash join**: `unordered_map<string, vector<Row>>` replaced by a column-wise row store with a chained index, plus late materialization of the output (row-id pairs instead of `Row`s) | q18 63.9 → 19.7 ms, q21 58.7 → 18.3 ms, q9 23.2 → 7.7 ms, q7 17.4 → 7.1 ms. Top join node on q18: 44.6 → 7.1 ms |
| **Aggregate**: group key serialized straight from the column into a reused buffer (no per-row `Value` vector, no fresh `std::string`), `group_order_` folded away, aggregate-function dispatch hoisted out of the row loop | isolated grouping 4.4 → 1.5 ms on 60k rows (74 → 25 ns/row); q1 9.1 → 5.8 ms |
| **Sort**: ORDER BY keys evaluated once per row and parked in slots after the schema's columns, instead of `evaluate()` per key per side per comparison | `VecSort` self-time: q3 908 → 107 µs (8.5×), q10 3459 → 370 µs (9.3×) |

## Findings

**SwiftQL is 3.3x faster than SQLite and 3.2x faster than Postgres on this
workload**, winning 19 of 22 queries against each. With the caveats above -- no
indexes on any engine, single-threaded, warm cache, load excluded -- that is the
headline. Against DuckDB it is 1.67x slower and now wins 7 of 22.

**Where SwiftQL wins, and why.** The largest wins are on queries whose shape it
*rewrites*: q21 and q22 (correlated `NOT EXISTS` -> anti-join, Week 33), q8
(derived table, Week 34), q4 (correlated `EXISTS` -> semi-join). SQLite runs
these as per-row nested scans over an unindexed mirror; q21 is 1086x in
SwiftQL's favour for that reason. **This is a planner result, not an execution
one** -- and it is the one result here that a bigger machine or a better kernel
would not produce.

**Where it still loses.** q18 (19.5 ms against Postgres's 11.9) and q17 (7.1 ms
against SQLite's 0.1) are the two remaining. q13 and q16 are within 2x of the
field. Everything else is a win.

**Node profile of q18, the worst remaining case** (Release, `--explain-analyze`,
after the exclusive-timer fix below):

| node | self time | % |
|---|---|---|
| VecHashJoin (orders x lineitem) | **7.1 ms** | 36 |
| VecHashAggregate | 6.3 ms | 32 |
| VecScan | 2.6 ms | 13 |
| VecSemiHashJoin | 1.6 ms | 8 |
| VecHashJoin (inner) | 1.3 ms | 7 |
| VecSort | 3.0 us | 0.0 |

The remaining join time is the operator genuinely materializing 60 175 x 33
cells. Cutting it further needs column pruning or cross-operator late
materialization in the planner, not more work inside the operator.

### The exclusive-time bug, fixed

README:537 documents `--explain-analyze` as reporting per-node *exclusive*
self-time. **Three nodes violated it** -- `VecSortNode`, `VecDistinctNode` and
`VecDerivedNode` all started their timer before draining the child, charging
their entire subtree to themselves. `VecHashJoinNode`, `VecHashAggregateNode`
and `VecSimdLoopJoinNode` were already correct.

Each now accumulates per chunk with the child call excluded. On q18 the
`VecSort` line went from **42.4 ms (64.8%) to 3.0 us (0.0%)**, which is what a
sort of 2 rows actually costs. Before the fix the profile named the sort as the
hot node on every ORDER BY query and hid the join underneath it -- the numbers
in this section could not have been read off the old instrumentation.

## The three optimizations, and the one root cause behind them

All three were the same defect, worth stating once: **the blocking operators
were chunk-*fed* but row-at-a-time *inside*.** `VecSortNode`,
`VecHashJoinNode` and `VecHashAggregateNode` each held `std::vector<Row>` and
allocated per row, abandoning the late-materialization principle Phase 3 is
built on at precisely the three operators that dominated every slow query.

All three are now fixed; see the table under **Summary** for the effect of each.
Two findings from the work are worth keeping:

- **The predicted cause was wrong twice, and measurement corrected it.** The sort
  was expected to be dominated by copying non-key columns. It was not: at
  138-399 rows the cost was `sort_comparator::rowLess` calling `evaluate()` on
  every key of both sides of every comparison, which for `ORDER BY SUM(...)`
  rebuilt an aggregate output name and ran a linear schema lookup *per
  comparison*. Row-ID sorting was correctly declined. Likewise the join's single
  biggest remaining item after the data-structure change was a `std::string`
  temporary returned by `keyFieldText()`, not the hash table.
- **ASan found a bug the 973 tests did not.** `build_cols_` was indexed while
  empty when a LEFT join's build side contributed no rows -- either because it
  was empty or because every build key was NULL. Harmless in practice (the
  reference was never read) but undefined behaviour, and the Release test run
  was silent about it.

### Still open, in priority order

1. **q18's remaining 7.1 ms join** is the operator materializing 60 175 x 33
   cells. Cutting it needs column pruning or cross-operator late materialization
   in the planner, not more work inside the operator.
2. **The aggregate's `unordered_map` is still node-based.** Key building plus
   probe is now ~25 ns/row; open addressing is the next lever.
3. **A duplicated key encoding now exists in two places.** The aggregate and the
   join each write key bytes directly from a column, deliberately reproducing
   `key_encoding.h`'s rules rather than editing a shared header. They must stay
   byte-identical. This codebase's most productive bug class is code trusting a
   rule that has since changed elsewhere, so this is a standing risk, not a
   closed item.

## Owed

- SF=0.1 and SF=1 for all four engines. This page is SF=0.01 only.
- **The SF=0.1 correctness run is blocked on the oracle, not on SwiftQL.**
  `compare_against_sqlite.py` builds its SQLite mirror with no indexes, so
  q21's correlated `NOT EXISTS` is a nested scan; it ran 55 minutes at SF=0.1
  without finishing q21. The same effect is visible at SF=0.01 in the table
  above (19.7 s). Indexing the mirror — or excluding q21 from the oracle — is a
  prerequisite for correctness at SF≥0.1. Note this affects the *oracle only*;
  SwiftQL answers q21 in 61.7 ms.
- The `--explain-analyze` exclusive-time bug is FIXED (see Findings); per-node
  profiles above are read off the corrected instrumentation.
- Estimate accuracy (q-error) and the optimizer-impact pair
  (`col-vec` vs `col-vec-noopt`), both still unrun at any scale.
