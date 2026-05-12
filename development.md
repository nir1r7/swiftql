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

Expected: **66 tests, 0 failures.**

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
Project [team, AVG(speed)]  rows_in=5  rows_out=5  time=21µs
  Having [AVG(speed) > 310]  rows_in=8  rows_out=5  time=11µs
    Aggregate [group_by=team, agg=AVG(speed)]  rows_in=1000  rows_out=8  time=1914µs
      SeqScan [laps, 9 columns]  rows_out=1000  time=36µs
Total: 1948µs
```

- `rows_in` — rows this node received from its child
- `rows_out` — rows this node passed up to its parent
- `time` — wall time spent inside this node's own code (µs), not including child time
- `Total` — end-to-end volcano execution time (µs)

> **Note:** `Total` does not include CSV loading or catalog parsing — those happen before the volcano tree opens. See [Process Overhead](#process-overhead) below.

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

Runs the same 22 queries against both SwiftQL and an in-memory SQLite database, then diffs the results.

```bash
python3 python_tools/compare_against_sqlite.py
```

Expected: **22 passed, 0 failed, 0 errors.**

Use this after any engine change to check for regressions.

---

## Process Overhead

When running queries via subprocess (as `run_queries.py` does), each invocation pays a fixed startup cost that has nothing to do with query execution:

1. Process fork + binary load (~10–50ms on macOS)
2. Catalog JSON parsing
3. CSV loading — `CSVLoader::load()` reads the full CSV into memory before the volcano tree opens

This is why `Process Overhead` is typically 100–200ms even for trivial queries, while `Total` (the engine's own timer) is in the hundreds of microseconds.

The `Total` line from `--explain-analyze` is the number to watch when optimizing the engine. Process overhead is a constant that won't move.

---

## Supported SQL

```sql
SELECT [DISTINCT] col1, col2, AGG(col), ...
FROM table
[JOIN other_table ON table.col = other_table.col]   -- Phase 2
[WHERE expr [AND expr ...]]
[GROUP BY col1, col2, ...]
[HAVING expr]
[ORDER BY col1, col2, ...]
[LIMIT N]
```

Aggregate functions: `COUNT(*)`, `COUNT(col)`, `SUM(col)`, `AVG(col)`, `MIN(col)`, `MAX(col)`

Predicates: `=`, `!=`, `<`, `>`, `<=`, `>=`, `IS NULL`, `IS NOT NULL`, `AND`, `OR`
