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
