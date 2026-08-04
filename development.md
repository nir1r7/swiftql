# SwiftQL — Development Guide

Practical reference for building, running, and testing the engine.

---

## Build

**First time:**
```bash
mkdir -p build && cd build
cmake ..
cmake --build . -j$(sysctl -n hw.logicalcpu)   # Mac
cmake --build . -j$(nproc)                       # Linux
```

**Rebuild after code changes** (from project root):
```bash
cmake --build build -j$(sysctl -n hw.logicalcpu)
```

Artifacts:
- `build/swiftql` — the query engine binary
- `build/tests/swiftql_tests` — the unit test binary

---

## Generate Test Data

```bash
python3 python_tools/generate_data.py
```

Creates `data/laps.csv` and `data/drivers.csv`. The default is 1,000 rows for `laps`. Pass `--rows N` for a larger dataset:

```bash
python3 python_tools/generate_data.py --rows 100000
```

---

## Unit Tests (C++)

```bash
cd build
./tests/swiftql_tests
```

> Must be run from inside `build/`. The tests resolve `"../catalog.json"` relative to their working directory.

Expected: **420 tests, 0 failures.**

---

## Running Queries

All commands are run from the **project root**.

### Basic query

```bash
./build/swiftql --catalog catalog.json --query "SELECT team, COUNT(*) FROM laps GROUP BY team"
```

### All CLI flags

| Flag | What it does |
|---|---|
| `--catalog <path>` | Path to `catalog.json` (required) |
| `--query "<sql>"` | SQL query to run; repeat the flag to run multiple queries in one process |
| `--explain` | Print the query plan tree for each query, without executing |
| `--explain-analyze` | Execute each query and print the plan tree annotated with timing and row counts |
| `--no-cache` | Skip the in-memory result cache |
| `--no-optimize` | Accepted flag, no effect in Phase 1 |
| `--storage row\|columnar` | Accepted flag, no effect in Phase 1 |
| `--execution volcano\|vectorized` | Accepted flag, no effect in Phase 1 |

### Examples

```bash
# Single query
./build/swiftql --catalog catalog.json \
  --query "SELECT team, COUNT(*) FROM laps GROUP BY team"

# Multiple queries in one process (enables the result cache to actually hit)
./build/swiftql --catalog catalog.json \
  --query "SELECT DISTINCT team FROM laps ORDER BY team" \
  --query "SELECT COUNT(*) FROM laps" \
  --query "SELECT DISTINCT team FROM laps ORDER BY team"
# ^ third query prints [cache hit] — same SQL as first, served from cache

# Show plan tree without running
./build/swiftql --catalog catalog.json --explain \
  --query "SELECT team, AVG(speed) FROM laps GROUP BY team HAVING AVG(speed) > 310"

# Run and show per-node timing
./build/swiftql --catalog catalog.json --explain-analyze \
  --query "SELECT team, AVG(speed) FROM laps GROUP BY team HAVING AVG(speed) > 310"

# Bypass cache to force re-execution every time
./build/swiftql --catalog catalog.json --no-cache \
  --query "SELECT DISTINCT team FROM laps ORDER BY team" \
  --query "SELECT DISTINCT team FROM laps ORDER BY team"
```

### Result cache

The result cache is keyed on the raw SQL string and lives for the lifetime of the process. It only becomes useful when multiple `--query` flags are passed — a repeated query in the same invocation is served from cache without re-executing:

```
-- SELECT DISTINCT team FROM laps ORDER BY team
team
-------------
AlphaTauri
...

-- SELECT DISTINCT team FROM laps ORDER BY team
[cache hit]
team
-------------
AlphaTauri
...
```

Cache is skipped for `--explain-analyze` (results aren't stored) and bypassed entirely with `--no-cache`.

### Understanding `--explain-analyze` output

```
Project [team, AVG(speed)]  rows_in=5  rows_out=5  time=8µs  (0.4%)
  Having [AVG(speed) > 310]  rows_in=8  rows_out=5  time=10µs  (0.5%)
    Aggregate [group_by=team, agg=AVG(speed)]  rows_in=1000  rows_out=8  time=1800µs  (85.7%)
      SeqScan [laps, 9 columns]  rows_out=1000  time=40µs  (1.9%)

Rows returned: 5

Parse:     28.4µs
Plan:      95.1µs
Execution: 2100µs
```

- `rows_in` — rows this node received from its child
- `rows_out` — rows this node passed up to its parent
- `time` — exclusive self-time (µs): time spent in this node's own code only; child operator time is excluded and counted in the child's `time` instead
- `(X.X%)` — this node's time as a percentage of total execution time; per-node percentages do not sum to 100% — the gap is measurement overhead from timing instrumentation (~10–25% typical)

Footer fields:
- `Rows returned` — number of rows in the final result set
- `Parse` — time to lex the SQL string and build the AST
- `Plan` — time to validate semantics and build the plan tree (no I/O)
- `Execution` — end-to-end volcano execution time; CSV load is excluded from all three timers (consistent with TPC-H benchmark methodology)

> **Note:** None of the engine timers include CSV loading — that happens between parse and plan, outside all clocks. See [Process Overhead](#process-overhead) below.

---

## Python Tools

All scripts are run from the **project root**.

### `run_queries.py` — batch runner

Runs a set of SQL queries against the SwiftQL binary and prints results.

```bash
# Run the default 22 test queries, print result tables
python3 python_tools/run_queries.py

# Show query plan trees instead of results
python3 python_tools/run_queries.py --explain

# Show per-node timing (explain-analyze mode)
python3 python_tools/run_queries.py --explain-analyze

# Load queries from your own .sql file (semicolon-separated)
python3 python_tools/run_queries.py --queries my_queries.sql

# Override binary or catalog path
python3 python_tools/run_queries.py --binary ./build/swiftql --catalog catalog.json
```

**Writing a custom `.sql` file:**
```sql
SELECT DISTINCT team FROM laps ORDER BY team;
SELECT COUNT(*) FROM laps WHERE season = 2025;
SELECT team, AVG(speed) FROM laps GROUP BY team
```

Each query is separated by `;`. The last query doesn't need one.

**Understanding the output:**

```
[1/22] SELECT team FROM laps LIMIT 5
------------------------------------------------------------------------
team
------------
Ferrari
McLaren
...
(13µs)
Process Overhead: 148635µs
```

- The timing printed by SwiftQL (`13µs`) is volcano execution time only.
- `Process Overhead` is the full wall time of the subprocess call — includes process startup, CSV loading, and catalog parsing. This number stays roughly constant regardless of query complexity.

### `compare_against_sqlite.py` — correctness harness

Runs the full correctness query suite against both SwiftQL and an in-memory SQLite database, then diffs the results.

```bash
python3 python_tools/compare_against_sqlite.py
```

Expected: **117 passed, 0 failed, 0 errors** (39 queries × 3 storage/execution modes).

Use this after any engine change to check for regressions.

---

## Process Overhead

When running queries via subprocess (as `run_queries.py` does), each invocation pays a fixed startup cost that has nothing to do with query execution:

1. Process fork + binary load (~10–50ms on macOS)
2. Catalog JSON parsing
3. CSV loading — `CSVLoader::load()` reads the full CSV into memory; this happens after parsing (so the engine knows which tables to load) but before planning, and is excluded from all engine timers

This is why `Process Overhead` is typically 100–200ms even for trivial queries, while `Execution` (the engine's own timer) is in the hundreds of microseconds.

The `Execution` line from `--explain-analyze` is the number to watch when optimizing the engine. Process overhead is a constant that won't move.

---

## Supported SQL

```sql
SELECT [DISTINCT] expr [AS alias], AGG(expr), ...
FROM table
[JOIN other_table ON table.col = other_table.col]   -- Phase 2
[WHERE expr [AND expr ...]]
[GROUP BY expr, ...]                                 -- expressions + aliases (Week 24)
[HAVING expr]
[ORDER BY expr [ASC|DESC], ...]                      -- expressions + aliases (Week 24)
[LIMIT N]
```

Aggregate functions: `COUNT(*)`, `COUNT(expr)`, `SUM(expr)`, `AVG(expr)`, `MIN(expr)`, `MAX(expr)` — arguments may be arbitrary numeric expressions, e.g. `SUM(speed * (1 - sector_1 / 100))`

Predicates: `=`, `!=`, `<`, `>`, `<=`, `>=`, `IS NULL`, `IS NOT NULL`, `AND`, `OR`

Arithmetic (Week 24): `+`, `-`, `*`, `/`, unary `-`, with SQL precedence (unary > `* /` > `+ -` > comparisons > `AND` > `OR`). SQLite semantics: `INT / INT` truncates; `x / 0` is `NULL` (on the vectorized path a NULL value degrades to a 0/`"NULL"` sentinel at materialization — `ColumnVector` has no null mask; the sentinel guard lives in both `vec_project_node.cc` and `vec_hash_aggregate_node.cc::fillChunk`, and Volcano remains the NULL-correct baseline). Select-list aliases (`AS`) are referenceable in `GROUP BY` and `ORDER BY`; in `GROUP BY`, input columns take precedence over aliases.
