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
- [Phase 4 — Vectorized Cost-Based Optimizer](#phase-4--vectorized-cost-based-optimizer-weeks-1623)
- [Phase 5 — TPC-H Compatibility + Benchmarks](#phase-5--tpc-h-compatibility--benchmarks-weeks-2437)
- [37-Week Plan](#37-week-plan)
- [Benchmarks](#benchmarks)
- [Build Instructions](#build-instructions)
- [Usage](#usage)
- [Limitations](#limitations)
- [Possible Extensions](#possible-extensions)

---

## Project Overview

SwiftQL is a **single-process analytical query engine**. It is not a full DBMS — there are no transactions, no multi-user sessions, and no write path. It is purely a **read query engine**, which is exactly the right scope for understanding how analytical database systems like Snowflake and Databricks work internally.

The project is structured in five progressive phases, each leaving a working and demonstrable system before moving to the next:

| Phase | Focus | Key Idea |
|---|---|---|
| 1 | Correct row-based SQL engine | Make it work |
| 2 | Columnar storage + encodings + pruning + hash join | Make storage smarter |
| 3 | Vectorized execution + late materialization | Make execution faster |
| 4 | Vectorized cost-based optimizer | Make planning smarter |
| 5 | TPC-H SQL coverage + benchmarks | Test the complete system |

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
- `JOIN ... ON` — hash join execution over columnar storage (Phase 2+). Several `JOIN` clauses and `AND`-chained equi-join keys (`ON a.x = b.x AND a.y = b.y`) parse, bind and plan as of Week 26; executing them is Week 27
- Aggregates: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX` — `COUNT` returns `INT`, `SUM`/`AVG` return `DOUBLE`, and `MIN`/`MAX` preserve their argument type (so `MIN(team)` is a `STRING`)
- General expressions (Week 24) — arithmetic `+ - * /` with SQL precedence and unary minus, expression aliases (`SELECT expr AS name`, referenceable in `GROUP BY`/`ORDER BY`), expressions in projection, aggregate arguments (`SUM(price * (1 - discount))`), grouping, and ordering; plan-time expression type checking. SQLite semantics: `INT / INT` truncates, `x / 0` is `NULL`
- Predicates and scalar functions (Week 25) — `[NOT] BETWEEN`, `[NOT] LIKE` (ASCII case-insensitive, matching SQLite), `[NOT] IN` over a constant list, searched `CASE`, `SUBSTRING`, ISO-8601 date literals (`date '1998-12-01'`, stored as `STRING`), and constant-folded interval arithmetic (`date '1994-01-01' + interval '1' year`)
- `EXPLAIN` — prints the query plan tree without executing; on the vectorized path shows three sections (logical plan, optimized logical plan with estimated rows, physical plan with the join's cost decision)
- `EXPLAIN ANALYZE` — executes the query and annotates each plan node with rows in, rows out, estimated rows (vectorized, optimizer on), exclusive self-time (child time excluded), and % of total execution time; footer shows rows returned and separate parse, plan, and execution times
- `--storage row | columnar` — switches the storage backend
- `--execution volcano | vectorized` — switches the execution model
- Query result cache — identical queries served from cache without re-execution
- Cost-based optimizer for vectorized execution — statistics, cardinality estimation, predicate pushdown, and physical join selection (Phase 4)
- Multi-way joins, richer expressions, subqueries, and TPC-H benchmarking (Phase 5)
- CSV-based table storage with a `catalog.json` metadata file

### Explicitly Out of Scope

- `CREATE TABLE` SQL — tables registered via catalog only
- Transactions / writes (`INSERT`, `UPDATE`, `DELETE`)
- Indexes
- Distributed execution
- Window functions, `OVER`, and recursive CTEs — no TPC-H query needs them
- Column ordinals, unary `+`, and scientific-notation float literals — see [Syntax Deliberately Not Supported](#syntax-deliberately-not-supported)

> SQL NULL itself is **not** out of scope and is fully modelled: `ColumnVector`
> carries a validity mask, expressions and aggregates propagate NULL, `AND`/`OR`
> are three-valued, and `ORDER BY` sorts NULL first ascending / last descending as
> SQLite does. `CASE` and `SUBSTRING` shipped in Week 25.

### Syntax Deliberately Not Supported

Plausible SQL that SwiftQL rejects. Each is a clean error, not a wrong answer.

| Not supported | Rejected with | Why |
|---|---|---|
| Column ordinals — `ORDER BY 1`, `GROUP BY 2` | `column ordinals are not supported; use a column name or a select-list alias` | A bare integer parses as a `Literal`, so ORDER BY 1 would sort every row by the same constant. Select-list aliases cover the need |
| Unary plus — `SELECT +speed` | parse error at the `+` | The grammar has unary `-` only; `+x` is a no-op |
| Scientific-notation floats — `1e5`, `1.5e3` | parse error — the exponent lexes as a separate identifier | The lexer's number rule has no exponent part |
| Scalar functions other than `SUBSTRING` — `ABS(x)`, `UPPER(x)` | parse error at the `(` | The call form accepts only the five aggregate keywords plus `SUBSTRING`. No TPC-H query in the documented dialect needs another one |
| General `NOT` — `WHERE NOT (x = 1)` | `NOT is supported only as NOT BETWEEN, NOT LIKE, NOT IN or IS NOT NULL` | Those four cover every TPC-H negation. A general prefix `NOT` would need its own node and three-valued kernel for no query's benefit. Both the leading position (`NOT x = 1`, what users write) and the postfix one (`x NOT 5`) give this message |
| `IN (subquery)` — `x IN (SELECT ...)` | `IN accepts a list of constant values only` | Week 32 lowers set-membership subqueries to semi-/anti-joins; that is a different production, not an extension of the constant list |
| Computed `LIKE` patterns — `x LIKE y` | parse error — a constant pattern string is required | TPC-H never computes a pattern, and a constant one is analysed once per query rather than per row |
| `LIKE ... ESCAPE` — matching a literal `%` or `_` | `unexpected trailing input after the end of the query` | There is no escape clause and no default escape character, so a literal wildcard cannot be matched. SQLite supports this; no TPC-H pattern needs it. Until Week 25 the parser silently **discarded** everything after the last clause it recognised, so an `ESCAPE` was dropped and the query returned wildcard results — a wrong answer, not an error. `Parser::parse` now requires end-of-input (a trailing `;` is still fine) |
| Trailing input — `SELECT team FROM laps LIMIT 2 GARBAGE` | `unexpected trailing input after the end of the query` | Same root cause as the row above. Pre-existing since Week 4 |
| `SUBSTRING` out-of-domain positions — `SUBSTRING(x, 0, 3)`, `SUBSTRING(x, -2)`, `SUBSTRING(x, 1, -1)` | `start position must be >= 1` / `length must be >= 0` | SQLite **defines** all three (`substr('Ferrari',0,3)` is `'Fe'`, a negative start counts from the right, a negative length takes the preceding characters). SwiftQL rejects them rather than reimplement three sign conventions no TPC-H query uses. Decided at plan time whenever the arguments are constant — which after constant folding is every realistic query; only a *computed* position reaches a per-row throw. Note `compare_against_sqlite.py` cannot police this: the query errors instead of producing rows to diff, so `SubstringOf.RejectsTheOutOfDomainArgumentsSqliteDefines` is the guard |
| Nested aggregates — `SUM(AVG(x))` | `aggregate functions cannot be nested` | Not meaningful without window functions |
| Dates outside `0000-01-01 .. 9999-12-31` — `date '1994-01-01' + interval '100000' year` | `date arithmetic result is outside the supported range 0000-01-01 .. 9999-12-31` | The ISO-8601 STRING representation carries exactly four year digits. Before Week 25 closed this, an out-of-range year wrapped modulo 10000 and returned the **input date** with no error, and a negative year rendered a non-digit byte (`000*-01-01`) that then compared as an ordinary STRING. Matches SQLite's `date()` range |
| Integer overflow promotion — `9223372036854775807 + 1` | `integer overflow in '+'` | SQLite promotes to REAL; SwiftQL cannot, because the INT/INT result type is fixed at plan time and truncating division depends on it. Erroring beats the silent signed-overflow UB this replaced |

> **Batch-evaluation consequence:** the vectorized path evaluates a whole chunk
> before `LIMIT` truncates it, so an expression that raises on a row the query
> would have discarded still raises. `SELECT lap_id * 2305843009213693952 FROM laps
> LIMIT 1` returns a row under `--execution volcano` and errors under
> `--execution vectorized`. This is inherent to eager batch evaluation; the
> alternative is evaluating row-at-a-time, which is the thing vectorization exists
> to avoid.

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
- `TableStats` — row count and per-column min, max, distinct count, null count, and average width — populated at load time for the Phase 4 optimizer
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
               (JOIN table_ref ON join_cond)*   ← Week 26: repeatable
               [WHERE expr]
               [GROUP BY group_list]
               [HAVING expr]
               [ORDER BY order_list]
               [LIMIT INT_LITERAL]

table_ref    → IDENT [[AS] IDENT]              ← table alias, required for a self-join
join_cond    → equality (AND equality)*        ← Week 26: multi-key equi-join;
                                                 each equality compares a column of
                                                 the joined relation with one of a
                                                 preceding relation
select_list  → select_item (COMMA select_item)*
select_item  → expr [AS IDENT]                 ← expression alias (Week 24)
group_list   → expr (COMMA expr)*              ← expression group keys (Week 24)
order_list   → expr [ASC|DESC] (COMMA expr [ASC|DESC])*

expr         → or_expr
or_expr      → and_expr (OR and_expr)*
and_expr     → compare (AND compare)*
compare      → additive [(= | != | < | > | <= | >=) additive]
             | additive IS [NOT] NULL
             | additive [NOT] BETWEEN additive AND additive   ← Week 25, desugared
             | additive [NOT] LIKE STRING_LITERAL             ← Week 25
             | additive [NOT] IN LPAREN const_list RPAREN     ← Week 25
additive     → multiplicative (('+' | '-') multiplicative)*   ← Week 24
multiplicative → unary (('*' | '/') unary)*                   ← Week 24
unary        → '-' unary | primary                            ← Week 24
primary      → IDENT [DOT IDENT]               ← optionally qualified column
             | agg_fn LPAREN expr RPAREN       ← aggregate call
             | COUNT LPAREN STAR RPAREN        ← COUNT(*)
             | CASE (WHEN expr THEN expr)+ [ELSE expr] END    ← Week 25
             | SUBSTRING LPAREN expr FROM expr [FOR expr] RPAREN   ← Week 25
             | SUBSTRING LPAREN expr COMMA expr [COMMA expr] RPAREN
             | DATE STRING_LITERAL             ← ISO-8601 date, a STRING Literal
             | INTERVAL STRING_LITERAL unit    ← folded away at plan time
             | INT_LITERAL
             | FLOAT_LITERAL
             | STRING_LITERAL
             | LPAREN expr RPAREN

agg_fn       → COUNT | SUM | AVG | MIN | MAX   ← keywords, not arbitrary identifiers
const_list   → literal (COMMA literal)*        ← constants only; IN (subquery) is Week 32
unit         → day[s] | month[s] | year[s]     ← matched as identifier text (case-insensitive), NOT reserved
```

> The function-call form is restricted to the five aggregate keywords plus
> `SUBSTRING`: `ABS(speed)` is a parse error, not an unknown-function error.
> `SUBSTRING` is the only scalar function; if a second one is ever needed, the
> right move is a generic `FunctionExpr { name, args }` with a name→kernel
> registry, not a third one-off node.
>
> `BETWEEN` is **desugared by the parser** into `a >= x AND a <= y` (`NOT BETWEEN`
> into `a < x OR a > y`). There is no `BetweenExpr` node, because the desugared
> shape is what predicate pushdown, zone-map pruning, the tight comparison loop and
> range selectivity all pattern-match on. Its bounds parse at the *additive* level
> so that `BETWEEN` binds tighter than `AND`, as SQL requires.

**AST node types:**
- `ColumnRef` — reference to a column by name (with optional table qualifier)
- `Literal` — a constant value
- `BinaryExpr` — left expr, operator, right expr (comparisons, AND/OR, and arithmetic `+ - * /`)
- `UnaryExpr` — prefix operator (unary minus) + operand
- `IsNullExpr` — expr + is_not_null flag
- `AggregateExpr` — function name, argument expr (any expression as of Week 24), is_star flag
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

**Phase 4 — Vectorized optimizer pipeline:**
- Binder resolves aliases and columns to stable identities
- Logical plan separates query meaning from executable operators
- Statistics and cardinality estimates drive predicate placement and physical join selection
- `VectorizedPlanBuilder` lowers the optimized plan into `VecPlanNode`s
- Volcano execution remains the correctness baseline and is not optimized

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
struct ColumnVector {
    std::variant<vector<int64_t>, vector<double>, vector<string>> data;
    TypeId type;
    // SQL NULL. all_valid == true means `validity` is empty and no row is NULL —
    // the common case, since ColumnarTable cannot express NULL and scan output is
    // therefore always all-valid. Reads go through valueAt(), writes through
    // appendColumnValue(); touching `data` directly bypasses the mask.
    bool all_valid = true;
    std::vector<uint8_t> validity;
};

struct DataChunk {
    std::vector<ColumnVector> columns;
    int num_rows = 0;
};

struct SelectionVector {
    std::vector<int> indices;  // valid row indices within the chunk
    int size = 0;
};
```

Expressions are evaluated a chunk at a time by `ExpressionExecutor`, which
resolves node dispatch once at compile time instead of per row. The scalar
`evaluate()` remains the semantic reference and the fallback for any expression
shape the executor declines to compile.

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

### Week 15 — Vectorized Aggregate + Vectorized Hash Join + Full Vectorized Path + Phase 3 Benchmarks

- `VecHashAggregateNode` — processes one chunk at a time, updates group-by hash map in batch; dictionary-encoded string columns use integer ID comparison in the hot loop
- `VecHashJoinNode` — probe phase operates over `DataChunk`, batch lookup into build-side hash map
- `VecLimitNode` — tracks rows emitted across chunks, truncates the final chunk when the limit is reached; enables early termination of the scan without reading the full table
- `VecSortNode` — blocking operator: collects all chunks into a flat buffer, sorts surviving rows by ORDER BY column indices, re-emits as `DataChunk`s
- `VecDistinctNode` — blocking operator: collects all surviving rows, deduplicates via row-hash set, re-emits as `DataChunk`s
- Benchmark: same 5 queries across all three supported mode combinations
- Batch size experiment on `SELECT AVG(speed) FROM laps`: sizes 128, 256, 512, 1024, 2048 — latency recorded for each, sweet spot documented

**Checkpoint:** All three execution mode combinations benchmarked. Batch size sensitivity documented. Vectorized hash join correct. Full query suite (including ORDER BY, DISTINCT, LIMIT) runs end-to-end on the vectorized path with no Volcano fallthrough.

---

## Phase 4 — Vectorized Cost-Based Optimizer (Weeks 16–23)

**Goal:** Add a statistics-driven optimizer to the columnar/vectorized path. Preserve Volcano execution as the correctness baseline and measure optimizer gains independently.

### Week 16 — Binder + Planning Correctness

- Resolve table aliases and qualified columns to stable relation/column identities
- Reject ambiguous references and preserve logical output order across join-side swaps
- Fix cache configuration keys, 64-bit row metrics, and zero-row plan reporting

**Checkpoint:** Bound expressions are unambiguous and existing queries remain correct.

### Week 17 — Logical Plan

- Add execution-independent scan, filter, join, aggregate, project, sort, distinct, and limit nodes
- Build logical plans from the bound AST without moving data into execution operators

**Checkpoint:** Existing vectorized queries produce complete logical plans.

### Week 18 — Vectorized Physical Planning

- Add `VectorizedPlanBuilder` to lower logical plans into `VecPlanNode`s
- Remove vectorized tree construction from `main.cc`
- Wire `--no-optimize` through the shared logical and physical planning path

**Checkpoint:** Optimized and unoptimized modes share one vectorized plan builder.

### Week 19 — Statistics Collection

- Collect row count plus column min, max, distinct count, null count, and average width
- Store statistics in `Catalog` after table loading

**Checkpoint:** Statistics are populated and tested for every loaded table.

### Week 20 — Cardinality Estimation

- Estimate scan, equality, range, conjunction, join, and aggregate cardinalities
- Use documented fallback selectivities for unsupported expressions

**Checkpoint:** Estimated row counts propagate through the logical plan.

### Week 21 — Predicate Optimization

- Split conjunctions and classify predicates by referenced relation
- Push filters to the lowest legal plan node
- Order scan-local predicates by expected work and cascade selection vectors

**Checkpoint:** Both join inputs are filtered before the join when legal.

### Week 22 — Cost Model + Physical Join Selection

- Add explicit CPU, data-volume, and hash-table memory costs
- Choose the filtered hash-join build side from cost estimates (hash is the only physical join algorithm at this stage; a second operator and the cost-based choice between algorithms are deferred to Week 23.5)
- Keep logical schema order independent of physical build/probe order

**Checkpoint:** The cheapest supported single-join plan is selected from estimates.

> **Scope note (data-volume cost):** The Week 22 cost model implements the CPU and
> hash-table memory terms; the data-volume (bytes-materialized) term is deferred to
> Week 28. At single-join scope both build-side options move the same data volume, so
> the term cannot change the decision and is not testable here — it first affects a
> plan choice in Week 28, where differing intermediate-result widths across join
> orderings make it discriminate. The hash-table memory term already uses real
> per-column `avg_width` statistics, so build-side selection accounts for row width.

### Week 23 — Explainability + Phase 4 Benchmarks

- Show logical and optimized plans, estimated rows, costs, and optimizer
  decisions on the vectorized path (row/Volcano `--explain` prints the
  physical plan only)
- Compare estimates with actual rows in `EXPLAIN ANALYZE`
- Benchmark vectorized execution with and without optimization

**Checkpoint:** Phase 4 gains and estimation errors are measured and documented.

### Week 23.5 — SIMD Small-Build Loop Join

Inserted extension. Gives the Week 22 cost-based optimizer a real second physical
join algorithm to choose against, so the "compare join algorithms" decision stops
being hypothetical.

- Add `VecSimdLoopJoinNode` — a vectorized inner equi-join that holds the small build side's INT keys in a flat contiguous buffer and probes them with SIMD comparisons (NEON on ARM, AVX2 on x86); the probe hot loop scans only the key buffer, touching payload columns only for matched rows — output is materialized, as with the hash join
- Cost the SIMD loop against the hash join and let the optimizer pick per join from cardinality estimates; restrict SIMD selection to INT keys with a small build side, falling back to hash join otherwise (STRING/DOUBLE keys, large builds)
- Ship a scalar reference path behind the same operator and calibrate the cost crossover from on-device measurement

> **Result (measured):** the SIMD loop join beats the hash join up to a crossover of
> ≈ 52–57 build rows (Apple M4 Pro, stable across 100k and 1M probe rows), so
> `CPU_SIMD_COMPARE = 0.02` models the crossover at 50. Methodology, full numbers, and
> the constant inversion in [docs/hash-vs-simd-crossover.md](docs/hash-vs-simd-crossover.md).

**Checkpoint:** The optimizer selects the SIMD loop join over the hash join when the build side is small, its results match the hash join and SQLite, and the measured hash/SIMD crossover point is documented.

---

## Phase 5 — TPC-H Compatibility + Benchmarks (Weeks 24–37)

**Goal:** Extend SwiftQL to a documented TPC-H SQL dialect, optimize multi-table queries, and publish correctness and performance results.

### Week 24 — General Expressions

- Add arithmetic precedence, unary minus, expression aliases, and expression type checking
- Support expressions in projection, aggregation, grouping, and ordering

**Checkpoint:** TPC-H revenue expressions parse, bind, and execute. ✅

Making the above *correct* pulled in five things the original two bullets did not
anticipate. They are dialect facts, so Phase 5's correctness report inherits them:

| Shipped | Why it was required |
|---|---|
| **NULL is represented natively** — `ColumnVector` carries a validity mask; reads/writes go through `valueAt` / `appendColumnValue` | `x / 0` → NULL is the first way ordinary SQL produces a NULL (CSV cannot express one). The vectorized path previously flattened every NULL to a `0` / `"NULL"` sentinel, which is indistinguishable from a real zero and makes the Week 29 outer join unimplementable |
| **`ExpressionExecutor`** — compiles an expression once, then one typed loop per node per chunk | The scalar `evaluate()` was being called per row inside the vectorized hot loops. On 1M rows that cost 290 ms for one aggregate expression against 16 ms for a plain column, and it is the whole reason vectorization exists |
| **Constant folding** (`foldConstants`, run at the end of binding) | Three fast paths pattern-match on `ColumnRef op Literal`; a constant subexpression defeated zone-map pruning, the tight comparison loop, and selectivity estimation simultaneously. `WHERE season = 2020 + 4` went 203 ms → 0.35 ms |
| **Three-valued `AND`/`OR`** in both evaluators | Propagating NULL made the two engines return *different answers* for the same query (0 rows vs 10000). TPC-H Q19 is an OR chain over nullable columns |
| **`compareForSort`** — a total order for `ORDER BY`, separate from the SQL comparison operators | The SQL operators return false for every comparison against NULL, making NULL equivalent to every value and equivalence non-transitive. That is not a strict weak ordering, so `std::stable_sort` was undefined behaviour: it reordered the **non-NULL** keys and dropped rows under `LIMIT` |

Also settled here: `MIN`/`MAX` preserve their argument type (so `MIN(team)` is a
`STRING`), INT arithmetic is overflow-checked rather than wrapping, and column
ordinals / nested aggregates / unary `+` are rejected rather than mis-answered —
see [Syntax Deliberately Not Supported](#syntax-deliberately-not-supported).

### Week 25 — Predicates + Scalar Functions

- Add `BETWEEN`, `LIKE`, `IN`, `CASE`, and `SUBSTRING`
- Add ISO date literals and constant-folded interval arithmetic

**Checkpoint:** Required non-subquery TPC-H expressions execute correctly. ✅

Two decisions shaped the result, and both are dialect facts Phase 5's
correctness report inherits:

| Shipped | Why it was required |
|---|---|
| **`BETWEEN` is desugared, not a node** — the parser emits `a >= x AND a <= y` (`NOT BETWEEN` → `a < x OR a > y`, valid De Morgan in Kleene 3VL) | Five mechanisms pattern-match on `ColumnRef op Literal`: `splitConjuncts`, `ChunkPruner`, `scanColumn`'s tight loop, `selectivity()`, and `orderByWork`. A `BetweenExpr` node would forfeit all five *and* cost 17 dispatch sites. Measured: a desugared `BETWEEN` skips 116/123 chunks on 1M rows — byte-identical to the longhand form. **Cost:** the left operand is cloned, so a *computed* one is evaluated twice — `SUBSTRING(team,1,3) BETWEEN 'A' AND 'M'` runs two SUBSTRING kernels. Every TPC-H `BETWEEN` has a bare column on the left, and the AND cascade evaluates the second copy over survivors only, so the duplication is bounded |
| **`CASE` has no vectorized kernel, deliberately** | `evaluate()` short-circuits; a chunk kernel cannot. Since INT arithmetic is overflow-checked, an eager kernel raises on rows whose branch is discarded — so the two evaluators would legitimately disagree. `compileNode` declines, and the fallback is correct-but-slow: measured 406 ms vs 63 ms per 1M rows for the same aggregate (Release). `LIKE` and `IN` *do* compile — `IN` costs the same as a native comparison, since the list is hashed once at plan time. The masked-evaluation `CASE` kernel is Week 37 work, gated on Q8/Q12/Q14 profiling |

Three further things the original two bullets did not anticipate:

- **The dispatch checklist was under-counted.** It documented 14 sites; there are
  **17**. `checkGroupedRefs` (a function separate from `Validator::validateExpr`),
  `collectSlots` and `restampSlots` all fail silently and all were missing. The
  first lets an ungrouped column inside a `CASE` reach `inferExprType` and fail
  with a far-from-the-cause `column not found`; the other two silently stop
  `LIKE`/`IN` conjuncts from being pushed below a join, which is where TPC-H
  Q12/Q14/Q16/Q19 spend their time. See
  [development.md → Extending the expression language](development.md#extending-the-expression-language).
- **`IN` restricted to a constant list** — which makes the set hashable once at
  compile time and, because the grammar has no NULL literal, makes NULL-in-list
  unreachable, collapsing SQL's three-valued `IN` rule to "operand NULL → NULL".
- **`IntervalLiteral` is a node that must not survive planning.** `foldNode`
  rewrites `date ± interval` to a date `Literal`; an interval that reaches
  `inferExprType` or `evaluate()` throws, because the query was not constant date
  arithmetic. C++17 has no `std::chrono::year_month_day`, so the civil-date
  conversion is hand-rolled in `src/common/date_util.h`.

> **Carried into Week 36.** `extract(year from d)` has no equivalent; the dialect
> rewrite is `SUBSTRING(d, 1, 4)`, which yields a **`STRING`** year while the TPC-H
> reference answers show an integer. Decide there whether the harness normalizes or
> the dialect grows a numeric conversion. Note `SUM(SUBSTRING(...))` is rejected at
> plan time, correctly, since the result is a STRING.

### Week 26 — Multi-Way Join Language + Binding

- Parse multiple explicit `JOIN ... ON` clauses
- Extend binding and logical planning to arbitrary relation counts
- Lift the Phase 4 validator restriction on `ON` conditions (single
  cross-relation equality, enforced by `classifyJoinCondition`): support
  multi-key equi-joins (`ON a.x = b.x AND a.y = b.y`) — required for TPC-H Q9

**Checkpoint:** Multi-table queries produce a qualified logical join tree. ✅

Executing those trees is Week 27. A multi-way or multi-key join binds, validates
and — on the vectorized path — builds a full logical plan, then refuses at
physical lowering with `... are planned but not yet executable (Week 27)`; the
same stance as Phase 1's stubbed hash join, and the reason
`compare_against_sqlite.py` grew a rejection suite (nothing new this week
returns rows to diff).

Both refusals are placed so that a genuine query defect outranks a temporary
engine limitation. The **multi-key** refusal is deferred past `Planner::plan`'s
plan-time type checks — all of them, including the projection's, which
`buildProjectSchema` performs last — because the merged join schema is built
from the two children's schemas and never reads the keys. So those checks are
valid for a multi-key query, and both engines report the same thing whichever
clause the second fault is in.

The **multi-way** refusal cannot be deferred the same way, and this is the one
place the two engines differ. `Planner::plan` builds exactly one join, so with
three relations the merged schema would be missing a relation's columns entirely
and a deferred type check would report a misleading `column not found`; Volcano
therefore refuses immediately after validation, while the vectorized path builds
the whole logical plan first and can report a real type error instead. Both
messages are true, neither engine accepts what the other rejects, and Week 27
turns it into an ordinary capability difference — row mode never gains multi-way
execution.

Four things the two bullets did not anticipate:

| Shipped | Why it was required |
|---|---|
| **`SelectStatement::join` → `joins`, an ordered vector** | `std::optional` is the type-level encoding of "at most one join", and 14 sites in `src/` branched on it. Making it a vector turns every one into a compile error rather than a place a second relation is silently dropped. The index *is* the arithmetic the rest of the week rests on: `joins[i]` is relation slot `i+1` |
| **`classify()` returns a slot, not a side** (`predicate_pushdown.cc`) | It collapsed "not slot 0" into `PushTarget::JOIN`, and `pushIntoJoin` attached those conjuncts to `join->children[1]`. Correct with two relations; with three, `children[1]` is one specific scan, so a conjunct was filtered **against the wrong table** — a wrong answer, not lost pushdown. Pushdown now walks the left-deep spine and routes each conjunct to the subtree owning its relation |
| **Dispatch site 18 extended in the same commit** (`Validator::validateJoinCondition`) | Relaxing `classifyJoinCondition` is what made it reachable: an `AND`-chain used to be rejected before it ran. It now dispatches every `Expr` subtype. The case that actually reaches it is an unqualified name matching no relation, which the Binder leaves unresolved and the classifier's unbound branch lets past |
| **Slot stamping generalized, in two places that must agree** | The merged join schema stamps the newly added side with the join's relation slot (was a hardcoded `1`), and `CardinalityEstimator`'s JOIN case does the same to its stats context. A standalone scan still stamps slot 0 locally, which is what keeps `restampSlots(…, 0)` and `ChunkPruner`'s `relation_slot < 1` scan-local test correct at any relation count |

> **Starting notes, from Week 25's foundations.**
> - **Relaxing `classifyJoinCondition` makes `Validator::validateJoinCondition`
>   a live silent dispatch site — extend both in the same commit.** It recurses
>   `ColumnRef` and `BinaryExpr` only, so a Week 25 node inside an `ON` clause
>   gets no column-existence check. It is unreachable today *only* because
>   `classifyJoinCondition` runs first and rejects anything that is not a single
>   `=` between two `ColumnRef`s. The moment multi-key `ON` is legal,
>   `ON a.x = b.x AND a.team LIKE 'F%'` validates nothing. It is site 18 in
>   [development.md → Extending the expression language](development.md#extending-the-expression-language).
> - The two-relation model is narrower than it looks — `relation_slot` is
>   already an `int` assigned positionally by the Binder, not a boolean, so
>   widening the range table to N entries is the natural generalization. Four
>   places hardcode the binary assumption: `classify()` in
>   `predicate_pushdown.cc` (a three-way `FROM`/`JOIN`/`RESIDUAL` enum), the
>   `relation_slot == 1` branch in `validator.cc`, `ChunkPruner`'s
>   `relation_slot < 1` scan-local test, and `SelectStatement::join` itself —
>   a `std::optional<JoinClause>` that 14 sites branch on.
> - The pushdown hazard is `classify()`, and it is a **wrong answer**, not just
>   lost pushdown. It collapses "references neither slot 0 nor a mix" into
>   `PushTarget::JOIN`, and `pushIntoJoin` then attaches those conjuncts to
>   `join->children[1]`. With two relations that is exactly right; with three or
>   more in a left-deep tree, `children[1]` is one specific scan, so a conjunct
>   belonging to a different relation is filtered against the wrong table.
>   `classify()` has to return a slot, not a side, before the tree grows a third
>   scan. `ChunkPruner`'s `relation_slot < 1` test is *not* affected — pushed
>   conjuncts are re-stamped to 0 by `restampSlots` because they sit above a
>   standalone single-relation scan, which stays true at any relation count.
> - A second `JOIN` is currently a clean parse error rather than a silently
>   truncated single-join answer — Week 25 made `Parser::parse` require
>   end-of-input. That is the correct pre-state to build on; before it, the
>   trailing `JOIN ... ON ...` was discarded and the query returned a one-join
>   result with no error.

### Week 27 — Multi-Way Join Execution

- Lower general logical join trees to vectorized hash joins
- Build a join graph and assign local and join predicates
- Route non-equality `ON` conjuncts (rejected since the Phase 4 audit) as
  residual post-join filters during predicate assignment — required for
  TPC-H Q21-style conditions

**Checkpoint:** Three-or-more-table joins execute correctly.

> **Starting notes, from Week 26's foundations.**
> - **Two guards must come down together with the lowering work**, and they are
>   the only reason a wrong multi-way answer is impossible today:
>   `checkLowerable` in `vectorized_plan_builder.cc` (join count, then key count
>   — that order is load-bearing, so both engines report the same reason) and the
>   `stmt.joins.size() > 1` / `keys.size() != 1` pair in `Planner::plan`. Volcano
>   has no multi-way execution planned: keep its refusal and narrow the message
>   to name the path (`... not supported on the Volcano path; use --execution
>   vectorized`), rather than deleting it and letting `HashJoinNode` produce
>   something. `Planner::plan`'s *multi-key* refusal is deferred to after the
>   plan-time type checks (a flag set in the join block, thrown after the ORDER
>   BY checks and the projection's) — delete that flag with the guard, and keep the multi-way one
>   early, since one join is all that function builds and a deferred check would
>   run against a schema missing a relation.
> - **De-duplicate join keys before building the hash-key tuple.**
>   `ON a.x = b.x AND a.x = b.x` yields two identical `JoinKey`s today. Harmless
>   while multi-key refuses, and semantically harmless as a predicate, but it
>   makes the probe tuple wider than it needs to be and it already double-counts
>   in `CardinalityEstimator`'s NDV product.
> - **`Validator::validateJoinCondition` (site 18) is a second opinion, not the
>   authority.** For a bound statement the Binder is what proves a column
>   exists, and site 18 re-checks it by *slot* — never by `table_name`, which
>   for an unqualified ref the Binder rewrites to its relation's table name, so
>   a name match lands on whichever relation is aliased to that name (this
>   rejected a legal query for one round of Week 26). Its Week 25 branches and
>   its `AggregateExpr` branch are pre-positions: `classifyJoinCondition`
>   refuses those shapes first today. When non-equality `ON` conjuncts become
>   legal residuals, bind them like any other expression and keep the slot check
>   slot-based.
> - **`keys[k].from_col` resolves against `children[0]`, whose merged schema can
>   hold the same column name at several slots.** `JoinKey::from_slot` carries
>   the binder slot for exactly this reason; use `Schema::indexOf(name, slot)`
>   when locating the probe column, not the bare-name overload, or a join on
>   `team` across three relations binds to whichever side comes first.
> - `--explain` of a multi-way join currently errors instead of printing the
>   logical section, because `main.cc` lowers before it prints the captured
>   sections. Lifting the guard fixes it for free; no reordering needed.

### Week 28 — Join Enumeration

- Add left-deep dynamic-programming join ordering with a configurable limit
- Use a greedy fallback for larger join graphs

**Checkpoint:** `EXPLAIN` shows cost-based multiway join order decisions.

### Week 29 — Outer Join

- Add logical and vectorized left outer hash join
- Preserve unmatched rows and stable output slots

**Checkpoint:** TPC-H Q13 join semantics are supported.

### Week 30 — Subquery Parsing + Binding

- Add nested query AST nodes and scoped name resolution
- Represent scalar, set-returning, and correlated subqueries

**Checkpoint:** Required TPC-H subquery forms bind correctly.

### Week 31 — Scalar + Uncorrelated Subqueries

- Execute scalar subqueries and materialized uncorrelated subqueries
- Validate scalar cardinality at runtime

**Checkpoint:** Uncorrelated TPC-H subqueries execute correctly.

### Week 32 — Semi-Joins + Anti-Joins

- Add vectorized semi-join and anti-join operators
- Lower `IN`, `NOT IN`, `EXISTS`, and `NOT EXISTS` where applicable

**Checkpoint:** Set-membership subqueries avoid nested-loop execution.

### Week 33 — Correlated Subqueries

- Decorrelate the correlated patterns required by TPC-H
- Retain a correct fallback for unsupported patterns

**Checkpoint:** Required correlated TPC-H queries execute correctly.

### Week 34 — Derived Tables + Distinct Aggregates

- Bind and execute subqueries in `FROM`
- Add per-group state for `COUNT(DISTINCT ...)`

**Checkpoint:** Rewritten Q15 and distinct aggregates are supported.

### Week 35 — TPC-H Data + Harness

- Add the TPC-H schema, pipe-delimited loader, and scale-factor workflow
- Add parameterized queries, warmups, repetitions, and reference comparison

**Checkpoint:** TPC-H data generation and automated query runs are reproducible.

### Week 36 — Query Coverage + Correctness

- Port queries to the documented SwiftQL dialect
- Close query-specific parser, execution, and optimizer correctness gaps
- Document supported scale and memory limits

**Checkpoint:** Supported TPC-H queries match reference results within numeric tolerance.

### Week 37 — TPC-H Benchmarks + Final Documentation

- Measure per-query latency, throughput, optimizer impact, and estimate accuracy
- Publish coverage, limitations, plans, and benchmark plots

**Checkpoint:** TPC-H results are reproducible and the full project story is documented.

---

## 37-Week Plan

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
| 16 | Binder + planning correctness | Stable qualified column identities |
| 17 | Logical plan | Vectorized queries represented independently of execution |
| 18 | Vectorized physical planning | Shared builder replaces planning in `main.cc` |
| 19 | Statistics collection | Catalog statistics populated on load |
| 20 | Cardinality estimation | Estimates propagate through logical plans |
| 21 | Predicate optimization | Filters pushed down and evaluated incrementally |
| 22 | Cost model + join selection | Filtered build side and join algorithm costed |
| 23 | Phase 4 explain + benchmarks | Optimizer gains and estimate errors documented |
| 23.5 | SIMD small-build loop join (extension) | Optimizer picks hash vs SIMD-loop, crossover measured |
| 24 | General expressions | Arithmetic and aliased expressions execute; NULL modelled natively, expressions compiled per chunk, constants folded |
| 25 | Predicates + scalar functions | `BETWEEN`/`LIKE`/`IN`/`CASE`/`SUBSTRING`, ISO dates, folded intervals; dispatch checklist corrected to 17 sites |
| 26 | Multi-way join language + binding | Arbitrary explicit joins bind correctly; pushdown routes by relation slot ✅ |
| 27 | Multi-way join execution | General vectorized join trees execute |
| 28 | Join enumeration | DP join ordering with greedy fallback works |
| 29 | Outer join | Left outer hash join correct |
| 30 | Subquery parsing + binding | Nested scopes and subquery forms bind |
| 31 | Scalar + uncorrelated subqueries | Uncorrelated subqueries execute |
| 32 | Semi-joins + anti-joins | Set-membership subqueries optimized |
| 33 | Correlated subqueries | Required TPC-H patterns decorrelated |
| 34 | Derived tables + distinct aggregates | Q15 rewrite and `COUNT(DISTINCT)` supported |
| 35 | TPC-H data + harness | Reproducible data and query workflow |
| 36 | Query coverage + correctness | Supported queries match reference results |
| 37 | TPC-H benchmarks + documentation | Coverage and performance published |

---

## Benchmarks

*To be populated during Weeks 12, 15, 23, and 37.*

> **Note:** Phase benchmarks exclude data loading to isolate query execution. Phase 5 adds formal TPC-H correctness and performance measurements.

### Phase Comparison

| Query | Row + Volcano (ms) | Col + Volcano (ms) | Col + Vectorized (ms) |
|---|---|---|---|
| `SELECT AVG(speed) FROM laps` | — | — | — |
| `SELECT COUNT(*) FROM laps WHERE season = 2025` | — | — | — |
| `SELECT team, speed FROM laps WHERE speed > 300` | — | — | — |
| `SELECT team, COUNT(*) FROM laps GROUP BY team` | — | — | — |
| `SELECT l.team, AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team` | — | — | — |

### Optimizer Impact (Phase 4, Col + Vectorized mode)

Measured in Week 23 — 1M rows, avg of 5 runs, Release build. Full analysis including per-node estimation accuracy (q-error) in [docs/phase3-vs-phase4.md](docs/phase3-vs-phase4.md).

| Query | No Optimizer (ms) | With Optimizer (ms) |
|---|---|---|
| `SELECT AVG(speed) FROM laps WHERE season = 2025 AND speed > 300` | 3.4 | 3.4 |
| `SELECT laps.team, COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id GROUP BY laps.team` | 131.5 | 123.1 |
| `... JOIN ... WHERE laps.season = 2025 AND drivers.age > 30 GROUP BY laps.team` | 30.0 | 13.4 |

Query 1 is flat by design — zone-map pruning already dominates it. Query 2's modest gain is the Week 23.5 algorithm decision (SIMD loop join on the 20-row build side). The filtered join (row 3) is where pushdown + build-side + algorithm selection pay together: **2.23x**.

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

# Disable the vectorized optimizer
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
- Multi-way and multi-key joins parse, bind and produce a logical join tree (Week 26); executing them is Week 27, until which they refuse with `... not yet executable`
- No subqueries or correlated expressions
- `SUM`/`AVG` accumulate in `double`. SQLite's `SUM` over an INTEGER column returns an exact 64-bit INTEGER; SwiftQL returns a DOUBLE, so a sum beyond 2^53 loses precision where SQLite would not. Deliberate: one accumulator type keeps the aggregate nodes simple, and TPC-H SF1 sums stay far below that bound. Stated here because the Week 36 correctness report has to declare it alongside the INT-overflow decision above
- Commas inside string values not supported in CSV input
- No persistence beyond CSV files and catalog JSON
- Cost-based optimization applies only to columnar/vectorized execution
- Result cache invalidation not implemented — cache is cleared on process restart only

---

## Possible Extensions

If the project completes ahead of schedule, the following extensions are candidates:

- **Binary columnar file format** — serialize `ColumnarTable` to a simple binary format on first load, read from binary on subsequent runs; eliminates CSV parsing overhead on cold start, analogous to how Parquet works
- **Parallel scan + parallel aggregation** — partition table chunks across threads using a thread pool; per-thread aggregation maps merged at the end; expected 2–4× speedup on scan-heavy workloads on multi-core systems
- **Richer optimizer statistics** — histograms, multi-column correlation statistics, and adaptive reoptimization
- **Larger-than-memory execution** — spill-capable hash joins, aggregates, and sorts
- **TPC-DS** — extend the SQL dialect and benchmark harness beyond TPC-H

---

*Built as a learning project targeting internship roles at Snowflake and Databricks. Each phase is independently demonstrable with correctness tests and benchmarks.*
