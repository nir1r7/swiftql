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

#### A.6b Before/after, measured — and the benchmark moves the WRONG way

To stop guessing, `6748cfc` (the commit immediately before the planner half of the fix)
was checked out into a detached worktree under `scratchpad/` and built Release with the
same compiler and flags. Nothing in the working tree was touched; the worktree was
removed afterwards. Both binaries, `--storage row --execution volcano
--explain-analyze --no-cache`, TPC-H sf0.1:

| query | PRE plan | PRE exec | PRE total | POST plan | POST exec | POST total | total |
|---|---|---|---|---|---|---|---|
| `SELECT l_quantity FROM lineitem LIMIT 1` | 64 µs | 7 µs | **71 µs** | 109 312 µs | 63 µs | **109 375 µs** | **1540x slower** |
| `SELECT COUNT(*) FROM lineitem WHERE l_quantity > 30` | 49 µs | 234 890 µs | **234 939 µs** | 104 160 µs | 177 396 µs | **281 556 µs** | 1.20x slower |
| `SELECT l_returnflag, l_linestatus, SUM(l_quantity) ... GROUP BY ... ORDER BY ...` | 55 µs | 271 837 µs | **271 892 µs** | 114 948 µs | 224 150 µs | **339 098 µs** | 1.25x slower |
| `SELECT l_orderkey FROM lineitem ORDER BY l_orderkey LIMIT 5` | 40 µs | 1 748 114 µs | **1 748 154 µs** | 104 703 µs | 892 029 µs | **996 732 µs** | 1.75x **faster** |

Two things fall out of this table that neither the commit nor the gate could have known.

1. **The change is not uniformly a cost.** Where the plan has a sort or a join above the
   scan, narrower rows are genuinely cheaper to carry and the narrowing pays for itself
   (row 4: 1.75x faster end to end). Where the plan is a bare scan/aggregate, or where a
   `LIMIT` means almost nothing is read, it is pure loss (rows 1-3). It is a *trade*, and
   it was landed as if it were free.

2. **In all four rows `benchmark.py`'s number moves in the flattering direction.** Its
   `run_once` reads `Execution:` only, so it would report 24% faster, 17% faster, 51%
   faster and 9x faster respectively — including for the three queries that got *slower*.
   The regression is not merely unmeasured; the instrument the project would reach for
   actively reports the opposite sign. That is the substance of S-9, and it is now
   measured rather than argued.

---

## Part B — what passes 1 and 2 missed

### B.1 Chunking, on data built to break it — 182 more queries, 0 divergences

Row storage never prunes and has no chunks; columnar does both. So any chunk wrongly
skipped, any group lost at a boundary, any accumulator reset per chunk shows up as a
row-vs-columnar difference. Two batches:

**TPC-H sf0.01 (56 queries).** `lineitem` is 60 144 rows = 8 chunks, and pruning
genuinely fires here (`chunks_skipped=7/8` on `l_orderkey < 100`, confirmed with
`--explain-analyze`). Covered: every comparison operator against a zone map (`=`, `<`,
`>`, `<=`, `>=`) including predicates that prune ALL chunks and predicates that prune
NONE; conjunctions and disjunctions; INT / DOUBLE / STRING / date-shaped-STRING zone
maps; aggregates accumulating over all 8 chunks (`SUM`, `AVG`, `MIN`, `MAX`,
`COUNT(DISTINCT)`); groups whose members span every chunk and groups confined to the
last one; `LIMIT` at 8191/8192/8193/16385; `LIMIT` combined with pruning that skips the
FIRST chunks; single joins with the predicate on each side; LEFT JOINs; and
`SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax))` for float accumulation order.
**0 divergent.**

**Synthetic boundary tables (126 queries).** Row counts 0, 1, 8192, 8193, 16384, 16385
plus a 20 000-row table that is a SINGLE RLE run spanning three chunks — the shapes real
data does not give you. Each with `COUNT`/`SUM`/`MIN`/`MAX`/`AVG`, `COUNT(DISTINCT)`
over INT and STRING, `GROUP BY` on a low-cardinality RLE column and on a dictionary
column, `ORDER BY ... LIMIT` both directions, predicates straddling row 8192,
`SELECT *`, and joins/LEFT JOINs with the **empty** relation on each side.
**0 divergent.**

The empty relation was also checked for CORRECTNESS, not just agreement, since nothing
in the shipped suites has one: `COUNT(*)`=0, `SUM/MIN/MAX/AVG`=NULL, `COUNT(DISTINCT)`=0,
`GROUP BY` over it = 0 rows, and `t1 LEFT JOIN t0` = 1 row with `COUNT(b.k)`=0 — all
byte-identical to SQLite's answers.

### B.2 The pruning-hint-names-the-wrong-relation class — closed structurally

The class that "has bitten once" is a conjunct whose column name exists in the scanned
table but whose reference belongs to a DIFFERENT relation, matched against zone maps by
name. `chunk_pruner.h::collectSimplePredicates` gates on
`col->id.isLocal() && col->id.localSlot(...) < 1`, and `ColumnId::isLocal()` is
`level_ == 0` (`column_id.h:49`). So:

- a JOIN-side ref carries schema slot 1 and is dropped — the FROM scan cannot be pruned
  by the other table's `team`/`driver_id`;
- a CORRELATED ref carries level >= 1 and fails `isLocal()` before its slot is ever
  read. The header's Week 33 paragraph says this is now REACHABLE (the Validator refusal
  is gone) and that it DECLINES rather than throws. That is what the code does, and
  `localSlot()` would throw loudly rather than silently mis-index if the order of the
  two tests were ever swapped.

Two shapes still worth naming, both benign and both deliberate: a literal on the LEFT
(`WHERE 350 < l.speed`) is not collected at all, and a NULL literal (from a
zero-row materialized scalar subquery) is declined explicitly. Both cost pruning, never
correctness.

### B.3 Loading and value fidelity — the divergence surface is thinner than it looks

`CSVLoader::load` is the ONLY parse, and `CSVToColumnar::convert(rows, schema)` is built
from its output, so a parsing difference between the two storage modes is not
representable. What IS representable is a conversion/encoding difference, and the
encodings were read for it:

- **DOUBLE is never encoded** (`csv_to_columnar.cc:83`, `case TypeId::DOUBLE: break;`) —
  stored as a raw `std::vector<double>`. So the RLE equality hazard that would collapse
  `0.0` and `-0.0` into one run cannot reach a DOUBLE column. INT RLE compares
  `int64_t` exactly; STRING dictionary compares `std::string` exactly.
- **NaN / infinity are accepted by the loader** — `std::stod("nan")` and `stod("inf")`
  consume the whole field, so `parseField` returns them without complaint. They are the
  same `double` in both storages. Zone maps built over them degrade safely:
  `v < mn` / `v > mx` are both false for NaN, so a NaN either becomes an unusable
  min/max pair (and `canSkipChunk` then skips nothing) or is invisible to a min/max that
  a real value set — and in both cases the predicate it would have been pruned by is
  itself false for NaN. No wrong skip is reachable from this.
- **Ragged rows are refused** with file+line+expected/got (`csv_loader.cc:43-51`), and
  `parseField` requires FULL consumption for INT and DOUBLE, so a mistyped catalog
  column is loud rather than silently truncating (`"1996-01-02"` as DOUBLE throws
  instead of yielding 1996.0). Both properties are shared by both storage modes because
  the loader is shared.
- **`getValue` bounds**: the three raw-vector variants use `.at()`, but
  `DictionaryEncoder::decode` (`dict[codes[row_idx]]`) and `RLEColumn::get` are
  unchecked. Reachable only with `row_idx >= num_rows`, and `num_rows` is set from
  `rows.size()` with every column populated in the same pass, so the widths cannot
  disagree. Noted, not a finding.

This is consistent with, and does not re-derive, the standing conclusion that NULL
*representation* cannot be differentially tested because `ColumnArray` has no validity
concept.

### B.4 Scan order

Both storage modes emit in file order: the row path walks `rows_[cursor_]` upward, the
columnar path walks `row_idx` upward and reconstructs. Pruning does not perturb that —
it advances `cursor_` past a whole chunk, so the relative order of what survives is
unchanged. The sharpest test of this is a `LIMIT` with **no** `ORDER BY` over a query
where columnar prunes and row does not:

    SELECT l_orderkey FROM lineitem WHERE l_orderkey > 58000 LIMIT 3

Row storage starts at row 0 and filters; columnar skips chunks 0-6 and starts inside
chunk 7. Both must nevertheless return the same three rows, and do. Same for
`SELECT l_orderkey, l_quantity ... WHERE l_orderkey > 58000 ORDER BY ... LIMIT 7`,
`SELECT team FROM laps LIMIT 1025` and `SELECT k FROM t8193 LIMIT 8192`.

### B.5 Randomized differencing across the three cells — 1 200 queries, 0 divergences

`python_tools/random_diff.py` is vectorized-only (`VEC = ["--execution","vectorized",
"--storage","columnar"]`, line 65), so **the row leg has never been randomly
differenced** — every row-vs-columnar claim in this project rests on hand-written
entries. Closed here for this pass: a seeded generator emitting shapes `Planner::plan`
can actually run (<=1 join, no derived tables, no IN/correlated subqueries) — inner and
LEFT joins in both orders, ON residuals, conjunctive and disjunctive WHERE, GROUP BY with
HAVING, scalar aggregates, DISTINCT, and **partial `ORDER BY` + `LIMIT`**, which is the
shape whose cut SQL leaves unspecified and this project nonetheless requires to agree.

Three seeds x 400 queries, byte-exact across all four invocations: **0 divergent.**
Not vacuous — on seed 20260808, 284 of 400 return non-empty results, 64 return zero
rows, and 52 are refusals whose message must also match in all four cells.

Grand total for this pass: **1 588 query x cell comparisons, 0 divergences.**

### B.6 Does the scan-schema fix make the Week 37 `VecScanNode` item easier or harder?

**Slightly harder as written, and easier if S-9 is fixed first.** Both halves matter.

Harder, because the fix created a new obligation and hid the tool that discharges it.
`VecScanNode` reads its columns **by name** out of `ColumnarTable`
(`vec_scan_node.cc:50-58`), so it is width-agnostic today. A row-backed constructor
cannot be: it must index `rows_[r][i]` positionally, and the logical layer already hands
its SCAN a NARROWED schema (`blockOutputSchema` -> `buildScanSchema`,
`logical_plan.cc:512`). So a row-backed `VecScanNode` needs exactly the same
rows-must-match-the-narrowed-schema step `Planner::plan` now performs — and `narrowRows`
is a **local lambda inside `Planner::plan`**, unreachable from
`vectorized_plan_builder.cc`. Week 37 therefore either duplicates it (the two-paths
drift this codebase keeps paying for, and pass 2's own estimate says "no planner
change") or first hoists it into `logical_plan.{h,cc}` beside `narrowSchema` — about
15 extra lines, moving pass 2's 70-90 to roughly **85-105**.

Easier, if S-9's fix lands first. If the narrowing becomes a `keep_` index vector
consumed inside the scan node rather than a plan-time rewrite of the data, then the
row-backed `VecScanNode` fill loop pass 2 already sized ("walk `rows_[start..start+count)`
and push `row[i]`") becomes `row[keep_[i]]` — **one indirection in a loop that has to be
written anyway**, no shared helper, no hoist, no drift risk. The two items are
complementary and should be done in that order.

### B.7 S-8 re-checked, and the level above it

`S-8` (duplicate COLUMN name in `catalog.json`, silently wrong in columnar only) is
**fixed and live**. `catalog.cc:49-55` refuses it at load, verified in every cell:

    Error: catalog: table 't': column 'k' is declared twice; give one of them a distinct name

against `t(k INT, k INT, b INT)` over `k,k,b / 1,100,7 / ...` — the exact input pass 2
used. See A.5 for the second, undocumented reason that refusal is now load-bearing.

**The level above it — `catalog.cc:90` `tables_.emplace(meta.name, ...)` — is
reachable, and it is NOT a storage-seam defect.** Measured rather than reasoned. With

    {"tables":[{"name":"t","file":"data/t1.csv", cols k,b},
               {"name":"t","file":"data/t2.csv", cols k,b}]}

`std::unordered_map::emplace` does not overwrite, so the FIRST entry wins and the second
is discarded whole — file, schema and all. `SELECT k,b FROM t` returns `t1.csv`'s three
rows in **all three cells**, identically; and with a wider second entry,
`SELECT c FROM t` reports `SELECT: column not found: 'c'` in all three. So:

- it is a **silent wrong answer at the catalog layer** (you queried the table you
  declared second and got the one you declared first, with no diagnostic);
- it is **not** a row-vs-columnar divergence, and cannot become one: both storage modes
  read the same single `TableMetadata` and `main.cc` builds the columnar table from the
  rows that metadata loaded. There is no second implementation to disagree with.

That is the material difference from S-8, which WAS a divergence. Severity **LOW**, and
it belongs to catalog input validation rather than to this seam. The fix is four lines
in the same loop that now refuses duplicate columns — `if (tables_.count(name)) throw` —
and it is strictly cheaper than the S-8 fix already merged beside it.

---

## Findings

### S-9 — MEDIUM — `narrowRows` is an O(rows x width) plan-time pass on the row leg, and every instrument this project owns reports it with the wrong sign

Concrete shape, TPC-H sf0.1, `--storage row --execution volcano`, Release build:

    SELECT l_quantity FROM lineitem LIMIT 1
      pre  6748cfc   Plan 64 µs      Execution 7 µs      total 71 µs
      post 922ca15   Plan 109 312 µs Execution 63 µs     total 109 375 µs   (1540x)

Full table and the two-sided analysis at A.6/A.6b. `benchmark.py` reports this query
9x FASTER, because `run_once` greps `Execution:` and nothing else; three of the four
measured queries got slower while all four of its numbers improved. Not a correctness
defect and the answers are right in every cell — but the regression is unbounded in the
data size, `--explain` pays it in full, and the project's own harnesses are structurally
unable to see it, which is exactly how it passed a green gate. Fix at A.6: move the
narrowing into `SeqScanNode::next()` as a `keep_` index vector, mirroring the columnar
branch's existing `reconstructed_row_`. This also makes the Week 37 item cheaper (B.6).

### L-1 — LOW — the S-8 refusal is now load-bearing for a second reason its comment does not state

`narrowRows` resolves kept columns with `full.indexOf(c.name)` (first match) while
`narrowSchema` keeps EVERY same-named column, so a duplicate column name would put one
index into `keep` twice and the second `std::move(r[i])` would move an already-moved
`Value`. `catalog.cc:49-55` makes that unreachable — but its 20-line comment justifies
itself entirely by the COLUMNAR interleaving bug. A future reader relaxing it "because
row storage was never affected" would break the row path. One sentence in that comment
closes it. (A.5)

### L-2 — LOW — duplicate TABLE name in `catalog.json` silently keeps the first, in every mode

Reachable, measured, and consistent across all three cells — so it is a catalog
input-validation gap, not a storage divergence. `tables_.emplace` does not overwrite;
the second entry's file and schema are discarded with no diagnostic. Four lines beside
the S-8 refusal fix it. (B.7)

### L-3 — LOW — `narrowRows`' early return is correct only because of an unstated property of `narrowSchema`

`if (narrowed.size() == full.size()) return rows;` leaves the rows WIDE. That is safe
only because `narrowSchema` builds the narrowed schema as an order-preserving subset of
`full`, so equal size implies identity. Nothing states or tests that coupling, and it is
in a different file. Give `buildScanSchema` any reordering or synthesising step and this
becomes a silent wrong-column bug on the row leg. Comparing the column NAMES rather than
the sizes would cost nothing and remove the dependency. (A.1)

### Already-recorded items — one is marginally sharper than recorded

`development.md`, `week-36-plan.md`'s half-run verification claim, and `run_tpch.py`'s
dead-and-loose refusal pins are all queued and none is worse than recorded, with one
detail worth a line: of `VOLCANO_BOUNDARIES`' six entries, the two IN-subquery pins do
not merely duplicate the catch-all, **they never matched their intended message at all**.
`planner.cc:72` emits "IN subqueries are lowered to a semi-join..."; the pins read
"IN (subquery) is lowered to a semi-join" and "IN subquery", and neither is a substring
of it (`subqueries` vs `subquery`). Verified by string comparison. Every Volcano IN
refusal in the TPC-H matrix is therefore classified by the catch-all
`"not supported on the Volcano path"`. So the item is not "specific pins made redundant
by a catch-all" but "specific pins that were never live, masked by a catch-all" —
same fix, slightly worse provenance.

---

## Summary

| severity | count |
|---|---|
| BLOCKER | 0 |
| HIGH | 0 |
| MEDIUM | 1 (S-9) |
| LOW | 3 (L-1, L-2, L-3) |

Evidence base for this pass: **1 588 query x cell comparisons across all three real
storage x engine cells plus `--no-optimize`, 0 divergences** — 106 hand-written
(including 37 built specifically to break the new tie-break symmetry), 182 chunking and
boundary shapes (TPC-H sf0.01 plus synthetic tables at 0/1/8192/8193/16384/16385 rows and
a single RLE run spanning three chunks), and 1 200 randomized, which is the first
randomized differencing the ROW leg has ever had.

**Verdict: the storage seam is clean. `narrowRows` is correct — it moves rather than
aliases, no consumer indexes by catalog position, and the tie-break symmetry it bought
holds on the adversarial shapes the original fix did not test — but it was landed as
free and it is not: it is an unbounded plan-time cost that this project's benchmark
reports as a speed-up. No blockers; the audit ends here.**
