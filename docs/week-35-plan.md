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
