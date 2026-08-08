# Seam audit — storage, PASS 2

Branch `claude/phase5-week26-qomtkb`, HEAD `ee9c9d7`.
Scope: settle S-0 from pass 1; then hunt the seam fresh (value fidelity, chunking,
scan order, loading, catalog).

Status: IN PROGRESS (written incrementally; see summary block at end).

## Part A — the ruling on S-0

### A.1 There are THREE storage×engine cells, not four

Read from source, not from the mode labels:

| storage | engine | reachable? | where decided |
|---|---|---|---|
| row | volcano | yes | default |
| columnar | volcano | yes | `main.cc:454` |
| **row** | **vectorized** | **NEVER — refused for every query** | `main.cc:547-551` `"Error: --execution vectorized requires --storage columnar"` |
| columnar | vectorized | yes | — |

The harness's "four modes" (`run_tpch.py:69`, `compare_against_sqlite.py:2406-2421`)
are `row-volcano`, `col-volcano`, `col-vec`, `col-vec-noopt`. The fourth is an
**optimizer flag**, not a storage or engine cell. So:

- the vectorized engine has exactly ONE storage backend;
- the row storage format has exactly ONE engine;
- **the only row-vs-columnar comparison that exists anywhere in this project is
  Volcano-row vs Volcano-columnar.**

### A.2 The row/columnar delta is confined to ONE node

`Planner::plan` builds the *same* operator tree in both storage modes. Diffing
the two branches line by line, the storage mode changes exactly three things:

1. `planner.cc:237-241` — which `SeqScanNode` constructor is used (row vector vs
   `ColumnarTable` + reconstruction loop, `plan_nodes.cc:61-82`);
2. the chunk-pruning hint, which is passed **only** on the columnar constructor
   (`planner.cc:238`) — the row constructor has no such parameter;
3. the scan's **schema width**: columnar gets `scan_schema` (narrowed by
   `buildScanSchema`), row gets `meta.schema` (the full catalog schema).
   `planner.cc:186` vs `planner.cc:240`.

Everything above the scan consumes `Row` and is byte-identical. **The storage
seam on the Volcano path IS `SeqScanNode`.** There is no storage seam at all on
the vectorized path, because only one storage format reaches it.

### A.3 Volcano's refusals, enumerated from source

`Planner::plan` has exactly four refusals, and nothing else is refused there:

| # | cite | shape refused |
|---|---|---|
| 1 | `planner.cc:23` | `stmt.joins.size() > 1` — three or more relations |
| 2 | `planner.cc:70` | any `IN (subquery)` in WHERE/HAVING, at any depth |
| 3 | `planner.cc:75` | any **correlated** subquery |
| 4 | `planner.cc:118` | a derived table in FROM **or in any JOIN** |

Note what is NOT refused: an **uncorrelated** scalar or `EXISTS` subquery. Those
are materialized above both engines (`main.cc:500-545`) and reach `Planner::plan`
as constants, so they execute in both storage modes.

### A.4 S-0 is OVERSTATED — the oracle DOES span Phase 5 shapes

Phase 5 is weeks 24-37 (`README.md:742`). S-0 says the row/columnar oracle spans
*no* Phase 5 plan shape. That is false. `QUERIES` — the list
`compare_against_sqlite.py:2406-2411` runs in row-volcano AND col-volcano — is
composed at `compare_against_sqlite.py:1850-1861` and contains, by count:

| suite | Phase 5 week | queries in BOTH storage modes |
|---|---|---|
| `WEEK24_EXPRESSION_QUERIES` | 24 | 2 |
| `WEEK25_PREDICATE_QUERIES` | 25 | 16 |
| `WEEK25_CASE_QUERIES` | 25 | 7 |
| `WEEK25_SUBSTRING_QUERIES` | 25 | 4 |
| `WEEK25_JOIN_QUERIES` | 25 | 2 |
| `WEEK26_ALIAS_SHADOW_QUERIES` | 26 | 2 |
| `WEEK27_JOIN_QUERIES` | 27 | 7 |
| `WEEK27_KEY_ENCODING_QUERIES` | 27 | 5 |
| `WEEK29_OUTER_JOIN_QUERIES` | 29 | 17 |
| `WEEK30_ALIAS_REBIND_QUERIES` | 30 | 3 |
| `WEEK31_SUBQUERY_QUERIES` | 31 | 17 |
| `WEEK34_DISTINCT_AGG_QUERIES` | 34 | 7 |
| **total** | | **89** |

plus `ENGINE_AGREEMENT_QUERIES` (2), run across `SWEEP_MODES`
(`compare_against_sqlite.py:2219-2223`) which is row-volcano / col-volcano /
col-vec.

**The one that matters most for THIS seam is Week 29.** `pruningHintForPreservedSide`
(`predicate_pushdown.cc:137`) is the single Phase 5 change that exists *only* to
protect chunk pruning, and `Planner::plan:233` is one of its two call sites. All
17 `WEEK29_OUTER_JOIN_QUERIES` run in both storage modes, including
`... drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id WHERE l.season = 2024`
— the exact shape that routes a null-supplying-side conjunct at the preserved
side's scan as a hint. So the storage-critical Phase 5 code path IS under a
differential oracle.

### A.5 What S-0 is actually true about, restated exactly

The four shapes at A.3 have **one** executable cell (col-vec), so for those there
is no second storage implementation to compare against. But:

- those four shapes are refused *before* any scan is built, so **none of them can
  reach `SeqScanNode`** — the one node the two storage modes differ in;
- the columnar storage FORMAT (`ColumnarTable`, RLE, dictionary, zone maps,
  `getValue`) is exercised by 100% of those shapes through `VecScanNode`;
- the row storage format has no chunks, no encodings, no zone maps and no
  pruning — it is `std::vector<Row>` straight from the loader, indexed
  (`plan_nodes.cc:84-86`). There is no row-storage machinery a plan shape can break.

So the correct statement is:

> For multi-way joins, derived tables, `IN`-semi/anti joins and correlated
> subqueries there is no cross-storage oracle **and no cross-storage code path
> either**: those shapes never reach the node where the two storage modes differ.
> The uncovered risk is not "row storage is untested against Phase 5", it is
> "`VecScanNode`'s pruning under those shapes has no differential net" — which is
> narrower, and is exactly what pass 1's S-1/S-2/S-3 hand-verified.

### A.6 Ranking

**S-0 = LOW.** Not informational-only (it is a real, permanent coverage gap that a
future week can widen), but not MEDIUM or above, because:

- no wrong answer is reachable through it today;
- the surface it fails to cover (`SeqScanNode` + `ChunkPruner` via
  `Planner::plan`) is *unreachable* from every shape in the gap;
- the surface it DOES leave netless (`VecScanNode` pruning on multi-way/derived/
  semi-anti/correlated) has one guard per route and pass 1 exercised each with an
  input that would have exposed its absence.

I disagree with the brief's framing that S-0 means "an entire storage mode is
untested against every feature Phase 5 built". Row storage is a `std::vector<Row>`
with an index; there is nothing in it for a Phase 5 feature to be tested *against*.

### A.7 The cheapest change that creates a shared storage surface — sized

Not a harness change: nothing in Python can create the surface, because the
refusal is `main.cc:548` in the engine.

**Give `VecScanNode` a row-backed constructor and delete `main.cc:548`.**

- `vec_scan_node.h` — a second ctor `VecScanNode(std::string, std::vector<Row>, Schema)`
  plus a `std::vector<Row> rows_` member and a `use_columnar_` flag (mirrors
  `SeqScanNode`, which already has exactly this pair).
- `vec_scan_node.cc` — in `nextChunk()`, when row-backed: skip the pruning loop
  entirely (no zone maps), then fill each `ColumnVector` by walking
  `rows_[start..start+count)` and pushing `row[i]` via `appendColumnValue`.
  ~35 lines; the batching, `sel`/`filter_applied` reset and stats code is shared.
- `vectorized_plan_builder.{h,cc}` — `Lowering` carries a second map
  `std::unordered_map<std::string, std::vector<Row>>` and the `SCAN` case picks
  the ctor. ~15 lines, plus the same `scan_uses` copy-vs-move rule.
- `main.cc` — delete the refusal; do not clear `table_rows` when storage is row;
  pass the right map to `VectorizedPlanBuilder::build`. ~10 lines.
- harness — add `("row-vec", ["--execution","vectorized","--storage","row"])` to
  `SWEEP_MODES`, `MODES` in `run_tpch.py`, and the vec-only suites.

Total ≈ 70-90 lines, no planner change, no refusal relaxed, no new semantics.

What it buys, and this is the point: **every** Phase 5 shape then runs on both
storage formats, and the row leg has *no zone maps*, so the comparison is
literally a pruning-on vs pruning-off oracle — the exact differential that
catches a wrong prune, on the exact shapes that have none today. It also
subsumes the `--storage-stats` / shared-table-representation item `main.cc:515`
already defers to Week 37.

The alternatives are all more expensive: lifting refusal #1 means a second
multi-way production in a path with no logical layer (`planner.cc:98-114`
measured this and declined); lifting #2/#3 means `JoinSemantics` in
`hash_join_node.cc` and buys 2 TPC-H cells by the same measurement.

---

## Part B — the seam taken fresh

Structural note that shortens several of the questions below: on the Volcano path
the two storage modes differ ONLY inside `SeqScanNode` (A.2), and on the
vectorized path there is only one storage mode. So "can row and columnar
disagree" is always a question about `SeqScanNode` plus the one converter that
feeds it.

### B.1 PASS — loading cannot differ, because there is only one loader

`main.cc:440` is the ONE production loader call, and it runs for BOTH storage
modes. `--storage columnar` then converts the ALREADY-PARSED rows
(`main.cc:454-458`, `CSVToColumnar::convert(rows, s)`). The columnar path is
literally the row path plus a transformation, so quoting, escapes, empty field
vs NULL, numeric parsing, trailing whitespace and encoding are identical **by
construction**, not by agreement. Every `FileFormat` question (delimiter, header,
trailing delimiter) is answered once, before storage is chosen.

The same `Schema` object drives both (`tmeta.schema` at `main.cc:440`,
`catalog.getTable(name).schema` at `main.cc:456`), so `parseField`'s type and
`convert`'s `asInt/asDouble/asString` dispatch cannot disagree.

### B.2 PASS — NULL: neither format can represent it, so they cannot disagree

`CSVLoader::parseField` (`csv_loader.cc:92-121`) has no NULL production: an
empty INT/DOUBLE field throws out of `stoll`/`stod`, and an empty STRING field
becomes `Value("")`, which is the empty string, not NULL. So no `Value` reaching
storage is ever null in EITHER format. `ColumnarTable` has no validity mask and
needs none; `std::vector<Row>` could hold a null `Value` but nothing ever puts
one there.

The Python oracle agrees by the same rule (`_coerce_field`,
`compare_against_sqlite.py:1875-1886`: `int("")` / `float("")` raise, text stays
`""`), so a genuinely empty field fails on both sides rather than diverging.

Every NULL in this engine is manufactured above the scan (outer-join extension,
`Value::null()` at `plan_nodes.cc:716`; empty aggregate groups; `CASE` with no
`ELSE`). None of those is storage-dependent.

### B.3 PASS — value fidelity is bit-exact in both directions

| declared type | row | columnar | round trip |
|---|---|---|---|
| INT | `Value(int64_t)` from `stoll` | `asInt()` -> `vector<int64_t>`, maybe `RLEColumn` (`int64_t` values) | exact |
| DOUBLE | `Value(double)` from `stod` | `asDouble()` -> `vector<double>`, never encoded (`csv_to_columnar.cc:83`) | exact |
| STRING | `Value(std::string)` | `asString()` -> `vector<string>` -> `DictionaryEncoder` | exact; `decode()` returns a `const&` into `dict`, and `Value(std::string)` copies, so no dangling |

Type is never *inferred* for a base table — it is declared in `catalog.json` and
parsed once (`catalog.cc:24-28`). The only inferred column types in the engine
belong to derived relations and aggregate outputs, both of which live above the
scan; a derived relation is vectorized-only, so only one storage mode ever sees
it and the two cannot infer differently.

### B.4 PASS — chunking: boundaries, spans, cuts

- `CHUNK_SIZE = 8192` (`columnar_table.h:15`), `BATCH_SIZE = 1024`
  (`vec_types.h:13`). **1024 divides 8192**, so a vectorized batch never straddles
  a zone-map chunk and `row_cursor_ % CHUNK_SIZE == 0` is reached exactly at every
  boundary. If either constant changed to a non-divisor, `VecScanNode:19-27`
  would silently stop testing some chunks — worth a static assert, but not a
  finding today.
- Chunk boundaries are IDENTICAL across columns by construction: the same
  `start += CHUNK_SIZE` loop runs per column over the same `num_rows`
  (`csv_to_columnar.cc:99`).
- A group spanning chunks / an aggregate accumulating across chunks: Volcano's
  `HashAggregateNode` is fed one row at a time and knows nothing about chunks;
  the vectorized one is a blocking operator over all batches. Neither has a
  per-chunk reset.
- `LIMIT` cutting mid-chunk: `LimitNode` counts emitted rows; `VecLimitNode`
  truncates a batch. Neither interacts with zone-map chunks.
- Empty table (`num_rows == 0`): `zone_maps[name]` is an empty vector, every
  scan loop is skipped, and `shouldSkip`'s `chunk_idx >= size()` guard
  (`chunk_pruner.h:96`) declines. Both modes return 0 rows.

### B.5 PASS — every hint the pruner consumes, re-enumerated

`ChunkPruner::shouldSkip` has exactly two consumers, `SeqScanNode::next`
(`plan_nodes.cc:66`) and `VecScanNode::nextChunk` (`vec_scan_node.cc:22`). The
hints reaching them, exhaustively:

| # | producer | hint | wrong-relation guard |
|---|---|---|---|
| 1 | `Planner::plan:233` -> FROM scan | `pruningHintForPreservedSide(stmt.where, INNER\|LEFT, slots(scan_schema))` | FROM is always relation 0 here (only one join is buildable); `chunk_pruner.h:70` `localSlot(...) < 1`; LEFT adds the all-slots-preserved test |
| 2 | `Planner::plan:261,265` -> JOIN scan | **explicit `nullptr`** | n/a |
| 3 | `vectorized_plan_builder.cc:441` -> JOIN children[0] | `pruningHintForPreservedSide(...)`, gated on `leftmost_is_slot0` | `== 0` is stricter than the pruner's `< 1`, so a `-1` ref cannot ride in |
| 4 | `vectorized_plan_builder.cc:443` -> JOIN children[1] | **explicit `nullptr`** | n/a |
| 5 | `vectorized_plan_builder.cc:644` -> FILTER child | own predicate, only when the child is `SCAN` or `JOIN`; the INCOMING hint is discarded | a filter above AGGREGATE/DERIVED yields no hint |
| 6 | `vectorized_plan_builder.cc:304` -> DERIVED body | **explicit `nullptr`** | the body is a different block |
| 7 | AGGREGATE / PROJECT / SORT / DISTINCT / LIMIT cases | **`nullptr`** each | n/a |
| 8 | `VectorizedPlanBuilder::build` root | `nullptr` | n/a |

There is no ninth. The correlated-ref route the Week 30 tripwire was written for
is closed at `chunk_pruner.h:69` by `col->id.isLocal()` — verified in code, and
this is the one `development.md` claim about storage I depended on (its
"Relation slots and query levels" table, ChunkPruner row: *"Now declines a
`query_level > 0` ref"*). **The claim is accurate.**

### B.6 PASS — scan order is identical across storage modes, and the Week-37 GROUP BY fix is sound across them

`SeqScanNode::next` walks `cursor_` 0..n in both modes (`plan_nodes.cc:62` vs
`:84`), and `CSVToColumnar::convert` preserves file order in every column
(`csv_to_columnar.cc:52-60`), so the two formats deliver rows in the SAME order,
not merely in some order.

Consequences checked one level up:

- `87c08a2`'s fix records `group_order` as **first-encounter** order of the group
  key (`plan_nodes.cc:229,262`). First encounter is a function of input order,
  and input order is identical across storage modes, so the fix is sound across
  storage and not only across engines. `ENGINE_AGREEMENT_QUERIES` is run over
  `SWEEP_MODES` = row-volcano / col-volcano / col-vec
  (`compare_against_sqlite.py:2219-2223`), so this is asserted, not assumed.
- Chunk pruning cannot perturb that order: the hint is the WHOLE `stmt.where`
  (`planner.cc:233`), a pruned chunk contains only rows the `FilterNode` above
  would have rejected anyway, and pruning never reorders survivors.
- `HashJoinNode` output order is probe order in both modes; the build-side bucket
  vectors are filled from the same value sequence, so multi-match order matches.
- `SortNode`/`VecSortNode` are stable sorts over identical input order.

### B.7 PASS — no operator retains a `Row*` across a scan advance

This is the one bug class that is genuinely storage-shaped: the columnar
`SeqScanNode` returns `&reconstructed_row_`, a buffer it REUSES on every call
(`plan_nodes.cc:74,79`), while the row scan returns `&rows_[cursor_++]`, a
pointer into a stable vector. Any operator that holds a `Row*` across a child
`next()` is correct in row mode and reads freed/overwritten data in columnar
mode. Checked every buffering operator:

- `HashJoinNode` build: `hash_table_[key].push_back(*row)` — **copies**
  (`plan_nodes.cc:694`).
- `HashJoinNode` probe: `current_probe_row_` IS retained across `next()` calls
  (`plan_nodes.h:252`) — but the branch that drains a bucket never calls
  `left_->next()` (`plan_nodes.cc:707-770`), so the buffer cannot be advanced
  while the pointer is live. Correct, and delicately so.
- `SortNode`: `sorted_rows_.push_back(*row)` — **copies** (`:515`).
- `HashAggregateNode`: reads values out of `*row` into `key`/accumulators, keeps
  no pointer.
- `DistinctNode`, `HavingNode`, `FilterNode`, `LimitNode`: pass the pointer
  straight up without outliving one `next()`.

### B.8 PASS (measured) — the two Volcano pruning routes, exercised so a leak would show

Pass 1 verified this on a purpose-built table but never showed the hint *reaching*
a Volcano scan. Both routes shown here on a fresh 20000-row table `zm(k,d,s,g)`
clustered ASCENDING on `k` (3 chunks, disjoint zone maps 0-8191 / 8192-16383 /
16384-19999) joined to `alt(j,k)` whose `k` is 100000..100019 — **deliberately
disjoint from `zm.k`, and deliberately sharing the column name `k`**, so a leaked
hint prunes every chunk and the count collapses to 0.

Route 1, LEFT join, WHERE on the PRESERVED side (`--storage columnar`, Volcano):

    SELECT COUNT(*) AS c FROM zm LEFT JOIN alt ON zm.g = alt.j WHERE zm.k > 16383
      SeqScan [zm, 2 columns] chunks_skipped=2/3   rows_out=3616
    row 10331 | col-volcano 10331 | col-vec 10331     (Python oracle 10331)

Pruning is LIVE on the Volcano columnar path — 2 of 3 chunks skipped — and the
answer still matches row mode. Pass 1 never demonstrated this.

Route 2, LEFT join, WHERE on the NULL-SUPPLYING side, shared column name:

    SELECT COUNT(*) AS c FROM zm LEFT JOIN alt ON zm.g = alt.j WHERE alt.k > 100010
      SeqScan [zm, 2 columns]        <- NO chunks_skipped field: hint WITHHELD
    row 25714 | col-volcano 25714 | col-vec 25714     (Python oracle 25714)

`pruningHintForPreservedSide` returned `nullptr` (slot 1 not in preserved {0}),
so `pruning_where_` is null and `explain()` prints no counter at all.

Route 3, the same shape as an INNER join — this is the load-bearing leg, because
`pruningHintForPreservedSide` returns the hint **unconditionally** for
`JoinType::INNER` (`predicate_pushdown.cc:139`) and Volcano never runs pushdown,
so the whole `WHERE` reaches the scan:

    SELECT COUNT(*) AS c FROM zm JOIN alt ON zm.g = alt.j WHERE alt.k > 100010
      SeqScan [zm, 2 columns] chunks_skipped=0/3    <- hint ATTACHED, 0 skipped
    row 25714 | col-volcano 25714 | col-vec 25714

The counter printing at all proves `pruning_where_ != nullptr`, i.e. the
predicate `alt.k > 100010` really was handed to `zm`'s scan. The only thing
between it and `zm`'s zone maps is `chunk_pruner.h:70`'s
`col->id.localSlot(...) < 1` on the slot-1 ref. It holds; 0 chunks skipped;
answer correct. A leak here is 0 rows, so this is unmissable.

### B.9 PASS (measured) — zone-map boundary sweep over DOUBLE and STRING

Pass 1 recorded this as a target NOT reached ("the clustered table has no DOUBLE
column"; DOUBLE covered only via reconstruction). Closed here.

Same `zm` table: `k` INT ascending, `d` DOUBLE ascending (`i*0.25 - 1000.5`, so
negatives and fractions), `s` STRING ascending fixed-width, `g` INT NOT clustered
(zone map spans the full range in every chunk, so it can prune nothing). For every
column, every per-chunk min and max as the literal, all six operators
(`= < > <= >= !=`):

    144 boundary queries x 3 cells (row-volcano, col-volcano, col-vec)
    checked against an independent Python count
    -> 0 mismatches, 0 cross-mode divergences

DOUBLE and STRING zone maps behave exactly as INT ones. `!=` never prunes
(`chunk_pruner.h:86` falls through to `return false`), which is correct.

### B.10 FINDING S-8 (LOW) — a duplicate column name in `catalog.json` is a silent wrong answer in columnar storage only

**Concrete failing input.** `catalog.json`:

    {"tables":[{"name":"t","file":"data/t.csv","columns":[
      {"name":"k","type":"INT"},{"name":"k","type":"INT"}]}]}

`data/t.csv`:

    k,k
    1,100
    2,200
    3,300

Measured, all three cells:

| query | row-volcano | col-volcano | col-vec |
|---|---|---|---|
| `SELECT k FROM t` | `1, 2, 3` | **`1, 100, 2`** | **`1, 100, 2`** |
| `SELECT COUNT(*), SUM(k) FROM t` | `3, 6` | **`3, 103`** | **`3, 103`** |

No error, no warning, at any layer.

**Mechanism.** `ColumnarTable::columns` and `::zone_maps` are keyed by column
NAME (`columnar_table.h:31,33`). `CSVToColumnar::convert` pass 1
(`csv_to_columnar.cc:26-45`) `emplace`s a fresh vector per schema column, so the
second `k` overwrites the first's vector; pass 2 (`:48-60`) then pushes BOTH
columns' values into that ONE vector, per row. The vector becomes
`[r0c0, r0c1, r1c0, r1c1, ...]` while `num_rows` stays 3, so `getValue("k", r)`
returns element `r` — i.e. row 1 reads row 0's SECOND column. Row storage is
positional (`csv_loader.cc:53-66`) and is unaffected.

`catalog.cc:22-27` does not check for a repeated name.

**Why LOW and not higher.** It is unreachable from SQL — a catalog must be
hand-edited to produce it; `generate_tpch.py` and the shipped `catalog.json` emit
distinct names; and row mode's own answer for an ill-formed schema is arbitrary
too (it silently picks the first `k`). It is a finding rather than a non-issue
because (a) it is the ONLY input found in two passes that makes the two storage
formats disagree, (b) it is silent, and (c) the engine already refuses the
identical shape one layer up: `derivedRelationSchema` (`logical_plan.cc:494-501`)
THROWS `"column '<c>' is produced twice; give one of them an alias"` for a
derived relation, for exactly this reason. The catalog is the inconsistency.

**Fix, if Week 37 wants it:** ~4 lines in `catalog.cc`'s column loop — a
`std::unordered_set<std::string>` and a throw naming the table and the column,
mirroring the multi-character-delimiter refusal already there (`catalog.cc:44`).
Pass 1 saw the code and recorded it as out of scope; pass 2's brief puts the
catalog in scope, and this pass has the failing input and the side-by-side
outputs, so it is counted.

### B.11 LOW-adjacent observation, NOT counted (outside this seam)

`vectorized_plan_builder.cc:304`, the `DERIVED` case, lowers its body with
`lowerNode(...)` while all eight other cases use `lower(...)`. `lower` is the
wrapper that stamps `phys->estimated_rows = node->estimated_rows`
(`:276-280`), so the physical node directly under a `VecDerivedNode` keeps the
`-1` sentinel and `--explain` prints no `est=` for it. Display-only, vectorized-
only, no storage involvement — recorded because it is a one-character
inconsistency in a file this audit read closely, and belongs to whoever owns the
optimizer/EXPLAIN seam.

### B.12 PASS with a stated boundary — the scan-width asymmetry between the two storage modes

The two Volcano storage branches do NOT build the same scan schema:

    planner.cc:186   Schema scan_schema = buildScanSchema(stmt, meta.schema);   // narrowed
    planner.cc:238   SeqScanNode(from_table, columnar..., scan_schema, prune_hint);
    planner.cc:240   SeqScanNode(from_table, rows...,     meta.schema);          // FULL

Measured on the same query (`... FROM zm LEFT JOIN alt ... WHERE zm.k > 16383`):

    row      SeqScan [zm, 4 columns]
    columnar SeqScan [zm, 2 columns]

So **row storage never gets projection pushdown**, and the merged join schema is
a different width in the two modes. This means the differential oracle is
comparing two structurally different plans — which is a strength, not a defect,
but it should be stated rather than assumed away.

I looked for an observable consequence and found none, for one reason that is
checkable: `narrowSchema` (`logical_plan.cc:72-79`) filters by BARE NAME against
one `required` set that is applied to **both** join sides identically, so a name
that survives on one side survives on the other and the relative order of
same-named columns is preserved. Every `indexOf(name, slot)` and every bare-name
`indexOf(name)` fallback therefore lands on the same logical column in both
widths.

The one way this could bite is a `collectCols` dispatch miss: a `ColumnRef`
living inside an `Expr` subtype `collectCols` does not descend into would be
narrowed away in columnar and present in row, giving `"column not found"` in one
storage mode only. **Checked exhaustively against `ast.h`** — the subtypes are
`ColumnRef`, `Literal`, `BinaryExpr`, `UnaryExpr`, `IsNullExpr`, `AggregateExpr`,
`InExpr`, `LikeExpr`, `CaseExpr`, `SubstringExpr`, `SubqueryExpr`,
`IntervalLiteral`. `collectCols` (`logical_plan.cc:10-68`) handles all of them;
`Literal` and `IntervalLiteral` carry no column, and `InExpr::values` is
`std::vector<Value>` (`ast.h:110`), literals by construction, so descending into
`operand` alone is complete. There is no eleventh subtype. `BETWEEN` has no node
— it is desugared — so it needs no case.

Boundary: this is verified for the SHAPES Volcano accepts. `buildScanSchema` is
also used by the logical layer (`logical_plan.cc:513,840`), so the vectorized
path narrows too and the row path is the only full-width one anywhere.

### B.13 PASS — catalog consistency, other than S-8

- One `Schema` object drives the loader, the columnar converter and both scans
  (`main.cc:440,456`), so the two paths cannot disagree about a column's TYPE.
- There is no nullability in the catalog at all (`catalog.cc:24-28` reads only
  `name` and `type`), so there is nothing for the two paths to disagree about.
- `ColumnarTable::schema` is written at construction and **never read** —
  grepped: the only `columnar_table_` member reads in the tree are `num_rows`,
  `zone_maps`, `columns` and `getValue`. Both scan nodes use their own
  `schema_` parameter. A dead field, not a divergence.
- The result cache key includes `storage` (`main.cc:31,36,44`), so a row answer
  can never be served for a columnar query. Checked because a missing field here
  would be a blocker.
- `TableStats::compute` runs on the ROW data before conversion
  (`main.cc:446-448`), so statistics — and therefore every cost decision — are
  identical in both storage modes. That is what makes the row/columnar
  comparison a comparison of storage rather than of plans.

---

## Part C — the three data points the orchestrator raised after the gate

### C.1 The 168 four-mode queries are NOT Phase 1-4 leftovers

`QUERIES` — the list the SQLite oracle runs in all four modes, i.e. the ONLY
list that runs in **both storage modes** — is composed at
`compare_against_sqlite.py:1850-1861` from 25 named suites. Twelve of them are
Phase 5 (weeks 24-37), contributing **89 of the 168** queries:

    WEEK24_EXPRESSION 2, WEEK25_PREDICATE 16, WEEK25_CASE 7,
    WEEK25_SUBSTRING 4, WEEK25_JOIN 2, WEEK26_ALIAS_SHADOW 2,
    WEEK27_JOIN 7, WEEK27_KEY_ENCODING 5, WEEK29_OUTER_JOIN 17,
    WEEK30_ALIAS_REBIND 3, WEEK31_SUBQUERY 17, WEEK34_DISTINCT_AGG 7

So **the surface S-0 calls empty holds 89 Phase 5 queries**, more than half the
four-mode suite. The 101 vectorized-only queries are exactly the four shapes
`Planner::plan` refuses (A.3) plus the language refusals; every one of their
bucket names is a Phase 5 week, but the converse — that no Phase 5 week has
four-mode coverage — does not follow and is false.

Verified by running six of them side by side (`--storage row` vs
`--storage columnar`), including the Week 29 outer-join pruning shape, the Week 31
materialized scalar subquery, the Week 34 `COUNT(DISTINCT)`, the Week 26 multi-key
join and the `87c08a2` tie-at-LIMIT query: **6/6 identical output, byte for byte.**

### C.2 The TPC-H refusal pins — a stale entry and one loose entry, neither load-bearing for this seam

`run_tpch.py:81-88` `VOLCANO_BOUNDARIES` is matched first-hit by `classify`
(`run_tpch.py:220-222`), and it lists:

    1  "multi-way joins are not supported on the Volcano path"      -> refusal 1
    2  "IN (subquery) is lowered to a semi-join"                    -> DEAD
    3  "IN subquery"                                                -> LOOSE
    4  "correlated subqueries are decorrelated to a semi-join"      -> refusal 3
    5  "derived tables (FROM (subquery)) are not supported ..."     -> refusal 4
    6  "not supported on the Volcano path"                          -> catch-all

- **Entry 2 can never match.** The engine's text is `"IN subqueries are lowered
  to a semi-join and are not supported on the Volcano path"`
  (`planner.cc:71-73`); the pin says `"IN (subquery) is lowered"`. Harmless
  today only because entry 3 catches the same cell — which is precisely the
  half-landed-move signature `compare_against_sqlite.py`'s own behavioural sweep
  exists to catch, in a file that sweep does not cover.
- **Entry 3 is loose enough to mislabel a LANGUAGE refusal as a Volcano
  boundary.** `"IN subquery"` also matches
  `"IN subquery: the left operand must be a column reference ..."` and
  `"IN subquery: supported only as a whole top-level WHERE conjunct ..."`, which
  fire identically on ALL FOUR modes and are dialect gaps, not capability
  boundaries. Such a cell would be counted as `REFUSED_EXPECTED` (boundary
  coverage) in every mode instead of `UNPORTED` (Week 37 worklist), inflating
  "Volcano refusals pinned by message" and shrinking the gap list.

Neither weakens anything in Part A: **A.3's four refusals are read from
`planner.cc` and confirmed by running the binary**, not taken from the harness's
pins. Recorded here because the orchestrator asked and because the owner is the
TPC-H reporting seam, not storage. Rank LOW; the fix is to delete entry 2 and
tighten entry 3 to `"IN subqueries are lowered to a semi-join"`.

`compare_against_sqlite.py` does NOT have this problem: `VOLCANO_MULTIWAY`,
`VOLCANO_IN`, `VOLCANO_CORRELATED` and `VOLCANO_DERIVED`
(`compare_against_sqlite.py:605-611`) are four distinct full-sentence pins, so a
query refused for the wrong reason fails there.

### C.3 NULL — never differentially tested across storage, and cannot be, because columnar has no NULL to test

Established:

- `CSVToColumnar::convert` is the **only** producer of a `ColumnarTable` in the
  whole tree (grepped: two hits, its definition and `main.cc:457`), and its input
  is always rows straight from `CSVLoader::load`.
- `CSVLoader::parseField` (`csv_loader.cc:92-121`) has **no NULL production** at
  all: an empty INT/DOUBLE field throws, an empty STRING field is `Value("")`.
- `ColumnArray` (`columnar_table.h:13`) is a variant of three dense typed vectors
  with **no validity concept**. `ColumnVector`'s own invariant states it:
  *"ColumnarTable cannot express NULL, so scan output is always all-valid"*
  (`vec_types.h:22-25`).

So the orchestrator's inference is exactly right and now has a proof: **no loaded
table in either format can contain a NULL, so row-vs-columnar NULL
*representation* has never been differentially tested by any harness.** The
correct conclusion, though, is that there is nothing there to test — the columnar
side has no representation to disagree with. A NULL comes into existence only
above the scan (`nullExtend`, `plan_nodes.cc:715-718`; empty aggregate groups;
`CASE` with no `ELSE`), and on the Volcano path — the only path with two storage
modes — it lives in a `Row` that has already left the scan and is therefore
storage-independent by construction.

What IS differentially tested across storage is NULL *semantics*, and well:
`NULL_SEMANTICS_QUERIES` (6), `THREE_VALUED_LOGIC_QUERIES` (8),
`NULL_ORDERING_QUERIES` (7) and `WEEK29_OUTER_JOIN_QUERIES` (17) are all inside
`QUERIES`, so 38 NULL-bearing queries run in row-volcano AND col-volcano on every
gate.

**The gap that IS real, and it belongs to Week 37:** if a future loader ever
learns to express NULL (a `\N` token, an empty-field-is-NULL rule), it will need
a validity representation in `ColumnarTable` that today does not exist, and on
that day row-vs-columnar NULL storage becomes a live divergence surface with no
harness coverage whatsoever. Worth one line in the Week 37 hand-off; not a
finding today, because the input cannot be constructed.

---

## Summary

Status: **COMPLETE.**

### Counts by severity

| severity | count | findings |
|---|---|---|
| BLOCKER | **0** | — |
| HIGH | **0** | — |
| MEDIUM | **0** | — |
| LOW | **2** | **S-8** duplicate catalog column name is a silent wrong answer in columnar storage only (B.10, with side-by-side outputs); **S-0** restated and re-ranked (A.6) |
| out of seam, recorded not counted | 2 | `run_tpch.py`'s dead + loose Volcano refusal pins (C.2); `DERIVED` lowering uses `lowerNode` where every other case uses `lower`, so `--explain` drops one `est=` (B.11) |

### Ruling on S-0

**S-0 as written is OVERSTATED, and its severity is LOW, not "the single most
important thing on this seam".**

- *Overstated*: "the row/columnar oracle does not span **any** Phase 5 plan shape"
  is false. 89 of the 168 four-mode queries are Phase 5 (C.1), including all 17
  `WEEK29_OUTER_JOIN_QUERIES` — and Week 29's `pruningHintForPreservedSide` is the
  one Phase 5 change that exists *only* to protect chunk pruning. The
  storage-critical Phase 5 code path is under a differential oracle, contrary to
  the finding.
- *Understated in one place*: pass 1 said "four modes". There are only **three**
  storage×engine cells — `--storage row --execution vectorized` is refused for
  every query (`main.cc:548`, confirmed by running it). The fourth "mode" is an
  optimizer flag. So the row/columnar oracle is even narrower than the mode count
  suggests: it is Volcano-only, always.
- *True, restated exactly*: for multi-way joins, derived tables, `IN` semi/anti
  joins and correlated subqueries there is one executable cell. But those four are
  refused **before any scan is built**, so none of them can reach `SeqScanNode` —
  the one node the two storage modes differ in (A.2). The uncovered surface is not
  "row storage untested against Phase 5"; row storage is a `std::vector<Row>` with
  an index and has no chunks, encodings, zone maps or pruning for a plan shape to
  break. The uncovered surface is `VecScanNode`'s pruning on those four shapes,
  which is narrower, has one guard per route (B.5, all eight enumerated), and was
  hand-verified in pass 1 and re-verified at runtime here (B.8).

So I do **not** accept the brief's framing that S-0 means an entire storage mode
is untested against every feature Phase 5 built. LOW is the honest rank: a real,
permanent coverage gap that a future week can widen, with no reachable wrong
answer behind it today.

### Cheapest change for a shared storage surface (Week 37 sizing)

**Give `VecScanNode` a row-backed constructor and delete `main.cc:548`.**
≈70-90 lines total (`vec_scan_node.{h,cc}` ~40, `vectorized_plan_builder` ~15,
`main.cc` ~10, harness mode entries ~5). No planner change, no refusal relaxed,
no new semantics. Full sizing at A.7. It makes every Phase 5 shape run on both
storage formats, and because the row leg has **no zone maps**, the comparison is
literally a pruning-on vs pruning-off oracle — the exact differential that catches
a wrong prune, on the exact shapes that have none today. Every alternative
(lifting a Volcano refusal) is larger and buys less, by this project's own
measurement at `planner.cc:98-114`.

### Clean-because-it-ran vs clean-because-it-never-ran

The distinction the brief asked for, made explicitly:

**Ran and agreed** — 89 Phase 5 queries + 79 Phase 1-4 queries in both storage
modes on every gate; 38 NULL-bearing queries among them; 144 zone-map boundary
queries over INT, **DOUBLE and STRING** across three cells against a Python oracle
(B.9, closing a gap pass 1 recorded as not reached); 3 adversarial shared-column-name
prune routes with a leak collapsing the count to 0 (B.8), including the first
runtime demonstration that pruning is live on the Volcano columnar path
(`chunks_skipped=2/3`) and that the hint is genuinely withheld on the
null-supplying side (no counter printed at all).

**Never ran** — multi-way joins, derived tables, `IN` semi/anti joins, correlated
subqueries: one cell each, no cross-storage comparison possible. Also: NULL
storage representation, which cannot run because neither format can hold a NULL
(C.3).

**Newly failed** — one input, S-8, and it needs a hand-malformed catalog.

### Verdict

The storage seam is sound: the two formats differ in exactly one node, deliver
rows in the same order, round-trip every value bit-exactly, share one loader and
one catalog schema, and every one of the eight pruning-hint routes withholds or
slot-tests correctly under an adversarial shared column name — but `catalog.json`
will accept a duplicate column name that columnar storage then silently
mis-answers, and S-0 is a narrower, lower-severity gap than pass 1 left it.
