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

Expected: **638 tests, 0 failures.**

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

Expected: **596 passed, 0 failed, 0 errors**: 124 queries × 4 modes (row/Volcano,
columnar/Volcano, columnar/vectorized, and columnar/vectorized with
`--no-optimize`), plus 12 rejections × the same 4 modes, plus the multi-way
capability split — 13 multi-way queries × the 2 vectorized modes, diffed against
SQLite, and the same 13 asserted to be refused × the 2 Volcano modes.

The multi-way block is 6 Week 27 execution shapes plus 7 Week 28 join-ordering
ones. Running the ordering queries in the `--no-optimize` vectorized mode as well
is the point, not duplication: that mode keeps the **written** join order, so the
pair is what makes this file able to catch a reordering that changes an answer.

> A query that runs in only some modes has to be listed separately, not dropped
> from the harness and not run everywhere. Multi-way joins are the first such
> case: run them in all four and two modes report a refusal as a failure; run
> them in none and the checkpoint has no oracle at all.

Queries containing `ORDER BY` are compared **in emitted order**; the rest are
sorted first, since SQL does not specify row order without `ORDER BY`. Sorting
both sides unconditionally is what made a broken sort comparator invisible to this
harness for an entire phase.

Use this after any engine change to check for regressions.

> **Known blind spot.** Rows are normalized through a dict keyed by column
> *name*, so duplicate names in a merged join schema (invariant 3 — two
> `driver_id`, two `team`) collapse on both sides identically. A column-identity
> or column-order regression in a `SELECT *` multi-way join is therefore
> invisible to this file. Covered on the C++ side by
> `JoinEnumeration.ReorderedPlansReturnTheWrittenOrdersRows`, which diffs raw
> chunk values. Read the silence as absence of coverage, not as coverage.

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
[[LEFT [OUTER]] JOIN other ON key = key [AND ...]]* -- Phase 2; multi-key +
                                                     -- residual ON conjuncts:
                                                     -- Week 27 (all modes).
                                                     -- 3+ relations: vectorized
                                                     -- only (Week 27).
                                                     -- LEFT OUTER: Week 29
                                                     -- (all modes)
[WHERE expr [AND expr ...]]
[GROUP BY expr, ...]                                 -- expressions + aliases (Week 24)
[HAVING expr]
[ORDER BY expr [ASC|DESC], ...]                      -- expressions + aliases (Week 24)
[LIMIT N]
```

Aggregate functions: `COUNT(*)`, `COUNT(expr)`, `SUM(expr)`, `AVG(expr)`, `MIN(expr)`, `MAX(expr)` — arguments may be arbitrary numeric expressions, e.g. `SUM(speed * (1 - sector_1 / 100))`. `MIN`/`MAX` also accept `STRING` arguments and return a `STRING`

Predicates: `=`, `!=`, `<`, `>`, `<=`, `>=`, `IS NULL`, `IS NOT NULL`, `AND`, `OR`,
and (Week 25) `[NOT] BETWEEN`, `[NOT] LIKE`, `[NOT] IN (constants)`

> **Week 27 join scope.** An `ON` clause decomposes into equi-join **keys** —
> a cross-relation equality or an `AND`-chain of them — plus **residuals**: every
> other conjunct (`a.x < b.x`, `d.age > 30`, `a.x = a.y`, a Week 25 node),
> executed as a post-join filter. That is semantics-preserving because the join
> is inner, and the residuals are folded into the `WHERE` conjunction so
> predicate pushdown routes them by relation slot like any other conjunct — a
> single-relation one lands on its own scan. Two things are still errors: an `ON`
> clause yielding **no key** (`ON a.x < b.x` alone, or an `OR`, which is one
> indivisible conjunct) — that is a cross product, and there is no operator for
> it — and a forward reference to a relation joined later.
>
> **Three or more relations execute on `--execution vectorized` only.**
> `Planner::plan` builds exactly one join, so row and columnar Volcano refuse
> with `multi-way joins are not supported on the Volcano path; use --execution
> vectorized`. It is the correctness baseline, not the feature-complete path, and
> deleting the guard would make its single `HashJoinNode` silently drop a
> relation. Multi-key and residual-`ON` joins execute on **every** path.

> **Week 29 outer joins.** `LEFT [OUTER] JOIN` preserves every row of the left
> input: one whose key matches nothing is emitted once, with NULL across the
> joined relation's columns. Three things change relative to an inner join, and
> all three are semantic rather than cosmetic:
>
> - **`ON` and `WHERE` stop being interchangeable.** `R ⋈_(p∧q) S ≡ σ_q(R ⋈_p S)`
>   is an inner-join identity. For an outer join, a row failing `q` in the `ON`
>   is *not a match* and is null-extended, while the same `q` in the `WHERE`
>   deletes it. So an outer join's residual conjuncts stay on the join
>   (`LogicalJoin::on_residual`) instead of joining the `WHERE` conjunction — it
>   is TPC-H Q13's `o_comment not like '%special%requests%'`.
> - **Predicate pushdown declines the null-supplying side.** `σ_p(R ⟕ S)` is not
>   `σ_p(R) ⟕ σ_p(S)`: filtering `S` first makes left rows lose matches, and the
>   join then null-extends exactly the rows the `WHERE` existed to remove. Such a
>   conjunct stays above the join, where three-valued logic drops the
>   null-extended rows. The preserved side still pushes.
> - **Join enumeration declines the whole tree.** `R ⟕ S ≠ S ⟕ R` and
>   associativity fails, so no reordering is legal without conflict/eligibility
>   sets. `--explain` prints no `order=` line for such a tree, because there was
>   no decision.
>
> The build side is forced rather than costed: the preserved side must be the
> probe input (`build=<table> ... (outer: the preserved side must probe)`), and
> the SIMD loop join — an inner equi-join with no unmatched path — is never
> selected. `RIGHT`/`FULL` are out of scope; no TPC-H query in the documented
> dialect needs them.

Week 25 also adds `CASE WHEN ... THEN ... [ELSE ...] END`, `SUBSTRING`, ISO date
literals and constant-folded interval arithmetic — see [Week 25 dialect notes](#week-25-dialect-notes).

Not supported, each rejected with a clear error rather than a wrong answer:
column ordinals (`ORDER BY 1`, `GROUP BY 2` — use an alias), unary `+`,
scientific-notation floats (`1e5`), scalar functions other than `SUBSTRING`
(`ABS(x)`), general `NOT` (only the four `IS NOT NULL` / `NOT BETWEEN` /
`NOT LIKE` / `NOT IN` forms), nested aggregates (`SUM(AVG(x))`), and INT
arithmetic that overflows 64 bits (SQLite promotes to REAL; SwiftQL cannot,
because the INT/INT result type is fixed at plan time). Full list with rationale
in readme.md.

### Week 25 dialect notes

| Decision | Why |
|---|---|
| `BETWEEN` is **desugared in the parser** into `a >= x AND a <= y` (`NOT BETWEEN` into `a < x OR a > y`, valid De Morgan in Kleene 3VL) | The desugared shape is what `splitConjuncts`, `ChunkPruner`, `scanColumn` and `selectivity()` pattern-match on, so both bounds get pushdown, zone-map pruning, the tight typed loop and range estimation. A `BetweenExpr` node would forfeit all four and cost 17 dispatch sites. Visible consequence: `--explain` prints the two comparisons, not `BETWEEN` |
| `BETWEEN` bounds parse at the **additive** level | `BETWEEN` binds tighter than `AND`. Parsing them with `parseExpr()` makes `a BETWEEN 1 AND 2 AND b > 3` swallow the second `AND` into the range |
| `BETWEEN`'s left operand is **cloned**, so a computed one is evaluated twice | The price of desugaring: `SUBSTRING(team,1,3) BETWEEN 'A' AND 'M'` plans as two independent SUBSTRING kernels. Bounded in practice — every TPC-H `BETWEEN` has a bare column on the left, and the AND cascade runs the second copy over survivors only. `HAVING SUM(x) BETWEEN ...` is unaffected: the cloned aggregate dedupes to one spec by `output_name` |
| `SUBSTRING` rejects SQLite's out-of-domain positions | SQLite defines `substr(x,0,3)`, negative starts and negative lengths; SwiftQL raises instead. Unlike column ordinals or unary `+` this is not purely a parse-time rejection, so `inferExprType` decides it at plan time whenever the arguments are constant — after folding, every realistic query. Only a *computed* position can still abort mid-execution |
| `LIKE` is **ASCII case-insensitive** and takes a **constant pattern** | Case-insensitivity matches SQLite's default, which keeps `compare_against_sqlite.py` a valid oracle; TPC-H patterns are case-uniform so results are unaffected either way. The fold is spelled out as `A-Z` rather than `std::tolower`, which is locale-dependent (under ISO-8859-1 it maps 0xC9 to 0xE9) and would make LIKE results depend on the process locale. A constant pattern lets the executor analyse it once per query instead of per row. **No `ESCAPE` clause**, so a literal `%` or `_` cannot be matched — listed in readme.md's divergence table |
| `IN` takes a **literal list only** | Makes the set hashable once at compile time, and — since the grammar has no NULL literal — makes NULL-in-list unreachable, collapsing SQL's three-valued `IN` rule to "operand NULL → NULL". `IN (subquery)` is Week 32 and lowers to a semi-join, a different production |
| `CASE` has **no vectorized kernel** | `evaluate()` short-circuits; a chunk kernel cannot. `CASE WHEN i < 100 THEN i * 1000000000000 ELSE 0 END` would raise a `checkedMul` overflow for rows whose branch is discarded, and the differential tests would be right to fail. `compileNode` declines, so the fallback is correct-but-slow — see the measured cost below. A masked kernel (evaluate each branch only over its own selection) is the fix when Q8/Q12/Q14 profiling justifies it |
| Dates are **STRING**, not a new `TypeId` | ISO-8601 sorts lexicographically, and zone maps are built for every column including STRING — so dates get chunk pruning and `scanColumn<std::string>` for free. `DATE 'x'` is validated against a real calendar at parse time |
| Intervals fold at **plan time**, never at runtime | `IntervalLiteral` exists only for `foldNode` to consume: `date '1998-12-01' - interval '90' day` becomes `Literal("1998-09-02")`, restoring the `ColumnRef op Literal` shape. An interval that survives folding throws from `inferExprType` and `evaluate` — the query was not constant date arithmetic. Every TPC-H interval expression is constant |
| Dates are bounded to **0000-01-01 .. 9999-12-31**, and interval counts are range-checked *before* any arithmetic | `formatIsoDate` writes exactly four year digits, so an out-of-range year wrapped modulo 10000 instead of failing: `+ interval '100000' year` returned the **input date unchanged**, and a negative year emitted `'0' + (-6)` = `'*'`, producing the non-date string `000*-01-01` that then flowed into comparisons and zone maps as an ordinary STRING. Bounding the count first also removes three signed-overflow UB sites (`addDays`, `addMonths`, and the `-count` in `foldDateInterval`, which now uses `checkedNegate`) — date folding had been bypassing `checked_arith.h` entirely |
| Reserved words added | `BETWEEN LIKE IN CASE WHEN THEN ELSE END SUBSTRING FOR DATE INTERVAL`. Interval units (`day`/`month`/`year`) are **not** reserved — they are matched as identifier text, so `year` stays usable as a column name (TPC-H aliases `o_year` / `l_year`) |

**Measured** (1M rows, Release, per-node self-time from `--explain-analyze`).
`LIKE` and `IN` compile to chunk kernels; `CASE` does not, and that is the whole
cost of the decision above:

| Node | Expression | Self-time |
|---|---|---|
| `VecHashAggregate` | `SUM(speed)` — plain column | 62 ms |
| `VecHashAggregate` | `SUM(speed * (1 - sector_1))` — compiled | 63 ms |
| `VecHashAggregate` | `SUM(CASE WHEN season = 2024 THEN speed ELSE 0 END)` — **uncompiled fallback** | **406 ms** |
| `VecFilter` | `speed > 300` — `scanColumn` fast path | 2.4 ms |
| `VecFilter` | `season IN (2023, 2024)` — compiled `IN_SET` | 2.6 ms |
| `VecFilter` | `team LIKE '%a%'` — compiled `LIKE` | 32 ms |

`IN` costs the same as a native comparison: the list is hashed once at compile
time, so the row loop is one probe. `LIKE` is more expensive because wildcard
matching genuinely is — but it is a chunk loop, not a per-row `evaluate()`. The
`CASE` fallback is 6.4x the compiled path, because it sets `needs_row` in
`VecHashAggregateNode::consumeAll` and rebuilds a full `Row` per row.

**A selectivity rule for `LIKE` was written, measured, and removed.** Postgres'
`DEFAULT_MATCH_SEL` of 0.005 looked like an easy win. It made
`WHERE team LIKE 'Fer%' AND lap_id BETWEEN 900000 AND 900100` **1.5–1.7x slower**
(min of 9, interleaved, 1M rows, Release):

| Query | `LIKE` = 0.005 | no `LIKE` rule (0.5) |
|---|---|---|
| `team LIKE 'Fer%' AND lap_id BETWEEN 900000 AND 900100` | 500 µs | **325 µs** |
| `team LIKE '%a%' AND lap_id BETWEEN 900000 AND 900100` | 1821 µs | **1085 µs** |

Two things went wrong at once, and both are worth remembering. The estimate was
*wrong* — that pattern keeps 25% of the table, not 0.5% — and `orderByWork` ranks
conjuncts on **selectivity alone**, so being wrong in the low direction promoted
the most expensive predicate to the front of the cascade, where it ran on every
surviving row instead of on the ~100 the ranges would have left. An accurate
`LIKE` estimate is only safe once ordering is cost-aware, which
`predicate_pushdown.cc` already defers to Week 28; land the pair together there.
`IN` is not exposed to this: its `k/ndv` comes from real statistics, and its
kernel costs what a comparison costs.

> **Reproducing these numbers.** They need a 1M-row dataset and an optimized
> build; the checked-in `data/laps.csv` is 10k rows and the default `cmake ..`
> configure sets no `CMAKE_BUILD_TYPE`, so `build/` is unoptimized and roughly
> 20x slower. `data/*.csv` are git-tracked, so restore them afterwards:
>
> ```bash
> cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j
> python3 python_tools/generate_data.py --rows 1000000
> ./build-rel/swiftql --catalog catalog.json --storage columnar --execution vectorized \
>   --explain-analyze --no-cache --query "<one of the queries above>"
> git checkout -- data/          # the CSVs are tracked; put them back
> ```

Arithmetic (Week 24): `+`, `-`, `*`, `/`, unary `-`, with SQL precedence (unary > `* /` > `+ -` > comparisons > `AND` > `OR`). SQLite semantics: `INT / INT` truncates; `x / 0` is `NULL`. Select-list aliases (`AS`) are referenceable in `GROUP BY` and `ORDER BY`; in `GROUP BY`, input columns take precedence over aliases.

### Key serialization

Six operators build a string key from a row's key columns and compare the
strings: both hash joins, both hash aggregates, both distinct operators. They all
depend on the same two properties, and every one that restated the rules locally
got at least one of them wrong — so the rules live in
`src/execution/key_encoding.h` and nothing else may encode a key.

| Property | What breaks without it |
|---|---|
| **Injective framing** — `<len>:<bytes>` per field, `'\x01'` terminated | A bare sentinel is decodable only if no field contains it, and a STRING cell reaches the key verbatim from the CSV. `("A\x01B","C")` and `("A","B\x01C")` encoded alike, so two rows differing in **both** keys joined |
| **Identifying text, not display text** — exact DOUBLE rendering, never `Value::toString()`'s `%.15g` | 3245 distinct sector sums in `data/laps.csv` share only 2526 `%.15g` texts, so `DISTINCT` and `GROUP BY` merged 706 pairs of different numbers and miscounted every merged group |

NULL is the one place the three uses legitimately differ, so it is the caller's
choice: a join drops the row (NULL equals nothing, so it can never match), while
`GROUP BY` and `DISTINCT` keep it as a group of its own under the `'N'` marker —
which no value's text can imitate, since those start with a length digit. Volcano
`DistinctNode` was missing that marker, so a NULL and the string `'NULL'` deduped
together while the vectorized path kept them apart.

A NaN key is dropped by a join for the same reason a NULL is: two NaNs serialize
identically, but `Value::operator==` calls them unequal, so matching them would
make a join accept a pair its own `WHERE` predicate rejects. Grouping keeps NaN
instead, as one group covering both signs — `%.17g` renders a sign-bit-set NaN
as `-nan`, so the encoder drops the sign rather than splitting the group. SQLite
has neither case, since it stores NaN as NULL; see readme.md's Limitations.

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

**Where NULLs come from.** Until Week 29 a NULL could only be *computed*
(`x / 0`, a `CASE` with no matching branch), because `ColumnarTable` cannot
express one and a CSV cell cannot either. A `LEFT JOIN` is the first construct
that manufactures NULLs from ordinary catalog data, which is also what makes
`compare_against_sqlite.py` a real NULL oracle: before it, NULL semantics were
only reachable from in-memory operator tests. `VecHashJoinNode` needed no
materialization change for it — `fillOutChunk` already writes through
`appendColumnValue`, which back-fills the validity prefix on the first NULL.

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

Eighteen functions dispatch on `Expr` subtype. **Ten fail silently** when a new
one is missed — no error, no crash, a wrong answer or a lost optimization
somewhere far away. Adding a node type means visiting all of them.

Ordered by how hard the failure is to find:

| # | Site | On an unhandled subtype | What breaks |
|---|---|---|---|
| 1 | `exprKey` — `expr_utils.h` | returns `"?"` | **Silent.** Two different expressions get the same identity, so unrelated GROUP BY keys collide |
| 2 | `collectCols` — `logical_plan.cc` | returns | **Silent.** The column is narrowed out of the scan schema, then execution fails with "column not found" far from the cause |
| 3 | `Binder::bindExpr` — `binder.cc` | returns | **Silent.** Child `ColumnRef`s keep `relation_slot = -1`, so join-side resolution silently falls back to bare-name |
| 4 | `Validator::validateExpr` — `validator.cc` | falls through | **Silent.** No column-existence or aggregate-placement checks inside the new node |
| 5 | `checkGroupedRefs` — `validator.cc` | falls through | **Silent.** A separate function from #4. An ungrouped column inside the new node passes validation, then fails at plan time with `column not found` from `inferExprType` against the post-aggregate schema — the classic far-from-the-cause error |
| 6 | `substituteInto` — `logical_plan.cc` | returns | **Silent.** A group-key reference inside the new node is not rewritten post-aggregate |
| 7 | `collectAggregates` — `expr_utils.h` | returns | **Silent.** An aggregate nested inside the new node is never collected as a spec |
| 8 | `collectSlots` — `predicate_pushdown.cc` | empty slot set | **Silent, performance — and, since Week 27, silent correctness.** `soleSlot()` sees no single relation and returns `-1`, so the conjunct is evaluated above the join as a residual instead of on its own scan: right answers, lost pushdown. The second caller is `classifyJoinCondition`, which uses it to reject a *forward reference* inside a residual `ON` conjunct of any shape — a missed subtype makes that reference invisible, and the conjunct then resolves against whatever column of that name the left tree happens to have. Declared in `predicate_pushdown.h` for that reason: one walker, two callers, never a private copy. It covers `AggregateExpr` too, which pushdown alone never needs (aggregates are forbidden in `WHERE`) but an `ON` conjunct can contain — `validateJoinCondition` refuses those one line later, so the full pipeline cannot tell a blind walker from a seeing one |
| 9 | `restampSlots` — `predicate_pushdown.cc` | returns | **Silent, performance.** Must stay in lockstep with #8: a pushed conjunct keeps its own relation's slot (any `k >= 1`) below the join, where `ChunkPruner` ignores it and the zone-map hint is lost |
| 10 | `exprToString` — `expr_utils.h` | returns `"?"` | Visible: output column literally named `?` |
| 11 | `cloneExpr` — `expr_utils.h` | **throws** | Loud. Marked `DISPATCH SITE` for this reason |
| 12 | `inferExprType` — `logical_plan.cc` | **throws** | Loud, at plan time |
| 13 | `evaluate` — `evaluator.cc` | **throws** | Loud, at execution |
| 14 | `foldNode` — `constant_folding.cc` | returns false | Safe: no folding |
| 15 | `ExpressionExecutor::compileNode` | returns `nullptr` | Safe: caller falls back to `evaluate()` — slow, never wrong |
| 16 | `evalPredicate` — `columnar_eval.cc` | `evalFallback` | Safe. Needs no change for a new node: the fallback routes through `PredicateExecutorCache`, so adding a kernel at #15 is enough to make it fast |
| 17 | `CardinalityEstimator::selectivity` | `FALLBACK_SELECTIVITY` | Safe: a flat 0.5 guess. Add a real rule only when you can also afford to be *right* — `orderByWork` ranks conjuncts on selectivity alone, so an estimate that is low and wrong promotes an expensive predicate ahead of cheap ones. `IN` gets `k/ndv`; `LIKE` deliberately does not (see below) |
| 18 | `Validator::validateJoinCondition` — `validator.cc` | falls through | **Extended in Week 26**, in the same commit that relaxed `classifyJoinCondition` to accept multi-key equi-joins. It now dispatches every `Expr` subtype and its relation list is keyed by *ref name* (alias when present), without which every aliased qualifier fell through the unknown-qualifier escape and was checked by nothing. For a bound statement it re-checks by *relation slot*, never by `table_name` — the Binder rewrites an unqualified ref's `table_name` to its relation's table name, so matching on it lands on whichever relation is aliased to that name and rejects a legal query. Name matching survives only for validator-only callers that skip the Binder. **Live since Week 27**: `classifyJoinCondition` now hands every non-key conjunct on as a residual instead of refusing it on shape, so the Week 25 branches and the `AggregateExpr` branch are reached for real. This is the ONLY column-existence check those conjuncts get — `Validator::validate` runs before the residual is folded into the `WHERE` conjunction, so `validateExpr` never sees it. A gap here surfaces as a far-from-the-cause `column not found` from `inferExprType` |

`ChunkPruner::shouldSkip` is not on the list: `collectSimplePredicates` returns
immediately on anything that is not a `BinaryExpr`, so a new node contributes no
pruning hint while its sibling conjuncts still prune. Correct, just slower.

Sites 14–17 are safe **by design** — they degrade to a correct slower path. That
is the pattern to copy for anything new: prefer "decline and fall back" over
"assume and guess."

Recommended order for a new node type:

1. Lexer tokens, then the parser rule at the right precedence level (see the
   grammar in readme.md — decide the slot before writing code).
2. Sites 11–13 first (`cloneExpr`, `inferExprType`, `evaluate`). They throw, so
   they turn every later mistake into a loud failure instead of a quiet one.
3. Sites 1–10. Write a test per site; the silent ones cannot be caught by eye,
   and a test that only asserts "something threw" does not prove the right site
   caught it — assert on the message (see
   `Validation.UngroupedColumnInsideWeek25NodesIsRejected`, which passes for the
   wrong reason without that check).
4. Sites 14–17 last — these are optimizations. Correctness is already complete
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
