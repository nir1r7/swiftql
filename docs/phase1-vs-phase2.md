# Phase 2 Benchmark Results

Row vs columnar storage, 1M rows, avg of 5 runs. Both modes use the Volcano (row-at-a-time) iterator model — Phase 3 vectorized execution is where batch processing will change the picture.

## Dataset

F1 laps table, 9 columns, 1M rows. **Data is sorted by season** (all 2022 rows first, then 2023, 2024, 2025). This makes zone-map chunk pruning extremely effective for season-filtered queries — entire contiguous blocks of chunks can be skipped without reading a single row.

## Memory Footprint

```
Row storage:    343 MB
Columnar:        57 MB
Compression:      6.0x
```

Dictionary encoding (strings → int32 IDs) and RLE (repeated season/round values across sorted runs) account for most of the reduction. This 6x smaller footprint means more of the working set fits in cache — a benefit that barely shows up under Volcano but will compound in Phase 3 once execution processes data in column-array batches.

## Results

```
Query                                        Row (ms)   Col (ms)  Speedup
-------------------------------------------------------------------------
Full scan aggregate                             645.5      943.1    0.68x
Selective filter + zone-map pruning             804.5      340.1    2.37x
Projection pushdown (2 of 9 cols)              1510.2     2287.2    0.66x
GROUP BY dictionary-encoded string              841.6     1202.5    0.70x
Hash join + aggregate                          4200.6     3733.0    1.13x
```

## Query Analysis

**Full scan aggregate** (`SELECT AVG(speed) FROM laps`) — row wins at 0.68x

No pruning, no skipping — every row must be visited. Under Volcano, columnar storage pays `getValue()` per row (std::variant dispatch + index math) while row storage reads a struct field directly. The layout advantage of isolating the `speed` column doesn't materialize when you're processing one row at a time.

---

**Selective filter + zone-map pruning** (`WHERE season = 2025`) — columnar wins at 2.37x

This is the flagship Phase 2 result. Because data is sorted by season, ~750k of 1M rows live in chunks whose zone-map max is below 2025 — those chunks are skipped entirely. Row storage has no chunk metadata and scans all 1M rows regardless. Columnar skips roughly 75% of the data before touching a single value.

---

**Projection pushdown (2 of 9 cols)** (`SELECT team, speed WHERE speed > 300`) — row wins at 0.66x, both are slow

`speed > 300` passes ~70% of rows (speed range: 280–345), so almost no chunks are pruned. Under Volcano, "only read 2 columns" still means two `getValue()` calls per row — the variant dispatch overhead multiplies rather than disappears. Row storage's contiguous struct layout beats columnar's indirection here. This query will look very different in Phase 3, where columnar can load the two needed column arrays as tight vectors and process them in batches.

---

**GROUP BY dictionary-encoded string** (`GROUP BY team`) — row wins at 0.70x

`team` is dictionary-encoded in columnar mode (string → int32 ID). Every row requires a decode lookup to recover the string key for the hash aggregate. Row storage holds the string directly. No pruning opportunity, same Volcano overhead — the encoding adds a layer rather than saving one under row-at-a-time access.

---

**Hash join + aggregate** (`JOIN drivers ON driver_id, GROUP BY team`) — roughly tied at 1.13x

Build side is 20 driver rows; probe side is 1M laps. Both modes are dominated by hash table probe overhead at scale, not storage I/O. The marginal columnar edge is likely noise — or a slight cache benefit from accessing only the join-key column rather than the full row during the probe phase.

## Takeaway

Phase 2's columnar storage has one clear superpower: **chunk pruning on sorted data**. Everything else runs slower under columnar + Volcano because the `getValue()` abstraction overhead outweighs any layout benefit when processing one row at a time.

Phase 3 (vectorized execution) addresses this directly — amortizing dispatch cost over batches of ~1024 rows and enabling tight loops over column arrays. The projection pushdown and GROUP BY queries are the ones to watch.