# Week 35 — TPC-H Data + Harness

> **Teaching plan, not an implementation.** No code in this document is meant to
> be pasted into the tree. Every snippet is illustrative and written in this
> project's existing patterns and naming so that the shape is recognisable when
> you do write it.

**README bullets for this week:**

- Add the TPC-H schema, pipe-delimited loader, and scale-factor workflow
- Add parameterized queries, warmups, repetitions, and reference comparison

**Checkpoint:** TPC-H data generation and automated query runs are reproducible.

**What makes this week pivotal.** Every week from 26 onward has treated "TPC-H
22/22" as design context while being unable to *measure* it. Week 36 is the week
that measures it. So the harness built here is what every Week 36 claim rests
on, and a harness that reports a number without reporting the conditions under
which the number was obtained is worse than no harness: it converts four known,
written-down capability boundaries into an unqualified pass.

---

## Progress

> Kept current during implementation. A successor resumes from here.

| Task | State |
|---|---|
| 1 — TPC-H schema | **done** — `python_tools/tpch_schema.py` |
| 2 — pipe-delimited loader | **done** — `FileFormat` on `TableMetadata`, `parseField` full-consumption, 5 new `TblLoaderTest` |
| 3 — scale-factor workflow | **done** — `generate_tpch.py`, seeded `generate_data.py`, `data/f1/sf-small` |
| 4 — parameterized queries / runner | **done** — `tpch_queries.py` (22 templates + validation params), `run_tpch.py` |
| 5 — reference comparison | **done** — `--format tsv`, catalog-driven `load_from_catalog`, `rows_equal(rel_tol=)` |
| 6 — mode-coverage report | **done** — 22x4 cell matrix, computed census, Q22 plan fingerprint |
| 7 — randomized result differencing | **done** — `python_tools/random_diff.py`, 40 shapes both legs in 61 s |
| 8 — behavioural rejection sweep | **done** — 17 suites / 157 entries executed, plus a computed mode census |

### Round-2 audit fixes (`scratchpad/audits/week-35-round-2.md`)

| Fix | State |
|---|---|
| 1 — the gate step that cannot fail (`--baseline` + `--write-baseline` on one path) | **done** — the write moved after the comparison and is refused unless the run passed; proved on a synthetic regression |
| 2 — q2's INERT is a parameter artifact | pending |
| 3 — q19's ALL_NULL is a parameter artifact | pending |
| 4 — a narrowed `--queries` run prints a full-shaped PASS | pending |
| 5 — README records Q17 as supported; the spec's Q17 text is refused | pending |
| 6 — q18's "unreachable" holds only at threshold 300 | pending |
| after 2+3 — regenerate `docs/tpch-baseline.json` from a full run | pending |

**All eight tasks are implemented.** The standing-rule sweep is done (README
Limitations on NaN, commas-in-CSV and the stale 56/168 census; `normalize()`'s
blind-spot comment; `CSVLoader`'s header comment; the README Week 35 section).

**Owed and carried forward, explicitly:** per-guard mutation coverage for the
other 25 guards, and per-entry query-shape re-derivation. Both re-declined with
reasons in the README rather than dropped — see the Week 35 section's closing
note.

### Next, for a successor — in this order, one commit each

1. ~~**Wire `run_tpch.py` into the `verify` skill as a fifth gate step.**~~
   **DONE.** `.claude/skills/verify/SKILL.md` now runs five gates; gate 5 is
   `python3 python_tools/run_tpch.py --catalog data/tpch/sf0.01/catalog.json
   --baseline docs/tpch-baseline.json`. The two open decisions were settled:
   - **Missing data fails loudly.** `run_tpch.py` checks the catalog exists
     before anything else and exits non-zero naming
     `generate_tpch.py --scale 0.01 --out-dir data/tpch/sf0.01`, with the words
     "the gate did not run". It does **not** generate the data itself: a gate
     that silently manufactures its own inputs can regenerate them differently
     and invalidate the baseline it is checking. The generator is seeded, so a
     manual regeneration reproduces the baselined files.
   - **No subset, no nightly.** A full 22×4 run measured **5m08s here** (not the
     ~25 min recorded above — that figure was from a different machine or a
     colder cache; both are real observations, this one is what the skill
     quotes). Five minutes runs every time. `--queries` narrows it while
     iterating and the baseline comparison scopes itself to the queries that
     ran, but a verdict block may only report a full run — stated in the skill.
   - `run_tpch.py` gained one function, `gate_line()`, printed as the run's last
     line: `GATE tpch: PASS (17/22 meaningful vs SQLite: 4 in all four modes, 13
     vectorized-only; 3 vacuous; 2 unported)`. The verifier copies it rather
     than composing a count, so the mode split and the separate vacuous/unported
     tallies cannot be dropped on the way into the block. Three verdicts, always
     agreeing with the exit code: `PASS`, `FAIL`, and `NO-BASELINE` for a run
     invoked without `--baseline`, which is not a pass because it cannot see a
     regression. Regression detection itself was already there
     (`compare_baseline`) and was not changed.
   - `.claude/skills/phase5-week/SKILL.md`'s verdict block is five gate lines
     plus `VERDICT`, and the README (Layer 9 table + Week 35 section) records
     that the gate has five steps.
2. **The two lesser round-1 audit findings**, both recorded in
   `scratchpad/audits/week-35-round-1.md` and neither yet touched:
   - `compare_against_sqlite.py:1874` compares floats as `abs(x-y) > tol`, so two
     NaNs — or two same-signed infinities — compare **equal**, because
     `nan > tol` is False. `normalize`'s `coerce` produces both, since
     `float("nan")` parses. Grounded in the code; no TPC-H input demonstrated
     reaching it.
   - `random_diff.py:113-117` projects only `driver_id` and `team`, and only from
     `rels[:3]`. `driver_id` is also the join key in every generated join, so
     every projected `rN.driver_id` is equal across relations **by construction**
     and a plan defect that emitted the wrong relation's copy would be invisible.
     Only `team` discriminates, and relations 4-8 are never projected at all.
     This bounds what the 40/40 result-preserving figure licenses.

### Measured this week

TPC-H at SF=0.01, synthetic data (see `data/tpch/sf0.01/PROVENANCE.txt`).
**All figures below are from one full 22x4 run recorded in
`docs/tpch-sf0.01-report.json`, with `docs/tpch-baseline.json` its summary.**
Language is deliberate throughout: **matches SQLite over the same files**, never
"correct" and never "TPC-H compliant" — the published answer set does not apply
to this data.

- **matched SQLite: 20/22.** **Meaningfully answered: 17/22** — **4 in all four
  modes** (q1, q6, q12, q14) and **13 in the two vectorized modes only** (q3,
  q4, q5, q7, q8, q9, q10, q11, q13, q15, q16, q20, q22). 34 of the 88 query x
  mode cells are Volcano refusals pinned by message.
- **3 vacuous — matched while asserting nothing about the feature under test.**
  Each was found by the mutation check (`tpch_queries.MUTATIONS`), not by
  inspection: neuter the one predicate carrying the query's characteristic
  feature and require the answer to change.
  - **q2 INERT** (2 modes) — deleting the correlated `MIN(ps_supplycost)`
    subquery leaves the answer byte-identical. Only one part survives
    `p_size = 15 AND p_type LIKE '%BRASS' AND r_name = 'EUROPE'` on this data,
    so the subquery selects what was already selected.
  - **q18 EMPTY** (2 modes) — `SUM(l_quantity) > 300` is unreachable at SF=0.01.
  - **q19 ALL_NULL** (4 modes) — no row matches any of the three OR arms, so the
    comparison is a `SUM` over nothing against a `SUM` over nothing.
- **2 not answered**, both refused BY NAME for documented reasons rather than
  failing: q17 (`a correlated scalar subquery is decorrelated only when its
  select list is a single aggregate` — the standard text is
  `0.2 * AVG(l_quantity)`), q21 (`only an equality between two columns can
  become a join key` — a correlated inequality). Both are UNPORTED in the two
  vectorized modes and REFUSED_EXPECTED in the two Volcano ones, so they are
  0-mode queries and Week 36's worklist.
- **q16 is no longer among them.** The round-1 audit's blocker — its `NOT IN`
  anti-join matched nothing, so the whole predicate could be deleted without
  changing a byte — was fixed by seeding the phrase into the generator
  (`f7c6cdb`). Re-measured here: 5 suppliers match the inner `LIKE`, deleting the
  anti-join changes the answer, and SwiftQL still matches SQLite in both
  vectorized modes.

**Why the figure moved, since three different numbers are in the history.** The
harness first reported **20/22** and that count is still right for "matched the
oracle"; the round-1 audit corrected it to **18** by naming q16 alongside q18;
the mutation check finds **17**, because it can see two vacuity shapes the audit
could not spot by hand (q2's inert subquery and q19's all-NULL aggregate) while
q16 is now genuinely discriminating. 17 is the honest figure and it is the one
Week 36 should raise.
- **Q22's provenance, read off a plan rather than argued**:
  `{'LogicalDerived': 2, 'LogicalAntiJoin': 2}` — the correlated half IS Week
  33's `NOT EXISTS` anti-join, the `custsale` half IS Week 34's derived table,
  and **no** correlated-scalar LEFT-join rewrite is present. That settles Week
  34's hand-forward mechanically.
- Peak child RSS at SF=0.01: ~65 MB (row/Volcano), ~83 MB (columnar/vectorized).
  `lineitem` columnar: 24.8 MB raw -> 10.7 MB encoded (ratio 0.43).
- **Randomized result differencing (Week 28's deferred gap): 40 generated
  3-8 relation shapes, both legs, in 61 s** against `data/f1/sf-small` — 40/40
  result-preserving (optimized ≡ `--no-optimize`) and 40/40 matching SQLite.
  Week 28's note predicted "a 40-query randomized differential [could] run in
  under a minute"; 61 s is that prediction, met.

### A third form of trap 1, found rather than anticipated

Week 28 named two traps (sort before diffing; `normalize()`'s name-keying). A
third has the same shape and the generator's first run walked straight into it:
**`LIMIT n` with no `ORDER BY` selects an unspecified `n` rows**, so a reordered
join legitimately returns a *different subset of the same answer*. The first
batch reported four `OPTIMIZER CHANGED THE RESULT` failures, every one of them
`500 rows vs 500` — same count, different rows, both correct. A `LIMIT` is only
comparable under a **total** order; the generator now emits neither, and bounds
result size by cardinality instead.

Cardinality is the real budget, and the `--no-optimize` leg spends it: that leg
runs the filter *above* the joins, so it materializes the unfiltered product —
which is exactly the 35.6 s Week 28 measured. Measured here: at three `laps`
relations a batch runs 12.7 s per query; at two it is well under a second, and
every further relation is `drivers` (unique `driver_id`, so a 1:1 lookup that
adds a join for the enumerator to reorder without multiplying rows).

### Defect found and fixed

**A subquery inside a derived-table body reached type inference unmaterialized**
— an `internal:` error surfaced to the user on TPC-H Q22. Two causes, both the
standing rule's shape:

1. `materializeSubqueries` never recursed into a derived body, while
   `collectQueryTables` — ten lines above it in the same file — *was* extended
   to when Week 34 landed derived tables.
2. `main.cc` guarded the whole materialization block with `stmt.has_subquery`,
   which is per-BLOCK by design (`ast.h` records that propagating it upward
   would turn projection pushdown off for every derived-table query and give the
   wrong Volcano refusal message). A new predicate,
   `needsSubqueryMaterialization`, asks the whole-tree question without
   disturbing the flag.

Five diffed regression queries in `WEEK35_SUBQUERY_IN_DERIVED_BODY_VEC_ONLY`
(both `FROM` and `JOIN` positions, scalar and `EXISTS` bodies, and two derived
relations where only the second holds a subquery), plus their Volcano refusals.

### The sweep found its own blind spot

Matching `_REJECTED` / `_REFUSED` as a **suffix** silently skipped
`WEEK26_REJECTED_QUERIES` and `WEEK30_REJECTED_QUERIES`, whose names end in
`_QUERIES`. It was found by deliberately injecting a stale expectation into one
of them and watching the sweep report *clean* — the exact failure the plan's own
"a sweep that discovers zero suites is green too" warning describes. Substring
matching took the sweep from 14 suites to **17, 157 entries executed**.

The computed census then contradicted the README's prose immediately: the stated
figure was **56** queries diffed in two modes; the computed figure is **93**
(the correlated-scalar suite alone grew from 8 to 23). The README now cites the
harness instead of carrying the number.

---

## Task list

1. [The TPC-H schema in a three-type engine](#task-1--the-tpc-h-schema-in-a-three-type-engine) — eight tables, no `DATE`, no `DECIMAL`, and a second catalog rather than a second copy of the first
2. [The pipe-delimited loader](#task-2--the-pipe-delimited-loader) — `CSVLoader` hard-codes a comma *and* unconditionally eats line 1; `.tbl` has neither a header nor a clean line end
3. [The scale-factor workflow](#task-3--the-scale-factor-workflow) — making a 500-row fixture a first-class artifact instead of a second catalog to keep in sync
4. [Parameterized queries, warmups, repetitions](#task-4--parameterized-queries-warmups-repetitions) — the run harness, and what its timers must exclude
5. [Reference comparison, and what the oracle cannot check](#task-5--reference-comparison-and-what-the-oracle-cannot-check) — a catalog-driven `load_sqlite`, numeric tolerance, and two named blind spots
6. [Reporting mode coverage honestly](#task-6--reporting-mode-coverage-honestly) — the per-query mode matrix that stops "22/22" from being a lie, and Q22's provenance
7. [Randomized result differencing at SF-small](#task-7--randomized-result-differencing-at-sf-small) — Week 28's deferred gap, with its two named traps built in from the start
8. [The behavioural rejection sweep and the standing sweep rule](#task-8--the-behavioural-rejection-sweep-and-the-standing-sweep-rule) — Week 34's harness lesson, automated

**Prerequisite knowledge for the week as a whole.** The TPC-H schema and the
`dbgen` `.tbl` format; how `std::filesystem` resolves a relative path against a
catalog file's directory (`Catalog::Catalog` already does this and it is the
lever Task 3 turns); the difference between *absolute* and *relative* floating
point tolerance; and this project's four-mode matrix (row/columnar ×
Volcano/vectorized, plus `--no-optimize` on the vectorized leg) as
`compare_against_sqlite.py::main` spells it out. You do **not** need the slot /
`ColumnId` model this week — nothing here touches the planner.

---

## Task 1 — The TPC-H schema in a three-type engine

### Why it matters

Everything downstream keys off this file. `Catalog::Catalog` reads
`catalog.json` and builds one `TableMetadata{name, filepath, Schema}` per table;
`Schema` is what `CSVLoader::load` types each field with, what
`TableStats::compute` summarises for the cost model, what `CSVToColumnar::convert`
lays out columnwise, and what the Binder resolves every `ColumnRef` against. A
wrong type in this file is not a type error — it is a **silently wrong value**,
because `CSVLoader::parseField` calls `std::stod` and `std::stod("1996-01-02")`
returns `1996.0` without complaining. That single fact is why Task 2 hardens
`parseField` and why this task comes first.

The schema also fixes what Week 36 can even attempt. The engine has exactly
three types — `TypeId::INT`, `TypeId::DOUBLE`, `TypeId::STRING` — and
`Catalog::parseTypeId` throws on anything else. TPC-H's `DATE` and
`DECIMAL(15,2)` have to land somewhere, and where they land is a *dialect fact*
the Week 36 correctness report has to declare, exactly as the README's
Limitations already declare the `SUM`-in-`double` divergence.

### Conceptual explanation

TPC-H is eight tables in a snowflake around `lineitem`:

```
region ← nation ← supplier ← partsupp → part
                     ↑           ↑        ↑
         nation ← customer ← orders ← lineitem
```

Cardinalities scale linearly with the scale factor SF, except `nation` (25) and
`region` (5), which are fixed:

| table | rows at SF | columns |
|---|---|---|
| `lineitem` | 6,001,215 × SF | 16 |
| `orders` | 1,500,000 × SF | 9 |
| `partsupp` | 800,000 × SF | 5 |
| `part` | 200,000 × SF | 9 |
| `customer` | 150,000 × SF | 8 |
| `supplier` | 10,000 × SF | 7 |
| `nation` | 25 | 4 |
| `region` | 5 | 3 |

**Three type mappings, and each one is a decision with a consequence:**

1. **`DATE` → `STRING`.** This is already decided and already documented:
   `src/common/date_util.h`'s header says a date *is* a `STRING` holding
   `YYYY-MM-DD`, chosen because ISO-8601 sorts lexicographically and therefore
   inherits zone-map chunk pruning, `scanColumn<std::string>`'s tight comparison
   loop and `BETWEEN` for free. Do not re-litigate it; just make sure
   `l_shipdate`, `l_commitdate`, `l_receiptdate` and `o_orderdate` are typed
   `STRING` and *not* `DOUBLE`.
2. **`DECIMAL(15,2)` → `DOUBLE`.** Forced: there is no exact numeric type. The
   README Limitations bullet already states the consequence ("`SUM`/`AVG`
   accumulate in `double` … a sum beyond 2^53 loses precision where SQLite would
   not … TPC-H SF1 sums stay far below that bound"). What is *new* this week is
   that the same choice sets the comparison tolerance — see Task 5, where an
   absolute 1e-5 tolerance is shown to be unusable for a revenue sum.
3. **`CHAR(1)` / `VARCHAR(n)` → `STRING`.** Free. Note `l_returnflag` and
   `l_linestatus` are single characters with very low cardinality, so
   `DictionaryEncoder` will take them — which is what makes Q1 a good columnar
   showcase in Week 37.

**A second catalog, not a second copy of the first.** `Catalog::Catalog`
resolves each `"file"` against the *catalog file's own directory*:

```cpp
std::filesystem::path catalog_dir =
    std::filesystem::absolute(catalog_path).parent_path();
...
std::string abs_path = (catalog_dir / rel_path).string();
```

That existing property is the whole scale-factor mechanism (Task 3). A catalog
placed at `data/tpch/sf0.01/catalog.json` with `"file": "lineitem.tbl"` finds
its data next to itself, from any working directory. So: **one catalog file per
scale factor, generated, never hand-edited**, and the F1 `catalog.json` at the
repo root is left completely alone. Merging the TPC-H tables into the root
catalog would be the worse choice for a reason this project has hit before —
`compare_against_sqlite.py`'s `load_sqlite()` would then have to create eight
tables it never queries for every one of its ~500 existing diffs.

### Code snippets

The generated per-scale-factor catalog, abbreviated to two tables:

```json
{
  "tables": [
    {
      "name": "lineitem",
      "file": "lineitem.tbl",
      "format": {"delimiter": "|", "header": false, "trailing_delimiter": true},
      "columns": [
        {"name": "l_orderkey",      "type": "INT"},
        {"name": "l_partkey",       "type": "INT"},
        {"name": "l_suppkey",       "type": "INT"},
        {"name": "l_linenumber",    "type": "INT"},
        {"name": "l_quantity",      "type": "DOUBLE"},
        {"name": "l_extendedprice", "type": "DOUBLE"},
        {"name": "l_discount",      "type": "DOUBLE"},
        {"name": "l_tax",           "type": "DOUBLE"},
        {"name": "l_returnflag",    "type": "STRING"},
        {"name": "l_linestatus",    "type": "STRING"},
        {"name": "l_shipdate",      "type": "STRING"},
        {"name": "l_commitdate",    "type": "STRING"},
        {"name": "l_receiptdate",   "type": "STRING"},
        {"name": "l_shipinstruct",  "type": "STRING"},
        {"name": "l_shipmode",      "type": "STRING"},
        {"name": "l_comment",       "type": "STRING"}
      ]
    },
    {
      "name": "region",
      "file": "region.tbl",
      "format": {"delimiter": "|", "header": false, "trailing_delimiter": true},
      "columns": [
        {"name": "r_regionkey", "type": "INT"},
        {"name": "r_name",      "type": "STRING"},
        {"name": "r_comment",   "type": "STRING"}
      ]
    }
  ]
}
```

The `"format"` object is Task 2's; it is shown here because the schema file is
where it lives. **`"format"` must be optional**, defaulting to
`{",", header: true, trailing_delimiter: false}` — that default *is* the current
behaviour, which is what keeps the root `catalog.json` and all 20-odd
`CSVLoader::load` call sites in `tests/` working unchanged.

The single source of truth for the schema is the generator, not the JSON:

```python
# python_tools/tpch_schema.py — ONE definition, consumed by the catalog writer,
# by load_sqlite(), and by the reference-answer typing. Three copies of a
# 60-column schema is precisely the two-paths drift Weeks 26/28/30 each undid.
#
# (swiftql_type, sqlite_type) per column. They are separate because SQLite
# gets the *stricter* declaration on purpose — see Task 5.
TPCH_TABLES = {
    "region": [
        ("r_regionkey", "INT",    "INTEGER"),
        ("r_name",      "STRING", "TEXT"),
        ("r_comment",   "STRING", "TEXT"),
    ],
    # ... nation, part, supplier, partsupp, customer, orders ...
    "lineitem": [
        ("l_orderkey",      "INT",    "INTEGER"),
        # DECIMAL(15,2) -> DOUBLE: forced, no exact numeric type exists.
        ("l_extendedprice", "DOUBLE", "REAL"),
        # DATE -> STRING: date_util.h's documented representation. Typing this
        # DOUBLE does NOT error -- std::stod("1996-01-02") returns 1996.0.
        ("l_shipdate",      "STRING", "TEXT"),
        # ...
    ],
}
```

### Implementation guidance

1. Write `python_tools/tpch_schema.py` first, with all eight tables. Do it by
   transcribing the TPC-H spec's DDL, not from memory — column *order* is
   load-bearing, because `.tbl` is positional and the loader zips fields to
   `schema.column(i)` by index.
2. Add a catalog writer that emits the JSON above. Emit it into the scale
   factor's own directory (Task 3).
3. **Do not touch `catalog.json` at the repo root.** Task 3 gives the CLI and
   the harnesses a `--catalog` path; they already accept one.
4. Cross-check the transcription mechanically before trusting it. The cheap
   check is a column count per table against the table above (16, 9, 5, 9, 8, 7,
   4, 3) plus a `dbgen`-independent one: every `.tbl` line must split into
   exactly that many fields.

**Mistakes specific to this codebase:**

- **Typing a date column `DOUBLE`.** No error, ever — `std::stod` stops at the
  first `-` and yields `1996.0`. Every date predicate then silently answers on a
  year-shaped double. This is the single most likely way to lose a week.
- **Typing a key column `DOUBLE`.** Join keys go through `key_encoding.h`;
  `l_orderkey` as a DOUBLE and `o_orderkey` as an INT will not match, because
  the serialized key encodings differ by type. Every `orderkey` must be `INT`.
- **Assuming `nation`/`region` scale.** They do not. A generator that multiplies
  every row count by SF produces a `nation` table with 0 rows at SF=0.01, and
  then Q5/Q7/Q8 silently return nothing.
- **`Catalog::parseTypeId` throws on `"DATE"` and `"DECIMAL"`** with
  `Unknown type in catalog: ...`. That is the *good* case — it is loud. The
  quiet failure mode is the one above.

### Verification

- `./build/swiftql --catalog data/tpch/sf0.01/catalog.json --query "SELECT COUNT(*) FROM lineitem"` returns 60012 at SF=0.01, and the six scaling tables return `round(base × SF)` while `nation` returns 25 and `region` returns 5.
- A per-column smoke query proves the *types* rather than the counts:
  `SELECT MIN(l_shipdate), MAX(l_shipdate) FROM lineitem` must return two
  `YYYY-MM-DD` strings, not two numbers. If it returns `1992` and `1998`, a date
  column is typed `DOUBLE`.
- `SELECT MIN(l_extendedprice), MAX(l_extendedprice) FROM lineitem` must
  bracket roughly [900, 105000]; a `STRING`-typed price shows up as
  lexicographic min/max instead.
- `--storage-stats` (already in `main.cc`) prints per-table columnar MB; record
  it, Task 3 needs the ladder.

---

## Task 2 — The pipe-delimited loader

### Why it matters

`CSVLoader::load` is the only ingestion path in the engine — the row path uses
it directly and the columnar path is `CSVToColumnar::convert(CSVLoader::load(...))`.
It currently cannot read a `.tbl` file at all, and it fails in **two different
ways, one of them silent**:

```cpp
// src/storage/csv_loader.cc
std::string line;
std::getline(file, line);        // (A) SKIPS LINE 1 UNCONDITIONALLY
while (std::getline(file, line)) {
    if (line.empty()) continue;
    auto fields = splitLine(line);              // (B) delimiter defaults to ','
    if (fields.size() != static_cast<size_t>(schema.size())) {
        throw std::runtime_error("Column count mismatch in CSV row");
    }
    ...
}
```

(A) is the silent one. A `.tbl` file has **no header**, so the first row of
every table is discarded and nothing says so. At SF=0.01 that is one missing
region, one missing customer, one missing lineitem — enough to make every
aggregate wrong by a hair and nothing to make it visible. (B) is loud: a
comma-split `.tbl` line yields one field, `1 != 16`, and you get
`Column count mismatch in CSV row`.

There is a third: `.tbl` lines are **terminated** by `|`, not separated by it,
so a correct pipe split yields `schema.size() + 1` fields with an empty last
one.

Downstream, this is also the week the README's `Commas inside string values not
supported in CSV input` limitation stops binding for TPC-H input: `l_comment`
and `c_comment` are full of commas, and the pipe delimiter is exactly what makes
them loadable. State that in the sweep (Task 8) rather than leaving the
limitation reading as though it still applies everywhere.

### Conceptual explanation

The delimiter and the header flag are **properties of a table's file**, not
global settings and not arguments the caller happens to remember. They therefore
belong on `TableMetadata`, beside `filepath` and `schema`, and travel with it.
That is the same "put it in the type" discipline `TableRef` used in Week 34 and
`SelectStatement::joins` used in Week 26: a global or a defaulted extra argument
lets a call site compile while meaning the wrong thing.

`CSVLoader::load(filepath, schema)` has ~20 call sites, almost all in `tests/`.
Two shapes are available:

| shape | consequence |
|---|---|
| `load(const TableMetadata&)` | Every call site becomes a compiler error — a worklist, which is what this project reaches for when a change *must* be considered everywhere. But most of those sites are tests that already hold a `TableMetadata` (`fm`, `jm`, `m`, `lm`), so the edit is mechanical and the churn is real but small |
| `load(filepath, schema, FileFormat fmt = FileFormat::csv())` | Zero call sites break. The risk is the inverse of Week 34's `TableRef` argument: a site that *should* have passed the table's format silently keeps the CSV default |

**Prefer the second, with one deliberate exception**: give `TableMetadata` the
`FileFormat` field and make `main.cc`'s single production call site pass
`tmeta.format`. There is exactly one production loader call
(`main.cc:367`); the other ~20 are fixtures that genuinely are CSV. Breaking 20
test files to protect one call site is not the minimum code that solves the
problem, and the one site that matters is small enough to verify by eye. Record
that reasoning — it is the opposite call from Week 26/34 and it should be
visible *why*.

**Hardening `parseField` is not optional here, and it is not gold-plating.**
The argument is concrete: this week hand-types roughly 60 columns across eight
tables, and the failure mode of a mistyped column is `std::stod("1996-01-02")
== 1996.0` — a silently wrong value, the exact shape this project has recorded
three times (Week 33's three silent wrong answers, Week 34's `COUNT`-over-empty).
A full-consumption check converts every one of those into a loud error at load
time, in about six lines.

### Code snippets

```cpp
// src/catalog/table_metadata.h
// A file's shape travels with the table, not with the caller. Defaults ARE the
// pre-Week-35 behaviour, so the F1 catalog.json and every tests/ fixture keep
// loading byte-identically without mentioning a format at all.
struct FileFormat {
    char delimiter          = ',';
    bool has_header         = true;
    bool trailing_delimiter = false;   // TPC-H .tbl TERMINATES each field

    static FileFormat csv() { return FileFormat{}; }
    static FileFormat tbl() { return FileFormat{'|', false, true}; }
};

struct TableMetadata {
    std::string name;
    std::string filepath;
    Schema      schema;
    FileFormat  format {};   // defaulted: absent "format" in catalog.json = CSV
};
```

```cpp
// src/storage/csv_loader.cc — the three changes, in place.
std::vector<Row> CSVLoader::load(const std::string& filepath,
                                 const Schema& schema,
                                 const FileFormat& fmt) {
    ...
    std::string line;
    // (A) a header is a PROPERTY, not an assumption. A .tbl has none, and
    // eating line 1 discards a real row with no diagnostic at all.
    if (fmt.has_header) std::getline(file, line);

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        auto fields = splitLine(line, fmt.delimiter);   // (B)

        // (C) TPC-H terminates rather than separates, so a correct split leaves
        // one trailing empty field. Drop it only when the format says to, and
        // only when it really is empty -- an unconditional pop_back would eat a
        // genuine last column whose value happens to be empty.
        if (fmt.trailing_delimiter && !fields.empty() && fields.back().empty()) {
            fields.pop_back();
        }

        if (fields.size() != static_cast<size_t>(schema.size())) {
            // Row number in the message: at 60k rows "Column count mismatch"
            // alone is unactionable.
            throw std::runtime_error(
                "Column count mismatch in " + filepath + " line " +
                std::to_string(line_no) + ": expected " +
                std::to_string(schema.size()) + ", got " +
                std::to_string(fields.size()));
        }
        ...
    }
}
```

```cpp
// src/storage/csv_loader.cc — parseField, hardened.
Value CSVLoader::parseField(const std::string& field, TypeId type) {
    switch (type) {
        case TypeId::INT: {
            size_t pos = 0;
            int64_t v = std::stoll(field, &pos);
            // Full consumption is the whole point: without it a mistyped
            // column converts silently. stod("1996-01-02") is 1996.0, and a
            // date column typed DOUBLE then answers every predicate on a
            // year-shaped number with no error anywhere.
            if (pos != field.size()) {
                throw std::runtime_error(
                    "not an INT: '" + field + "'");
            }
            return Value(v);
        }
        case TypeId::DOUBLE: {
            size_t pos = 0;
            double v = std::stod(field, &pos);
            if (pos != field.size()) {
                throw std::runtime_error("not a DOUBLE: '" + field + "'");
            }
            return Value(v);
        }
        case TypeId::STRING:
            return Value(field);
        ...
    }
}
```

### Implementation guidance

1. `FileFormat` on `TableMetadata`, defaulted. Compile. Nothing should break —
   if something does, you changed the default.
2. `Catalog::Catalog` reads the optional `"format"` object. Use
   `table_json.value("format", json::object())` and then `.value("delimiter",
   ",")` etc., so an absent key is the CSV default rather than an exception.
   Note the JSON delimiter arrives as a **string**; take `[0]` and reject a
   multi-character one loudly, because `splitLine` takes a `char`.
3. Thread `fmt` through `CSVLoader::load` with a default argument; pass
   `tmeta.format` at `main.cc:367`. That is the only production site.
4. Harden `parseField` last, and *expect* it to start throwing — that is it
   finding your Task 1 typos, not a regression.

**Mistakes specific to this codebase:**

- **Making `pop_back()` unconditional.** `l_comment` is the last `lineitem`
  column and is never empty in `dbgen` output, but `r_comment` at some scale
  factors, and any hand-written fixture, can be. Gate it on
  `fmt.trailing_delimiter` **and** on the field actually being empty.
- **Forgetting `CSVToColumnar::convert`.** It takes `(rows, schema)` and is
  downstream of the loader, so it needs no change — but it calls
  `row[c].asDouble()` unconditionally and `ColumnarTable` has no NULL
  representation. That is fine for `dbgen` output, which has no NULLs, and it is
  the reason the validity-mask item below stays declined.
- **`std::stoll` throws `std::invalid_argument`**, which derives from
  `std::logic_error`, **not** `std::runtime_error`. Check that `main.cc`'s
  top-level handler catches `std::exception`; if it only catches
  `std::runtime_error`, a malformed field terminates the process instead of
  printing an error, and `run_swiftql`'s `RuntimeError` path in the harness
  never sees the message.

**The NaN / validity-mask item, addressed rather than carried.** The README's
Limitations bullet on NaN says closing the divergence "belongs to Week 35, which
rewrites the loader anyway". This week **re-declines it, explicitly**:

- A validity mask means giving `ColumnarTable` a NULL representation, which
  touches every scan, every encoding (`DictionaryEncoder`, `RLEColumn`) and every
  zone map (`ChunkPruner`'s min/max). That is a storage-layer week, not a
  harness week, and spending this week's budget there loses the thing that makes
  Week 36 measurable at all.
- The divergence is, in the README's own words, "in the *justification*, not in
  any answer on committed data", and `dbgen` emits neither NULLs nor NaNs, so
  TPC-H does not make it more urgent.
- What the loader rewrite *does* buy for free: the hardened `parseField` above
  makes the cell `nan` a **load error** on the `.tbl` path instead of a
  `Value(NaN)`, because `std::stod("nan")` consumes the whole field and returns
  NaN today. If you want the hole narrowed at zero cost, add an explicit
  `std::isnan` / `std::isinf` rejection in the `DOUBLE` case — but note it would
  change CSV behaviour too, so it is a *decision*, not a freebie, and it belongs
  in the sweep of Task 8 if taken.

Whichever you choose, the Limitations bullet must stop saying "belongs to Week
35" and start saying what Week 35 did. That is the standing rule.

### Verification

- `SELECT COUNT(*) FROM region` returns **5**, not 4. That single query is the
  regression test for the unconditional header skip, and it is the one to write
  first because it fails silently today.
- `SELECT * FROM region` returns five rows whose `r_regionkey` values are
  `0,1,2,3,4` — the header-skip bug removes key 0 specifically.
- Round-trip the F1 data: `python3 python_tools/compare_against_sqlite.py` must
  still be all-green after the `FileFormat` change, with no query touched. That
  is the proof the default is the old behaviour.
- A negative test in `tests/test_storage.cc`: a fixture `.tbl` with a
  deliberately mistyped `DOUBLE` date column must now throw, and the message
  must name the offending field.

---

## Task 3 — The scale-factor workflow

### Why it matters

"Reproducible" is the checkpoint word, and today the project is not. Look at
`generate_data.py`: it `import random` and never seeds it. Two runs of
`python3 python_tools/generate_data.py --rows 100000` produce **different data**,
so every benchmark number and every hand-checked row count in the repo is
attached to a dataset that cannot be recreated. That is a one-line fix and it is
the difference between this week's checkpoint passing and reading as though it
did.

The second half is the one Week 28 handed here by name. Its randomized
*result* differencing never completed because the `--no-optimize` leg of a
multi-way self-join over 10k `laps` rows takes tens of seconds (35.6 s measured
on one query), so batches of 100, 40 and 14 queries all timed out. The fix is
not a faster optimizer; it is a **smaller fixture**, and the reason it belongs
to this week is that a scale-factor workflow is what makes a 500-row `laps` a
first-class artifact rather than "a second catalog to keep in sync". Task 7
spends that fixture.

So this task's real deliverable is a single concept that covers both datasets:
**a dataset is a directory containing its data files and its own
`catalog.json`.** `Catalog::Catalog` already resolves `"file"` relative to the
catalog's own directory, so a directory is self-contained and selecting one is
just `--catalog <dir>/catalog.json`. Nothing in the engine changes.

### Conceptual explanation

```
data/
├── laps.csv                    # unchanged: the default F1 dataset
├── drivers.csv
├── f1/
│   └── sf-small/               # 500 laps, 20 drivers — Task 7's fixture
│       ├── catalog.json
│       ├── laps.csv
│       └── drivers.csv
└── tpch/
    ├── sf0.01/
    │   ├── catalog.json        # generated by Task 1's writer
    │   ├── region.tbl ... lineitem.tbl
    └── sf0.1/
        └── ...
```

`catalog.json` at the repo root is **untouched**, so every existing invocation,
every test fixture and both Python harnesses keep working with no argument
changes. That is the property that makes this cheap.

**Where the TPC-H data comes from, and the honesty this forces.** Two options,
and they are not equivalent:

| source | what you can claim |
|---|---|
| Official `dbgen` | The data is the spec's, so the published SF1 **answer set** is a valid reference and Week 36 can say "matches the reference answers" |
| A Python generator in `generate_data.py`'s style | The *schema* and the referential integrity are right; the value distributions are not the spec's, so the published answers do **not** apply and SQLite-over-the-same-data is the only oracle |

Take `dbgen` if it is available and vendor nothing (it is a separate,
differently-licensed codebase — a `Makefile` target that builds it from a local
checkout, plus a clear error if it is absent, is the right amount). Fall back to
the Python generator otherwise. **Either way, record which one produced the data
in the run report**, because it changes what Week 36's checkpoint sentence
("match reference results within numeric tolerance") is allowed to mean. This is
the single most likely place for Week 36 to overclaim.

**Which scale factors are actually reachable is a measurement, not a choice.**
`main.cc` loads every table a query touches into `std::vector<Row>` *first*, and
only then converts to columnar and calls `table_rows.clear()`. So peak memory is
the **row** form, and a `Row` is a `std::vector<Value>` of variants — tens of
bytes per cell plus a heap allocation per string. `lineitem` at SF=1 is 6M rows
× 16 columns; that peak is in the gigabytes. Do not guess: `--storage-stats`
already exists in `main.cc` and prints per-table columnar MB. Measure a ladder
(SF 0.01 / 0.1 / 1) and publish it — Week 36's third bullet is literally
"Document supported scale and memory limits", and this is the measurement that
bullet is waiting for.

### Code snippets

```python
# python_tools/generate_data.py — the reproducibility fix and the out-dir.
# --seed defaults to a FIXED value: an unseeded generator makes every number in
# the README attach to data nobody can recreate.
parser.add_argument("--seed", type=int, default=20250101)
parser.add_argument("--out-dir", default="data",
                    help="dataset directory; a catalog.json is written beside "
                         "the CSVs so the directory is self-contained")
args = parser.parse_args()
random.seed(args.seed)          # BEFORE generate_drivers / generate_laps
```

```python
# python_tools/dataset.py — one writer, both datasets. The catalog is DERIVED
# from the schema definition, never hand-edited: a hand-edited second catalog is
# the "second catalog to keep in sync" the Week 28 note said to avoid.
def write_catalog(out_dir, tables):
    """tables: [(name, filename, [(col, swiftql_type)], FileFormat-ish dict)]"""
    catalog = {"tables": [
        {
            "name": name,
            "file": filename,                 # RELATIVE: Catalog resolves it
                                              # against this catalog's own dir
            **({"format": fmt} if fmt else {}),   # omitted => CSV default
            "columns": [{"name": c, "type": t} for c, t in cols],
        }
        for name, filename, cols, fmt in tables
    ]}
    with open(os.path.join(out_dir, "catalog.json"), "w") as f:
        json.dump(catalog, f, indent=2)
```

```bash
# The whole workflow, three commands. Each is idempotent given its seed.
python3 python_tools/generate_data.py --rows 500 --seed 20250101 \
        --out-dir data/f1/sf-small          # Task 7's fixture
python3 python_tools/generate_tpch.py --scale 0.01 --out-dir data/tpch/sf0.01
python3 python_tools/generate_tpch.py --scale 0.1  --out-dir data/tpch/sf0.1
```

### Implementation guidance

1. Seed `generate_data.py` first and regenerate nothing. The committed
   `data/laps.csv` stays as it is — `compare_against_sqlite.py` diffs SwiftQL
   against SQLite over *the same file*, so it is data-independent, but some C++
   fixtures under `tests/data/` are not, and there is no reason to disturb them.
   Note in the commit message that the committed CSVs predate the seed and are
   therefore not reproducible from it; that is honest and costs nothing.
2. Add `--out-dir` and the catalog writer. Verify by pointing the CLI at the new
   directory and running one query.
3. Generate `sf-small` for F1. **500 rows of `laps`, 20 drivers** — the Week 28
   note names 500 specifically, and the point is that `--no-optimize`'s cost is
   dominated by the unfiltered scan, so 20× fewer rows is roughly 20× less of
   the dominant term.
4. Generate TPC-H at 0.01 and 0.1. Measure the memory ladder with
   `--storage-stats` and record it in the run report.

**Mistakes specific to this codebase:**

- **Regenerating `data/laps.csv` with the new seed.** The benchmark tables in
  the README were measured on the committed data; silently replacing it makes
  every published number unreproducible in the other direction.
- **Sorting.** `generate_data.py` sorts `laps` by `season` by default, and the
  docstring explains why: the benchmark docs assume clustered seasons so
  zone-map pruning can skip chunks. The `sf-small` fixture must keep that
  default, or Task 7's randomized queries silently exercise a different pruning
  regime than the rest of the suite.
- **Assuming `nation`/`region` scale with SF** — see Task 1. At SF=0.01 they are
  still 25 and 5.
- **Writing an absolute `"file"` path into a generated catalog.** It works on
  your machine and breaks the moment the repo moves. Relative is not a style
  preference here; it is what makes the directory self-contained.

### Verification

- Determinism: generate `sf-small` twice into two directories and `diff -r` them.
  Byte-identical or the seed is not doing its job.
- Self-containment: `cd /tmp && /path/to/build/swiftql --catalog
  /path/to/data/tpch/sf0.01/catalog.json --query "SELECT COUNT(*) FROM nation"`
  returns 25 from an unrelated working directory.
- Non-interference: `python3 python_tools/compare_against_sqlite.py` from the
  repo root is unchanged and all-green.
- The Week 28 claim, checked directly: time one `--no-optimize` multi-way
  self-join on `data/laps.csv` and the same query on `data/f1/sf-small`. If the
  small one is not roughly two orders of magnitude faster, Task 7's budget does
  not exist and you need to know now, not in Task 7.

---

## Task 4 — Parameterized queries, warmups, repetitions

### Why it matters

This is the mechanism Week 36 runs correctness through and Week 37 runs
performance through. Two properties matter more than any feature: the same
command produces the same answer twice (reproducible), and the number it reports
measures what it claims to measure.

The second is where this codebase has a specific, easy-to-trip wire. Read
`main.cc`'s query loop: `table_rows` is declared **inside** it, so *every query
in every invocation reloads every table it touches from disk*. Wall-clock
timing therefore measures CSV parsing, which the README's methodology explicitly
excludes ("CSV load excluded from all timers, consistent with TPC-H benchmark
methodology"). `benchmark.py` already does the right thing and parses the
`Execution:` line out of `--explain-analyze`; the TPC-H runner must do the same
and must not invent a wall-clock number beside it.

The second wire is the result cache. `CacheKey{query, storage, execution,
no_optimize}` is keyed on the raw SQL text, and the cache lives for the process.
So *repetitions of the same query inside one process return a cached result from
repetition 2 onward* unless `--no-cache` is passed. A repetition loop without
`--no-cache` measures an `unordered_map` lookup and reports it as query latency.

### Conceptual explanation

**Parameterized queries.** TPC-H queries are templates with substitution
parameters — Q1's `DELTA`, Q2's `SIZE`/`TYPE`/`REGION`, Q6's
`DATE`/`DISCOUNT`/`QUANTITY`. The spec defines *validation* parameter values,
which are the ones the published answer set corresponds to, and a `qgen` seed
for randomized ones. Those are two different uses and must not share a default:

- **Correctness runs use the validation parameters, always.** They are fixed,
  they are what a reference answer means, and a randomized parameter makes a
  correctness result unrepeatable.
- **Benchmark runs (Week 37) may use seeded random parameters**, to stop a
  single parameter's data distribution from standing in for the query.

Store the template and the parameters separately, and substitute by **named
placeholder**, never by positional `%s` or `.format()` — a TPC-H query body
contains `{` and `%` characters inside `LIKE` patterns, and `str.format` will
either eat them or raise.

**Warmups.** Be precise about what a warmup can mean here, because each CLI
invocation is a **fresh process**. Across invocations, a warmup warms the OS
page cache and nothing else — the result cache, the statistics cache
(`catalog.setStats` is guarded by `hasStats`) and the allocator all die with the
process. Within one invocation, `--query` may be repeated (`args.queries` is a
vector) and *then* warmup means something: statistics are computed once, and the
result cache would serve repetition 2 if you let it. Say which one you are
doing. The honest default is: one warmup invocation whose timing is discarded,
then N timed invocations, and a note in the report that the process is cold each
time by design.

**Repetitions.** Report the **median** of N and the min, not the mean.
`benchmark.py` uses `avg of 5`; a mean over 5 samples is dominated by any single
scheduler hiccup, and TPC-H per-query times here will span three orders of
magnitude. If you change the statistic, do not silently change
`benchmark.py`'s — either leave it alone or change both and say so (the standing
sweep rule).

### Code snippets

```python
# python_tools/tpch_queries.py — templates and parameter sets, kept apart.
# :NAME placeholders, substituted by explicit replace: a TPC-H body contains
# '%' inside LIKE patterns and '{' nowhere useful, so both %-formatting and
# str.format are wrong tools here.
TEMPLATES = {
    "q6": """
        SELECT SUM(l_extendedprice * l_discount) AS revenue
        FROM lineitem
        WHERE l_shipdate >= ':DATE'
          AND l_shipdate < ':DATE_PLUS_1Y'
          AND l_discount BETWEEN :DISCOUNT - 0.01 AND :DISCOUNT + 0.01
          AND l_quantity < :QUANTITY
    """,
}

# The spec's validation parameters. Correctness runs use ONLY these -- a random
# parameter makes a correctness result unrepeatable, which is the opposite of
# this week's checkpoint.
VALIDATION_PARAMS = {
    "q6": {"DATE": "1994-01-01", "DATE_PLUS_1Y": "1995-01-01",
           "DISCOUNT": "0.06", "QUANTITY": "24"},
}

def render(qid, params):
    sql = TEMPLATES[qid]
    # longest name first: :DATE_PLUS_1Y must not be eaten by :DATE
    for name in sorted(params, key=len, reverse=True):
        sql = sql.replace(":" + name, params[name])
    if ":" in sql.replace("::", ""):        # cheap unsubstituted-placeholder guard
        raise ValueError(f"{qid}: unsubstituted parameter in rendered SQL")
    return " ".join(sql.split())
```

```python
# python_tools/run_tpch.py — timing that measures what it claims to.
def time_once(catalog, mode_args, sql):
    """Return the engine's own Execution time in microseconds, or None.

    Wall clock is NOT used: main.cc reloads every table the query touches on
    every invocation (table_rows is declared inside the per-query loop), and the
    README's methodology excludes CSV load from all timers. benchmark.py already
    parses this line; same convention, deliberately.
    """
    cmd = ["./build/swiftql", "--catalog", catalog] + mode_args + [
        # --no-cache is MANDATORY under repetition. CacheKey is
        # {query, storage, execution, no_optimize}, so without it repetition 2
        # onward measures an unordered_map lookup and calls it query latency.
        "--no-cache", "--explain-analyze", "--query", sql,
    ]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None
    m = re.search(r'Execution:\s+([\d.]+)µs', r.stdout)
    return float(m.group(1)) if m else None


def time_query(catalog, mode_args, sql, warmups=1, reps=5):
    for _ in range(warmups):
        time_once(catalog, mode_args, sql)      # discarded: warms the page cache
                                                # only -- the process is cold again
    samples = [t for _ in range(reps)
               if (t := time_once(catalog, mode_args, sql)) is not None]
    if not samples:
        return None
    samples.sort()
    return {"median_us": samples[len(samples) // 2],
            "min_us": samples[0],
            "n": len(samples)}
```

### Implementation guidance

1. Transcribe the 22 templates with their **validation** parameters. Leave the
   dialect port to Week 36 — this week only needs the templates to exist and
   render; a template that SwiftQL cannot yet parse is a *recorded* outcome, not
   a blocker (Task 6 has a state for it).
2. Build the renderer with the longest-name-first substitution and the
   unsubstituted-placeholder guard. That guard catches the single most common
   error in this kind of harness.
3. Build `time_once` / `time_query` around the `Execution:` line.
4. Wire the runner to take `--catalog`, `--scale`, `--modes`, `--reps`,
   `--warmups`, and to emit a machine-readable per-query record (JSON) as well
   as the human table. Task 6 consumes that record.

**Mistakes specific to this codebase:**

- **Omitting `--no-cache`.** The most expensive mistake available this week,
  because it produces plausible numbers.
- **Timing with wall clock.** It measures CSV parsing, which dominates at TPC-H
  sizes and is excluded by the README's own methodology.
- **Passing several `--query` args to amortize the load.** It does not: the
  loader runs inside the per-query loop. What it *does* change is that `multi`
  mode prefixes each result with `\n-- <query>\n`, which the existing
  `parse_swiftql_output` does not understand — so multi-query invocations
  silently mis-parse. One query per invocation.
- **Assuming `--explain-analyze` and a plain run cost the same.** They do not
  quite (per-node timers), but the `Execution:` line is the same quantity
  `benchmark.py` has always reported, so the comparison is internally
  consistent. Do not mix the two sources.

### Verification

- Render all 22 templates and assert no `:` placeholder survives.
- Run one query with `--reps 5`. The five samples' spread should be a few
  percent; if repetition 2 onward is ~100× faster than repetition 1, `--no-cache`
  is missing.
- Run the whole set twice with the same seed and scale factor and diff the JSON
  records' *result* fields (not the timings). Identical, or "reproducible" is
  not true.

---

## Task 5 — Reference comparison, and what the oracle cannot check

### Why it matters

`compare_against_sqlite.py` is the correctness oracle for this project and has
been since Phase 1. Pointing it at TPC-H breaks it in four places, one of which
would make **Q1 fail against a correct answer**. Finding that in Week 36, in the
middle of porting queries, is the expensive version.

### Conceptual explanation

**(1) `load_sqlite()` is hard-coded to two tables.** It contains literal
`CREATE TABLE laps (...)` / `CREATE TABLE drivers (...)` and literal
9-placeholder and 5-placeholder `INSERT`s, with `LAPS_CSV` / `DRIVERS_CSV` as
module constants. It must become catalog-driven: read the same `catalog.json`
the engine reads, create each table from it, and bulk-load each file with the
same delimiter/header rules Task 2 gave the loader. Deriving both sides from one
schema definition (`tpch_schema.py`) is the point — a hand-maintained SQLite DDL
beside a hand-maintained catalog is two copies that will disagree, and a
disagreement here reads as an engine bug.

Give SQLite the *stricter* declaration where you can: `INTEGER`/`REAL`/`TEXT`
per column, so a mistyped `.tbl` field shows up as a SQLite affinity surprise
too, giving you two independent chances to catch a Task 1 typo.

**(2) The numeric tolerance is wrong for TPC-H, and it is wrong in the
dangerous direction — too tight.** Today:

```python
if abs(x - y) > 1e-5:   # rows_equal, ABSOLUTE
```

That is fine for F1 speeds around 300. It is not fine for a revenue sum. Two
independent error sources push past it:

- **Printing.** `Value::toString` formats a DOUBLE with `%.15g`
  (`src/common/value.cc:110`). Fifteen significant digits is one short of a
  round-trip, so the text SwiftQL hands the harness already carries ~1e-15
  *relative* error. On a sum of 2.3e9 that alone is ≈ 2e-6 absolute — the same
  order as the whole tolerance.
- **Summation order.** SwiftQL and SQLite accumulate in different orders over
  ~6e4 rows (more at higher SF), so the true results differ by roughly
  `n · eps · magnitude` ≈ 1e-11 relative, ≈ 0.03 absolute on that same sum.

`0.03 > 1e-5`, so Q1 fails on a correct answer. The fix is a **relative**
tolerance with a small absolute floor for values near zero, applied to the TPC-H
comparison **only** — the 168 F1 queries pass at 1e-5 absolute today and
loosening them globally silently weakens every diff in the file.

**(3) `parse_swiftql_output` cannot read TPC-H output.** It does
`headers = lines[0].split()` and `re.split(r'  +', line)` and then
**silently drops any row whose field count does not match the header count**.
Three TPC-H-specific ways that goes wrong:

- **A computed output column's name contains spaces.** `aggregateOutputName`
  *is* `exprToString`, and `exprToString` renders a `BinaryExpr` as
  `"(" + left + " " + op + " " + right + ")"` — with spaces, and the parens are
  documented as load-bearing. So Q1's `SUM(l_extendedprice * (1 - l_discount))`
  becomes the single output column name
  `SUM((l_extendedprice * (1 - l_discount)))`, which `.split()` shreds into
  seven "headers". Every row then fails the length check and is dropped, and the
  diff reports an empty SwiftQL result. A `SELECT ... AS revenue` alias in the
  ported query dodges it (the AST has `alias` for exactly this), but the harness
  must not *depend* on the port remembering.
- **An empty string field.** `printResults` pads to a data-derived width, so an
  empty value renders as pure padding and merges with its neighbour's gap. Field
  count drops by one; row silently dropped.
- **A string value containing two consecutive spaces** splits into two fields.

The right fix is to stop parsing aligned text: add a `--format tsv` output mode
to `printResults` — a header line and one tab-separated line per row, no
padding — and have the TPC-H harness pass it. That is a dozen lines in
`main.cc`, it removes all three ambiguity classes at once, and because it is a
**new flag with the aligned form still the default**, the existing suites and
`test_new_queries.py` are untouched. Task 7 needs it too, for a different reason.

**(4) The oracle has two blind spots, and a TPC-H harness has to state them.**
Both are already recorded in the repo; the harness's job is to make the silence
legible rather than to fix them:

- **It cannot hold a query that errors.** `run_swiftql` raises on a non-zero
  exit, so a refused query produces no rows to diff. That is why the file has a
  parallel `run_rejection_suite` and why every capability boundary is asserted
  by *message* in the modes that refuse it. A TPC-H query the dialect cannot
  express is therefore not "a failure" and not "a pass" — it is a third state,
  and Task 6 gives it a name.
- **SQLite cannot parse a derived-table column alias list.** `FROM (SELECT ...)
  AS d (c1, c2)` is legal in SwiftQL (Week 34) and a syntax error in SQLite, so
  that feature has C++ coverage only. If a ported TPC-H query uses the alias-list
  form (Q15's `revenue` view rewrite is the tempting place), the oracle cannot
  check it — rewrite the port to alias inside the body instead, and record the
  choice.

Add a third, discovered above: **a query whose true answer is zero rows is
indistinguishable from a parse failure**, because `parse_swiftql_output` returns
`[]` for both (`printResults` emits only header + separator for an empty result,
so `len(lines) < 3`). With `--format tsv` this stops being true, which is
another reason to take that fix.

### Code snippets

```python
# python_tools/compare_against_sqlite.py — load_sqlite, catalog-driven.
SQLITE_TYPE = {"INT": "INTEGER", "DOUBLE": "REAL", "STRING": "TEXT"}

def load_from_catalog(catalog_path):
    """Build an in-memory SQLite mirror of whatever catalog.json describes.

    Replaces the hard-coded laps/drivers DDL: one schema definition feeds both
    engines, so a mistyped column cannot mean two different things on the two
    sides of the diff.
    """
    base = os.path.dirname(os.path.abspath(catalog_path))
    spec = json.load(open(catalog_path))
    conn = sqlite3.connect(":memory:")
    conn.row_factory = sqlite3.Row

    for t in spec["tables"]:
        cols = t["columns"]
        conn.execute("CREATE TABLE {} ({})".format(
            t["name"],
            ", ".join(f'{c["name"]} {SQLITE_TYPE[c["type"]]}' for c in cols)))

        # Same delimiter/header rules the C++ loader got in Task 2, read from
        # the same place. Two readers of one declaration, not two declarations.
        fmt = t.get("format", {})
        delim   = fmt.get("delimiter", ",")
        header  = fmt.get("header", True)
        trailer = fmt.get("trailing_delimiter", False)

        placeholders = ",".join("?" * len(cols))
        with open(os.path.join(base, t["file"])) as f:
            if header:
                next(f)
            rows = []
            for line in f:
                line = line.rstrip("\n")
                if not line:
                    continue
                fields = line.split(delim)
                if trailer and fields and fields[-1] == "":
                    fields.pop()
                rows.append([_coerce(v, c["type"]) for v, c in zip(fields, cols)])
            conn.executemany(f"INSERT INTO {t['name']} VALUES ({placeholders})", rows)
    conn.commit()
    return conn
```

```python
# python_tools/compare_against_sqlite.py — tolerance.
def rows_equal(a, b, rel_tol=0.0, abs_tol=1e-5):
    """Compare normalized row lists.

    Default is ABSOLUTE 1e-5, unchanged: the 168 F1 queries pass at it today and
    loosening it globally would silently weaken every diff in this file.

    TPC-H callers pass rel_tol. Absolute 1e-5 is unusable there for two
    independent reasons, both measurable rather than theoretical:
      * Value::toString prints a DOUBLE with %.15g -- one digit short of a
        round trip -- so the text already carries ~1e-15 RELATIVE error, which
        on a revenue sum near 2.3e9 is ~2e-6 ABSOLUTE on its own.
      * SwiftQL and SQLite accumulate the same sum in different orders over
        ~6e4 rows, differing by ~n*eps ~ 1e-11 relative, ~0.03 absolute there.
    0.03 > 1e-5, so Q1 would fail on a CORRECT answer.
    """
    if len(a) != len(b):
        return False
    for row_a, row_b in zip(a, b):
        if len(row_a) != len(row_b):
            return False
        for x, y in zip(row_a, row_b):
            if isinstance(x, float) and isinstance(y, float):
                if abs(x - y) > max(abs_tol, rel_tol * max(abs(x), abs(y))):
                    return False
            elif x != y:
                return False
    return True
```

Suggested TPC-H setting: `rel_tol=1e-9, abs_tol=1e-6`. That is four orders of
magnitude above the printing and summation noise (so it never fires spuriously)
and many orders below any arithmetic defect this engine could have (a wrong
`1 - l_discount` is a percent-level error, not a 1e-9 one). Write that reasoning
next to the constant; a tolerance without a derivation gets loosened by the next
person who sees a red test.

### Implementation guidance

1. `--format tsv` in `main.cc` first — everything else in this task is easier
   once output parsing is unambiguous. Keep the aligned printer as the default
   and do not touch `parse_swiftql_output`.
2. Make `load_sqlite` catalog-driven and prove it by pointing it at the **root**
   `catalog.json` and re-running the existing suite. All-green, no query
   changed: that is the proof the refactor is behaviour-preserving before it is
   asked to do anything new.
3. Add the `rel_tol` parameter with the default that keeps existing callers
   byte-identical.
4. Only then point it at `data/tpch/sf0.01/catalog.json`.

**Mistakes specific to this codebase:**

- **Loosening the default tolerance instead of adding a parameter.** It
  weakens 168 queries to fix 22, and nothing will ever tell you.
- **`conn.execute` per row.** `load_sqlite` does that today for 10k rows; at
  SF=0.1 `lineitem` is 600k and per-row `execute` will dominate the run.
  `executemany` inside one transaction.
- **Comparing by column name.** `normalize()` builds a tuple from
  `row.values()`, i.e. **positionally**, and its docstring already warns that
  the dict *keying* collapses duplicate names. Since SwiftQL's computed column
  names (`SUM((a * (1 - b)))`) will never equal SQLite's, positional is the only
  workable comparison — do not "fix" it into a name-matched one.
- **The `STRING`-year question, and a coincidence to make deliberate.** Week 25
  handed forward that `SUBSTRING(d, 1, 4)` yields a `STRING` year where the TPC-H
  answers show an integer, and asked Week 36 to decide whether the harness
  normalizes or the dialect grows a conversion. Note that `normalize()`'s
  `coerce` already does `round(float(v), 6)` with a string fallback, so `'1995'`
  and `1995` **already compare equal** — the harness normalizes today, *by
  accident*. Record that explicitly this week so the Week 36 decision is a
  decision rather than an artefact nobody noticed.

### Verification

- Existing suite green after the `load_sqlite` refactor, with zero query edits.
- `--format tsv` round-trip: for a query with a computed column, the TSV parse
  and the aligned parse must return the same rows. For Q1-shaped SQL, the
  aligned parse returns `[]` and the TSV parse returns 4 rows — that difference
  *is* the bug this fixes, and it is worth capturing as a test.
- Tolerance calibration, both directions: Q1 must pass at `rel_tol=1e-9`, and a
  deliberately corrupted expected value (`× 1.000001`) must still **fail**. A
  tolerance that passes everything is not a tolerance.
- Point the oracle at the root catalog and at the TPC-H catalog in the same
  session; neither run may alter the other's result.

---

## Task 6 — Reporting mode coverage honestly

### Why it matters

This is the task the week exists for. Week 36 will produce a number, that number
will go in the README, and it will be read as the phase's headline result. Three
facts already on the page make an unqualified "22/22" false:

1. **Two capability boundaries are open.** `IN (subquery)` (Week 32), correlated
   subqueries (Weeks 33/34) and derived tables (Week 34) are **refused on the
   Volcano path** — `Planner::plan` builds exactly one `HashJoinNode` out of
   `stmt.joins` and runs no `LogicalPlanBuilder`, so it can hold neither a second
   join nor a relation that is a plan. Multi-way joins have the same shape. The
   README already counts the cost: **56 queries diffed in two modes against 168
   in four**. Most TPC-H queries are multi-way joins, so *most of the 22 will be
   two-mode queries*. A report that says "22/22" without saying "in how many
   modes" converts a documented, deliberate limitation into a silent claim of
   full coverage.
2. **A refusal is not a pass and not a failure.** The oracle cannot hold a query
   that errors; `run_swiftql` raises. So "SwiftQL refused it on Volcano" has to be
   a *third* outcome, and it only counts as coverage when the refusal was
   asserted **by message** — which is precisely the discipline
   `run_rejection_suite` already enforces and the reason every capability
   boundary in `main()` is asserted on both halves.
3. **Q22 is not claimed closed.** Week 34's note is explicit: Q22's correlated
   component is a `NOT EXISTS` that Week 33 already decorrelated, while what
   Q22 additionally needs is a derived table; Q22 is "**not** claimed closed on
   the strength of the correlated-scalar rewrite alone: verify it against the
   ported query in Week 36 and record which half was which." Making that
   verifiable rather than assumed is this task's second half.

### Conceptual explanation

**Model the outcome as a per-(query, mode) cell, not a per-query boolean.** Four
modes, named exactly as `compare_against_sqlite.py::main` names them, so the two
files agree:

| mode key | flags |
|---|---|
| `row-volcano` | *(none — the defaults)* |
| `col-volcano` | `--storage columnar` |
| `col-vec` | `--execution vectorized --storage columnar` |
| `col-vec-noopt` | `--execution vectorized --storage columnar --no-optimize` |

and six outcomes, each with a distinct meaning:

| outcome | meaning | counts toward coverage? |
|---|---|---|
| `MATCH` | rows equal the oracle within tolerance | **yes** |
| `MISMATCH` | rows differ | no — a defect |
| `REFUSED_EXPECTED` | errored, and the message contains the boundary's expected substring | **yes** — the boundary is under test |
| `REFUSED_UNEXPECTED` | errored with a message nobody predicted | no — either a defect or a stale expectation |
| `UNPORTED` | the dialect cannot express this query yet | no — Week 36 work, recorded not hidden |
| `ORACLE_BLIND` | SwiftQL answered but SQLite cannot express the query | no — see Task 5's blind spots |

The rules that make the report honest fall straight out:

- A query is **"correct in mode M"** only on `MATCH`. `REFUSED_EXPECTED` is
  *boundary* coverage, not correctness coverage, and the two are reported on
  separate lines. Collapsing them is exactly how "22/22 in four modes" gets
  written down.
- The headline is a **pair**: how many queries answered correctly, and in how
  many modes each. Never one number.
- **The 56 / 168 counts are computed, not typed.** The README states them today
  as prose; once the harness computes the mode census, the prose can cite the
  harness and stop going stale. That is the same argument Week 34 used for
  counting the deferral cost rather than describing it.

**Q22's provenance, made mechanical.** `--explain` prints the logical plan, and
the node *names* carry the semantics on purpose — `LogicalJoin::explain` returns
`LogicalSemiJoin [`, `LogicalAntiJoin [`, `LogicalAntiJoin [NOT IN, `,
`LogicalLeftJoin [` or `LogicalJoin [`, and a derived relation prints
`LogicalDerived [<alias>, ...]`. So "which half was which" is a **plan
fingerprint**, not a judgement call. Capture `--explain` once per query
alongside the result, extract the multiset of node kinds, and store it in the
per-query record. For Q22 that record should show an **`LogicalAntiJoin`** (Week
33's `NOT EXISTS` decorrelation) *and* a **`LogicalDerived`** (the `custsale`
relation), and **no** `LogicalLeftJoin` from a correlated-scalar rewrite. If it
shows something else, Week 34's note was wrong about which half is which and you
have found that out mechanically instead of by argument.

That fingerprint pays twice: it is also the raw material for Week 37's
optimizer-impact numbers and for the `join-ordering=skipped (outer join)`
accounting, since Week 34's LEFT-join rewrite turns reordering off for the whole
tree.

### Code snippets

```python
# python_tools/tpch_report.py
MODES = [
    ("row-volcano",    []),
    ("col-volcano",    ["--storage", "columnar"]),
    ("col-vec",        ["--execution", "vectorized", "--storage", "columnar"]),
    ("col-vec-noopt",  ["--execution", "vectorized", "--storage", "columnar",
                        "--no-optimize"]),
]

# Boundaries this project has already decided and already pins by message.
# Substrings, matched the way run_rejection_suite matches them -- "it failed" is
# not the property under test; "it failed for the stated reason" is.
VOLCANO_BOUNDARIES = [
    "multi-way joins are not supported on the Volcano path",
    "IN (subquery)",                       # Week 32's message, exact text at the call site
    "correlated subqueries are decorrelated to a semi-join",
    "derived tables (FROM (subquery)) are not supported on the Volcano path",
]

def classify(qid, mode_key, rc, stdout, stderr, oracle_rows, tol):
    if rc != 0:
        # A refusal counts as coverage ONLY when it is the refusal we predicted.
        # An unexplained error is not "the boundary working"; it is unknown.
        for msg in VOLCANO_BOUNDARIES:
            if msg in stderr:
                return ("REFUSED_EXPECTED", msg)
        return ("REFUSED_UNEXPECTED", stderr.strip().splitlines()[0][:120])
    rows = parse_tsv(stdout)               # Task 5's --format tsv
    if oracle_rows is None:
        return ("ORACLE_BLIND", None)      # SQLite cannot express this query
    ordered = "ORDER BY" in qid_sql(qid).upper()
    ok = rows_equal(normalize(rows, ordered),
                    normalize(oracle_rows, ordered),
                    rel_tol=tol.rel, abs_tol=tol.abs)
    return (("MATCH" if ok else "MISMATCH"), None)
```

```python
# The report. Note what it REFUSES to print: a bare "22/22".
def render_summary(cells):
    """cells: {(qid, mode_key): (outcome, detail)}"""
    correct_modes = {q: sum(1 for m, _ in MODES
                            if cells[(q, m)][0] == "MATCH")
                     for q in QUERY_IDS}

    answered   = [q for q, n in correct_modes.items() if n > 0]
    four_mode  = [q for q, n in correct_modes.items() if n == 4]
    two_mode   = [q for q, n in correct_modes.items() if n == 2]
    unported   = [q for q in QUERY_IDS
                  if any(cells[(q, m)][0] == "UNPORTED" for m, _ in MODES)]

    # Boundary coverage is counted SEPARATELY from correctness. A refusal is
    # the boundary working, not the query answering, and merging the two is
    # exactly how "22/22 in four modes" gets written down.
    pinned = sum(1 for c, _ in cells.values() if c == "REFUSED_EXPECTED")
    unknown = [k for k, (c, _) in cells.items() if c == "REFUSED_UNEXPECTED"]

    print(f"TPC-H correctness @ {SCALE}, oracle = SQLite over the same data")
    print(f"  answered correctly:        {len(answered)}/22")
    print(f"    in all four modes:       {len(four_mode)}")
    print(f"    in the two vectorized modes only: {len(two_mode)}")
    print(f"  not yet ported:            {len(unported)}  {unported}")
    print(f"  query x mode cells:        {4 * len(QUERY_IDS)}")
    print(f"    Volcano refusals pinned by message: {pinned}")
    if unknown:
        print(f"  !! unexplained errors:     {unknown}")   # never silent
```

### Implementation guidance

1. Build the cell matrix first and print it raw (a 22 × 4 grid of outcome
   names). Read it before writing any summary line — the summary is a lossy view
   and you want to see the loss.
2. Take the `VOLCANO_BOUNDARIES` substrings from the **actual message strings in
   the source**, not from this document. They are pinned in
   `compare_against_sqlite.py`'s `WEEK32_SEMI_JOIN_VOLCANO_REJECTED`,
   `WEEK33_DECORRELATED_VOLCANO_REJECTED`, `WEEK34_DERIVED_TABLE_VOLCANO_REJECTED`
   and `MULTIWAY_VOLCANO_REJECTED`; reuse those lists rather than retyping them,
   so a message change breaks one place.
3. Capture `--explain` per (query, mode) into the record. Extract node kinds by
   the exact prefixes `LogicalSemiJoin`, `LogicalAntiJoin`, `LogicalLeftJoin`,
   `LogicalDerived`, `LogicalJoin`.
4. Add an explicit Q22 assertion using that fingerprint, with a comment quoting
   Week 34's note so the next reader knows why it exists.
5. Emit both the human table and the JSON. Week 37 wants the JSON.

**Mistakes specific to this codebase:**

- **Counting `REFUSED_EXPECTED` as a pass.** The single failure mode this whole
  task exists to prevent.
- **Treating any non-zero exit as a refusal.** A segfault, a missing catalog and
  a `Table not found` all exit non-zero. `REFUSED_UNEXPECTED` must be loud.
- **Assuming a query refused on Volcano is refused for the reason you think.**
  Week 34's harness already documents the ordering hazard: on the Volcano path
  the *capability* refusal fires before the shape-specific one, which is why
  `WEEK34_CORRELATED_SCALAR_REFUSED` runs in the vectorized modes only. Match the
  message; do not infer it.
- **Hard-coding "56 queries in two modes".** Compute it. The number moves the
  moment a suite grows, and a stale number in a report about honesty is a bad
  look.
- **Reporting a mode count for `UNPORTED` queries.** They have no modes; they
  have a reason. Print the reason.

### Verification

- Feed the reporter a synthetic cell matrix where one query matches in two modes
  and is refused-with-the-expected-message in the other two. The summary must
  say "two-mode", must not say "four", and must count two pinned refusals.
- Flip one of those refusals to an unexpected message. The `!! unexplained
  errors` line must appear and the exit code must be non-zero.
- Run against the F1 dataset with a query set that includes a known
  vectorized-only shape (any `WEEK32_SEMI_JOIN_VEC_ONLY` entry). The report must
  independently rediscover the two-mode classification the existing suite already
  encodes by hand — that agreement is the proof the classifier is right.
- Q22, once ported: the fingerprint contains `LogicalAntiJoin` and
  `LogicalDerived`. Record it in the run report verbatim.

---

## Task 7 — Randomized result differencing at SF-small

### Why it matters

Week 28 deferred this here by name, with the diagnosis already done: its
randomized coverage is at **plan** level (300 shapes checked for a legal
`order=`, `cost <= written`, no negative `est=`), and randomized **result**
differencing never completed because the `--no-optimize` leg of a multi-way
self-join over 10k `laps` rows took tens of seconds — 35.6 s measured on one
query — so batches of 100, 40 and 14 all timed out. Every optimizer week from 29
onward inherited that shape of coverage: result preservation rests on
hand-written differentials (44 queries in one audit round, 9 in the next), not
on a generator.

The blocker was data size, not budget, which is why it lands in the week that
owns the data. With Task 3's 500-row fixture, `--no-optimize`'s dominant term —
the unfiltered scan — shrinks by ~20×, and a 40-query differential should run in
well under a minute over the same join shapes.

### Conceptual explanation

**What is being differenced.** Two legs, and they answer different questions:

1. **Optimized ≡ `--no-optimize`, both on `col-vec`.** This is the *result
   preservation* property Week 28 wanted: the optimizer must not change an
   answer. It needs no oracle, so it works for every shape the engine can run —
   including ones SQLite would be awkward about.
2. **Optimized ≡ SQLite.** The correctness leg. It is bounded by the oracle's
   blind spots (Task 5), so it runs on the subset SQLite can express.

Run both. Leg 1 alone can pass on two identically-wrong plans; leg 2 alone
cannot isolate the optimizer.

**The two traps the Week 28 note names, and what they actually mean:**

**Trap 1 — sort before diffing, but understand *when*.** A query with no
`ORDER BY` has no specified row order, and reordering a join legitimately changes
physical emission order; that is not a result difference. So the unordered
comparison must sort both sides — which `normalize(preserve_order=False)`
already does. The subtle half is the *other* direction, and it is the one that
will bite: if the generator emits an `ORDER BY`, `run_query_suite`'s heuristic
(`"ORDER BY" in query.upper()`) switches to **ordered** comparison, and then a
sort key with **ties** makes the comparison order-sensitive on rows whose order
SQL does not specify. A reordered join breaks those ties differently and the diff
reports a false failure. Rule for the generator: either emit **no** `ORDER BY`,
or emit one that is a **total** order (append a unique column such as
`lap_id`). Do not emit a partial `ORDER BY`.

**Trap 2 — `normalize()` keys rows by column *name*.** Its docstring says so
plainly: rows arrive as dicts keyed by column name, so duplicate names collapse,
and a merged join schema legally carries several columns of the same name (two
`driver_id`, two `team` on a `laps`/`drivers` join or any self-join). Both
engines collapse identically, so the file **cannot see** a column-identity or
column-order regression in a `SELECT *` multi-way join. A generator that emits
`SELECT *` over a multi-way join is therefore diffing a *narrower* row than it
thinks it is — and self-joins are exactly what a multi-relation generator
produces most of.

Two ways out, and take both: have the generator project **named, distinctly
aliased** columns rather than `SELECT *`; and compare **raw sorted output**
(Task 5's `--format tsv`, split positionally, never through a dict) so column
count and order survive the comparison. The C++ side already covers the
`SELECT *` case — `JoinEnumeration.ReorderedPlansReturnTheWrittenOrdersRows`
diffs raw chunk values — so this is about not *believing* the Python side covers
it.

**Shape of the generator.** Reuse Week 28's plan-level shape vocabulary: 3–8
relations drawn from `laps` and `drivers` with distinct aliases, an equi-join
chain on real key columns, an optional `WHERE` of one or two conjuncts, an
optional `GROUP BY` over a low-cardinality column. Seed it and **print the seed
in the failure message** — an unreproducible randomized failure is not a bug
report.

### Code snippets

```python
# python_tools/random_diff.py
def generate_query(rng, n_relations):
    """A 3-8 relation equi-join chain over aliased laps/drivers.

    Two rules encode the traps:
      * project NAMED, DISTINCTLY ALIASED columns -- never SELECT *. normalize()
        keys rows by column name, so duplicate names from a merged join schema
        collapse on BOTH sides and the diff silently narrows.
      * either no ORDER BY, or a TOTAL one. A partial ORDER BY makes the
        comparison order-sensitive on rows whose order SQL does not specify, and
        a reordered join breaks those ties differently -- a false failure.
    """
    aliases = [f"l{i}" for i in range(n_relations)]
    froms   = f"laps {aliases[0]}"
    for a, b in zip(aliases, aliases[1:]):
        froms += f" JOIN laps {b} ON {a}.driver_id = {b}.driver_id"

    # distinct output names, so nothing collapses
    projection = ", ".join(f"{a}.speed AS s_{a}" for a in aliases[:3])
    where = f" WHERE {aliases[0]}.season = {rng.choice([2022, 2023, 2024, 2025])}"
    return f"SELECT {projection} FROM {froms}{where}"


def diff_one(catalog, sql, seed):
    opt   = run_tsv(catalog, VEC, sql)                    # optimized
    noopt = run_tsv(catalog, VEC + ["--no-optimize"], sql)
    # Positional, raw, sorted. NOT normalize(): its dict keying is the blind
    # spot this leg exists to avoid.
    if sorted(opt) != sorted(noopt):
        raise AssertionError(
            f"optimizer changed the result\n  seed={seed}\n  sql={sql}")
```

### Implementation guidance

1. Generate against `data/f1/sf-small/catalog.json`. Confirm the budget with a
   single 8-relation `--no-optimize` run before generating 40 of them.
2. Leg 1 (optimized ≡ `--no-optimize`) first — it needs no SQLite and it is the
   property Week 28 actually deferred.
3. Add leg 2 (≡ SQLite) for the shapes SQLite accepts. Reuse Task 5's
   catalog-driven `load_sqlite` pointed at the `sf-small` catalog.
4. Fix the seed by default so a scheduled run is reproducible; allow
   `--seed random` explicitly for exploration, and print the seed on every
   failure either way.

**Mistakes specific to this codebase:**

- **Using `normalize()` for this leg.** It is the documented blind spot. Compare
  raw positional output.
- **Emitting `SELECT *`.** Same reason, from the other end.
- **Emitting a partial `ORDER BY`.** False failures that look like optimizer
  bugs, which is the worst possible false positive to hand a future week.
- **Generating shapes the vectorized path refuses.** A 3+ relation join is
  vectorized-only; that is fine because both legs run on `col-vec`. But a
  generated correlated or `IN` shape run on Volcano would refuse, and a refusal
  in a *result* differ is an error, not a data point. Keep the generator to
  shapes both legs can run.
- **Forgetting that `data/f1/sf-small` sorts `laps` by season.** Keep it; the
  clustered-season assumption is what the rest of the project's zone-map
  behaviour is measured under.

### Verification

- **Budget:** 40 generated queries, both legs, complete in under a minute. That
  is the number Week 28's note predicts; if it does not hold, say so rather than
  quietly shrinking the batch.
- **The differ can fail.** Run one query with a deliberately corrupted expected
  value, or temporarily force a wrong build side, and confirm a red result. A
  differential that has never failed has not been shown to work — Week 33's
  dead-assertion finding is the same lesson.
- **Trap 2 is really closed:** generate one `SELECT *` self-join by hand, run it
  through `normalize()` and through the positional comparison, and confirm the
  two see a different number of columns. Keep that as a regression test with a
  comment pointing at `normalize()`'s docstring.
- Record the batch size, the seed and the wall time in the run report, so Week 36
  and Week 37 inherit a number rather than an impression.

---

## Task 8 — The behavioural rejection sweep and the standing sweep rule

### Why it matters

Week 34 recorded the lesson directly: **a textual cross-check cannot detect a
suite entry whose move half-landed.** A rejection entry that was moved out of one
list and not into another, or whose expected message drifted, still *reads*
correct. Two stale rejection entries were caught in Week 34 and retired; the
check that found them was behavioural — *run every rejection entry and assert it
still errors* — not textual. Week 34's own commit history names it as the
replacement for a half-check.

That matters more this week than in any prior one, because Week 35 rewrites the
loader and adds a second dataset, and Task 6's whole report is built on the
premise that a pinned refusal means what it says. If a refusal has gone stale —
the query now *answers* — Task 6 will classify it `REFUSED_UNEXPECTED` at best,
and at worst the boundary will have moved without anyone noticing and the
two-mode/four-mode census will be wrong.

### Conceptual explanation

Three checks, cheap, and they compose:

1. **Structural.** Every suite named `*_REJECTED` / `*_REFUSED` in the module is
   a non-empty list of `(query, expected_substring)` 2-tuples with a non-empty
   expected substring. Catches a half-landed move where a bare string was
   appended to a list of pairs.
2. **Disjointness.** No query text appears in both a positive suite (one fed to
   `run_query_suite`) and a rejection suite. That *is* the half-landed-move
   signature: the entry was added to its new home and not removed from its old
   one, so the same SQL is simultaneously asserted to return rows and to error.
   Textual review will not see it; set intersection will.
3. **Behavioural.** Every rejection entry is executed and must still error, with
   the expected substring present, in at least the modes its suite is run in.
   This is the check Week 34 identified as the one that works. Note the harness
   *already* does this as a side effect for every suite `main()` runs — the value
   of making it explicit is that it covers suites `main()` has stopped running,
   which is exactly the half-landed case.

Discover the suites by **name convention over `globals()`**, not by a
hand-maintained list. A hand-maintained list of suites is one more thing a move
can half-land in.

### Code snippets

```python
# python_tools/compare_against_sqlite.py — the meta-check.
# Week 34's lesson, automated: a TEXTUAL cross-check cannot see a suite entry
# whose move half-landed. Running every entry can.
def sweep_rejection_suites(modes):
    """Structural + disjointness + behavioural sweep over every *_REJECTED /
    *_REFUSED suite in this module. Discovered by NAME, never by a curated list:
    a curated list is one more place a half-landed move can hide."""
    suites = {name: obj for name, obj in globals().items()
              if (name.endswith("_REJECTED") or name.endswith("_REFUSED"))
              and isinstance(obj, list)}

    failures = []

    # (1) structural
    for name, suite in suites.items():
        if not suite:
            failures.append(f"{name}: empty suite")
        for entry in suite:
            if not (isinstance(entry, tuple) and len(entry) == 2
                    and isinstance(entry[1], str) and entry[1]):
                failures.append(f"{name}: malformed entry {entry!r}")

    # (2) disjointness -- the half-landed-move signature
    positive = set()
    for name, obj in globals().items():
        if name.endswith("_QUERIES") or name.endswith("_VEC_ONLY"):
            if isinstance(obj, list) and all(isinstance(q, str) for q in obj):
                positive |= set(obj)
    for name, suite in suites.items():
        overlap = positive & {q for q, _ in suite}
        for q in overlap:
            failures.append(
                f"{name}: also in a positive suite -- a move half-landed: {q[:70]}")

    # (3) behavioural -- the check that actually works
    for name, suite in suites.items():
        for query, expected in suite:
            if not any(_errors_with(query, expected, extra) for _, extra in modes):
                failures.append(
                    f"{name}: no longer errors with {expected!r}: {query[:70]}")
    return failures
```

### Implementation guidance

1. Write the structural and disjointness checks first — they are pure and run in
   milliseconds, and disjointness alone would have caught Week 34's two entries.
2. Add the behavioural leg, and run it over the four modes. Expect it to be the
   slowest part of the file; that is acceptable for what it buys, but gate it
   behind a flag if the full suite's runtime matters.
3. Wire it into `main()` and into the exit code. A sweep whose findings are
   printed but do not fail the run is a sweep nobody reads.

**Mistakes specific to this codebase:**

- **Requiring an entry to error in *all* modes.** Several suites deliberately
  run in two modes only, and several assert *different* messages per path — the
  Volcano capability refusal fires before the shape-specific one
  (`WEEK32_LOWERING_REFUSED` vs `WEEK32_LOWERING_REFUSED_VOLCANO`,
  `WEEK34_CORRELATED_SCALAR_REFUSED` vectorized-only). "Errors with the expected
  message in at least one mode" is the correct assertion for a generic sweep;
  the per-mode precision stays in `main()` where the mode split is explicit.
- **Name-convention drift.** `WEEK33_CORRELATED_BINDS` is a rejection suite
  whose name ends in neither `_REJECTED` nor `_REFUSED` — `main()` zips it with
  `WEEK33_CORRELATED_EXPECT` to make the pairs. Either rename it or add it
  explicitly, and leave a comment saying which, or the sweep silently skips it.
- **`WEEK33_CORRELATED_EXPECT` is a prefix, not a message.** Week 33 recorded
  this: the suite asserts only the shared `correlated subquer`, so per-shape
  drift is invisible. The sweep will report those entries green. That is a known
  weakness, not a fix this week owes — but say so, because a green sweep that
  quietly under-asserts is the failure mode the sweep exists to prevent.

### The standing rule, applied to this week

> When a refusal, guard or invariant is removed or changed, sweep every comment,
> precondition, assertion and header citing it. Three silent wrong answers came
> from that shape in Week 33 alone.

Week 35's own list, to work through before closing the week:

- **`CSVLoader::load`'s header assumption.** Its comment says `// skip header
  line`. After Task 2 it is conditional. `csv_loader.h`'s declaration comment
  ("split a CSV line into raw string fields") now describes a delimiter that is
  a parameter.
- **The `Commas inside string values not supported in CSV input` limitation.**
  Still true for CSV, now sidestepped for `.tbl` — which is *why* TPC-H comments
  load. Say both halves.
- **The NaN limitation's "belongs to Week 35, which rewrites the loader
  anyway".** Task 2 re-declines it. That sentence must be replaced with what
  Week 35 actually did and why, or it reads as a promise the week broke.
- **`rows_equal`'s tolerance docstring.** It now takes `rel_tol`; the docstring
  must say which callers use which and why the default did not move.
- **`normalize()`'s blind-spot comment.** It says "Read this file's silence on
  `SELECT *` joins as absence of coverage, not as coverage." Task 7 adds a
  positional comparison path that *does* see it — for the randomized differ
  only. Update the comment to say which of the two comparisons is which, or the
  next reader will assume the blind spot is closed everywhere.
- **The README's `56 queries in two modes against 168 in four`.** Once Task 6
  computes the census, cite the harness rather than the prose.
- **`benchmark.py`'s `avg of 5`** if Task 4 adopts the median. Either both or
  neither.

### Verification

- Introduce a deliberate half-landed move locally: copy one entry from a
  rejection suite into a positive suite without removing it. The disjointness
  check must fail. Revert.
- Introduce a deliberate stale expectation: change one expected substring to
  something the engine never emits. The behavioural check must fail. Revert.
- Run the full `compare_against_sqlite.py` with the sweep enabled and confirm it
  is green *and* that the sweep actually visited every suite — print the suite
  count and the entry count, and eyeball them against the file. A sweep that
  discovers zero suites is green too.
- Walk the standing-rule list above and tick each item in the week's closing
  commit message.

---

## Closing note — what Week 36 inherits, and what it must not assume

- **A number with conditions attached.** The harness reports correctness *and*
  the mode census, separately. Week 36's checkpoint sentence must carry the mode
  count with it.
- **A stated oracle.** Whether the data came from official `dbgen` or from the
  Python generator decides whether "reference results" can mean the published
  answer set or only SQLite-over-the-same-data. Week 35 records which; Week 36
  must not upgrade the claim.
- **Three named things the oracle cannot check**: a query that errors, a
  derived-table column alias list, and (until Task 5's `--format tsv` lands) a
  zero-row result versus a parse failure.
- **A measured scale ceiling**, from Task 3's memory ladder — which is exactly
  what Week 36's "Document supported scale and memory limits" bullet is waiting
  for.
- **Q22's plan fingerprint**, so "which half was which" is read off a plan
  rather than argued from the query text.
- **Not built here, deliberately:** the dialect port itself, the
  `extract(year from d)` decision (Week 25 assigned it to Week 36 — Task 5 only
  records that the harness currently normalizes it *by accident*), and any
  performance conclusion, which is Week 37's.
