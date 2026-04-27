# SwiftQL — Mini Analytical SQL Engine

A toy analytical SQL engine built in C++, designed as a learning project targeting internship roles at companies like Snowflake and Databricks. SwiftQL takes SQL queries as input, parses them, plans their execution, and runs them against structured tabular data stored as CSV files.


---

## Table of Contents

- [Project Overview](#project-overview)
- [Feature Scope](#feature-scope)
- [Architecture](#architecture)
- [Data Domain](#data-domain)
- [Phase 1 — Correct Row-Based Engine](#phase-1--correct-row-based-engine-weeks-17)
- [Phase 2 — Columnar Storage](#phase-2--columnar-storage-weeks-811)
- [Phase 3 — Vectorized Execution](#phase-3--vectorized-execution-weeks-1214)
- [16-Week Plan](#16-week-plan)
- [Benchmarks](#benchmarks)
- [Build Instructions](#build-instructions)
- [Usage](#usage)
- [Limitations](#limitations)

---

## Project Overview

SwiftQL is a **single-process analytical query engine**. It is not a full DBMS — there are no transactions, no multi-user sessions, and no write path. It is purely a **read query engine**, which is exactly the right scope for understanding how analytical database systems like Snowflake and Databricks work internally.

The project is structured in three progressive phases, each leaving a working and demonstrable system before moving to the next:

| Phase | Focus | Key Idea |
|---|---|---|
| 1 | Correct row-based SQL engine | Make it work |
| 2 | Columnar storage + encodings + pruning | Make storage smarter |
| 3 | Vectorized execution | Make execution faster |

**Tech stack:**
- Core engine: C++
- Build system: CMake
- Testing: GoogleTest
- Benchmarking: Google Benchmark / custom harness
- Data generation + correctness testing: Python

---

## Feature Scope

### In Scope

- `SELECT`, `FROM`, `WHERE`, `GROUP BY`, `ORDER BY`, `LIMIT`
- Aggregates: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX`
- `EXPLAIN` — prints the query plan tree instead of executing
- `--mode row | columnar | vectorized` — switches execution path
- Hash join (Phase 3 stretch goal)
- CSV-based table storage with a `catalog.json` metadata file

### Explicitly Out of Scope

- `CREATE TABLE` SQL (tables registered via catalog only)
- Subqueries
- `HAVING`
- Transactions / writes (INSERT, UPDATE, DELETE)
- Indexes
- Distributed execution

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
│   ├── catalog/      # Catalog, TableMetadata
│   ├── storage/      # CSVLoader, ColumnarTable, encoders
│   ├── parser/       # Lexer, Parser, AST nodes
│   ├── planner/      # Validator, plan nodes
│   ├── execution/    # Operators (row + vectorized)
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
- `Value` — `std::variant<int64_t, double, std::string>` holding one cell's data
- `ColumnDef` — name + TypeId for one column
- `Schema` — ordered list of `ColumnDef` with lookup by name
- `Row` — `std::vector<Value>` representing one table row
- Error / result types — how the engine signals failures

### Layer 2 — Catalog

The engine's directory of what tables exist.

- `TableMetadata` — table name, file path, Schema
- `Catalog` — loads and stores all `TableMetadata`; answers "does table X exist?", "what columns does it have?", "where is its file?"
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
- `RLEColumn` — stores repeated-value columns as `(value, run_length)` pairs
- `ColumnChunk` — a segment of a column with min, max, and row count metadata for pruning

### Layer 4 — Parser

Takes a raw SQL string and produces a structured Abstract Syntax Tree (AST).

**Grammar (restricted subset):**

```
select_stmt  → SELECT select_list FROM IDENT
               [WHERE expr]
               [GROUP BY col_list]
               [ORDER BY col_list]
               [LIMIT INT_LITERAL]

select_list  → expr (COMMA expr)*
col_list     → IDENT (COMMA IDENT)*

expr         → or_expr
or_expr      → and_expr (OR and_expr)*
and_expr     → compare (AND compare)*
compare      → primary [(= | != | < | > | <= | >=) primary]
primary      → IDENT
             | IDENT LPAREN expr RPAREN     ← aggregate call
             | IDENT LPAREN STAR RPAREN     ← COUNT(*)
             | INT_LITERAL
             | FLOAT_LITERAL
             | STRING_LITERAL
             | LPAREN expr RPAREN
```

**AST node types:**
- `ColumnRef` — reference to a column by name
- `Literal` — a constant value
- `BinaryExpr` — left expr, operator, right expr
- `AggregateExpr` — function name, argument expr, is_star flag
- `SelectStatement` — select list, from table, optional where/group-by/order-by/limit

Hand-written recursive descent parser — no parser generator library.

### Layer 5 — Planner & Validator

Bridges the gap between the AST and the execution plan.

**Semantic validation:**
- FROM table exists in catalog
- All referenced columns exist in the table schema
- Aggregate functions applied to compatible types only
- Non-aggregated SELECT columns appear in GROUP BY when aggregates are present

**Plan nodes:**
- `SeqScanNode` — read from a table
- `FilterNode` — apply a predicate
- `ProjectNode` — select output columns / compute expressions
- `AggregateNode` — group by + aggregation
- `SortNode` — ORDER BY
- `LimitNode` — LIMIT N

**Example plan** for `SELECT team, AVG(speed) FROM laps WHERE season = 2025 GROUP BY team`:

```
Project [team, AVG(speed)]
  Aggregate [group_by=team, agg=AVG(speed)]
    Filter [season = 2025]
      SeqScan [laps, 4 columns]
```

### Layer 6 — Execution Engine

**Phase 1 — Volcano / Iterator model:**

Each operator implements:
```cpp
void open();    // initialize state
Row* next();    // return next row, nullptr when exhausted
void close();   // release resources
```

Operators pull from their children — the top-level consumer calls `next()` on the root, which recurses down to the scan.

| Operator | Behaviour |
|---|---|
| `SeqScanNode` | Returns rows one at a time from the loaded row vector |
| `FilterNode` | Calls child, evaluates predicate, discards non-matching rows |
| `ProjectNode` | Calls child, evaluates select expressions, emits projected row |
| `HashAggregateNode` | Consumes all child rows into a hash map, then emits one result row per group |
| `SortNode` | Consumes all child rows, sorts, then emits in order |
| `LimitNode` | Passes rows through until N have been emitted |

**Phase 3 — Vectorized model:**

Instead of one row at a time, operators exchange chunks:

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
| `VecScanNode` | Reads 1024 rows at a time, returns `DataChunk*` |
| `VecFilterNode` | Evaluates predicate across all rows, produces a `SelectionVector` — no data copied |
| `VecProjectNode` | Evaluates projections across valid indices, materializes projected chunk |
| `VecHashAggregateNode` | Processes one chunk at a time, updates group-by hash map in batch |

### Layer 7 — CLI

```
./swiftql --catalog catalog.json --query "SELECT team, AVG(speed) FROM laps WHERE season = 2025 GROUP BY team"
./swiftql --catalog catalog.json --query "..." --explain
./swiftql --catalog catalog.json --query "..." --mode vectorized
```

### Layer 8 — Python Tooling

| Script | Purpose |
|---|---|
| `generate_data.py` | Generates synthetic F1 CSVs at configurable scale (1k / 100k / 1M rows) |
| `run_queries.py` | Runs a query file against SwiftQL and captures output |
| `compare_against_sqlite.py` | Runs same queries against SQLite, diffs results — correctness oracle |
| `benchmark.py` | Automates benchmark runs across modes, generates results table and plots |

---

## Data Domain

F1-themed tables generated synthetically via Python scripts.

| Table | Columns |
|---|---|
| `laps` | lap_id, driver_id, team, speed, sector_1, sector_2, sector_3, season, round |
| `drivers` | driver_id, name, nationality, team, age |
| `races` | race_id, round, circuit, country, season |
| `pit_stops` | stop_id, lap_id, driver_id, duration_ms, season |

Note: When MVP is built, should use TPC-H for benchmark testing

---

## Phase 1 — Correct Row-Based Engine (Weeks 1–7)

**Goal:** A working end-to-end SQL engine. User types a query, engine returns correct results. Nothing fast yet — just correct.

### Week 1 — Project Scaffold + Common Layer

- Full folder structure and CMake build system with GoogleTest
- `TypeId`, `Value`, `ColumnDef`, `Schema`, `Row`
- Comparison operators on `Value`
- Unit tests: construct rows manually, assert types and comparisons

**Checkpoint:** Build system works, Value/Schema/Row solid and tested.

### Week 2 — Catalog + CSV Loader

- `TableMetadata`, `Catalog` with JSON loading via nlohmann/json
- `CSVLoader::load(filepath, schema)` → `std::vector<Row>`
- `generate_data.py` — generates F1 CSVs from day one

**Checkpoint:** Catalog resolves table names. CSV loads into typed rows.

### Week 3 — Lexer + AST Node Definitions

- `TokenType` enum covering all keywords, operators, literals, punctuation
- `Token` struct with type, raw value, line/col for error messages
- `Lexer` with `nextToken()` and `peek()`
- AST node structs: `ColumnRef`, `Literal`, `BinaryExpr`, `AggregateExpr`, `SelectStatement`

**Checkpoint:** Lexer correctly tokenizes the full SQL target subset.

### Week 4 — Recursive Descent Parser

- `Parser` class consuming `Lexer` output
- One method per grammar rule
- Operator precedence: OR → AND → comparison → primary
- `ParseError` with message and position on unexpected tokens

**Checkpoint:** Parser produces correct AST for all target query patterns.

### Week 5 — Planner + Validator + Plan Nodes

- `Validator` — semantic checks against the catalog
- `PlanNode` abstract base with `open()`, `next()`, `close()`
- Plan node classes: `SeqScanNode`, `FilterNode`, `ProjectNode`, `AggregateNode`, `SortNode`, `LimitNode`
- `Planner::plan(SelectStatement, Catalog)` → `PlanNode*` tree

**Checkpoint:** Plan trees built correctly. Bad queries rejected with clean error messages.

### Week 6 — Expression Evaluator + Execution Operators

- `Value evaluate(Expr*, const Row&, const Schema&)`
- Handles: ColumnRef, Literal, BinaryExpr with comparison and logical operators
- Full operator implementations: SeqScan, Filter, Project, HashAggregate, Sort, Limit

**Checkpoint:** `SELECT team, AVG(speed) FROM laps WHERE season = 2025 GROUP BY team` returns correct results.

### Week 7 — CLI + Integration Tests + Phase 1 Polish

- `main.cc` with `--catalog`, `--query`, `--explain`, `--mode` args
- Aligned result printer
- `compare_against_sqlite.py` correctness harness — 20 test queries passing vs SQLite
- Consistent error handling throughout — no crashes on bad input

**Checkpoint:** Phase 1 complete. All 20 test queries pass. `--explain` works. Project demonstrable.

---

## Phase 2 — Columnar Storage (Weeks 8–11)

**Goal:** Replace row storage with a columnar layout. Add encodings. Add chunk-level pruning. Benchmark the difference.

### Week 8 — Columnar Data Model + Conversion

- `ColumnArray` typed column arrays
- `ColumnarTable` — collection of columns + schema + row count
- `CSVToColumnar` converter — CSV rows transposed into column arrays
- `SeqScanNode` rewritten to read from `ColumnarTable` by row index
- All 20 test queries still pass

**Checkpoint:** Engine correct on columnar layout.

### Week 9 — Projection Pushdown + Encodings

- `required_columns` set on `SeqScanNode` — planner pushes down which columns are needed
- `DictionaryEncoder` for string columns
- `RLEColumn` for repeated integer columns

**Checkpoint:** Fewer columns touched per query. Storage size reduced. Measured and recorded.

### Week 10 — Chunk-Level Pruning

- `ColumnChunk` with min, max, row count metadata per 8,192-row segment
- `ChunkPruner` — skips chunks provably non-matching for simple predicates (`col = val`, `col < val`, `col > val`)
- Wired into `SeqScanNode`

**Checkpoint:** Selective queries skip chunks. Speedup measured on large dataset.

### Week 11 — Phase 2 Benchmarking + Cleanup

Benchmark queries on 1M-row dataset in both row and columnar modes:

| Query | What it tests |
|---|---|
| `SELECT AVG(speed) FROM laps` | Full column scan aggregate |
| `SELECT COUNT(*) FROM laps WHERE season = 2025` | Selective filter + pruning |
| `SELECT team, speed FROM laps WHERE speed > 300` | Projection of 2 of 8 columns |
| `SELECT team, COUNT(*) FROM laps GROUP BY team` | Group by on encoded string column |

Metrics per query: latency (ms, average of 5 runs), rows/sec, storage size.

**Checkpoint:** Row vs columnar benchmark numbers documented. Phase 2 demonstrably faster.

---

## Phase 3 — Vectorized Execution (Weeks 12–14)

**Goal:** Replace row-at-a-time Volcano model with batch processing. Demonstrate the speedup.

### Week 12 — Batch Abstraction + Vectorized Scan

- `DataChunk` and `SelectionVector` abstractions
- `VecScanNode` — reads 1024 rows at a time from `ColumnarTable`
- New operator interface: `DataChunk* nextChunk()`
- Row-based operators remain intact — both paths coexist

**Checkpoint:** VecScan returns correct chunks. Total row count across all chunks equals table size.

### Week 13 — Vectorized Filter + Project

- `VecFilterNode` — evaluates predicate across entire chunk, produces `SelectionVector` without copying data
- `VecProjectNode` — evaluates projections across valid indices, materializes projected chunk
- Same 20 test queries pass on vectorized path

**Checkpoint:** Selection vector pattern working. Vectorized path correct.

### Week 14 — Vectorized Aggregate + Benchmarking + Hash Join (stretch)

- `VecHashAggregateNode` — processes one chunk at a time, updates group-by hash map in batch
- Benchmark: same 4 queries across all 3 modes (row / columnar / vectorized)
- Batch size experiment: 128, 256, 512, 1024, 2048 — latency recorded for each

**Stretch goal — Hash Join:**
- Extend parser with `JOIN ... ON ...` clause
- Build phase: scan smaller table into `unordered_map<Value, Row>`
- Probe phase: for each chunk from the larger table, probe the hash map in batch

**Checkpoint:** All 3 modes benchmarked. Batch size sweet spot documented. Hash join working if time allowed.

---

## 16-Week Plan

| Week | Focus | Checkpoint |
|---|---|---|
| 1 | Scaffold + Common layer | Build system works, Value/Schema/Row tested |
| 2 | Catalog + CSV loader | Tables load from JSON + CSV |
| 3 | Lexer + AST nodes | Tokenizer correct for full SQL subset |
| 4 | Recursive descent parser | AST produced for all target queries |
| 5 | Planner + validator | Plan trees built, bad queries rejected cleanly |
| 6 | Expression eval + operators | End-to-end queries return correct results |
| 7 | CLI + integration tests | 20 queries pass vs SQLite, --explain works |
| 8 | Columnar layout | Engine still correct on columnar storage |
| 9 | Projection pushdown + encodings | Fewer columns touched, storage size reduced |
| 10 | Chunk pruning | Chunks skipped on selective queries |
| 11 | Phase 2 benchmarks | Row vs columnar numbers documented |
| 12 | DataChunk + VecScan | Batch reads correct |
| 13 | VecFilter + VecProject | Selection vector pattern working |
| 14 | VecAggregate + benchmarks | All 3 modes benchmarked, batch size tuned |
| 15 | Integration + plots | Clean build, benchmark graphs generated |
| 16 | README + verbal prep | Project recruiting-ready |

---

## Benchmarks

*To be populated during Weeks 11 and 14.*

### Phase 1 vs Phase 2 vs Phase 3
Note: Benchmark times should be measured after the CSV is loaded

| Query | Row (ms) | Columnar (ms) | Vectorized (ms) |
|---|---|---|---|
| `SELECT AVG(speed) FROM laps` | — | — | — |
| `SELECT COUNT(*) FROM laps WHERE season = 2025` | — | — | — |
| `SELECT team, speed FROM laps WHERE speed > 300` | — | — | — |
| `SELECT team, COUNT(*) FROM laps GROUP BY team` | — | — | — |

### Batch Size Sensitivity (Phase 3)

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
# Run a query
./swiftql --catalog catalog.json --query "SELECT team, AVG(speed) FROM laps WHERE season = 2025 GROUP BY team"

# Explain the query plan without executing
./swiftql --catalog catalog.json --query "..." --explain

# Choose execution mode
./swiftql --catalog catalog.json --query "..." --mode row
./swiftql --catalog catalog.json --query "..." --mode columnar
./swiftql --catalog catalog.json --query "..." --mode vectorized
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

---

## Limitations

- No write path — INSERT, UPDATE, DELETE are not supported
- No `CREATE TABLE` SQL — tables must be registered via `catalog.json`
- Single table queries only in Phase 1 (join support added as Phase 3 stretch goal)
- No subqueries or `HAVING` clause
- No NULL handling — all values assumed non-null
- Commas inside string values not supported in CSV input
- No query optimizer — plan is rule-based only, no cost model
- No persistence beyond CSV files and catalog JSON

---

*Built as a learning project targeting internship roles at Snowflake and Databricks. Each phase is independently demonstrable with correctness tests and benchmarks.*