# Phase 3 Benchmark Results

Row+Volcano vs columnar+Volcano vs columnar+Vectorized, 1M rows, single run per batch-size point, avg of 5 runs for the main table. **Build: Release (-O3).** Phase 2 doc used a Debug build — absolute numbers are not cross-doc comparable; ratios within each doc are valid. Phase 3 adds vectorized (batch) execution on top of the columnar storage layer built in Phase 2.

## Dataset

Same F1 laps table as Phase 2: 9 columns, 1M rows, **sorted by season**. Zone-map chunk pruning (Phase 2) and vectorized execution (Phase 3) are independent optimizations — they compose, and the zone-map + vectorized result for the selective filter query shows exactly that.

## Memory Footprint

```
Row storage:    343 MB
Columnar:        57 MB
Compression:      6.0x
```

Unchanged from Phase 2 — Phase 3 adds no storage overhead.

## Benchmark Queries

```sql
-- 1. Full scan aggregate
SELECT AVG(speed) FROM laps

-- 2. Selective filter + zone-map pruning
SELECT COUNT(*) FROM laps WHERE season = 2025

-- 3. Projection pushdown (2 of 9 cols)
SELECT team, speed FROM laps WHERE speed > 300

-- 4. GROUP BY dictionary-encoded string
SELECT team, COUNT(*) FROM laps GROUP BY team

-- 5. Hash join + aggregate
SELECT laps.team, AVG(laps.speed) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id GROUP BY laps.team
```

## Results

```
Query                                        Row (ms)   Col (ms)   Vec (ms)  Col/Row  Vec/Row
---------------------------------------------------------------------------------------------
Full scan aggregate                             165.4       72.2       17.0    2.29x    9.75x
Selective filter + zone-map pruning             250.8       42.1        8.8    5.96x   28.58x
Projection pushdown (2 of 9 cols)               307.4      220.8       37.6    1.39x    8.17x
GROUP BY dictionary-encoded string              198.7      106.1       51.4    1.87x    3.86x
Hash join + aggregate                           234.4      223.1      218.6    1.05x    1.07x
```

## Query Analysis

**Full scan aggregate** (`SELECT AVG(speed) FROM laps`) — **9.75x** vs row, **4.25x** vs columnar+Volcano

The vectorized path processes the `speed` column as a tight `vector<double>` loop: one `std::visit` dispatch per 1024-row batch rather than one per row. Under Volcano, every row costs a virtual `next()` call, a `getValue()` variant dispatch, and a branch to the accumulator. Vectorized eliminates all three from the inner loop.

---

**Selective filter + zone-map pruning** (`WHERE season = 2025`) — **28.58x** vs row, **4.80x** vs columnar+Volcano

The two optimizations compound. Zone-map chunk pruning (Phase 2) skips ~75% of the 1M rows before any execution happens — VecScanNode checks the zone-map per chunk and only emits chunks that could contain `season = 2025`. For the ~250k rows that survive pruning, the vectorized filter loop applies the predicate over the `season` int64 column array in batches, selecting passing row indices into a `SelectionVector` without materializing rejected rows. Columnar+Volcano also gets zone-map pruning but still pays per-row dispatch on the passing rows.

---

**Projection pushdown (2 of 9 cols)** (`WHERE speed > 300, SELECT team, speed`) — **8.17x** vs row, **5.87x** vs columnar+Volcano

Vectorized's second-largest win. The filter operates on the `speed` double column array; the project materializes only `team` (dictionary-decoded strings) and `speed` (doubles) — the other 7 columns are never touched. Under row storage, even "reading 2 columns" means walking the full struct. Under columnar+Volcano, `getValue()` on 2 columns still fires per row. Vectorized accesses both column arrays in tight independent passes.

---

**GROUP BY dictionary-encoded string** (`GROUP BY team`) — **3.86x** vs row, **2.06x** vs columnar+Volcano

Meaningful gains but the smallest multiplier on data-access work. VecScanNode decodes the dictionary-encoded `team` column into `std::string` values before they enter the vectorized pipeline — so `VecHashAggregateNode` still hashes `std::string` keys per row in the accumulation loop. The batch extraction of group keys (one `std::visit` per 1024-row chunk) helps, but string allocation and hashing remain the bottleneck. A future optimization would pass raw dictionary integer IDs through `ColumnVector` and hash integers instead.

---

**Hash join + aggregate** (`JOIN drivers ON driver_id, GROUP BY team`) — **1.07x** vs row, **~1.02x** vs columnar+Volcano

Near-flat across all three modes. The build side is 20 driver rows (trivial). The probe side is 1M laps rows — but the bottleneck is 1M hash table lookups on `driver_id`, not data access. Hash lookup latency is independent of whether rows arrive one at a time or in batches; the vectorized path's batch output buffering adds no meaningful speedup when the dominant cost is pointer chasing into an `unordered_map`. This query benefits from neither zone-map pruning (no selective WHERE) nor column-array loops (join key extraction is one lookup per output row regardless).

## Batch Size Sensitivity

Query: `SELECT AVG(speed) FROM laps` — columnar + vectorized, 1M rows, single run per size.

```
BATCH_SIZE   Execution (ms)
--------------------------
128            17.5
256            17.5
512            17.6
1024           17.0
2048           18.0
```

The curve is flat. `SELECT AVG(speed)` is memory-bandwidth-bound: the inner loop reads a `double` and adds it to an accumulator — no branching, no allocation. At any of the tested batch sizes (128–2048 rows × 8 bytes = 1–16 KB), the working set fits in L1 cache, so call-overhead savings from larger batches and cache-pressure costs from larger batches cancel out. The 1024 default sits at the measured minimum and is a reasonable choice for queries that ARE overhead-bound (more operators in the pipeline, string processing, aggregation over many groups).

## Takeaway

Phase 3 vectorized execution delivers real speedups on every query that isn't bottlenecked by hash-table lookups. The compound effect of zone-map pruning (Phase 2) plus vectorized execution (Phase 3) on sorted data is the headline result: **28.6x** faster than row storage for a season-filtered count. The two queries that remain slow in relative terms — GROUP BY on strings and hash join — share the same root cause: their hot path is pointer-heavy operations (string hashing, hash table probes) that benefit less from batch amortization than arithmetic-heavy column scans do. Phase 4 (SIMD intrinsics, dictionary-ID grouping without string decode, partitioned hash join) would address exactly those two cases.
