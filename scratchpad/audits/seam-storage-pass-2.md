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

