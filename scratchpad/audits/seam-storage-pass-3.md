# Seam audit — storage — pass 3 (final)

Branch `claude/phase5-week26-qomtkb`, HEAD `922ca15`. Gate already GREEN on this tree.
Written incrementally; sections appear in the order they were established.

Prior passes: `seam-storage-pass-1.md` (S-0 headline), `seam-storage-pass-2.md`
(refuted S-0, returned 0 BLOCKER / 0 HIGH / 0 MEDIUM / 2 LOW).

Three real cells, not four. `main.cc:548` refuses `--execution vectorized` unless
`--storage columnar`, so the grid is:

    A  --storage row      --execution volcano
    B  --storage columnar --execution volcano
    C  --storage columnar --execution vectorized
    (+ --no-optimize as a fourth harness "mode", which is C with the optimizer off)

---

## Part A — audit of `narrowRows` (fix round 2, `70570dc` / `074aeb7`)

### A.0 What the change actually is

`src/planner/planner.cc:188-237` defines a plan-time lambda `narrowRows`, applied at
three sites — the FROM scan (`:291`), the self-join right scan (`:317`), and the
ordinary join right scan (`:324`). Every one is a ROW-storage site; the columnar
sites are untouched.

### A.1 Where are the narrowed rows materialised, and what is their lifetime?

**They are not a pointer into storage, and there is no aliasing.** The chain, verbatim:

- `main.cc:441` `table_rows[tname] = CSVLoader::load(...)` — the map is a LOCAL of the
  per-query loop body (`main.cc:392`), reloaded from CSV for every `--query`. Nothing
  outlives the iteration.
- `Planner::plan` takes `std::unordered_map<std::string, std::vector<Row>> table_rows`
  **by value** (`planner.h`), so the planner already owns a distinct map.
- `narrowRows(std::vector<Row> rows, ...)` takes its vector **by value** and is called
  with `std::move(table_rows.at(...))`, so the vector is moved in, not copied. It
  returns the same named parameter by value, which C++ implicitly treats as an rvalue,
  so that is a move too.
- `SeqScanNode(std::string, std::vector<Row> rows, Schema)` (`plan_nodes.cc:46`) stores
  `rows_(std::move(rows))`. **The scan node owns the vector outright.**
- `SeqScanNode::next()` returns `&rows_[cursor_]` — a pointer into the node's OWN
  member, whose lifetime is the plan's. Same as before the fix; the fix changed the
  vector's *width*, not who holds it.

So the "narrowed backing" question has no teeth: there is no separate backing. The
old path did not point into shared storage either — `table_rows` was always moved in.

**Cost.** Per narrowed table, one pass over N rows; each row allocates a new
`std::vector<Value>` of the kept width, `std::move`s the kept `Value`s into it (so
STRING payloads are moved, never copied), then destroys the old vector. That is
2N allocator operations at plan time, and it is charged to the plan timer
(`main.cc:653` brackets `Planner::plan`). It is skipped entirely when
`narrowed.size() == full.size()` — `SELECT *`, and any statement with
`has_subquery` still set (`buildScanSchema` returns `full_schema` for both).
Measured below (A.6).

**The early-return is sound, and for a non-obvious reason.** The guard is on SIZE,
not identity — `if (narrowed.size() == full.size()) return rows;`. That is only safe
because `narrowSchema` (`logical_plan.cc:72`) builds the narrowed schema by walking
`full` **in order** and keeping a subset, so equal size implies equal content in equal
order. It is not a free-standing schema. If a future change ever gives
`buildScanSchema` a reordering or a synthesising step, this guard becomes a silent
wrong-column bug — the rows would be left wide while a same-width, differently-ordered
schema is handed to the scan. Noted as L-3 below; not a finding today.

### A.2 Does any consumer still index by catalog position?

Searched every consumer of a scanned row on the Volcano path. None does.

- `SeqScanNode::next()` row path — returns the row whole; columnar path reconstructs
  by NAME from `schema_` (`plan_nodes.cc:73-75`), so both are schema-relative.
- `Evaluator` / `ColumnRef` — resolves through `Schema::indexOf(name)` /
  `indexOf(name, relation_slot)` against the *node's* `outputSchema()`.
- `HashJoinNode` — key indices resolved by name against `left_->outputSchema()` /
  `right_->outputSchema()`; `build_width_ = right_->outputSchema().size()`
  (`plan_nodes.cc:675`), so LEFT-JOIN null extension widens by the NARROWED build
  width, not the catalog width. Verified at runtime (A.4).
- `sort_comparator` — `min(schema.size(), row.size())`; both now the narrowed width.
- `preserved_slots` (`planner.cc:283`) and `pruningHintForPreservedSide` — derived from
  `scan_schema`, which was already true before the fix.
- `TableStats::compute(table_rows[name], tmeta.schema)` (`main.cc:445`) runs at LOAD
  time, before any planning, on full-width rows. Unaffected.
- `grep` for `catalog`/`getTable` in `src/planner/plan_nodes.cc` and
  `src/execution/evaluator.cc`: **zero hits**. No execution-layer node can see the
  catalog schema at all, which is the structural reason this class is closed rather
  than merely absent.

### A.3 `hidden`, `SELECT *`, pushdown, pruning hints

- **`hidden`**: no interaction, structurally. `narrowSchema` copies whole `ColumnDef`s
  out of the CATALOG schema, and `Catalog::Catalog` never sets `hidden` (it is
  default-`false` in `logical_plan.h:37` / `schema.h:39`). The two producers of
  `hidden` are `extractAggregates` (aggregate output schemas, above the scan) and
  `subquery_decorrelation.cc:655` (a lowered correlated scalar's synthetic relation) —
  and the latter cannot reach this path at all, since `Planner::plan` refuses every
  correlated subquery at `planner.cc:75-79`. `planner.cc:431` keeps the `if
  (col.hidden) continue;` test in the Volcano star expansion anyway, matching
  `build()`; that is the right call and it is unreachable-by-refusal, not dead.
- **`SELECT *`**: `buildScanSchema` returns `full_schema` on `select_star`, so
  `narrowRows` early-returns and the row path is byte-identical to pre-fix. Covered by
  five `SELECT *` controls in the batch below, including `SELECT *` over a join.
- **Predicate pushdown / join enumeration / cardinality estimation**: vectorized-only
  (`main.cc:566-588`). They never see a row-storage scan.
- **Chunk pruning**: columnar-only. The row `SeqScanNode` constructor has no
  `pruning_where` parameter at all, so a row scan cannot prune and cannot mis-name a
  relation. The hint's `preserved_slots` derivation is shared and unchanged.
- **Join key extraction**: `classifyJoinCondition` runs on the AST before any scan is
  built and yields column NAMES; `buildScanSchema` collects from `stmt.joins[].condition`
  (`logical_plan.cc:331`), so a key can never be narrowed away.
- **Zero-width scans**: `SELECT COUNT(*) FROM laps` narrows to a **0-column** schema and
  10 000 zero-width rows. This is new for the row leg (columnar has done it since
  Week 30). Verified: all cells return 10000. `SELECT 1 FROM drivers LIMIT 5` likewise.

### A.4 Runtime verification — 106 queries, three cells + `--no-optimize`

`scratchpad`-local driver, byte-exact positional comparison of `--format tsv --no-cache`
output (stdout+stderr, columnar encoding banner filtered), across:

    A  --storage row      --execution volcano
    B  --storage columnar --execution volcano
    C  --storage columnar --execution vectorized
    D  C + --no-optimize

Batch 1 (69 queries) — zero-width scans, raw-join-row sorts with LIMIT, `SELECT *`
controls, self-joins, LEFT JOINs with null extension, shared column names across both
relations, aggregates, expression GROUP BY keys, CASE/SUBSTRING/LIKE/IN/BETWEEN,
ordinals and aliases, DISTINCT, uncorrelated scalar subqueries, and LIMITs at
1023/1024/1025 (`CHUNK_SIZE` boundaries): **0 divergent.**

Batch 2 (37 queries) — deliberately adversarial for THIS fix. Each one has an ORDER BY
key that ties, a LIMIT cutting inside the tie, and a shape where the pre-fix wide leg's
narrowed-away columns would have decided the cut *lexicographically before* the columns
the narrow leg consults. The commit message's own example is benign because
`drivers.driver_id` is column 0 in both legs; the discriminating shape is one relation's
tie broken *within* a repeated key, e.g.

    SELECT d.team, l.speed FROM drivers d JOIN laps l ON d.driver_id = l.driver_id
      ORDER BY d.team LIMIT 5

    merged narrow  [d.driver_id, d.team, l.driver_id, l.team, l.speed]
    merged wide    [d.driver_id, d.name, d.nationality, d.team, d.age,
                    l.lap_id, l.driver_id, l.team, l.speed, l.sector_1, ...]

Within one driver, `d.driver_id` ties; the wide leg then reaches `l.lap_id` (unique)
and cuts by insertion order, while the narrow leg reaches `l.team`/`l.speed` and cuts by
value — and `l.speed` IS projected, so the difference is visible. 37 such shapes
(inner, LEFT, self-join, DESC, under DISTINCT/GROUP BY/HAVING, single-relation):
**0 divergent.**

Total **106 queries x 4 invocations, 0 divergences.** The fix holds on the shapes that
were previously luck-dependent, not merely on the one the commit measured.

### A.5 The one live coupling the fix introduced (documented, not a defect)

`narrowRows` resolves each kept column with `full.indexOf(c.name)` — **first match**.
`narrowSchema` keeps EVERY column whose name is in `required`. So if a catalog schema
ever held two columns of the same name, `keep` would contain the same index twice and
the second `std::move(r[i])` would move an already-moved-from `Value`.

That shape is exactly S-8, and fix round 2 refused it at the source
(`catalog.cc:49-55`, verified live: `catalog: table 't': column 'k' is declared twice`
in every cell). **What is new is that the refusal is now load-bearing for a second
reason.** Its comment justifies itself entirely by the COLUMNAR interleaving bug; after
`70570dc` it is also the only thing keeping `narrowRows` from double-moving on the ROW
path. Anyone who later decides "this only affects columnar, relax it for row storage"
would reintroduce a different bug in the leg they thought was safe. Recorded as L-1.


### A.6 The cost — **S-9, MEDIUM: `narrowRows` is an O(N x width) plan-time pass on the row leg, and every harness this project owns is blind to it**

The commit prices the change as "one pass over the table's rows at plan time, moving
Values rather than copying them ... acceptable at this project's scale". That was
asserted, not measured. Measured, on a **Release** build (`-O3 -DNDEBUG`, configured
into the scratchpad so `build/` was untouched), TPC-H sf0.1, `lineitem` = 600 572 rows
x 16 columns, `--storage row --execution volcano --explain-analyze --no-cache`:

| query | Plan | Execution |
|---|---|---|
| `SELECT * FROM lineitem LIMIT 1` (narrowRows early-returns) | **43.5 µs** | 16.6 µs |
| `SELECT l_quantity FROM lineitem LIMIT 1` (keeps 1 of 16) | **103 677 µs** | 15.6 µs |
| `SELECT <15 named cols> FROM lineitem LIMIT 1` (keeps 15 of 16) | **110 540 µs** | 11.2 µs |
| `SELECT COUNT(*) FROM lineitem WHERE l_quantity > 30` | **105 056 µs** | 184 910 µs |

The same four on the Debug binary in `build/` (which is what every harness actually
runs): 108.9 µs / 1 015 322 µs / 2 561 656 µs / 1 021 073 µs.

Read it plainly:

- Planning a row-storage query over `lineitem` went from **43 µs to 104 ms** — a factor
  of **~2 400** in Release, ~9 300 in Debug. It is not a constant: it is O(rows x kept
  width), so it grows with the data and it grows *with the number of columns you keep*
  (15-of-16 costs more than 1-of-16 — this is pure overhead, not saved work).
- On `SELECT ... LIMIT 1` the engine now narrows all 600 572 rows to serve **one**.
  Planning is 6 600x execution. That is a Volcano pipeline paying a full
  materialisation before the first `next()`.
- On the full-scan aggregate, plan is **57% of execution** — the query is ~1.6x slower
  end to end.
- `--explain` pays it in full: `Planner::plan` runs before the `if (args.explain)`
  branch (`main.cc:653-661`), so printing a plan for a row-storage query now rewrites
  the entire table first.

**Why nothing caught it, and this is the part that matters more than the number:**

- `python_tools/benchmark.py:67-88` — `run_once` calls the binary with
  `--explain-analyze` and greps **`Execution:` only**. Plan time is not parsed, not
  summed, not reported. Its `MODES` list *does* include `("row", ["--storage","row"])`,
  so row storage is benchmarked — with the one number that cannot see this.
- `python_tools/run_tpch.py:69-74` runs `row-volcano` for **correctness**, against
  recorded answers. No timing.
- The gate's "TPC-H baseline unchanged" is a correctness baseline. It is true and it is
  silent about this.

So the harness suite can report the row leg getting *faster* — narrower rows are
genuinely cheaper to copy through Sort/HashJoin — while total time per query got
worse, because the work moved from the timer that is read into the timer that is not.
That is a measurement-integrity problem, not just a slow pass.

**It is fixable without giving back the correctness fix, and cheaply.** The narrowing
does not have to happen at plan time or all at once. `SeqScanNode` already has the
machinery on its OTHER path: the columnar branch reconstructs one row per `next()` into
a reused `reconstructed_row_` member (`plan_nodes.cc:71-76`). The row branch can do the
same — keep the wide `rows_`, store the `keep` index vector the lambda already computes,
and in `next()` fill `reconstructed_row_` from `rows_[cursor_][keep[i]]`. Same narrowed
output schema, same tie-break fix, O(1) extra memory, no plan-time pass, and it deletes
the double-move hazard of A.5 as a side effect (an index used twice is now a copy, not
a move). It also makes the two `SeqScanNode` paths symmetric, which is the shape the
rest of this file keeps asking for.

Severity **MEDIUM**: no wrong answer, and row storage is the correctness baseline rather
than the performance path. It is not LOW because the regression is unbounded in the data
size, it is paid by `--explain`, and the project's own instruments are structurally
unable to see it — which is how it survived a green gate.
