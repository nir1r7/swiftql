# Phase 4 Benchmark Results

Vectorized execution with vs without the cost-based optimizer, 1M rows, avg of 5 runs. **Build: Release (-O3)**, same configuration as the Phase 3 doc. Phase 4 changes *planning only* — both modes run the identical vectorized operator set; the optimizer (`--no-optimize` off) reshapes the tree via predicate pushdown (Week 21), annotates it with cardinality estimates (Week 20), and picks the hash-join build side from cost estimates (Week 22). Week 23 adds the instrumentation this doc is built from: three-section `EXPLAIN`, `est=` vs `rows_out=` in `EXPLAIN ANALYZE`, and the join's cost decision in plan output.

## Dataset

Same F1 laps table as Phases 2/3: 9 columns, 1M rows, **sorted by season** (regenerate with `generate_data.py`, then sort — the generator emits random season order). `drivers` is 20 rows. Storage is unchanged from Phase 2 — the optimizer adds no memory footprint.

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
Conjunct ordering (single table)                    3.4        3.5    0.99x       3.9x       1.0x
Join build-side selection (no filter)             132.1      131.2    1.01x       1.0x       1.0x
Pushdown below join (filtered both sides)          32.5       23.0    1.41x       3.9x       1.0x
```

q-error = max(est, actual) / min(est, actual) per plan node, parsed from `EXPLAIN ANALYZE`; estimates only exist in optimized mode.

## Query Analysis

**Conjunct ordering** (`WHERE season = 2025 AND speed > 300`) — **0.99x**, flat

The query runs in 3.5ms total, and the optimizer has almost nothing left to optimize. Zone-map pruning (Phase 2) skips 91 of 123 chunks in *both* modes — the pruning hint is routed during lowering, independent of the optimizer passes — and the surviving filter work is ~0.9ms. Reordering two conjuncts (`season = 2025` at selectivity 0.25 cascades before `speed > 300` at ~0.68) can only shave a fraction of that fraction, which is below run-to-run noise. The optimizer correctly does no harm here; the win it was built for needs more expensive predicates than two numeric comparisons.

---

**Join build-side selection, no filter** (`JOIN drivers ON driver_id`) — **1.01x**, parity by design

With no WHERE clause, the filtered cardinality estimates equal the raw table sizes (1M laps, 20 drivers), so the Week 22 cost model reaches the same conclusion as the pre-Week-22 "smaller table builds" heuristic. The new `EXPLAIN` decision line shows the margin:

```
VecHashJoin [driver_id = driver_id] (materialize) build=drivers cost=1000040 (alt=2016120)
```

Building 1M-row laps would cost ~2x (1M inserts at 2.0 CPU each, plus hash-table memory) versus 1M probes at 1.0 each. Parity is the correct outcome — the optimizer agreeing with an already-optimal default is evidence it works, not that it doesn't. The flip case the heuristic *cannot* get right (a selective filter shrinking the big table below the small one) is pinned by plan-shape unit tests; query 3 shows the same machinery paying off in latency.

---

**Pushdown below join, filtered both sides** (`WHERE laps.season = 2025 AND drivers.age > 30`) — **1.41x**, the headline

`EXPLAIN ANALYZE` shows exactly where the 9.5ms goes. Unoptimized, the WHERE evaluates *above* the join, so the join materializes every season-2025 lap against all 20 drivers before the filter discards 56% of its output:

```
VecFilter [laps.season = 2025 AND drivers.age > 30]    rows_in=254528   rows_out=112496   time=1699µs
  VecHashJoin [driver_id = driver_id] (materialize)    rows_in=254528   rows_out=254528   time=14128µs
```

Optimized, each conjunct lands on its own scan. The build side shrinks from 20 drivers to 9 before the hash table is built, so rows for under-30 drivers never match — the join emits 112,496 rows directly instead of 254,528, and the post-join filter disappears:

```
VecHashJoin [...] (materialize) build=drivers cost=250019 (alt=506034)   rows_out=112496   time=10912µs
  VecFilter [laps.season = 2025]                                         rows_out=250697
  VecFilter [drivers.age > 30]                                           rows_out=9
```

The saving is the join's output materialization — `VecHashJoinNode` assembles full output `Row`s, so halving its output halves the most expensive work in the query. Zone-map pruning (91/123 chunks) fires identically in both modes and is not part of the delta.

## Estimation Accuracy

Per-node estimates vs actuals for query 3 (the full pipeline):

```
Node                              est        actual     q-error
---------------------------------------------------------------
VecScan [laps]                    1000000    254528     3.93x
VecFilter [laps.season = 2025]    250000     250697     1.00x
VecScan [drivers]                 20         20         1.00x
VecFilter [drivers.age > 30]      9          9          1.00x
VecHashJoin [driver_id]           116667     112496     1.04x
VecHashAggregate [team]           7          5          1.40x
```

Where statistics can answer, the estimates are excellent: the season equality lands within 0.3% (NDV=4, uniform seasons), the age range predicate is exact, and the join estimate is within 4%. The aggregate's 1.40x comes from estimating group count via the `team` column's table-wide NDV (7) while only 5 teams survive the age filter — NDV doesn't shrink through predicates, a known limitation of NDV-based grouping estimates.

The 3.9x outlier at the scan is a *measurement semantics* gap, not a bad estimate: the estimator stamps the scan with its logical cardinality (the table really has 1M rows), but the physical scan's `rows_out` counts rows *after* zone-map pruning skipped 91 of 123 chunks. The estimator doesn't model pruning. This is harmless today — every downstream estimate keys off selectivities, not the scan actual — but it matters for Week 28 join enumeration, where scan costs feed plan choice; modeling expected pruning is the natural fix.

## Takeaway

The Phase 4 optimizer pays exactly where plans have room to differ: a filtered join gets **1.41x** from pushing predicates below the join and shrinking the build side, while queries whose default plan was already optimal stay flat instead of regressing — the cost model reproduces the old heuristic when the heuristic is right, and beats it only when filters change the picture. Estimation quality supports the decisions: median q-error 1.0x, with the two known gaps (NDV through predicates, zone-map pruning at scans) documented rather than hidden. Week 23's explainability makes all of this inspectable per query — logical vs optimized plan diff, per-node est/actual, and the join's costed decision — which is the tooling Weeks 23.5 (SIMD loop join vs hash join) and 28 (join enumeration) will be validated with.
