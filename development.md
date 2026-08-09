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

Expected: **747 tests, 0 failures.**

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
| `--no-optimize` | Skip **every** optimizer pass — pushdown, join enumeration, cost-based join selection. The differential oracle's fourth mode: optimized and `--no-optimize` must agree, row for row, on every supported query. Note `foldConstants` is **not** an optimizer pass and is *not* gated by this — see *Constant folding* |
| `--storage row\|columnar` | Which storage layout the loader builds (Phase 2 on) |
| `--execution volcano\|vectorized` | Which executor runs the plan (Phase 3 on). `vectorized` requires `--storage columnar`, and is the only path with multi-way joins, `IN`/`EXISTS` lowering, correlated subqueries and derived tables — the Volcano path refuses each by name |
| `--storage-stats` | Print each columnar table's total size in MB **and exit without running the query**. Requires `--storage columnar`; it is a no-op under `--storage row`. (The per-column raw→encoded lines and the `[columnar]` ratio are printed by the loader on every columnar run, not by this flag) |
| `--format aligned\|tsv` | Output rendering. `tsv` is what the Python harnesses parse; `aligned` is the default and pads columns |

> ⚠️ **CORRECTED (Week 37 doc sweep).** The last three rows of this table read
> *"Accepted flag, no effect in Phase 1"* — true when written, at Week 7, and
> carried unchanged through four phases in which each of them became the axis the
> engine is tested along. `--storage` and `--execution` are the mode matrix the
> regression harness sweeps; `--no-optimize` is one leg of the correctness
> oracle. Two flags the CLI has always parsed were also missing entirely.

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

Expected: **932 passed, 0 failed, 0 errors**: 169 queries × 4 modes (row/Volcano,
columnar/Volcano, columnar/vectorized, and columnar/vectorized with
`--no-optimize`), plus 14 rejections × the same 4 modes, plus the multi-way
capability split — 16 multi-way queries × the 2 vectorized modes, diffed against
SQLite, and the same 16 asserted to be refused × the 2 Volcano modes — plus the
subquery block × the same 4 modes (14 correlated forms that must *bind* and reach
the Week 33 refusal, and 19 that must fail earlier for their own stated reason),
plus the Week 31 multi-relation-body split (1 query × 2 vectorized modes diffed,
× 2 Volcano modes refused).

The multi-way block is 6 Week 27 execution shapes plus 7 Week 28 join-ordering
ones. Running the ordering queries in the `--no-optimize` vectorized mode as well
is the point, not duplication: that mode keeps the **written** join order, so the
pair is what makes this file able to catch a reordering that changes an answer.

The subquery block changed shape in Week 31. Its 25 **uncorrelated** queries now
return rows and moved into `QUERIES`, where they are diffed against SQLite in all
four modes — the only oracle for the rules a materialization gets wrong: a scalar
over zero rows, a scalar whose one row is NULL, and `NOT IN` over a set
containing NULL (never TRUE, so no rows). What is left as a rejection suite is
the **correlated** half, and reaching *that* refusal is still the assertion:
lexing, parsing, nested-scope resolution, correlation detection and validation of
the nested query all had to succeed to get there. The second suite exists so the
refusal cannot become a catch-all that hides a real defect: a bad nested table, a
bad nested column, a wrong arity or a disallowed position must all outrank it.

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
[WHERE expr [AND expr ...]]                          -- subqueries: uncorrelated
                                                     -- execute (Week 31);
                                                     -- correlated bind only
                                                     -- (Week 33)
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
>   sets. `--explain` reports `join-ordering=skipped (outer join)` and prints no
>   `order=` line — a decision was available and was refused, which is worth
>   saying, but no order was chosen.
>
> A fourth consequence is quieter: the predicate a join hands its preserved side
> as a **zone-map pruning hint** is now the place the null-supplying side's
> conjuncts live, so an outer join withholds it unless every relation it mentions
> is preserved. One rule (`pruningHintForPreservedSide`, `predicate_pushdown.h`)
> for both hint routes — `VectorizedPlanBuilder`'s JOIN case and `Planner::plan`'s
> FROM scan — because a second copy is how the two engines drift apart.
>
> The build side is forced rather than costed: the preserved side must be the
> probe input (`build=<table> ... (outer: the preserved side must probe)`), and
> the SIMD loop join — an inner equi-join with no unmatched path — is never
> selected. `RIGHT`/`FULL` are out of scope; no TPC-H query in the documented
> dialect needs them.

> **Week 30 subqueries — parsing and binding only.** Three forms are in the
> grammar: scalar `(SELECT ...)`, `[NOT] EXISTS (SELECT ...)` and
> `x [NOT] IN (SELECT ...)`. `IN (subquery)` is a **different production** from
> the constant list — `InExpr`'s design is a set hashed once at compile time,
> which a subquery cannot be — and Week 32 lowers it to a semi-/anti-join.
>
> They are legal in `WHERE` and `HAVING` only. `SELECT`, `GROUP BY`, `ORDER BY`
> and `JOIN ... ON` each refuse with their own message: no TPC-H query puts one
> elsewhere, and allowing one in a select list would make `buildProjectSchema`
> type it and `aggregateOutputName` name an output column after it. `FROM
> (subquery)` is a derived table, added in Week 34 — a different construct
> (a `TableRef`, i.e. a RELATION of the block) rather than an extension of this
> position rule.
>
> **A subquery is the first nested scope in this engine.** `ColumnRef` carries a
> `query_level` beside its `relation_slot`: the slot is a position in the range
> table of the scope `query_level` blocks out, so an inner slot 1 and an outer
> slot 1 are different relations and a slot read without its level compares two
> numbering domains. Resolution walks innermost-first and then outward; an inner
> block shadows an outer one (not an ambiguity — that check is per scope), and a
> name that resolves outward marks every scope between it and the supplying block
> correlated.
>
> **Uncorrelated subqueries execute (Week 31), by materialization.** Uncorrelated
> means the body references no relation of any enclosing block, so its value
> cannot depend on the outer row: it is run ONCE before the outer query is
> planned, and the node is replaced by a constant — a value for a scalar, a truth
> value for `EXISTS`, a constant list for `IN`. That is the same argument
> constant folding makes (`constant_folding.h`): the substituted `Literal` puts
> the predicate back into the `ColumnRef op Literal` shape that zone-map pruning,
> `scanColumn`'s tight loop and range selectivity all pattern-match on. The pass
> is `materializeSubqueries` (`planner/subquery_materialization.h`), called from
> `main.cc` ABOVE both engines, so the four modes agree by construction; the
> nested query runs on the engine that contains it, and honours `--no-optimize`.
>
> **Correlated subqueries do not.** Every parse, bind and validate error a query
> is entitled to fires first — including the nested query's own, which is
> validated against its own `FROM` schema — and then one check at the end of
> `Validator::validate` raises `correlated subqueries are not yet executable
> (Week 33)`. One site, four modes. That check is also the containment the
> [slot-consumer table](#relation-slots-and-query-levels) rests on: a `ColumnRef`
> with `query_level > 0` exists only inside a correlated subquery.
>
> Three rules to know before touching the pass. A scalar over **zero rows** is
> NULL, and so is one over a single NULL row — the first constant NULL in the
> engine, typed from the body's output schema (`Literal::null_type`). A scalar
> over **more than one row** is a run-time error. And `NOT IN` over a set
> containing NULL is **never TRUE**, so it folds to a constant false; the
> positive form drops the NULLs, because UNKNOWN and FALSE are indistinguishable
> to every consumer reachable from `WHERE`/`HAVING` — which stops being true the
> day a general `NOT` is added.

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
| `CASE` has **no vectorized kernel** | `evaluate()` short-circuits; a chunk kernel cannot. `CASE WHEN i < 100 THEN i * 1000000000000 ELSE 0 END` would raise a `checkedMul` overflow for rows whose branch is discarded, and the differential tests would be right to fail. `compileNode` declines, so the fallback is correct-but-slow — see the measured cost below. A masked kernel (evaluate each branch only over its own selection) is the fix when Q8/Q12/Q14 profiling justifies it. **This row states a general rule as a property of `CASE`, and seam audit pass 4's E-13 is what that cost.** The rule is: *any construct whose two implementations disagree about which sub-expressions get evaluated changes whether a query errors.* `AND` is the same construct with the polarity reversed — there it was the SCALAR evaluator that was eager and the CHUNK path that short-circuited — and that half went undrawn for four passes. Both engines now cascade `AND` in a filter (`evaluatePredicate` in `evaluator.cc`, `evalPredicate` in `columnar_eval.cc`), and the general rule lives in `parser/expr_totality.h` |
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

### Written evaluation order, and why it is a plan property

Per-row evaluation is **not total** — `evaluate()` can throw on a row (integer
overflow, `SUBSTRING` out-of-domain positions, a comparison across the STRING
boundary). So *which rows an expression is evaluated on* decides whether a query
**errors**, and SwiftQL fixes that set in the **plan** rather than leaving it to
whichever engine happens to run it. The rule, stated once in
`src/parser/expr_totality.h`:

- **A conjunct of a filter is evaluated on the rows for which every conjunct
  written before it evaluated TRUE.** Both engines implement exactly this cascade
  (`evaluatePredicate` in `evaluator.cc`, `evalPredicate` in `columnar_eval.cc`).
  This makes `AND` non-commutative in the presence of a raiser and is a
  **user-visible dialect fact** — it is in the README's dialect section for that
  reason, with the measured pair.
- **A join's `ON` residual is evaluated on every candidate pair the keys
  matched**, in the probe loop, **eagerly over the whole residual conjunction**.
  There is no conjunct cascade inside a residual; `evaluate()` computes both
  operands of an `AND` before it looks at the operator. All four legs agree, so
  it is not a divergence — it is the cascade rule not holding for a construct the
  parser accepts. Recorded on `LogicalJoin::on_residual`.
- **Every other expression is evaluated on every row that reaches its node** —
  and the rule **binds the rewrite, not the expression**: a pass may not change
  which rows reach a node holding an expression that can raise. *An expression
  need not move to be moved.* That last clause is where five consecutive audit
  passes landed, each on the one entry a shorter enumeration was missing.

The obligation is therefore **per expression whose row set a rewrite changes, not
per move the rewrite makes** — a strictly larger set. The screen is
`exprMayRaise` / `conjunctMayRaise` / `firstMayRaise` (`parser/expr_totality.h`),
a conservative over-approximation with **one** implementation and five consumers:
predicate pushdown (twice — the conjunct moves *and* `on_residual`), zone-map
chunk pruning, the `LIMIT`-below-a-raising-projection rule, and decorrelation
(in both directions: a lift can remove a guard *or* add one).

### Constant folding

`foldConstants` (`constant_folding.cc`) runs as the last step of `Binder::bind`,
rewriting constant arithmetic subtrees to literals before validation. It is
unconditional, and both execution paths and `--no-optimize` get it.

> ⚠️ **RETRACTED (Week 37 doc sweep; seam audit pass 2's B-5, pass 4's P4-6).**
> This paragraph used to end *"folding cannot change results, so it is
> canonicalization rather than a cost-based decision"*. It was withdrawn from
> `constant_folding.h` and from `binder.cc` by execution, and this prose copy —
> the canonical version the headers paraphrase — was the last unqualified
> statement of it in the repository. **What is true is narrower:** for any
> expression `foldNode` agrees to fold, the folded node evaluates to the *same
> `Value`* the original would have, on every row. What does **not** follow is
> that the query's *outcome* is unchanged — five consumers pattern-match on the
> *shape* of the tree and see a `Literal` the user did not write; `binder.cc`
> carries the census with a verdict each. And the pass is **load-bearing for
> correctness**, not an optimization: it is the only thing that removes an
> `IntervalLiteral`, and `inferExprType` and `evaluate` both throw on one that
> survives, so every TPC-H date-range predicate depends on it having run.
> Gating it behind `--no-optimize` does not make those queries slower; it makes
> them **error, on the differential leg only** (`logical_plan.cc` names that
> exact reader). It is unconditional because it must be, not because it is free.

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

## Relation slots and query levels

A `relation_slot` is a **position in a range table**, and since Week 30 there can
be more than one range table in a query: `ColumnRef::query_level` says which. The
pair is the identity; a slot read without its level compares two different
numbering domains, and the failure is silent — the wrong relation's column, or a
check quietly skipped.

That collapse appeared **five times in one week**, across three audit rounds, at
consumers nobody had listed — twice at sites found only when this table was
itself audited for completeness. This table is the list. Add a row when you add a
consumer, and re-check every "contained" row on the day a new nested-scope
construct lands (Week 31's uncorrelated subqueries — done, see below; Week 32's
semi-/anti-joins; Week 34's derived tables — done, see below).

**A missing row is worse than a wrong one.** A future week reads this as
already-checked and skips the verification. `ChunkPruner` was absent from both
halves for exactly that reason: it *is* mentioned elsewhere in this file, under
the dispatch checklist, answering a different question — which reads as
considered-and-dismissed.

**Two different things are called `relation_slot`, and only one of them can be
wrong.** `ColumnDef::relation_slot` (`schema.h`) is a *schema* slot: a schema is
built for one query block, so there is no level to lose and every reader of it is
safe by construction. The rest — `ColumnRef`, `GroupByColumn`, `AggregateSpec`,
`JoinKey::from_slot`, `ColumnStatsEntry` — carry a slot that came from binder
resolution and therefore *can* name an enclosing block's relation.

### Week 33: the pair is a type (`ColumnId`)

`ColumnRef`, `GroupByColumn` and `AggregateSpec` no longer carry a bare
`relation_slot` (or, on the first two, a separate `query_level`). They carry a
single `ColumnId` (`src/common/column_id.h`), whose slot is **private**: there is
no implicit conversion to `int` and no public member, so a bare integer cannot be
passed where a qualified reference is required. Reading the slot costs a named
call:

| Call | Means | Fails how |
|---|---|---|
| `id.isLocal()` | "is this reference from the block I am planning?" | — |
| `id.isResolved()` | "did the Binder stamp it?" (`-1` = resolve by bare name) | — |
| `id.localSlot(site)` | "I am in this reference's scope, give me the position" | **throws**, naming `site`, if the ref is correlated |
| `id.slotInOwnScope(site)` | "I want the slot *without* being in its scope" — the escape hatch | never; every use is greppable by `site` and justified at the call |
| `id.couldBeSameRelation(other)` | `checkGroupedRefs`' match rule: levels equal, unresolved is a wildcard | — |
| `id.outward()` | decorrelation's level decrement | throws on a level-0 id |

**The escape hatch has exactly two justified users**, and a third would be
suspicious: `exprKey` (`expr_utils.h`), which hashes the pair as an identity and
never indexes anything with it, and `Binder::checkCorrelatedAggregateArg`, which
walks the scope chain out `level()` steps *first* and then indexes that scope's
range table — the only layer that can.

**`ColumnDef::relation_slot`, `Schema::indexOf(name, slot)` and
`ColumnStatsEntry::relation_slot` were deliberately NOT migrated.** They are
schema slots. Narrowing happens where a binder slot is handed to them, which is
what every `localSlot(...)` call site in the tables below is.

**The concrete case that justifies the 87 sites.** Recorded so nobody proposes
simplifying `ColumnId` back to an `int`. `inferExprType` (dispatch site 12)
resolved a `ColumnRef` *slot-first* against the schema of the block being
planned. For a correlated ref that slot indexes an **enclosing** block's range
table — and pre-migration the lookup would not have failed, because `team` and
`driver_id` exist in *both* shipped tables, so `indexOf("team", 0)` against the
wrong schema is a clean **hit**. `inferExprType` would have returned the wrong
relation's column type and carried on. After the migration it throws, naming
itself, and it was found on the first correlated query ever planned — minutes
after Week 33 removed the refusal, not by an audit round.

**What this replaces.** The tables below used to be the containment: a prose list
that a consumer could be missing from, and twice was. They are now the *audit
trail* for a containment the compiler enforces. Adding a consumer still means
adding a row — but a consumer that forgets the level no longer compiles, and one
that asserts a level it does not have throws with its own name in the message.

### Reachable with a correlated ref (before `Validator::validate` refuses)

Each row's "level-aware" claim is now expressed in code as one of the calls
above; the parenthesised call is which one.

| Consumer | Status |
|---|---|
| `Binder::resolveColumnRef` (`binder.cc`) | **Level-aware.** The producer: walks scopes outward and stamps `(level, slot)` together |
| `Binder::checkCorrelatedAggregateArg` | **Level-aware.** Resolves the argument's type through the scope chain — the only layer that can, which is why the check lives here rather than in `Validator` |
| `collectSlots` (`predicate_pushdown.cc`, site 8) | **Level-aware.** Maps `level > 0` to `-1`, this walker's "cannot name it here" value, which makes all three callers conservative |
| `exprKey` (`expr_utils.h`, site 1) | **Level-aware** since round 2. Was wrong: `0#driver_id` meant two different columns, so a *correlated* expression group key satisfied an ungrouped *local* reference, and `substituteInto` would have rewritten on the same collision |
| `checkGroupedRefs` (`validator.cc`, site 5) | **Level-aware.** Returns for `query_level > 0`, and the plain-column match compares the level as well as the slot |
| `validateExpr` (`validator.cc`, site 4) | **Level-aware.** Returns for `query_level > 0`: the Binder verified the ref against the scope that supplies it |
| The SUM/AVG argument type check (`validator.cc`) | **Level-aware** since round 2. Was wrong: indexed `stmt.joins` — the *inner* list — with an outer slot, so the same illegal `SUM(d.name)` was caught or skipped depending on the inner query's own join order |
| The GROUP BY existence check (`validator.cc`) | **Level-aware.** Skips on `g.query_level > 0`. Testing `!table_name.empty()` instead made the outcome depend on the *enclosing* block's relation count |
| The ORDER BY bare-column existence check (`validator.cc`) | **Level-aware.** Tests `query_level == 0` before looking the name up in this block's FROM schema. Same shape and same round-1 commit as the GROUP BY row above; listed because the value of this table is completeness, and a correct-but-unlisted entry is a hole in the audit trail |
| `validateJoinCondition` (`validator.cc`, site 18) | **Level-aware.** Returns for `query_level > 0`; `relations` is this block's range table |
| `classifyJoinCondition` (`join_condition.cc`) | **Level-aware.** Refuses to build a `JoinKey` from a `level > 0` operand, *before* the unbound positional branch, so a key-less nested join still reaches the cross-product refusal |
| The `JoinKey` STRING/numeric check (`validator.cc`) | **Two call sites since seam audit pass 3 (B3-2), and they answer differently.** The `stmt.joins` loop is still level-agnostic and safe by contract — its `from_slot`s come only from `classifyJoinCondition`, which cannot emit a correlated one. `Validator::validateJoinKeyTypes` is **level-aware by construction**: it walks the *finished plan*, where the three subquery producers' keys have already been rewritten into the domain of the block that owns them (`splitCorrelation` calls `id.outward()` before taking the slot, `lowerInSubqueries` takes the operand's own local slot), so there is no level left to be wrong about. The row above used to claim one check covered every `JoinKey`; it covered one of four producers |
| `foldNode` (`constant_folding.cc`, site 14) | Reads no slot. Descends into `SubqueryExpr::operand` only |

### Unreachable with a correlated ref today (behind the refusal)

**The containment changed shape in Week 31 and still holds.** It used to be "no
statement with a subquery is planned at all". It is now two facts that together
say the same thing:

1. `Validator::validate` refuses any statement with `has_correlated_subquery`
   (propagated upward by the Binder, so it means "correlated at any depth"), and
   both planner entry points call it first. A `ColumnRef` with `query_level > 0`
   exists **only** inside a correlated subquery, so none reaches a plan.
2. An **uncorrelated** subquery is materialized before planning
   (`subquery_materialization.h`): its body is handed to the planner as its own
   **top-level statement**, where every ref is level 0 against that statement's
   own range table, and the outer statement is left holding a constant. There is
   never more than one range table in play for one plan.

**Week 32 broke fact 2's last sentence, deliberately, and replaced it with a
different containment.** An uncorrelated `IN (subquery)` is no longer
materialized: `lowerInSubqueries` (`subquery_lowering.h`) plans the body
recursively and **grafts that subtree into the outer plan tree** as
`children[1]` of a `LogicalJoin{SEMI|ANTI}`. One plan now holds nodes built from
**two range tables**. Both are level 0 — no correlated ref is lowered this week,
so the `ColumnId { level, slot }` trigger stays unfired and now points at **Week
33**, which *is* the week that first lowers a correlated reference. The collision
is not between levels; it is between **two slot numbering domains at the same
level**.

The containment, and it is a strong one: a semi-join and an anti-join emit
**only left-side columns**, so a SEMI/ANTI `LogicalJoin`'s `output_schema` **is**
`children[0]->output_schema`, never a merged schema. Consequences, each relied on
by a row below:

- No expression above the join can name a body slot, because no body column is in
  scope — so the merged-schema slot stamping in `LogicalPlanBuilder::build` and
  `VectorizedPlanBuilder` never runs for such a node, and `buildProjectSchema` /
  `inferExprType` never resolve an outer name against a schema holding a
  same-named body column.
- `join_slot` is **-1**, meaning "children[1] is not a relation of this block's
  range table". Every reader of `join_slot` must decline on
  `semantics != STANDARD` or be provably unreachable for such a node.
- The only place a body slot is read is `JoinKey::join_col`, resolved against
  `children[1]`'s own schema — exactly the domain it came from.

`lowerInSubqueries` asserts the schema equality at construction. That single
assertion is what keeps the two domains from meeting.

**Week 34's derived tables broke this containment for real, and here is what
replaced it.** A derived table's columns *are* in scope above it, so keeping the
body's numbering out is not available. Week 34 **normalizes** instead: a derived
relation's own `output_schema` is stamped **slot 0** — exactly as a leaf scan's
own schema is — by `derivedRelationSchema` (`logical_plan.cc`), and the OUTER
slot is applied only by the merged join schema, by the same loop that stamps a
base relation. So what enters the outer plan carries outer numbering and nothing
else, and every `indexOf(name, slot)` above the graft is again answering a
question about one range table.

The two containments now coexist and cover different constructs:

| Construct | Containment | Enforced by |
|---|---|---|
| `SEMI`/`ANTI` join (Weeks 32–33) | The body is never in scope above the node: `output_schema` **is** `children[0]`'s | The assertion in `subquery_lowering.cc`, `VecHashJoinNode`'s width check, `rightKeyIndices` |
| Derived relation (Week 34) — `FROM (subquery)`, and the right side of a decorrelated correlated scalar | The body **is** in scope, and is normalized to one outer slot | `derivedRelationSchema`, plus the drift check in `buildRelation` comparing `blockOutputSchema` against the built plan |

A `STANDARD` join over a `LogicalDerived` — which Week 34's correlated-scalar
rewrite builds — relies on the second and is a wrong answer under the first
alone.

### Week 32 consumers (one plan, two range tables)

| Consumer | Status |
|---|---|
| `lowerInSubqueries` (`subquery_lowering.cc`) | **Safe by domain.** Reads `SubqueryExpr::operand`'s `relation_slot` into `JoinKey::from_slot`. The operand belongs to the **enclosing** query and is bound at level 0 there, so the slot is in the OUTER range table — the same domain `leftKeyIndices()` resolves against. `join_col` is the body's single output column, resolved against the body plan's own schema. The two never meet |
| `CardinalityEstimator::estimateNode`, SEMI/ANTI branch | **Safe by domain, but only since the merge was guarded.** `out.findForRef(from_col, from_slot)` uses the outer merged context; `right.find(join_col, -1)` is a bare-name lookup against the body's own one-relation context, where a bare name is unambiguous. The context-merge above the branch stamps `right.entries` with `join.join_slot` and so ran for SEMI/ANTI too, handing the parent body columns at slot -1 — columns not in the node's schema at all. It is now `if (semantics == STANDARD)`, which is what makes "reads `join.join_slot` only on the STANDARD path" a fact rather than a claim. Latent, not live: `StatsContext::find`'s bare-name fallback scans every entry, so a lookup missing on the left could have landed on a body column, but every outer ref above the join binds to a real slot today. Week 34's derived tables are what would have made it a wrong estimate |
| `VectorizedPlanBuilder`, SEMI/ANTI lowering | **Safe by domain — and it has a second `join_slot` reader that had to be guarded.** The lowering itself: `leftKeyIndices` against the probe (outer spine) schema, `rightKeyIndices` against the body's own; `output_schema` handed to the operator is the probe schema, unmerged, and the operator's constructor re-checks its width. But `collectSlotTables` (`vectorized_plan_builder.cc`) also reads `join->join_slot`, from the cost block, which runs **before** the SEMI/ANTI early return. It is reachable: two `IN` conjuncts stack two semi joins, and `rowWidth()` on the outer one's left child walks the inner one. Unguarded it stamped the **body's** table at key `-1`, which names no relation of this block. Latent, never a wrong answer — **because the entry could not be read**: the map's only consumer is `slot_tables.find(col.relation_slot)` over the child's `output_schema`, and a semi/anti join's output schema *is its left child's* (asserted in `subquery_lowering.cc`), so every column in it carries a real binder slot and nothing ever looks up `-1`. The width is bit-identical before and after the guard. (An earlier rationale said "the widths are discarded before `setCostDecision`"; that is true today only via a build-order fact in `logical_plan.cc` — no STANDARD join is ever built above a semi join — which the cost block does not enforce, so it is not the reason and must not be inherited as verified.) Still a live violation of the contract this table exists to enforce, so it now stamps only on `semantics == STANDARD` while still walking the left side. Third time this row was wrong by omission; it names both readers now |
| `JoinEnumeration::slotDeclineReason` (called `hasSlotOutsideRangeTable` when this row was written; renamed in `18af84f`) | **Level-agnostic, and now LIVE.** Fires on `join_slot == -1`, which is exactly a semi/anti join. Weeks 28–30 expected Week 31 to make this decline live and Week 31 reported it had not; **Week 32 is where it became live**, not Week 34. **CORRECTED (Week 37 doc sweep; seam audit pass 2's B-1, still open after three fix rounds):** this row used to end *"The decline is silent, in the same shape as the <3-relation one — there was no ordering decision to report"*. Both halves are false at HEAD. `18af84f` made the cause **reported** — `--explain` prints `join-ordering=skipped (semi/anti join)` on the `LogicalSemiJoin` line, verified on the shipped F1 catalog — and it did so by refuting the premise: on `FROM a JOIN b JOIN c WHERE x IN (SELECT …)` the block below the semi join *is* a fully inner three-relation spine the search could have reordered, so an ordering decision **was** available. Measured, same file: the spine alone gets `order=drivers@1,drivers@2,laps@0 cost=43104 (written=60637) method=dp`; with the `IN` it gets nothing. The row at *Week 34 consumers* below has said so since pass 2 — this one contradicted it in the same file for three fix rounds, which is the failure mode this table's own header warns about |
| `PredicatePushdown::pushIntoJoin` | **Declines.** Tests `semantics == STANDARD` alongside `join_type == INNER` before `by_slot.find(join_slot)`. A stronger reason than the outer join's: children[1]'s columns are not in the output schema at all, so a conjunct pushed there is unresolvable, not merely mis-scoped. The recursion into `children[0]` is unconditional **for a semi/anti join**, which is what keeps a `WHERE` conjunct reaching the spine's scans. It is **not** unconditional in general: Week 37 (seam audit pass 5, P5-B1) made it decline at a LEFT join whose `on_residual` can raise, because that residual is evaluated per CANDIDATE PAIR and the preserved-side push shrinks the pair set. This row read "stays unconditional" as a virtue for five weeks and was quoted back as one by the audit |
| `VecHashJoinNode` SEMI/ANTI probe | **Reads no slot.** Takes resolved column **indices**, and its output schema is the probe child's |
| `buildAggregateSchema` (Week 30 tripwire) | **Still armed, still unreached.** An `IN` body may hold its own `GROUP BY`; its `GroupByColumn`s are level 0 against the **body's** range table, and the body is validated and planned as its own block, so the guard does not fire. The call context changed even though the data did not — covered by a query in `WEEK32_SEMI_JOIN_VEC_ONLY` rather than by reading |
| `ChunkPruner::shouldSkip` (Week 30 tripwire) | **Still armed, still unreached.** Unchanged by this week |

So everything below still receives level-0 refs **by construction**, and both
guarded rows below stay **armed and unreached** — Week 31 checked each consumer
in this table and made none of them reachable. Do not replace a tripwire with
"real behaviour" until the week that genuinely lowers a correlated reference; on
the current schedule that is Week 33.

Two rows carry a guard of their own as well, because their failure mode is a
silent wrong answer rather than a miss, and because neither is protected by the
`collectSlots` → `soleSlot` → `-1` argument that covers `restampSlots`. They are
marked **guarded**; the rest are contained only.

| Consumer | Note for the week that removes the containment |
|---|---|
| `collectSimplePredicates` / `ChunkPruner::shouldSkip` (`chunk_pruner.h`) | **Guarded.** Tested `relation_slot < 1` on a WHERE-clause `ColumnRef` and then matched **by name** against the scanned table's zone maps. A correlated ref is `(level 1, slot 0)`, which reads as scan-local, so with a shared column name (`team`, `driver_id`) the wrong relation's zone maps prune the scan — chunks skipped silently, invariant 12's subject. Reached on the `--no-optimize` path, where the whole un-pushed WHERE goes to the FROM-side scan as a hint and pushdown never saw it. Now **declines** a `query_level > 0` ref: a pruning hint is an optimization, so contributing nothing is correct-and-slower. **Week 31 checked and did NOT arm it**: an uncorrelated body's hints are level-0 refs against that body's own scan. It gained a second decline in Week 31 for a different new input — a NULL literal, see *Null constants* below |
| `buildAggregateSchema` (`logical_plan.cc`) — and through it every `GroupByColumn` consumer: `HashAggregateNode` (`plan_nodes.cc`), `VecHashAggregateNode` (`vec_hash_aggregate_node.cc`), `CardinalityEstimator` | **Guarded.** `GroupByColumn` is the one struct that *carries* a level and whose every consumer ignored it. The failure is quieter than a miss: for `GROUP BY l.team` inside a subquery over `drivers`, `indexOf("team", 0)` is a clean **hit on the wrong relation**, so neither the bare-name fallback nor the `idx < 0` throw fires and the query groups by the wrong column. Now **throws**: grouping is not an optimization and a correlated key has no correct local fallback — its value comes from the outer row, which is Week 33's machinery. One guard covers all four consumers, since the other three run on a plan whose schema was built here. **Week 31 checked and did NOT arm it**: a correlated group key can only appear inside a correlated subquery, which is refused |
| `restampSlots` (`predicate_pushdown.cc`, site 9) | Stamps unconditionally, and has a **second, independent** proof: `collectSlots` gives a correlated ref `-1`, so `soleSlot` is `-1`, so the conjunct is never pushed and this is never called on one. Its `SubqueryExpr` branch touches only `operand`. Listed here rather than above because its only caller, `PredicatePushdown`, runs on a built logical plan — i.e. after the refusal |
| `AggregateSpec::relation_slot` (`logical_plan.cc`, and `plan_nodes.cc` / `vec_hash_aggregate_node.cc` resolving it) | Copied straight off a `ColumnRef`. This is the struct `GroupByColumn` was given a `query_level` for; it was contained instead. The invariant is stated at the field |
| `JoinKey::from_slot` (`join_enumeration.cc`, `logical_plan.cc`, `vectorized_plan_builder.cc`, `cardinality_estimator.cc`) | Contract stated at the struct: only meaningful while every key operand is level 0, which `classifyJoinCondition` enforces |
| `inferExprType`, `buildProjectSchema`, `buildAggregateSchema`, `collectCols` (`logical_plan.cc`) | Resolve `indexOf(name, slot)` against a plan schema |
| `resolveColumnIndex` (`evaluator.cc`) | Same, per row, with a bare-name fallback that would silently pick *some* column |
| `CardinalityEstimator` / `StatsContext::findForRef` | A miss degrades the estimate silently — the failure mode statistics code specialises in |
| `Planner::plan`, `VectorizedPlanBuilder` | Merged-schema stamping and pruning-hint slot sets |
| `materializeSubqueries` / `buildReplacement` (`subquery_materialization.cc`, Week 31) | **Reads no slot, safe by PRECONDITION — and the precondition is named here rather than assumed:** `Validator::validate` refuses a correlated statement first, so this pass only ever meets an uncorrelated `SubqueryExpr`, whose body is a self-contained query block. It moves the `IN` operand (already bound, level 0, this block's) into the substituted `InExpr` and copies nothing else. If a later week lets it run before validation, or on a correlated node, this row is void |
| `forEachSubquery` / `collectQueryTables` (`subquery_materialization.cc`, Week 31) | Reads no slot. Dispatch site 19, and the one walker that deliberately descends INTO the body — materialization asks "which statements must run, in what order", which is not a scope question. It reaches no `ColumnRef` at all |

### Week 34 consumers (a range-table entry that is a plan)

Added under the rule this file states for itself: add a row when you add a
consumer, and re-check every "contained" row on the day a new nested-scope
construct lands. A missing row is worse than a wrong one.

| Consumer | Status |
|---|---|
| `derivedRelationSchema` (`logical_plan.cc`) | **The producer of the containment.** Renames by the column-alias list, stamps every column `relation_slot = 0`, and REFUSES two output columns of one name — a base table cannot have them, a derived table can, and then *both* `indexOf` overloads are a coin flip |
| `blockOutputSchema` (`logical_plan.cc`) | **Safe by sharing, and checked.** The Binder needs the derived schema before `build` has run, so this composes the same helpers `build` uses (`buildScanSchema` / `extractAggregates` / `buildAggregateSchema` / `buildProjectSchema`). A private second derivation would be the two-paths drift Weeks 26/28/30 each undid, and the two would have to agree on `aggregateOutputName`, `hidden` columns and `SELECT *` expansion |
| `buildRelation` (`logical_plan.cc`) | **The drift check.** Asserts the planned subtree's `output_schema` equals the one the Binder resolved against, column for column. It compares objects from two different code paths, so it CAN fail — unlike the assertion Week 33 deleted for comparing a copy of an object with the object |
| `Binder::relationSchema` (`binder.cc`) | **Level-aware by construction, and the LATERAL rule.** Binds the body against `parent`, never the scope being built, so sibling `FROM` items are invisible; a reference reaching further out marks the body correlated and is refused by name. Known boundary: at top level there is no parent, so a lateral reference reports as an ordinary unresolved qualifier — a sibling genuinely is not in scope and the Binder cannot tell a lateral reference from a typo. Both halves are pinned in the rejection suite |
| `Validator::validateQuery` (`validator.cc`) | **Rebuilt around ONE range table.** It previously recomputed the same keying four times (FROM schema, the SUM/AVG slot arithmetic, the join-condition `relations` vector, the GROUP BY existence check). The SUM/AVG check resolved slot *k* through `catalog.getTable(joins[k-1].join_table)`, which has no answer for a derived relation — it now resolves through the range table, the same move Week 30 round 2 made for the correlated half |
| `TableRef::tableName(site)` (`ast.h`) | **The narrowing point, and the same discipline as `ColumnId::localSlot(site)`.** `table_name_` is private; every read states by name that its caller believes the relation is a catalog table, and throws otherwise |
| `countRelations` (`join_enumeration.cc`) | **Corrected.** Counted SCANS recursively, which equals the range-table size only while every scan belongs to this block. A derived relation is ONE relation whose body may hold many (over-count); a semi/anti `children[1]` is not a relation at all (also counted). It counts the spine now |
| `slotDeclineReason` (`join_enumeration.cc`, called `hasSlotOutsideRangeTable` when this row was written) | **Does NOT fire for a derived relation**, contrary to what Weeks 28–31 all predicted. With `countRelations` right, a derived relation has an in-range slot and the search reorders it normally (`method=dp`, optimized ≡ `--no-optimize`). **CORRECTED (seam audit pass 2, B-4):** this row used to end "No reported decline was added". One *was* — `18af84f` added `join-ordering=skipped (semi/anti join)` on exactly the argument this row denied, because a fully inner three-relation spine below a semi join is a decision that WAS available. The derived-relation half stands; the no-decline half was false |
| `joinCardinality`'s no-statistics branch | **RETRACTED — this row was FALSE IN BOTH HALVES** (seam audit pass 2, B-4). It is, sentence for sentence, the paragraph `18af84f` deleted from `join_enumeration.cc`; the commit swept the `.cc` and left this copy standing, in the table whose own header says *"A missing row is worse than a wrong one"*. `max(l, r)` does NOT run: `have_ndv` is set when EITHER side supplies an NDV and the non-derived side always does, so the multiplicative branch runs — and `method=written-floor` is NOT reachable that way; the DP won outright on both queries measured. **What is true is narrower:** a derived relation contributes no NDV, so a subset containing one is priced from the other side's statistics alone, and `max(l, r)` still runs when NEITHER side has any. The containment is unchanged and is the written-order bound in `reorder()`. Week 37 also swept the third copy, in `join_enumeration.h` |
| `leafScanTable` / `leafScanTableOf` / `isSingleRelation` | **Stop at the node.** All three descended `children[0]` to a `SCAN` and returned the BODY's base table for a derived leaf, attributing one table's `avg_width` to another relation's columns — the attribution error Week 27 refused to make. Nullable now; the callers fall back to the uniform proxy |
| `collectSlotTables` (`vectorized_plan_builder.cc`) | **Skips a derived `children[1]`** rather than stamping the body's table at a real slot. Its rationale block was also corrected for the fourth time: its discarded alternative argument rested on "no STANDARD join is ever built above a semi join", which Week 34's correlated-scalar rewrite makes routine |
| `PredicatePushdown::distribute` / `filterOnto` | **Was safe by WRAPPING; since Week 37 it ENTERS the body** (seam audit pass 3, B3-3). Wrapping above the `LogicalDerived` was correct, and it was also a *silent* decline that cost 2.9× on the simplest exhibiting query plus the body's zone-map pruning — in every shape, including with no join present. Two rules now, with different preconditions: ENTRY attaches the filter above the body's ROOT, which is safe for ANY body shape (`GROUP BY`, `LIMIT`, `DISTINCT` — the filter is above them); DESCENT below the body's projection requires every column named to be a PLAIN PASSTHROUGH, and declines otherwise. A refusal at the relation boundary is stamped on `LogicalDerived::pushdown_decision`. `restampSlots(c, 0)` stays correct *because of* the slot-0 normalization, and the derived→body remap is POSITIONAL for the same reason — the alias list renames and a joining body repeats names |
| `Lowering::lowerNode` DERIVED case / `VecDerivedNode` | **Reads no slot.** Forwards chunks unchanged and reports the renamed schema, which has to reach the physical layer because `resolveColumnIndex` and every `indexOf` above the graft look the new names up |
| `lowerCorrelatedScalars` (`subquery_decorrelation.cc`) | **Builds a `LogicalDerived`, not a special case.** Its slot is `range_table_size + n`, one past the last the Binder issued, and it is a real merged-schema slot because the outer `WHERE` reads the aggregate's column. The join is **LEFT**: a zero-row group must yield NULL, not a dropped outer row |
| `Planner::plan` | **Refuses.** Third Volcano capability refusal, after Week 32's `IN` and Week 33's correlated. It builds its scan from a catalog name and one `HashJoinNode`, so there is no shape there that can hold a relation which is a plan |

### Weeks 36–37 consumers (the totality screen)

**Added by the Week 37 doc sweep, under the rule this section states for itself
— and it was missing, which makes it the fourth time this list has been wrong by
omission.** `staticTypeOf` (`src/parser/expr_totality.h`) is a `ColumnId`
consumer: it does `indexOf(name, slot)` slot-first with a bare-name fallback, at
a named `localSlot("staticTypeOf")`. It arrived with the seam audit's totality
screen and no row was added for it.

| Consumer | Status |
|---|---|
| `staticTypeOf` / `exprMayRaise` / `conjunctMayRaise` (`parser/expr_totality.h`) | **Contained, and its failure mode is the OPPOSITE SIGN of every row above.** Resolution is the shared rule — slot-first when `isResolved() && isLocal()`, bare name otherwise — so a `level > 0` ref skips the slot lookup and lands on the fallback, which on a shared column name (`team`, `driver_id`, `l_suppkey`) is a clean **hit on the wrong relation**. Every other row here loses a check or reads the wrong column. This one is a **conservative screen**, so a wrong resolution does not make it answer *"I cannot type this"* — it makes it answer *"I typed it"*, and a false **FALSE** unfreezes a pass that should have declined. Its own header states the two errors are not symmetric: a false TRUE costs plan quality on one query, a false FALSE is a **divergence**. **Contained today, in two independent ways:** `refuseSurvivingCorrelatedRefs` (`subquery_decorrelation.cc`) refuses a correlated ref anywhere except a top-level equality in the body's `WHERE` — by name, for exactly this collapse — and `collectSlots` maps `level > 0` to `-1`, so pushdown never routes such a conjunct. **The live half of the risk is not the LEVEL but the SCHEMA**: pass 5 (E-20 / S-13 / P5-2) found the screen handed the *scanning relation's* schema rather than the one the expression was written against, and that was a divergence in three seams at once. Every call site must pass the schema the expression is evaluated in — `orderByWork` the child's, `pushIntoJoin` the join's, `refuseMaskedResidualRaiser` the full residual schema, `ChunkPruner` the hint's. Checked at HEAD: all four do |

### Null constants (Week 31)

A second "find every reader" question, with the same shape as the slot one and
the same answer method, recorded because it caught a live throw.

Until Week 31 a `Literal` could never hold a NULL: the grammar has no NULL
literal, and `foldNode` declines any fold that evaluates to one. A materialized
scalar subquery that returned **zero rows**, or **one NULL row**, produces the
first constant NULL in the engine — carrying its type on `Literal::null_type`,
because `Value::type()` throws when null and `inferExprType` must answer for
every node.

Every reader of a `Literal`'s value or type, audited:

| Reader | Status |
|---|---|
| `inferExprType` (`logical_plan.cc`) | **Reads `null_type`.** The producer's contract |
| `CardinalityEstimator::selectivity` | **Fixed in Week 31.** Both branches called `lit->value.type()` unguarded, so the OPTIMIZED vectorized path died with `Cannot get type of null Value` on a query `--no-optimize` answered correctly. Now returns `0.0`: a comparison against NULL is UNKNOWN for every row, which is the exact answer, not a fallback |
| `collectSimplePredicates` (`chunk_pruner.h`) | **Declines.** `canSkipChunk` would in fact skip nothing — `Value`'s comparisons are three-valued — but that is an accident of the operators, not a stated rule. Seam audit pass 4 stopped resting on it: `canSkipChunk` now declines a NULL bound and a type-mismatched one **explicitly**, because the same accident does NOT hold across the STRING boundary, where `Value::operator<` throws at scan time on zone-map metadata |
| `cloneExpr` (`expr_utils.h`, site 11) | **Copies `null_type`.** It is part of the node's meaning; dropping it retypes a cloned NULL as INT and makes `inferExprType` disagree with itself across a clone |
| `ExpressionExecutor::compileNode` (site 15) | **Declines** (pre-existing, now reachable). The decline cascades, so the enclosing predicate falls back to `evaluate()` — correct-but-slow, and the reason no kernel needs to know about null constants |
| `evalPredicate`'s fast path (`columnar_eval.cc`) | Already tested `lit->value.isNull()` and fell back |
| `foldNode` (`constant_folding.cc`, site 14) | Already refuses to PRODUCE one, and arithmetic over one evaluates to NULL, so it declines to fold THROUGH one |
| `evaluate` (`evaluator.cc`) | NULL-safe throughout: AND/OR are three-valued, every other operator propagates |
| `exprToString` / `literalKey` (`expr_utils.h`) | `Value::toString()` renders `"NULL"`; `literalKey` has an explicit null branch |
| The `ORDER BY` / `GROUP BY` column-ordinal checks (`validator.cc`) | Already guarded with `!lit->value.isNull()` |
| `SUBSTRING`'s constant-argument checks (`logical_plan.cc`) | Same guard |

### The structural alternative — done in Week 33

Making the level part of the *type* — a `ColumnId { int level; int slot; }`, so a
bare `int` cannot be passed where a qualified reference is required — would turn
all three of this week's findings into compile errors. It is the right end state
and it is **not** a Week 30 change: `grep -c 'relation_slot\|from_slot' src/`
reports **87** non-comment mentions across `parser/`, `planner/`, `execution/`,
`storage/` and `common/`, plus every test that hand-builds a `ColumnRef`, a
`GroupByColumn` or an `AggregateSpec` — and the
containment above means the change buys nothing until a correlated ref can
actually reach the second table. The week that removes the containment is the
week to do it, and this table is what makes that change reviewable.

**It landed in Week 33, as its own commit, before any correlation feature
work.** `ColumnId` is described above; the trigger fired exactly as predicted —
Week 33 removes the `has_correlated_subquery` refusal, so a correlated reference
reaches a plan node for the first time. What follows is the record of why it
waited, kept because the *deferral* was a decision and not an oversight.

**Week 31 is not that week, and said so before starting.** Uncorrelated
subqueries execute by materialization — the body is planned as its own top-level
statement and the outer node becomes a constant — so no `ColumnRef` with
`query_level > 0` reaches a logical plan. The trigger condition, restated for
whoever reads this next: *the first week in which a correlated reference is
lowered into a plan node.* On the current schedule that is Week 33; Week 32 trips
it only if it chooses to lower a correlated `EXISTS` to an anti-join, in which
case the structural change is its prerequisite and is done **as its own
standalone commit**, never folded into the feature.

## Extending the expression language

Nineteen functions dispatch on `Expr` subtype. **Ten fail silently** when a new
one is missed — no error, no crash, a wrong answer or a lost optimization
somewhere far away. Adding a node type means visiting all of them.

> **A node that CONTAINS a query changes the question at several of these.**
> `SubqueryExpr` (Week 30) is the first, and the rule it established is: descend
> into the parts written in **this** query block (the `IN` form's left-hand
> operand), never into the body. The body is a different scope — different
> schema, different range table, its own aggregate rule — and is handed to a
> fresh `Validator::validateQuery` / `Binder::bindQuery` instead. Sites 2, 5, 6,
> 7, 8 and 9 all turn on that distinction, and at three of them getting it
> backwards is a wrong answer rather than a lost optimization.

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
| 8 | `collectSlots` — `predicate_pushdown.cc` | empty slot set | **Silent, performance — and, since Week 27, silent correctness.** `soleSlot()` sees no single relation and returns `-1`, so the conjunct is evaluated above the join as a residual instead of on its own scan: right answers, lost pushdown. The second caller is `classifyJoinCondition`, which uses it to reject a *forward reference* inside a residual `ON` conjunct of any shape — a missed subtype makes that reference invisible, and the conjunct then resolves against whatever column of that name the left tree happens to have. Declared in `predicate_pushdown.h` for that reason: one walker, now THREE callers, never a private copy. The third (Week 29) is `pruningHintForPreservedSide`, which decides whether an outer join may hand its `WHERE` down to the preserved side's scan as a zone-map hint — and it is the one caller where an empty slot set is *dangerous* rather than merely lossy, because "mentions no relation" would read as "mentions nothing unpreserved". It therefore fails **closed**: empty means withhold. A new node type that this walker misses costs pushdown at the first two callers and would have silently reopened a null-supplying predicate's route to the wrong table's zone maps at the third. It covers `AggregateExpr` too, which pushdown alone never needs (aggregates are forbidden in `WHERE`) but an `ON` conjunct can contain — `validateJoinCondition` refuses those one line later, so the full pipeline cannot tell a blind walker from a seeing one. **Week 30** corrected the justification in `predicate_pushdown.h`/`.cc`, which claimed "every other caller treats empty as the conservative answer" — false of `classifyJoinCondition`, where an empty set means the forward-reference loop never runs and the conjunct is ACCEPTED. This caller fails closed on its OWN account, not on a property of the others. A `SubqueryExpr` contributes its operand's slots plus `-1` when the Binder marked it correlated: the body's slots belong to another scope's range table, while a correlated ref genuinely does reference this one and cannot be named here |
| 9 | `restampSlots` — `predicate_pushdown.cc` | returns | **Silent, performance.** Must stay in lockstep with #8: a pushed conjunct keeps its own relation's slot (any `k >= 1`) below the join, where `ChunkPruner` ignores it and the zone-map hint is lost |
| 10 | `exprToString` — `expr_utils.h` | returns `"?"` | Visible: output column literally named `?` |
| 11 | `cloneExpr` — `expr_utils.h` | **throws** | Loud. Marked `DISPATCH SITE` for this reason |
| 12 | `inferExprType` — `logical_plan.cc` | **throws** | Loud, at plan time |
| 13 | `evaluate` — `evaluator.cc` | **throws** | Loud, at execution |
| 14 | `foldNode` — `constant_folding.cc` | returns false | Safe **for the node itself**: no folding. Not safe for a CONTAINER node — every other one folds inside itself so the operand reaches `ChunkPruner` / `scanColumn` / `selectivity()` in the `ColumnRef op Literal` shape they pattern-match on. `SubqueryExpr` was missed here in Week 30 and corrected in its round-1 audit: fold the `IN` operand, never the body |
| 15 | `ExpressionExecutor::compileNode` | returns `nullptr` | Safe: caller falls back to `evaluate()` — slow, never wrong |
| 16 | `evalPredicate` — `columnar_eval.cc` | `evalFallback` | Safe. Needs no change for a new node: the fallback routes through `PredicateExecutorCache`, so adding a kernel at #15 is enough to make it fast |
| 17 | `CardinalityEstimator::selectivity` | `FALLBACK_SELECTIVITY` | Safe: a flat 0.5 guess. Add a real rule only when you can also afford to be *right* — `orderByWork` ranks conjuncts on selectivity alone, so an estimate that is low and wrong promotes an expensive predicate ahead of cheap ones. `IN` gets `k/ndv`; `LIKE` deliberately does not (see below) |
| 18 | `Validator::validateJoinCondition` — `validator.cc` | falls through | **Extended in Week 26**, in the same commit that relaxed `classifyJoinCondition` to accept multi-key equi-joins. It now dispatches every `Expr` subtype and its relation list is keyed by *ref name* (alias when present), without which every aliased qualifier fell through the unknown-qualifier escape and was checked by nothing. For a bound statement it re-checks by *relation slot*, never by `table_name` — the Binder rewrites an unqualified ref's `table_name` to its relation's table name, so matching on it lands on whichever relation is aliased to that name and rejects a legal query. Name matching survives only for validator-only callers that skip the Binder. **Live since Week 27**: `classifyJoinCondition` now hands every non-key conjunct on as a residual instead of refusing it on shape, so the Week 25 branches and the `AggregateExpr` branch are reached for real. This is the ONLY column-existence check those conjuncts get — `Validator::validate` runs before the residual is folded into the `WHERE` conjunction, so `validateExpr` never sees it. A gap here surfaces as a far-from-the-cause `column not found` from `inferExprType`. **Week 30** added a `SubqueryExpr` **throw** beside the `AggregateExpr` one: a residual carrying a subquery would reach a probe loop that cannot evaluate it, or be folded into the `WHERE` conjunction and routed by a relation slot it does not have. Note `classifyJoinCondition` runs one line earlier, so a subquery that also forward-references a later relation reports the forward reference — shape before contents. **The "re-checks by relation slot, never by `table_name`" rule above is only true within one query block.** `validateQuery` recurses into a NESTED statement's ON clauses, where a correlated ref is an ordinary top-level ref carrying an *enclosing* block's slot; indexing `relations` with it compares two numbering domains. Both this site and `classifyJoinCondition` therefore test `query_level` first — skip for existence here, and refuse to make a KEY there, or a key-less nested join joins on a fabricated key instead of hitting the cross-product refusal |

| 19 | `forEachSubquery` — `subquery_materialization.cc` (Week 31) | returns | **Loud, by construction.** The subquery inside the missed subtype is never materialized, survives into planning and hits site 12's throw. That backstop is the whole reason this walker may be added without becoming the eleventh silent site — do not "simplify" sites 12/13 into the generic unknown-subtype throw, which says nothing about which walker failed. It is also the ONE walker that deliberately descends INTO a `SubqueryExpr`'s body: Week 30's rule (descend into the parts written in THIS block, never into the body) is a rule about SCOPE, and "which statements must run, and in what order" is not a scope question — a nested body must be materialized before the body containing it can run. Its read-only twin `forEachSubqueryConst` answers the same question for the CLI's table loader, so a nested query's tables are loaded; the two must stay in step |

`ChunkPruner::shouldSkip` is not on the list *for the dispatch question*:
`collectSimplePredicates` returns immediately on anything that is not a
`BinaryExpr`, so a new node contributes no pruning hint. **Since seam audit pass
4 it does NOT follow that "its sibling conjuncts still prune", and that sentence
stood here until the pass found the divergence it describes.** A chunk skip
removes rows before the filter runs, so it can mask a raise the filter owed;
`collectSimplePredicates` therefore STOPS at the first conjunct that
`conjunctMayRaise` (`parser/expr_totality.h`) answers true for, and every
conjunct written after it contributes nothing. An unrecognised `Expr` subtype
answers "may raise", so a new node now costs pruning for the conjuncts *behind*
it as well as for itself. Still correct-and-slower, but it is no longer local.

**That answers the `Expr`-subtype question only, and this sentence has been read
as blanket clearance once already.** For the *relation-slot* question
`collectSimplePredicates` is a live consumer, and a dangerous one — it has its
own row in [Relation slots and query
levels](#relation-slots-and-query-levels).

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
