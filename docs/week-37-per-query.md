# Week 37 — per-query TPC-H latency, final

Milliseconds, median of 5 reps (3 at SF=1) after one discarded warmup.
**All engines measured on the same SwiftQL binary** (`build-release`, `-O3`,
containing every Week 37 optimization).

**Conditions, identical for all three tables:**

- SQLite and Postgres carry the TPC-H specification's own PRIMARY and FOREIGN key
  indexes, transcribed from `dbgen/dss.ri`, plus `ANALYZE`. These are properly
  configured competitors, not crippled ones.
- DuckDB and SwiftQL carry no indexes — both prune with automatic min/max zone
  maps instead, which is what their designs call for.
- SwiftQL runs `--storage columnar --execution vectorized`, single-threaded.
- `no-opt` is the same binary with `--no-optimize`: same storage, same executor,
  differing only in whether the logical plan was rewritten.
- `PG 1-thread` (SF=1 only) is Postgres with
  `max_parallel_workers_per_gather = 0`. Postgres's default allows 2 workers and
  every TPC-H table over 8 MB qualifies, so the default column is Postgres on up
  to 3 processes against SwiftQL's 1.
- Load excluded for every engine. Data is official `dbgen` V3.0.1.
- ⚠ marks a row-count disagreement — see the q15 note in
  [week-37-measurements.md](week-37-measurements.md). Excluded from all
  aggregates.

Machine: Apple silicon, 14 cores, 24 GB, macOS.

### SF=0.01

| query | name | SwiftQL | no-opt | SQLite | Postgres | DuckDB |
|---|---|---|---|---|---|---|
| q1 | Pricing Summary | **6.0** | 5.7 | 22.5 | 12.8 | 2.2 |
| q2 | Minimum Cost Supplier | **0.9** | 2.9 | 0.8 | 3.6 | 1.8 |
| q3 | Shipping Priority | **1.1** | 2.2 | 3.7 | 8.7 | 1.5 |
| q4 | Order Priority | **1.6** | 1.8 | 1.2 | 8.0 | 2.0 |
| q5 | Local Supplier Volume | **3.2** | 3.8 | 2.1 | 5.4 | 2.0 |
| q6 | Forecasting Revenue | **0.7** | 0.7 | 3.1 | 19.8 | 0.8 |
| q7 | Volume Shipping | **6.3** | 9.0 | 53.0 | 6.0 | 1.9 |
| q8 | National Market Share | **1.4** | 13.4 | 0.8 | 4.1 | 2.0 |
| q9 | Product Type Profit | **7.2** | 25.9 | 5.4 | 10.8 | 2.3 |
| q10 | Returned Item | **2.7** | 8.3 | 6.2 | 9.3 | 2.2 |
| q11 | Important Stock | **0.7** | 0.9 | 0.4 | 2.6 | 1.1 |
| q12 | Shipping Modes | **2.2** | 3.3 | 7.0 | 12.7 | 1.5 |
| q13 | Customer Distribution | **9.4** | 10.6 | 4.6 | 7.0 | 2.0 |
| q14 | Promotion Effect | **1.1** | 2.4 | 31.7 | 14.7 | 0.8 |
| q15 | Top Supplier | **1.3** | 1.3 | 92.2 | 28.9 | 1.2 |
| q16 | Parts/Supplier Rel. | **0.8** | 1.1 | 0.7 | 5.6 | 2.1 |
| q17 | Small-Quantity Order | **3.3** | 5.9 | 0.1 | 2.0 | 1.0 |
| q18 | Large Volume Customer | **6.0** | 6.0 | 4.4 | 11.8 | 2.4 |
| q19 | Discounted Revenue | **2.0** | 2.3 | 3.7 | 3.2 | 1.4 |
| q20 | Potential Part Promo | **0.3** | 0.3 | 0.2 | 2.6 | 0.6 |
| q21 | Suppliers Kept Waiting | **4.8** | 41.6 | 0.6 | 4.2 | 3.5 |
| q22 | Global Sales Opp. | **0.5** | 0.5 | 0.4 | 3.1 | 1.3 |

### SF=0.1

| query | name | SwiftQL | no-opt | SQLite | Postgres | DuckDB |
|---|---|---|---|---|---|---|
| q1 | Pricing Summary | **55.7** | 56.0 | 263.6 | 63.4 | 4.8 |
| q2 | Minimum Cost Supplier | **4.7** | 23.7 | 19.5 | 7.6 | 2.8 |
| q3 | Shipping Priority | **9.1** | 20.6 | 52.3 | 27.8 | 3.2 |
| q4 | Order Priority | **14.6** | 16.8 | 13.7 | 20.5 | 3.9 |
| q5 | Local Supplier Volume | **14.0** | 38.2 | 270.6 | 23.3 | 3.3 |
| q6 | Forecasting Revenue | **6.0** | 6.2 | 33.2 | 58.7 | 1.4 |
| q7 | Volume Shipping | **59.4** | 111.7 | 891.8 | 41.4 | 3.8 |
| q8 | National Market Share | **7.0** | 141.8 | 12.6 | 24.6 | 3.6 |
| q9 | Product Type Profit | **97.3** | 394.4 | 92.3 | 57.8 | 5.5 |
| q10 | Returned Item | **23.7** | 85.2 | 71.4 | 26.7 | 6.0 |
| q11 | Important Stock | **6.3** | 6.5 | 5.9 | 6.1 | 1.9 |
| q12 | Shipping Modes | **20.2** | 31.1 | 75.2 | 44.3 | 3.4 |
| q13 | Customer Distribution | **104.8** | 110.4 | 110.8 | 35.5 | 12.9 |
| q14 | Promotion Effect | **9.7** | 24.4 | 499.8 | 53.4 | 2.0 |
| q15 | Top Supplier | **11.5** | 11.5 | 1414.2 | 102.2 | 1.8 |
| q16 | Parts/Supplier Rel. | **6.1** | 9.2 | 7.6 | 32.4 | 4.1 |
| q17 | Small-Quantity Order | **44.3** | 68.5 | 3.5 | 8.3 | 2.4 |
| q18 | Large Volume Customer | **62.8** | 65.3 | 44.8 | 81.0 | 4.2 |
| q19 | Discounted Revenue | **17.1** | 21.2 | 38.9 | 5.8 | 3.9 |
| q20 | Potential Part Promo | **1.5** | 1.5 | 1.4 | 4.6 | 1.2 |
| q21 | Suppliers Kept Waiting | **59.8** | 455.0 | 43.6 | 39.3 | 9.5 |
| q22 | Global Sales Opp. | **2.9** | 2.8 | 3.3 | 6.2 | 2.5 |

### SF=1

| query | name | SwiftQL | no-opt | SQLite | Postgres | DuckDB | PG 1-thread |
|---|---|---|---|---|---|---|---|
| q1 | Pricing Summary | **561.4** | 560.6 | 2720.0 | 642.6 | 19.3 | 1794.8 |
| q2 | Minimum Cost Supplier | **45.1** | 241.2 | 251.3 | 113.7 | 5.2 | 136.5 |
| q3 | Shipping Priority | **103.8** | 304.1 | 731.1 | 245.1 | 10.0 | 610.6 |
| q4 | Order Priority | **156.7** | 198.7 | 144.2 | 202.3 | 9.5 | 508.3 |
| q5 | Local Supplier Volume | **149.3** | 444.2 | 3834.9 | 250.2 | 9.6 | 555.0 |
| q6 | Forecasting Revenue | **58.8** | 59.4 | 336.1 | 569.4 | 6.1 | 1531.1 |
| q7 | Volume Shipping | **659.9** | 1493.5 | 14300.3 | 1414.2 | 11.1 | 818.8 |
| q8 | National Market Share | **80.1** | 1701.5 | 1857.0 | 100.9 | 9.2 | 222.2 |
| q9 | Product Type Profit | **2235.4** | 5236.7 | 2203.4 | 766.5 | 23.4 | 1736.8 |
| q10 | Returned Item | **259.0** | 1050.0 | 784.6 | 246.9 | 18.5 | 918.8 |
| q11 | Important Stock | **18.4** | 47.7 | 53.4 | 21.4 | 2.5 | 20.4 |
| q12 | Shipping Modes | **204.7** | 376.4 | 778.8 | 386.8 | 10.9 | 1138.2 |
| q13 | Customer Distribution | **1484.4** | 1467.3 | 1609.6 | 263.2 | 26.5 | 499.3 |
| q14 | Promotion Effect | **98.6** | 264.9 | 7404.2 | 508.0 | 8.7 | 1366.5 |
| q15 ⚠ | Top Supplier | **119.5** | 117.4 | 21082.1 | 1014.7 | 6.8 | 2666.5 |
| q16 | Parts/Supplier Rel. | **53.4** | 86.3 | 90.4 | 170.4 | 12.7 | 355.1 |
| q17 | Small-Quantity Order | **1247.3** | 1289.6 | 46.2 | 49.2 | 6.7 | 51.8 |
| q18 | Large Volume Customer | **1085.3** | 983.2 | 457.7 | 863.6 | 16.9 | 745.7 |
| q19 | Discounted Revenue | **53.4** | 234.3 | 417.6 | 13.3 | 20.9 | 26.9 |
| q20 | Potential Part Promo | **12.8** | 13.2 | 16.4 | 36.6 | 3.5 | 39.1 |
| q21 | Suppliers Kept Waiting | **524.0** | 4848.2 | 546.6 | 667.6 | 34.6 | 698.9 |
| q22 | Global Sales Opp. | **26.5** | 26.3 | 33.9 | 23.3 | 9.0 | 49.6 |


## Aggregate (geometric mean of SwiftQL ÷ engine; below 1.0 = SwiftQL faster)

| | SF=0.01 | SF=0.1 | SF=1 |
|---|---|---|---|
| vs **SQLite** | 0.74× | **0.37×** | **0.33× (3.0× faster)** |
| vs **Postgres** (default, parallel) | **0.29×** | **0.65×** | **0.87× (1.15× faster)** |
| vs **Postgres** (single-threaded) | — | — | **0.49× (2.0× faster)** |
| vs **DuckDB** | 1.20× | 4.70× | 15.37× |
| optimizer (`no-opt` ÷ opt) | 1.69× | 1.92× | 2.22× |
| queries won vs SQLite | 9/22 | 15/22 | 17/21 |
| queries won vs Postgres | 18/22 | 15/22 | 14/21 |
| queries won vs DuckDB | 9/22 | 0/22 | 0/21 |

**SwiftQL is faster than both SQLite and Postgres at every scale factor
tested**, against indexed competitors, single-threaded, with no index of its
own. Against Postgres running on its default 3 processes it is still 1.15×
faster at SF=1.

**The DuckDB gap widens with scale** — 1.20× → 4.70× → 15.37× — and SwiftQL wins
9 of 22 at SF=0.01 but none at either larger scale. DuckDB uses 14 cores;
SwiftQL uses 1. Parallelism was deliberately out of scope.

**The optimizer's contribution grows with scale** — 1.69× → 1.92× → 2.22× — and
at SF=1 it improves 17 of 21 queries.
