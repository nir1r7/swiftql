# Phase 4 Benchmark Results

Vectorized execution with vs without the cost-based optimizer, 1M rows, avg of 5 runs. **Build: Release (-O3)**, same configuration as the Phase 3 doc. Phase 4 changes *planning only* — both modes run the same vectorized operator set; the optimizer (`--no-optimize` off) reshapes the tree via predicate pushdown (Week 21), annotates it with cardinality estimates (Week 20), and picks the join's build side *and algorithm* from cost estimates (Weeks 22/23.5 — `--no-optimize` stays hash-only with the pure row-count build heuristic). Week 23 adds the instrumentation this doc is built from: three-section `EXPLAIN`, `est=` vs `rows_out=` in `EXPLAIN ANALYZE`, and the join's cost decision in plan output. Join node times include the build phase and output materialization (post-audit fix), so per-node times account for ~87-90% of the execution total; the remainder is the CLI's own result collection.

## Dataset

Same F1 laps table as Phases 2/3: 9 columns, 1M rows, **sorted by season** (`generate_data.py` emits season-sorted data by default; pass `--no-sort` for random order). `drivers` is 20 rows. Storage is unchanged from Phase 2 — the optimizer adds no memory footprint.

## Benchmark Queries

Each query isolates one optimizer investment:

```sql
-- 1. Conjunct ordering (single table): Week 21 orders scan-local conjuncts most-selective-first
SELECT AVG(speed) FROM laps WHERE season = 2025 AND speed > 300

-- 2. Join build-side selection (no filter): Week 22 costs both build assignments
SELECT laps.team, COUNT(*) FROM laps JOIN drivers
ON laps.driver_id = drivers.driver_id GROUP BY laps.team

-- 3. Pushdown below join (filtered both sides): Week 21 pushes each predicate onto its own scan
SELECT laps.team, COUNT(*) FROM laps JOIN drivers
ON laps.driver_id = drivers.driver_id
WHERE laps.season = 2025 AND drivers.age > 30 GROUP BY laps.team
```

## Results

```
Query                                       No-Opt (ms)   Opt (ms)  Speedup  Max q-err  Med q-err
-------------------------------------------------------------------------------------------------
Conjunct ordering (single table)                    3.4        3.4    0.99x       3.9x       1.0x
Join build-side selection (no filter)             131.5      123.1    1.07x       1.0x       1.0x
Pushdown below join (filtered both sides)          30.0       13.4    2.23x       3.9x       1.4x
```

q-error = max(est, actual) / min(est, actual) per plan node, parsed from `EXPLAIN ANALYZE`; estimates only exist in optimized mode.

## Query Analysis

**Conjunct ordering** (`WHERE season = 2025 AND speed > 300`) — **0.99x**, flat

The query runs in 3.4ms total, and the optimizer has almost nothing left to optimize. Zone-map pruning (Phase 2) skips 91 of 123 chunks in *both* modes — the pruning hint is routed during lowering, independent of the optimizer passes — and the surviving filter work is ~0.9ms. Reordering two conjuncts (`season = 2025` at selectivity 0.25 cascades before `speed > 300` at ~0.68) can only shave a fraction of that fraction, which is below run-to-run noise. The optimizer correctly does no harm here; the win it was built for needs more expensive predicates than two numeric comparisons.

---

**Join build-side selection, no filter** (`JOIN drivers ON driver_id`) — **1.07x**, algorithm choice

With no WHERE clause, the filtered cardinality estimates equal the raw table sizes (1M laps, 20 drivers), so the build-side decision matches the pre-Week-22 "smaller table builds" heuristic. The gain comes from the Week 23.5 *algorithm* decision, which `--no-optimize` (hash-only) cannot make — with a 20-key INT build side, the SIMD loop join undercuts hashing every probe key:

```
VecSimdLoopJoin [driver_id = driver_id] (materialize) build=drivers cost=400020 (alt=1416400) algo=simd (hash=1000040)
```

The decision line reads: drivers builds (the alternative assignment would cost ~3.5x), and the SIMD loop at 400020 beats the best hash plan at 1000040. Most of the query's time is output materialization, which both algorithms share — hence 1.07x rather than the probe-loop's raw advantage. The flip case the old heuristic *cannot* get right (a selective filter shrinking the big table below the small one) is pinned by plan-shape unit tests; query 3 shows the same machinery paying off in latency.

---

**Pushdown below join, filtered both sides** (`WHERE laps.season = 2025 AND drivers.age > 30`) — **2.23x**, the headline

`EXPLAIN ANALYZE` accounts for the 16.6ms delta node by node. Unoptimized, the WHERE evaluates *above* the join, so a hash join materializes every season-2025 lap against all 20 drivers before the filter discards 65% of its output — and the join alone is 68% of the query:

```
VecFilter [laps.season = 2025 AND drivers.age > 30]    rows_in=254528   rows_out=87776    time=757µs     (2.6%)
  VecHashJoin [driver_id = driver_id] (materialize)    rows_in=254528   rows_out=254528   time=19889µs   (68.4%)
```

Optimized, each conjunct lands on its own scan, the build side shrinks from 20 drivers to 7 before the join is built (rows for under-30 drivers never match), the post-join filter disappears — and with a 7-key INT build side the algorithm decision picks the SIMD loop join over hashing:

```
VecSimdLoopJoin [...] build=drivers cost=50010 (alt=306100) algo=simd (hash=250020)   rows_out=87776   time=6564µs   (49.9%)
  VecFilter [laps.season = 2025]                                                      rows_out=250402
  VecFilter [drivers.age > 30]                                                        rows_out=7
```

The join drops from 19.9ms to 6.6ms (both figures include build and output materialization): fewer output rows to assemble, no hash-table build or probes, and a 7-key SIMD scan per probe key. Together with the vanished post-join filter that is ~14ms of the 16.6ms; the rest is the aggregate consuming a filtered stream and untimed CLI result collection. Zone-map pruning (91/123 chunks) fires identically in both modes and is not part of the delta.

## Estimation Accuracy

Per-node estimates vs actuals for query 3 (the full pipeline):

```
Node                              est        actual     q-error
---------------------------------------------------------------
VecScan [laps]                    1000000    254528     3.93x
VecFilter [laps.season = 2025]    250000     250402     1.00x
VecScan [drivers]                 20         20         1.00x
VecFilter [drivers.age > 30]      10         7          1.43x
VecSimdLoopJoin [driver_id]       125000     87776      1.42x
VecHashAggregate [team]           7          5          1.40x
```

Where statistics can answer exactly, the estimates are excellent: the season equality lands within 0.2% (NDV=4, uniform seasons) and the scans are exact. The age filter's 1.43x is honest range interpolation over a 20-row table — `(max-30)/(max-min)` of 20 rows says 10, the dice said 7 — and that error propagates linearly into the join estimate (1.42x). The aggregate's 1.40x comes from estimating group count via the `team` column's table-wide NDV (7) while only 5 teams survive the age filter — NDV doesn't shrink through predicates, a known limitation of NDV-based grouping estimates.

The 3.9x outlier at the scan is a *measurement semantics* gap, not a bad estimate: the estimator stamps the scan with its logical cardinality (the table really has 1M rows), but the physical scan's `rows_out` counts rows *after* zone-map pruning skipped 91 of 123 chunks. The estimator doesn't model pruning. This is harmless today — every downstream estimate keys off selectivities, not the scan actual — but it matters for Week 28 join enumeration, where scan costs feed plan choice; modeling expected pruning is the natural fix.

## Takeaway

The Phase 4 optimizer pays exactly where plans have room to differ: a filtered join gets **2.23x** from pushing predicates below the join, shrinking the build side, and switching to the SIMD loop join the small build enables, while the single-table query whose default plan was already optimal stays flat instead of regressing — the cost model reproduces the old heuristic when the heuristic is right, and beats it when filters or a second join algorithm change the picture. Estimation quality supports the decisions: the errors that exist (range interpolation on a 20-row table, NDV through predicates, zone-map pruning at scans) are documented rather than hidden, and none flipped a decision the actuals wouldn't also have made. Week 23's explainability makes all of this inspectable per query — logical vs optimized plan diff, per-node est/actual, and the join's costed side+algorithm decision — which is the tooling Week 28 (join enumeration) will be validated with.
