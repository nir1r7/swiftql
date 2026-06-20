# SwiftQL — Mini Analytical SQL Engine

A toy analytical SQL engine built in C++, designed as a learning project targeting internship roles at companies like Snowflake and Databricks. SwiftQL takes SQL queries as input, parses them, plans their execution, and runs them against structured tabular data stored as CSV files

> **Project thesis:** *"I built a correct SQL engine, then made storage smarter, then made execution significantly faster — and measured every step."*

---

## Table of Contents

- [Project Overview](#project-overview)
- [Feature Scope](#feature-scope)
- [Architecture](#architecture)
- [Data Domain](#data-domain)
- [Phase 1 — Correct Row-Based Engine](#phase-1--correct-row-based-engine-weeks-17)
- [Phase 2 — Columnar Storage + Hash Join](#phase-2--columnar-storage--hash-join-weeks-812)
- [Phase 3 — Vectorized Execution](#phase-3--vectorized-execution-weeks-1315)
- [Phase 4 — Cost-Based Optimizer](#phase-4--cost-based-optimizer-weeks-1618)
- [20-Week Plan](#20-week-plan)
- [Benchmarks](#benchmarks)
- [Build Instructions](#build-instructions)
- [Usage](#usage)
- [Limitations](#limitations)
- [Possible Extensions](#possible-extensions)

---

## Project Overview

SwiftQL is a **single-process analytical query engine**. It is not a full DBMS — there are no transactions, no multi-user sessions, and no write path. It is purely a **read query engine**, which is exactly the right scope for understanding how analytical database systems like Snowflake and Databricks work internally.

The project is structured in four progressive phases, each leaving a working and demonstrable system before moving to the next:

| Phase | Focus | Key Idea |
|---|---|---|
| 1 | Correct row-based SQL engine | Make it work |
| 2 | Columnar storage + encodings + pruning + hash join | Make storage smarter |
| 3 | Vectorized execution + late materialization | Make execution faster |
| 4 | Cost-based optimizer + predicate pushdown | Make planning smarter |

**Tech stack:**
- Core engine: C++
- Build system: CMake
- Testing: GoogleTest
- Benchmarking: Google Benchmark / custom harness
- Data generation + correctness testing: Python

---

## Feature Scope

### In Scope

- `SELECT`, `FROM`, `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT`
- `DISTINCT` — eliminates duplicate rows from output
- `IS NULL` / `IS NOT NULL` — null-aware predicate evaluation
- `JOIN ... ON` — hash join execution over columnar storage (Phase 2+)
- Aggregates: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`
- `EXPLAIN` — prints the query plan tree without executing
- `EXPLAIN ANALYZE` — executes the query and annotates each plan node with rows in, rows out, exclusive self-time (child time excluded), and % of total execution time; footer shows rows returned and separate parse, plan, and execution times
- `--storage row | columnar` — switches the storage backend
- `--execution volcano | vectorized` — switches the execution model
- Query result cache — identical queries served from cache without re-execution
- Cost-based optimizer — uses table and column statistics to reorder predicates and select join sides (Phase 4)
- Formal predicate pushdown — filters pushed as close to the scan as possible (Phase 4)
- CSV-based table storage with a `catalog.json` metadata file

### Explicitly Out of Scope

- `CREATE TABLE` SQL — tables registered via catalog only
- Subqueries
- Transactions / writes (`INSERT`, `UPDATE`, `DELETE`)
- Indexes
- Distributed execution
- Full SQL null semantics (three-valued logic) — null handling scoped to `IS NULL` / `IS NOT NULL` predicates and null display in output

---

## Architecture

The codebase is organized into clean, separated modules. Each module has a well-defined responsibility and a clear interface.

```
swiftql/
├── CMakeLists.txt
├── README.md
├── catalog.json
├── data/
│   ├── laps.csv
│   └── drivers.csv
├── src/
│   ├── common/       # Value, Schema, Row, TypeId
│   ├── catalog/      # Catalog, TableMetadata, TableStats
│   ├── storage/      # CSVLoader, ColumnarTable, encoders
│   ├── parser/       # Lexer, Parser, AST nodes
│   ├── planner/      # Validator, plan nodes, optimizer
│   ├── execution/    # Operators (volcano + vectorized)
│   └── cli/          # main.cc, result printer
├── include/
├── tests/
├── benchmarks/
└── python_tools/
    ├── generate_data.py
    ├── run_queries.py
    ├── compare_against_sqlite.py
    └── benchmark.py
```

### Layer 1 — Common (Foundation)

Everything else depends on this layer. No module reaches past it.

- `TypeId` enum — `INT`, `DOUBLE`, `STRING`
- `Value` — `std::variant<int64_t, double, std::string>` holding one cell's data, with null state
- `ColumnDef` — name + TypeId for one column
- `Schema` — ordered list of `ColumnDef` with lookup by name
- `Row` — `std::vector<Value>` representing one table row
- Error / result types — how the engine signals failures

### Layer 2 — Catalog

The engine's directory of what tables exist.

- `TableMetadata` — table name, file path, Schema
- `TableStats` — row count, per-column statistics (min, max, distinct count) — populated at load time, used by the Phase 4 optimizer
- `Catalog` — loads and stores all `TableMetadata` and `TableStats`; answers "does table X exist?", "what columns does it have?", "where is its file?"
- Backed by `catalog.json` on disk — no SQL DDL

Example `catalog.json`:

```json
{
  "tables": [
    {
      "name": "laps",
      "file": "data/laps.csv",
      "columns": [
        {"name": "lap_id",    "type": "INT"},
        {"name": "team",      "type": "STRING"},
        {"name": "speed",     "type": "DOUBLE"},
        {"name": "season",    "type": "INT"}
      ]
    }
  ]
}
```

### Layer 3 — Storage

Responsible for physically reading table data and turning it into something the execution engine can consume.

**Phase 1 — Row storage:**
- `CSVLoader` — reads a CSV file line by line, converts each line into a `Row` using the table schema
- Loaded rows held in memory as `std::vector<Row>` for the duration of a query

**Phase 2 — Columnar storage:**
- `ColumnArray` — a typed column: `std::variant<vector<int64_t>, vector<double>, vector<string>>`
- `ColumnarTable` — map of column name → ColumnArray + schema + row count
- `DictionaryEncoder` — maps unique strings to int IDs; stores column as `vector<int32_t>`
- `RLEColumn` — stores repeated-value columns as `(value, run_length)` pairs with a parallel `run_starts` prefix-sum array; `get(row_idx)` uses binary search — O(log n_runs); applied selectively when `n_runs < n_rows / 4` (2× threshold); 16 bytes/run
- `ColumnChunk` — a segment of a column with min, max, and row count metadata for zone-map pruning

### Layer 4 — Parser

Takes a raw SQL string and produces a structured Abstract Syntax Tree (AST). Hand-written recursive descent parser — no parser generator library.

**Grammar (restricted subset):**

```
select_stmt  → SELECT [DISTINCT] select_list FROM table_ref
               [JOIN IDENT ON expr]
               [WHERE expr]
               [GROUP BY col_list]
               [HAVING expr]
               [ORDER BY col_list]
               [LIMIT INT_LITERAL]

table_ref    → IDENT
select_list  → expr (COMMA expr)*
col_list     → IDENT (COMMA IDENT)*

expr         → or_expr
or_expr      → and_expr (OR and_expr)*
and_expr     → compare (AND compare)*
compare      → primary [(= | != | < | > | <= | >=) primary]
             | primary IS NULL
             | primary IS NOT NULL
primary      → IDENT
             | IDENT LPAREN expr RPAREN     ← aggregate call
             | IDENT LPAREN STAR RPAREN     ← COUNT(*)
             | INT_LITERAL
             | FLOAT_LITERAL
             | STRING_LITERAL
             | LPAREN expr RPAREN
```

**AST node types:**
- `ColumnRef` — reference to a column by name (with optional table qualifier)
- `Literal` — a constant value
- `BinaryExpr` — left expr, operator, right expr
- `IsNullExpr` — expr + is_not_null flag
- `AggregateExpr` — function name, argument expr, is_star flag
- `SelectStatement` — select list, from table, optional join, where, group-by, having, order-by, limit, distinct flag

### Layer 5 — Planner & Validator

Bridges the gap between the AST and the execution plan.

**Semantic validation:**
- `FROM` table exists in catalog
- All referenced columns exist in the relevant table schema
- Aggregate functions applied to compatible types only
- Non-aggregated `SELECT` columns appear in `GROUP BY` when aggregates are present
- `HAVING` only used when `GROUP BY` is present
- Aggregate functions not allowed in `WHERE` clause
- Join columns exist in their respective tables

**Plan nodes:**
- `SeqScanNode` — read from a table
- `FilterNode` — apply a predicate
- `ProjectNode` — select output columns / compute expressions
- `HashAggregateNode` — group by + aggregation functions
- `HavingNode` — post-aggregation filter
- `DistinctNode` — deduplication via hash set
- `SortNode` — `ORDER BY`
- `LimitNode` — `LIMIT N`
- `HashJoinNode` — build/probe hash join (execution wired in Phase 2; stubbed in Phase 1)

**Example plan** for `SELECT team, AVG(speed) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id WHERE season = 2025 GROUP BY team HAVING AVG(speed) > 300`:

```
Project [team, AVG(speed)]
  Having [AVG(speed) > 300]
    Aggregate [group_by=team, agg=AVG(speed)]
      Filter [season = 2025]
        HashJoin [laps.driver_id = drivers.driver_id]
          SeqScan [laps, 4 columns]
          SeqScan [drivers, 5 columns]
```

**Phase 4 — Optimizer pass (applied between planning and execution):**
- Formal predicate pushdown — `FilterNode`s moved as close to `SeqScanNode` as possible
- Predicate reordering — most selective predicates evaluated first using column distinct counts
- Join side selection — smaller table by row count chosen as build side

### Layer 6 — Execution Engine

**Execution modes are two orthogonal dimensions:**

| | Volcano (row-at-a-time) | Vectorized (batch) |
|---|---|---|
| **Row storage** | `--storage row --execution volcano` | — |
| **Columnar storage** | `--storage columnar --execution volcano` | `--storage columnar --execution vectorized` |

> `--storage row --execution vectorized` is not supported — vectorized execution is designed for and built on top of columnar storage. The three supported combinations allow clean isolation of storage gains vs execution gains in benchmarks.

**Phase 1 — Volcano / Iterator model:**

Each operator implements:
```cpp
void open();    // initialize state
Row* next();    // return next row, nullptr when exhausted
void close();   // release resources
```

| Operator | Behaviour |
|---|---|
| `SeqScanNode` | Returns rows one at a time from the loaded row vector |
| `FilterNode` | Calls child, evaluates predicate (including `IS NULL`), discards non-matching rows |
| `ProjectNode` | Calls child, evaluates select expressions, emits projected row |
| `HashAggregateNode` | Consumes all child rows into a hash map, emits one result row per group |
| `HavingNode` | Calls child, evaluates post-aggregation predicate, discards non-matching groups |
| `DistinctNode` | Calls child, tracks seen rows in a hash set, suppresses duplicates |
| `SortNode` | Consumes all child rows, sorts, emits in order |
| `LimitNode` | Passes rows through until N have been emitted |
| `HashJoinNode` | Build phase: smaller table into hash map. Probe phase: larger table probed row by row |

**Phase 3 — Vectorized model:**

Instead of one row at a time, operators exchange chunks. Late materialization is a first-class design principle: `VecFilterNode` produces a `SelectionVector` of valid row indices without copying or materializing data — columns are only fully materialized at `VecProjectNode` at the top of the pipeline.

```cpp
struct DataChunk {
    std::vector<ColumnVector> columns;
    int num_rows = 0;
};

struct SelectionVector {
    std::vector<int> indices;  // valid row indices within the chunk
    int size = 0;
};
```

| Operator | Behaviour |
|---|---|
| `VecScanNode` | Reads 1024 rows at a time from `ColumnarTable`, returns `DataChunk*` |
| `VecFilterNode` | Evaluates predicate across all rows in a tight loop, produces `SelectionVector` — no data copied |
| `VecProjectNode` | Materializes only required columns for rows passing the selection vector |
| `VecHashAggregateNode` | Processes one chunk at a time, updates group-by hash map in batch |
| `VecHashJoinNode` | Probe phase operates over `DataChunk` — batch lookup into build-side hash map |

### Layer 7 — Query Result Cache

Keyed on the raw SQL string. On a cache hit, cached result rows are returned without touching storage or execution. Cache is in-memory for the lifetime of the process. Bypassed with `--no-cache`.

```cpp
std::unordered_map<std::string, std::vector<Row>> result_cache;
```

Directly analogous to Snowflake's result cache.

### Layer 8 — CLI

```bash
./swiftql --catalog catalog.json --query "..."
./swiftql --catalog catalog.json --storage columnar --execution vectorized --query "..."
./swiftql --catalog catalog.json --query "..." --explain
./swiftql --catalog catalog.json --query "..." --explain-analyze
./swiftql --catalog catalog.json --query "..." --no-cache
./swiftql --catalog catalog.json --query "..." --no-optimize
```

### Layer 9 — Python Tooling

| Script | Purpose |
|---|---|
| `generate_data.py` | Generates synthetic F1 CSVs at configurable scale (1k / 100k / 1M rows) |
| `run_queries.py` | Runs a query file against SwiftQL and captures output |
| `compare_against_sqlite.py` | Runs same queries against SQLite, diffs results — correctness oracle |
| `benchmark.py` | Automates benchmark runs across all modes, generates results table and matplotlib plots |

---

## Data Domain

F1-themed tables generated synthetically via Python scripts.

> **Note:** Once the MVP is complete, TPC-H benchmark queries will be used for formal performance evaluation.

| Table | Columns |
|---|---|
| `laps` | lap_id, driver_id, team, speed, sector_1, sector_2, sector_3, season, round |
| `drivers` | driver_id, name, nationality, team, age |
| `races` | race_id, round, circuit, country, season |
| `pit_stops` | stop_id, lap_id, driver_id, duration_ms, season |

---

## Phase 1 — Correct Row-Based Engine (Weeks 1–7)

**Goal:** A working end-to-end SQL engine covering the full SQL surface area of the project. User types a query, engine returns correct results. Nothing fast yet — just correct.

Hash join is **parsed and planned** in this phase but execution is stubbed — join queries return a clean "not yet implemented" error at runtime. This keeps the SQL surface area complete from the start without coupling join execution to row storage.

### Week 1 — Project Scaffold + Common Layer

- Full folder structure and CMake build system with GoogleTest
- `TypeId`, `Value` (with null state), `ColumnDef`, `Schema`, `Row`
- Comparison operators on `Value`
- Unit tests: construct rows manually, assert types and comparisons

**Checkpoint:** Build system works. Value/Schema/Row solid and tested.

### Week 2 — Catalog + CSV Loader

- `TableMetadata`, `Catalog` with JSON loading via nlohmann/json
- `CSVLoader::load(filepath, schema)` → `std::vector<Row>`
- `generate_data.py` — generates F1 CSVs from day one

**Checkpoint:** Catalog resolves table names. CSV loads into typed rows.

### Week 3 — Lexer + AST Node Definitions

- `TokenType` enum covering all keywords (`SELECT`, `FROM`, `WHERE`, `GROUP`, `BY`, `HAVING`, `ORDER`, `LIMIT`, `DISTINCT`, `JOIN`, `ON`, `IS`, `NULL`, `AND`, `OR`, `NOT`, `AS`, `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`), operators, literals, punctuation
- `Token` struct with type, raw value, line/col for error messages
- `Lexer` with `nextToken()` and `peek()`
- AST node structs: `ColumnRef`, `Literal`, `BinaryExpr`, `IsNullExpr`, `AggregateExpr`, `SelectStatement`

**Checkpoint:** Lexer correctly tokenizes the full SQL target subset.

### Week 4 — Recursive Descent Parser

- `Parser` class consuming `Lexer` output
- One method per grammar rule
- Operator precedence: OR → AND → comparison → primary
- Support for: `DISTINCT`, `JOIN ... ON`, `HAVING`, `IS NULL` / `IS NOT NULL`
- `ParseError` with message and position on unexpected tokens

**Checkpoint:** Parser produces correct AST for all target query patterns including joins, having, distinct, and null predicates.

### Week 5 — Planner + Validator + Plan Nodes

- `Validator` — semantic checks against the catalog, including join column validation and having/group-by consistency
- `PlanNode` abstract base with `open()`, `next()`, `close()`
- Plan node classes: `SeqScanNode`, `FilterNode`, `ProjectNode`, `HashAggregateNode`, `HavingNode`, `DistinctNode`, `SortNode`, `LimitNode`, `HashJoinNode` (stubbed)
- `Planner::plan(SelectStatement, Catalog, table_rows)` → `PlanNode*` tree — accepts pre-loaded rows; planner performs no I/O

**Checkpoint:** Plan trees built correctly for all query types. Join queries plan but return "not yet implemented" at execution. Bad queries rejected with clean error messages.

### Week 6 — Expression Evaluator + Execution Operators

- `Value evaluate(Expr*, const Row&, const Schema&)` — handles `ColumnRef`, `Literal`, `BinaryExpr`, `IsNullExpr`
- Full operator implementations: `SeqScan`, `Filter`, `Project`, `HashAggregate`, `Having`, `Distinct`, `Sort`, `Limit`
- Null handling: null values propagate correctly through expressions; `IS NULL` / `IS NOT NULL` evaluate correctly; nulls display as `NULL` in output

**Checkpoint:** `SELECT DISTINCT team, AVG(speed) FROM laps WHERE season = 2025 AND speed IS NOT NULL GROUP BY team HAVING AVG(speed) > 300 ORDER BY team LIMIT 10` returns correct results.

### Week 7 — CLI + EXPLAIN ANALYZE + Result Cache + Integration Tests

- `main.cc` with `--catalog`, `--query`, `--storage`, `--execution`, `--explain`, `--explain-analyze`, `--no-cache`, `--no-optimize` args
- Aligned result printer with null display
- `EXPLAIN ANALYZE` — executes query; per-node exclusive self-time (child time excluded) and % of execution total; footer shows rows returned and parse/plan/execution breakdown (CSV load excluded from all timers, consistent with TPC-H benchmark methodology)
- Query result cache — `unordered_map<string, vector<Row>>`, bypassed with `--no-cache`
- `compare_against_sqlite.py` correctness harness — 20+ test queries passing vs SQLite
- Consistent error handling throughout — no crashes on bad input

**Checkpoint:** Phase 1 complete. All 20+ test queries pass vs SQLite. `--explain` and `--explain-analyze` work. Result cache demonstrated. Project fully demonstrable.

---

## Phase 2 — Columnar Storage + Hash Join (Weeks 8–12)

**Goal:** Replace row storage with a columnar layout. Add encodings and zone-map pruning. Wire up hash join execution over the columnar storage layer. Benchmark against Phase 1.

### Week 8 — Columnar Data Model + Conversion

- `ColumnArray` typed column arrays
- `ColumnarTable` — collection of columns + schema + row count
- `CSVToColumnar` converter — CSV rows transposed into column arrays
- `SeqScanNode` rewritten to operate on `ColumnarTable` by row index under `--storage columnar`
- All 20+ test queries still pass

**Checkpoint:** Engine correct on columnar layout. Both storage modes accessible via `--storage` flag.

### Week 9 — Projection Pushdown + Encodings

- `required_columns` set pushed down to `SeqScanNode` — planner determines which columns are needed, scan skips the rest
- `DictionaryEncoder` for string columns — unique strings mapped to `int32_t` IDs
- `RLEColumn` for integer columns with `n_runs < n_rows / 4` — `(value, run_length)` pairs plus a `run_starts` prefix-sum array enabling O(log n_runs) binary-search access; 16 bytes/run; columns exceeding the threshold stay as raw `vector<int64_t>`; `decodeRange()` deferred to Week 13 for vectorized path
- Storage size measured and recorded before/after encoding

**Checkpoint:** Fewer columns touched per query. Storage size reduced and measured.

### Week 10 — Zone-Map Chunk Pruning

- Each column split into `ColumnChunk`s of 8,192 rows
- Each chunk stores min, max, row count metadata
- `ChunkPruner` skips chunks provably non-matching for simple predicates (`col = val`, `col < val`, `col > val`)
- Wired into `SeqScanNode` — skipped chunks never accessed

**Checkpoint:** Selective queries skip chunks. Chunk skip count and speedup measured on large dataset.

### Week 11 — Hash Join Execution

- `HashJoinNode` execution wired up over columnar storage
- Build phase: scan the smaller table (by row count), populate `std::unordered_map<Value, std::vector<Row>>`
- Probe phase: scan the larger table row by row, probe hash map, emit joined rows
- Join queries execute end-to-end correctly
- All join test queries added to correctness harness and verified against SQLite

**Checkpoint:** Join queries execute correctly over columnar storage. Results match SQLite.

### Week 12 — Phase 2 Benchmarks + Cleanup

Benchmark queries on 1M-row dataset across `--storage row` and `--storage columnar` modes:

> **Note:** Benchmark times measured after CSV load to isolate query execution performance.

| Query | What it tests |
|---|---|
| `SELECT AVG(speed) FROM laps` | Full column scan aggregate |
| `SELECT COUNT(*) FROM laps WHERE season = 2025` | Selective filter + zone-map pruning |
| `SELECT team, speed FROM laps WHERE speed > 300` | Projection of 2 of 8 columns |
| `SELECT team, COUNT(*) FROM laps GROUP BY team` | Group by on dictionary-encoded string column |
| `SELECT l.team, AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team` | Hash join + aggregate |

Metrics per query: latency (ms, average of 5 runs), rows/sec, storage size.

**Checkpoint:** Row vs columnar benchmark numbers documented. Phase 2 demonstrably faster on analytical queries. Codebase cleaned and documented.

---

## Phase 3 — Vectorized Execution (Weeks 13–15)

**Goal:** Replace the row-at-a-time Volcano model with batch processing over columnar storage. Late materialization is a first-class design principle. Demonstrate and measure the speedup over Phase 2.

### Week 13 — Batch Abstraction + Vectorized Scan

- `DataChunk` and `SelectionVector` abstractions
- `VecScanNode` — reads 1024 rows at a time from `ColumnarTable`, returns `DataChunk*`
- New operator interface: `virtual DataChunk* nextChunk() = 0`
- Volcano operators remain intact — both execution paths coexist, selected via `--execution`

**Checkpoint:** `VecScan` returns correct chunks. Total row count across all chunks equals table size.

### Week 14 — Vectorized Filter + Project + Late Materialization

- `VecFilterNode` — evaluates predicate across entire chunk in a tight loop, produces `SelectionVector` — no data is copied or materialized
- `VecProjectNode` — only columns required for output are materialized, only for rows passing the selection vector — late materialization made explicit
- `EXPLAIN ANALYZE` updated to report materialization points per operator
- All 20+ test queries pass on vectorized path

**Checkpoint:** Selection vector pattern working. Late materialization documented in `EXPLAIN ANALYZE` output. Vectorized path correct.

### Week 15 — Vectorized Aggregate + Vectorized Hash Join + Phase 3 Benchmarks

- `VecHashAggregateNode` — processes one chunk at a time, updates group-by hash map in batch; dictionary-encoded string columns use integer ID comparison in the hot loop
- `VecHashJoinNode` — probe phase operates over `DataChunk`, batch lookup into build-side hash map
- Benchmark: same 5 queries across all three supported mode combinations
- Batch size experiment on `SELECT AVG(speed) FROM laps`: sizes 128, 256, 512, 1024, 2048 — latency recorded for each, sweet spot documented

**Checkpoint:** All three execution mode combinations benchmarked. Batch size sensitivity documented. Vectorized hash join correct.

---

## Phase 4 — Cost-Based Optimizer (Weeks 16–18)

**Goal:** Replace the rule-based planner with a statistics-driven optimizer. Use table and column statistics to make smarter planning decisions. Measure the impact independently of storage and execution changes.

### Week 16 — Statistics Collection

- `ColumnStats` — min, max, distinct count per column
- `TableStats` — row count + map of column name → `ColumnStats`
- Statistics computed at table load time and stored in `Catalog`
- `EXPLAIN ANALYZE` updated to show estimated vs actual row counts per plan node

**Checkpoint:** Stats populated for all tables on load. Visible in `EXPLAIN ANALYZE` output.

### Week 17 — Optimizer Rules

- **Formal predicate pushdown** — `FilterNode`s moved as close to `SeqScanNode` as possible in the plan tree, reducing rows flowing through upstream operators
- **Predicate reordering** — when multiple predicates exist in `WHERE`, most selective predicate (lowest `distinct_count / row_count` ratio) evaluated first
- **Join side selection** — smaller table by `row_count` chosen as build side in `HashJoinNode`; optimizer can override the order tables appear in the query
- Optimizer implemented as a plan tree rewrite pass between planning and execution

**Checkpoint:** Plan trees visibly reordered by optimizer. `EXPLAIN` shows pre- and post-optimization plans. Predicate ordering and join sides correct.

### Week 18 — Phase 4 Benchmarks + Full Project Benchmark Suite

- Benchmark optimizer gains in isolation: same queries, same storage and execution mode, optimizer on vs off via `--no-optimize`
- Document which query types benefit most, which are unaffected, and why
- Full 4-phase benchmark comparison — every benchmark query across every phase
- `benchmark.py` generates final comparison plots: latency by phase, rows/sec by phase, batch size sensitivity curve, optimizer impact

**Checkpoint:** Optimizer gains isolated and documented. Full benchmark suite complete. All plots generated.

---

## 20-Week Plan

| Week | Focus | Checkpoint |
|---|---|---|
| 1 | Scaffold + Common layer | Build system works, Value/Schema/Row tested |
| 2 | Catalog + CSV loader | Tables load from JSON + CSV |
| 3 | Lexer + AST nodes | Tokenizer correct for full SQL subset |
| 4 | Recursive descent parser | AST produced for all target queries incl. JOIN, HAVING, DISTINCT, IS NULL |
| 5 | Planner + validator | Plan trees built, join stubbed, bad queries rejected cleanly |
| 6 | Expression eval + operators | End-to-end queries correct incl. HAVING, DISTINCT, IS NULL, LIMIT |
| 7 | CLI + EXPLAIN ANALYZE + cache + tests | 20+ queries pass vs SQLite, EXPLAIN ANALYZE works, cache demonstrated |
| 8 | Columnar layout | Engine correct on columnar storage, --storage flag works |
| 9 | Projection pushdown + encodings | Fewer columns touched, storage size reduced and measured |
| 10 | Zone-map chunk pruning | Chunks skipped on selective queries, speedup measured |
| 11 | Hash join execution | Join queries execute correctly over columnar storage |
| 12 | Phase 2 benchmarks | Row vs columnar numbers + join benchmarks documented |
| 13 | DataChunk + VecScan | Batch reads correct, row count verified |
| 14 | VecFilter + VecProject + late materialization | Selection vector pattern correct, materialization documented |
| 15 | VecAggregate + VecHashJoin + Phase 3 benchmarks | All 3 mode combos benchmarked, batch size tuned |
| 16 | TableStats + ColumnStats | Stats populated on load, visible in EXPLAIN ANALYZE |
| 17 | Optimizer rules | Predicate pushdown, reordering, join side selection working |
| 18 | Phase 4 benchmarks + full suite | Optimizer gains isolated, all plots generated |
| 19 | Full integration pass | All modes correct, full test suite passing, clean build |
| 20 | README final + verbal prep | Project recruiting-ready |

---

## Benchmarks

*To be populated during Weeks 12, 15, and 18.*

> **Note:** All benchmark times measured after CSV load to isolate query execution performance. Once the MVP is complete, TPC-H benchmark queries will be used for formal evaluation.

### Phase Comparison

| Query | Row + Volcano (ms) | Col + Volcano (ms) | Col + Vectorized (ms) |
|---|---|---|---|
| `SELECT AVG(speed) FROM laps` | — | — | — |
| `SELECT COUNT(*) FROM laps WHERE season = 2025` | — | — | — |
| `SELECT team, speed FROM laps WHERE speed > 300` | — | — | — |
| `SELECT team, COUNT(*) FROM laps GROUP BY team` | — | — | — |
| `SELECT l.team, AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team` | — | — | — |

### Optimizer Impact (Phase 4, Col + Vectorized mode)

| Query | No Optimizer (ms) | With Optimizer (ms) |
|---|---|---|
| `SELECT AVG(speed) FROM laps WHERE season = 2025 AND speed > 300` | — | — |
| `SELECT l.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team` | — | — |

### Batch Size Sensitivity (Phase 3, `SELECT AVG(speed) FROM laps`)

| Batch Size | Latency (ms) |
|---|---|
| 128 | — |
| 256 | — |
| 512 | — |
| 1024 | — |
| 2048 | — |

---

## Build Instructions

```bash
# Clone the repository
git clone https://github.com/yourname/swiftql.git
cd swiftql

# Generate test data
python3 python_tools/generate_data.py --rows 100000

# Build
mkdir build && cd build
cmake ..
make -j$(nproc)

# Run tests
./tests/swiftql_tests
```

---

## Usage

```bash
# Run a query (defaults: row storage, volcano execution)
./swiftql --catalog catalog.json --query "SELECT team, AVG(speed) FROM laps WHERE season = 2025 GROUP BY team"

# Use columnar storage + vectorized execution
./swiftql --catalog catalog.json --storage columnar --execution vectorized --query "..."

# Print the query plan without executing
./swiftql --catalog catalog.json --query "..." --explain

# Execute and profile each plan node
./swiftql --catalog catalog.json --query "..." --explain-analyze

# Bypass the result cache
./swiftql --catalog catalog.json --query "..." --no-cache

# Disable the optimizer
./swiftql --catalog catalog.json --query "..." --no-optimize
```

**Example output:**

```
team       AVG(speed)
-----------------------
Ferrari    312.45
McLaren    308.91
Mercedes   310.17
```

**Example `--explain` output:**

```
Project [team, AVG(speed)]
  Aggregate [group_by=team, agg=AVG(speed)]
    Filter [season = 2025]
      SeqScan [laps, 4 columns]
```

**Example `--explain-analyze` output:**

```
Project [team, AVG(speed)]       rows_out=3                       time=0.1ms   (0.1%)
  Aggregate [group_by=team]      rows_in=48203   rows_out=3       time=12.4ms  (17.2%)
    Filter [season = 2025]       rows_in=1000000 rows_out=48203   time=38.2ms  (53.1%)
      SeqScan [laps, 4 columns]  rows_out=1000000                 time=21.3ms  (29.6%)

Rows returned: 3

Parse:     1.2ms
Plan:      0.8ms
Execution: 72.0ms
```

---

## Limitations

- No write path — `INSERT`, `UPDATE`, `DELETE` are not supported
- No `CREATE TABLE` SQL — tables must be registered via `catalog.json`
- Single join only — multi-way joins not supported
- No subqueries or correlated expressions
- Null handling scoped to `IS NULL` / `IS NOT NULL` predicates — full three-valued logic not implemented
- Commas inside string values not supported in CSV input
- No persistence beyond CSV files and catalog JSON
- Optimizer uses simple heuristics — no dynamic programming join ordering
- Result cache invalidation not implemented — cache is cleared on process restart only

---

## Possible Extensions

If the project completes ahead of schedule, the following extensions are candidates:

- **Binary columnar file format** — serialize `ColumnarTable` to a simple binary format on first load, read from binary on subsequent runs; eliminates CSV parsing overhead on cold start, analogous to how Parquet works
- **Parallel scan + parallel aggregation** — partition table chunks across threads using a thread pool; per-thread aggregation maps merged at the end; expected 2–4× speedup on scan-heavy workloads on multi-core systems

---

*Built as a learning project targeting internship roles at Snowflake and Databricks. Each phase is independently demonstrable with correctness tests and benchmarks.*
