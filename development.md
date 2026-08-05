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

Expected: **476 tests, 0 failures.**

`ctest` works too and runs the same binary with the right working directory:

```bash
cd build && ctest --output-on-failure
```

> The suite is registered via `enable_testing()` + `add_test()`. Before that,
> `ctest` printed "No tests were found!!!" and exited 0 — a false green for any
> ctest-based gate.

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

Expected: **324 passed, 0 failed, 0 errors** (81 queries × 4 modes: row/Volcano, columnar/Volcano, columnar/vectorized, and columnar/vectorized with `--no-optimize`).

Queries containing `ORDER BY` are compared **in emitted order**; the rest are
sorted first, since SQL does not specify row order without `ORDER BY`. Sorting
both sides unconditionally is what made a broken sort comparator invisible to this
harness for an entire phase.

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

Aggregate functions: `COUNT(*)`, `COUNT(expr)`, `SUM(expr)`, `AVG(expr)`, `MIN(expr)`, `MAX(expr)` — arguments may be arbitrary numeric expressions, e.g. `SUM(speed * (1 - sector_1 / 100))`. `MIN`/`MAX` also accept `STRING` arguments and return a `STRING`

Predicates: `=`, `!=`, `<`, `>`, `<=`, `>=`, `IS NULL`, `IS NOT NULL`, `AND`, `OR`

Not supported, each rejected with a clear error rather than a wrong answer:
column ordinals (`ORDER BY 1`, `GROUP BY 2` — use an alias), unary `+`,
scientific-notation floats (`1e5`), scalar functions (`ABS(x)` — Week 25),
nested aggregates (`SUM(AVG(x))`), and INT arithmetic that overflows 64 bits
(SQLite promotes to REAL; SwiftQL cannot, because the INT/INT result type is
fixed at plan time). Full list with rationale in readme.md.

Arithmetic (Week 24): `+`, `-`, `*`, `/`, unary `-`, with SQL precedence (unary > `* /` > `+ -` > comparisons > `AND` > `OR`). SQLite semantics: `INT / INT` truncates; `x / 0` is `NULL`. Select-list aliases (`AS`) are referenceable in `GROUP BY` and `ORDER BY`; in `GROUP BY`, input columns take precedence over aliases.

### NULL on the vectorized path

`ColumnVector` carries a validity mask (`all_valid` + `validity`), so SQL NULL is
represented natively on the vectorized path — all three modes agree with SQLite.
The mask is empty whenever `all_valid` is true, which is the common case
(`ColumnarTable` cannot express NULL, so scan output is always all-valid) and the
reason operators that never manufacture a NULL need no validity code.

Every operator reads and writes chunk cells through three helpers in
`src/execution/vec_types.h` — `valueAt` / `readColumnValue` to read, and
`appendColumnValue` to write. Reading a typed vector directly bypasses the mask
and silently turns a NULL into the placeholder value stored underneath it.

**Comparing NULL.** `Value`'s comparison operators are SQL three-valued: every one
returns false when either side is NULL, and callers are expected to test
`isNull()` first (as `evaluate()` does). They therefore do **not** define an
ordering over NULL — `!(a<b) && !(b<a)` makes NULL equivalent to every value, and
equivalence stops being transitive. Sorting needs `compareForSort` (`value.h`)
instead, which is a real total order with NULL as the minimum: NULLs first
ascending, last descending, matching SQLite. Handing the SQL operators to
`std::stable_sort` is undefined behaviour, not just odd NULL placement — it
reordered the non-NULL keys and dropped rows under `LIMIT`.

**Boolean connectives.** `AND`/`OR` are three-valued in both evaluators:
`false AND NULL` is false, `true OR NULL` is true, and only a genuinely
undetermined combination yields NULL. Propagating NULL unconditionally instead
made the two execution paths disagree on the same query.

### Expression evaluation

Two implementations, kept in agreement by differential tests:

| | Where | Cost model |
|---|---|---|
| `evaluate()` (`evaluator.cc`) | Volcano, and the vectorized fallback | tree walk with `dynamic_cast` dispatch, per row |
| `ExpressionExecutor` (`expression_executor.cc`) | every vectorized expression position | dispatch resolved once at compile time, one typed loop per node per chunk |

All three vectorized expression positions compile:

| Position | Owner of the compiled form |
|---|---|
| aggregate arguments, expression group keys | `VecHashAggregateNode::consumeAll` |
| `WHERE` / `HAVING` fallback subtrees | `PredicateExecutorCache` on `VecFilterNode` |
| projection expressions | `VecProjectNode::prepare` |

`evalPredicate` keeps its own fast paths — the AND cascade, the OR union, and the
tight `col op literal` comparison loops — because those beat a compiled
expression: they touch one column and allocate nothing. The executor serves the
fallback that used to rebuild a `Row` per row.

`evaluate()` remains the semantic reference. `ExpressionExecutor::compile()`
returns `nullptr` for any shape it has no kernel for, and the caller keeps the
per-row `evaluate()` path — so an uncovered expression is slow, never wrong.
`compile()` also asserts its own result type equals `inferExprType`, which is
what lets callers pre-allocate output columns before the row loop.

Measured (1M rows, Release, per-node self-time from `--explain-analyze`):

| Node | plain column | with an expression | before compiling |
|---|---|---|---|
| `VecHashAggregate` — `SUM(speed)` vs `SUM(speed*(1-sector_1))` | 16 ms | 17 ms | 290 ms |
| `VecFilter` — `speed > 300` vs `speed*2 > 600` | 0.64 ms | 1.7 ms | 222 ms |
| `VecProject` — `speed` vs `speed*2` | 8.7 ms | 1.0 ms | 162 ms |

### Constant folding

`foldConstants` (`constant_folding.cc`) runs as the last step of `Binder::bind`,
rewriting constant arithmetic subtrees to literals before validation. It is
unconditional — folding cannot change results, so it is canonicalization rather
than a cost-based decision, and both execution paths and `--no-optimize` get it.

It matters far beyond saving a multiply: three separate fast paths pattern-match
on the literal shape `ColumnRef op Literal`, and a constant subexpression defeats
all of them at once — zone-map chunk pruning (`chunk_pruner.h`), the tight typed
comparison loop (`columnar_eval.cc`), and equality/range selectivity
(`cardinality_estimator.cc`). `WHERE season = 2020 + 4` went from 203 ms with no
chunks skipped to 0.35 ms with pruning active.

Scope: arithmetic (`+ - * /`) and unary minus. Comparisons and `AND`/`OR` are
left alone — folding them buys nothing and would change the predicate shapes
pushdown and join classification inspect. A fold that evaluates to NULL (`1 / 0`)
or overflows is skipped, so the existing error still surfaces from its usual place.

### Aggregate result types

`aggregateResultType(function, arg_type)` in `logical_plan.h` is the single
source of truth, used by `inferExprType`, `buildProjectSchema`, and
`buildAggregateSchema`:

| Function | Result type |
|---|---|
| `COUNT` | `INT` |
| `SUM`, `AVG` | `DOUBLE` |
| `MIN`, `MAX` | same as the argument, including `STRING` |

`MIN`/`MAX` are order statistics — they return an element of the input domain,
so `MIN(team)` is a `STRING`. Typing them `DOUBLE` made that query throw
`bad_variant_access` at the materialization point.

---

## Extending the expression language

Fourteen functions dispatch on `Expr` subtype. **Six fail silently** when a new
one is missed — no error, no crash, a wrong answer somewhere far away. Adding a
node type (Week 25 adds five: `BETWEEN`, `LIKE`, `IN`, `CASE`, `SUBSTRING`) means
visiting all of them.

Ordered by how hard the failure is to find:

| # | Site | On an unhandled subtype | What breaks |
|---|---|---|---|
| 1 | `exprKey` — `expr_utils.h` | returns `"?"` | **Silent.** Two different expressions get the same identity, so unrelated GROUP BY keys collide |
| 2 | `collectCols` — `logical_plan.cc` | returns | **Silent.** The column is narrowed out of the scan schema, then execution fails with "column not found" far from the cause |
| 3 | `Binder::bindExpr` — `binder.cc` | returns | **Silent.** Child `ColumnRef`s keep `relation_slot = -1`, so join-side resolution silently falls back to bare-name |
| 4 | `Validator::validateExpr` — `validator.cc` | falls through | **Silent.** No column-existence or aggregate-placement checks inside the new node |
| 5 | `substituteInto` — `logical_plan.cc` | returns | **Silent.** A group-key reference inside the new node is not rewritten post-aggregate |
| 6 | `collectAggregates` — `expr_utils.h` | returns | **Silent.** An aggregate nested inside the new node is never collected as a spec |
| 7 | `exprToString` — `expr_utils.h` | returns `"?"` | Visible: output column literally named `?` |
| 8 | `cloneExpr` — `expr_utils.h` | **throws** | Loud. Marked `DISPATCH SITE` for this reason |
| 9 | `inferExprType` — `logical_plan.cc` | **throws** | Loud, at plan time |
| 10 | `evaluate` — `evaluator.cc` | **throws** | Loud, at execution |
| 11 | `foldNode` — `constant_folding.cc` | returns false | Safe: no folding |
| 12 | `ExpressionExecutor::compileNode` | returns `nullptr` | Safe: caller falls back to `evaluate()` — slow, never wrong |
| 13 | `evalPredicate` — `columnar_eval.cc` | `evalFallback` | Safe |
| 14 | `ChunkPruner::shouldSkip` | no pruning | Safe: correct, just slower |

Sites 11–14 are safe **by design** — they degrade to a correct slower path. That
is the pattern to copy for anything new: prefer "decline and fall back" over
"assume and guess."

Recommended order for a new node type:

1. Lexer tokens, then the parser rule at the right precedence level (see the
   grammar in readme.md — decide the slot before writing code).
2. Sites 8–10 first (`cloneExpr`, `inferExprType`, `evaluate`). They throw, so they
   turn every later mistake into a loud failure instead of a quiet one.
3. Sites 1–7. Write a test per site; the silent ones cannot be caught by eye.
4. Sites 11–14 last — these are optimizations. Correctness is already complete
   without them, so land the feature, then add kernels.
5. Add queries to `compare_against_sqlite.py`. It runs 4 modes and compares
   `ORDER BY` results in emitted order, so it catches cross-mode divergence and
   ordering defects that unit tests will not.

Two engine-wide invariants a new node must not break:

- **`inferExprType` is the contract.** The vectorized path pre-allocates output
  columns from it before the row loop, and `ExpressionExecutor::compile` asserts
  its own result type matches. A node whose runtime type can differ from its
  inferred type will produce `bad_variant_access`, not a SQL error.
- **`evaluate()` is the semantic reference.** Any vectorized kernel is a second
  implementation of it and must agree row-for-row, NULL-for-NULL. The differential
  tests in `test_vectorized.cc` (`ExpressionExecutor.MatchesEvaluate*`) are what
  hold the two together — extend that corpus, don't write kernel-only tests.
