# Week 37 — per-query TPC-H latency, final

Milliseconds, median of 5 reps (3 at SF=1) after one discarded warmup.
**All engines measured on one SwiftQL binary** (`build-release`, `-O3`), which
contains every Week 37 optimization *and* the two query-port fixes the published
TPC-H answer set exposed (see
[week-37-measurements.md](week-37-measurements.md) §13).

**Conditions, identical across all three tables:**

- SQLite and Postgres carry the TPC-H specification's own PRIMARY and FOREIGN key
  indexes (`dbgen/dss.ri`) plus `ANALYZE`. Properly configured competitors.
- DuckDB and SwiftQL carry none — both prune with automatic min/max zone maps.
- SwiftQL runs `--storage columnar --execution vectorized`, single-threaded.
- `no-opt` is the same binary with `--no-optimize`.
- `PG 1-thread` (SF=1) is Postgres with `max_parallel_workers_per_gather = 0`.
  Its default allows 2 workers and every TPC-H table over 8 MB qualifies, so the
  default column is Postgres on up to 3 processes against SwiftQL's 1.
- Load excluded for every engine. Official `dbgen` V3.0.1 data.
- ⚠ marks a row-count disagreement (q15 — see the measurements log). Excluded
  from every aggregate.

Machine: Apple silicon, 14 cores, 24 GB, macOS.

### SF=0.01

| query | name | SwiftQL | no-opt | SQLite | Postgres | DuckDB |
|---|---|---|---|---|---|---|
| q1 | Pricing Summary | **5.8** | 5.7 | 22.1 | 12.3 | 2.2 |
| q2 | Minimum Cost Supplier | **0.7** | 2.9 | 0.8 | 3.6 | 1.8 |
| q3 | Shipping Priority | **1.1** | 2.2 | 3.6 | 9.1 | 1.5 |
| q4 | Order Priority | **1.6** | 1.8 | 1.2 | 7.9 | 2.0 |
| q5 | Local Supplier Volume | **2.9** | 3.9 | 2.1 | 5.8 | 2.0 |
| q6 | Forecasting Revenue | **0.7** | 0.7 | 3.2 | 16.4 | 0.7 |
| q7 | Volume Shipping | **2.9** | 9.0 | 52.0 | 6.7 | 2.1 |
| q8 | National Market Share | **1.2** | 13.5 | 0.8 | 4.5 | 2.1 |
| q9 | Product Type Profit | **6.2** | 26.1 | 5.3 | 11.6 | 2.4 |
| q10 | Returned Item | **2.4** | 8.4 | 6.4 | 9.4 | 2.5 |
| q11 | Important Stock | **0.6** | 0.9 | 0.4 | 2.5 | 1.1 |
| q12 | Shipping Modes | **2.2** | 3.3 | 7.1 | 12.5 | 1.5 |
| q13 | Customer Distribution | **9.4** | 9.5 | 4.6 | 6.9 | 1.8 |
| q14 | Promotion Effect | **1.1** | 2.5 | 31.6 | 14.3 | 0.8 |
| q15 | Top Supplier | **1.3** | 1.3 | 95.2 | 27.0 | 1.2 |
| q16 | Parts/Supplier Rel. | **0.8** | 1.1 | 0.7 | 5.7 | 2.0 |
| q17 | Small-Quantity Order | **3.2** | 5.9 | 0.1 | 1.9 | 0.7 |
| q18 | Large Volume Customer | **6.1** | 6.0 | 4.3 | 11.9 | 2.3 |
| q19 | Discounted Revenue | **1.0** | 2.3 | 3.8 | 2.8 | 1.3 |
| q20 | Potential Part Promo | **2.8** | 2.7 | 0.4 | 2.6 | 2.1 |
| q21 | Suppliers Kept Waiting | **4.2** | 42.0 | 0.6 | 3.7 | 3.4 |
| q22 | Global Sales Opp. | **0.5** | 0.4 | 0.3 | 1.6 | 1.3 |

### SF=0.1

| query | name | SwiftQL | no-opt | SQLite | Postgres | DuckDB |
|---|---|---|---|---|---|---|
| q1 | Pricing Summary | **56.6** | 55.4 | 261.4 | 64.8 | 4.5 |
| q2 | Minimum Cost Supplier | **4.7** | 24.0 | 21.2 | 7.4 | 2.9 |
| q3 | Shipping Priority | **9.1** | 20.8 | 52.2 | 26.0 | 3.3 |
| q4 | Order Priority | **14.7** | 16.9 | 13.7 | 20.0 | 4.2 |
| q5 | Local Supplier Volume | **11.9** | 38.5 | 271.3 | 22.2 | 3.4 |
| q6 | Forecasting Revenue | **6.1** | 6.0 | 32.4 | 58.8 | 1.4 |
| q7 | Volume Shipping | **26.6** | 110.8 | 891.6 | 41.2 | 3.9 |
| q8 | National Market Share | **6.6** | 143.4 | 12.6 | 24.9 | 3.5 |
| q9 | Product Type Profit | **90.7** | 380.2 | 91.8 | 58.8 | 5.8 |
| q10 | Returned Item | **20.1** | 85.7 | 71.5 | 26.2 | 6.2 |
| q11 | Important Stock | **3.7** | 6.6 | 5.7 | 6.2 | 1.8 |
| q12 | Shipping Modes | **20.5** | 31.3 | 75.6 | 44.2 | 3.1 |
| q13 | Customer Distribution | **105.6** | 107.6 | 110.1 | 36.4 | 12.2 |
| q14 | Promotion Effect | **9.8** | 24.6 | 494.8 | 53.3 | 1.8 |
| q15 | Top Supplier | **11.5** | 11.5 | 1414.5 | 103.1 | 1.7 |
| q16 | Parts/Supplier Rel. | **6.2** | 9.3 | 7.7 | 34.7 | 4.0 |
| q17 | Small-Quantity Order | **42.0** | 68.9 | 3.6 | 7.7 | 2.5 |
| q18 | Large Volume Customer | **65.4** | 64.8 | 45.7 | 85.2 | 4.4 |
| q19 | Discounted Revenue | **6.4** | 21.1 | 37.8 | 6.0 | 3.1 |
| q20 | Potential Part Promo | **26.4** | 26.6 | 6.4 | 9.2 | 3.8 |
| q21 | Suppliers Kept Waiting | **49.6** | 464.3 | 43.4 | 39.2 | 9.4 |
| q22 | Global Sales Opp. | **2.9** | 2.9 | 3.3 | 7.1 | 2.5 |

### SF=1

| query | name | SwiftQL | no-opt | SQLite | Postgres | DuckDB | PG 1-thread |
|---|---|---|---|---|---|---|---|
| q1 | Pricing Summary | **554.2** | 551.8 | 2733.6 | 615.0 | 21.4 | 1794.8 |
| q2 | Minimum Cost Supplier | **42.3** | 241.4 | 248.4 | 114.2 | 5.3 | 136.5 |
| q3 | Shipping Priority | **88.8** | 273.6 | 744.0 | 245.0 | 9.9 | 610.6 |
| q4 | Order Priority | **157.5** | 195.3 | 143.8 | 216.0 | 9.6 | 508.3 |
| q5 | Local Supplier Volume | **125.8** | 462.7 | 3959.2 | 241.9 | 9.7 | 555.0 |
| q6 | Forecasting Revenue | **59.6** | 59.9 | 337.1 | 567.2 | 6.1 | 1531.1 |
| q7 | Volume Shipping | **317.7** | 1515.8 | 14669.9 | 1435.5 | 11.3 | 818.8 |
| q8 | National Market Share | **68.9** | 1701.8 | 1836.3 | 107.1 | 9.1 | 222.2 |
| q9 | Product Type Profit | **2316.6** | 5353.5 | 2225.5 | 745.8 | 23.3 | 1736.8 |
| q10 | Returned Item | **226.0** | 1034.9 | 776.0 | 245.4 | 19.0 | 918.8 |
| q11 | Important Stock | **18.3** | 47.5 | 53.0 | 24.5 | 2.7 | 20.4 |
| q12 | Shipping Modes | **213.4** | 400.6 | 769.0 | 389.1 | 11.2 | 1138.2 |
| q13 | Customer Distribution | **1484.6** | 1483.8 | 1575.0 | 256.0 | 23.9 | 499.3 |
| q14 | Promotion Effect | **96.8** | 273.5 | 7366.2 | 519.4 | 8.7 | 1366.5 |
| q15 ⚠ | Top Supplier | **118.7** | 120.0 | 22205.7 | 1030.9 | 6.8 | 2666.5 |
| q16 | Parts/Supplier Rel. | **52.6** | 88.0 | 90.3 | 175.3 | 12.8 | 355.1 |
| q17 | Small-Quantity Order | **947.7** | 1558.9 | 48.0 | 48.5 | 6.6 | 51.8 |
| q18 | Large Volume Customer | **1093.3** | 1048.6 | 460.9 | 785.4 | 17.5 | 745.7 |
| q19 | Discounted Revenue | **53.6** | 241.3 | 421.0 | 13.2 | 15.0 | 26.9 |
| q20 | Potential Part Promo | **505.4** | 460.2 | 101.9 | 108.1 | 12.1 | 39.1 |
| q21 | Suppliers Kept Waiting | **540.5** | 4945.7 | 541.2 | 699.5 | 35.7 | 698.9 |
| q22 | Global Sales Opp. | **26.2** | 26.3 | 33.3 | 23.1 | 9.0 | 49.6 |


## Aggregate (geometric mean of SwiftQL ÷ engine; below 1.0 = SwiftQL faster)

| | SF=0.01 | SF=0.1 | SF=1 |
|---|---|---|---|
| vs **SQLite** | 0.70× | **0.35×** | **0.33× (3.0× faster)** |
| vs **Postgres** (default, parallel) | **0.30×** | **0.62×** | **0.90× (1.11× faster)** |
| vs **Postgres** (single-threaded) | — | — | **0.54× (1.9× faster)** |
| vs **DuckDB** | 1.16× | 4.55× | 16.07× |
| optimizer (`no-opt` ÷ opt) | 1.92× | 2.22× | **2.44×** |
| queries won vs SQLite | 10/22 | 17/22 | 16/21 |
| queries won vs Postgres | 18/22 | 16/22 | 14/21 |
| queries won vs DuckDB | 10/22 | 0/22 | 0/21 |

**SwiftQL is faster than both SQLite and Postgres at every scale factor tested**,
against indexed competitors, single-threaded, with no index of its own. Against
Postgres on its default three processes it is still 1.11× faster at SF=1.

**The DuckDB gap widens with scale** — 1.16× → 4.55× → 16.07× — and SwiftQL wins
10 of 22 at SF=0.01 but none at either larger scale. DuckDB uses 14 cores.

**The optimizer's contribution grows with scale** — 1.92× → 2.22× → **2.44×** —
and at SF=1 it improves 17 of 21 queries.

## What the query-port fixes cost, and why the numbers are honest

q19 and q20 previously ran text that differed in meaning from the specification.
Fixing them made both queries *harder*, and the SF=1 table shows it:

| query | before the fix | after | note |
|---|---|---|---|
| q19 | 31.7 ms | 53.6 ms | the port had admitted an extra shipmode |
| q20 | 45.2 ms | **505.4 ms** | the port had replaced a correlated aggregate with `ps_availqty > 0` |

**q20 is now a loss** — 505 ms against SQLite's 102 ms and Postgres's 108 ms —
where the weakened version was competitive. That is the correct number for the
query TPC-H actually specifies, and it moved the SF=1 Postgres geometric mean
from 0.87× to 0.90×. Publishing the improvement without this would have been
reporting a benchmark we had made easier.
