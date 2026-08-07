#!/usr/bin/env python3
"""
tpch_schema.py — the ONE definition of the TPC-H schema for this project.

Consumed by:
  - generate_tpch.py      (writes the .tbl files and the per-scale catalog.json)
  - compare_against_sqlite.py (builds the SQLite mirror, catalog-driven)
  - run_tpch.py           (nothing directly; it reads the generated catalog)

Three copies of a 61-column schema is the two-paths drift Weeks 26/28/30 each
had to undo. One definition, several readers.

TYPE MAPPING — three decisions, each with a consequence the Week 36 correctness
report inherits:

  DATE          -> STRING   src/common/date_util.h's documented representation:
                            a date IS a STRING holding 'YYYY-MM-DD', chosen
                            because ISO-8601 sorts lexicographically and so
                            inherits zone-map chunk pruning, scanColumn<string>'s
                            tight loop and BETWEEN for free.
                            NOT DOUBLE: std::stod("1996-01-02") returns 1996.0
                            with no error at all. CSVLoader::parseField now
                            rejects a partially-consumed field precisely so this
                            mistake is loud, but the right type is still this one.

  DECIMAL(15,2) -> DOUBLE   Forced: the engine has three types and no exact
                            numeric. The README Limitations bullet already states
                            the consequence (SUM/AVG accumulate in double). What
                            is new in Week 35 is that this choice also sets the
                            comparison tolerance: an ABSOLUTE 1e-5 is unusable
                            for a revenue sum -- see rows_equal's docstring in
                            compare_against_sqlite.py.

  CHAR(n)/VARCHAR(n) -> STRING   Free. Note l_returnflag/l_linestatus are
                            single characters with tiny cardinality, so
                            DictionaryEncoder takes them.

SQLite gets the STRICTER declaration (INTEGER/REAL/TEXT) on purpose: a mistyped
column then shows up as an affinity surprise on the oracle side too, giving two
independent chances to catch a transcription error.

Column ORDER is load-bearing: .tbl is positional and the loader zips fields to
schema.column(i) by index.
"""

# (column_name, swiftql_type, sqlite_type)
TPCH_TABLES = {
    "region": [
        ("r_regionkey", "INT",    "INTEGER"),
        ("r_name",      "STRING", "TEXT"),
        ("r_comment",   "STRING", "TEXT"),
    ],
    "nation": [
        ("n_nationkey", "INT",    "INTEGER"),
        ("n_name",      "STRING", "TEXT"),
        ("n_regionkey", "INT",    "INTEGER"),
        ("n_comment",   "STRING", "TEXT"),
    ],
    "part": [
        ("p_partkey",     "INT",    "INTEGER"),
        ("p_name",        "STRING", "TEXT"),
        ("p_mfgr",        "STRING", "TEXT"),
        ("p_brand",       "STRING", "TEXT"),
        ("p_type",        "STRING", "TEXT"),
        ("p_size",        "INT",    "INTEGER"),
        ("p_container",   "STRING", "TEXT"),
        ("p_retailprice", "DOUBLE", "REAL"),
        ("p_comment",     "STRING", "TEXT"),
    ],
    "supplier": [
        ("s_suppkey",   "INT",    "INTEGER"),
        ("s_name",      "STRING", "TEXT"),
        ("s_address",   "STRING", "TEXT"),
        ("s_nationkey", "INT",    "INTEGER"),
        ("s_phone",     "STRING", "TEXT"),
        ("s_acctbal",   "DOUBLE", "REAL"),
        ("s_comment",   "STRING", "TEXT"),
    ],
    "partsupp": [
        ("ps_partkey",    "INT",    "INTEGER"),
        ("ps_suppkey",    "INT",    "INTEGER"),
        ("ps_availqty",   "INT",    "INTEGER"),
        ("ps_supplycost", "DOUBLE", "REAL"),
        ("ps_comment",    "STRING", "TEXT"),
    ],
    "customer": [
        ("c_custkey",    "INT",    "INTEGER"),
        ("c_name",       "STRING", "TEXT"),
        ("c_address",    "STRING", "TEXT"),
        ("c_nationkey",  "INT",    "INTEGER"),
        ("c_phone",      "STRING", "TEXT"),
        ("c_acctbal",    "DOUBLE", "REAL"),
        ("c_mktsegment", "STRING", "TEXT"),
        ("c_comment",    "STRING", "TEXT"),
    ],
    "orders": [
        ("o_orderkey",      "INT",    "INTEGER"),
        ("o_custkey",       "INT",    "INTEGER"),
        ("o_orderstatus",   "STRING", "TEXT"),
        ("o_totalprice",    "DOUBLE", "REAL"),
        ("o_orderdate",     "STRING", "TEXT"),   # DATE
        ("o_orderpriority", "STRING", "TEXT"),
        ("o_clerk",         "STRING", "TEXT"),
        ("o_shippriority",  "INT",    "INTEGER"),
        ("o_comment",       "STRING", "TEXT"),
    ],
    "lineitem": [
        ("l_orderkey",      "INT",    "INTEGER"),
        ("l_partkey",       "INT",    "INTEGER"),
        ("l_suppkey",       "INT",    "INTEGER"),
        ("l_linenumber",    "INT",    "INTEGER"),
        ("l_quantity",      "DOUBLE", "REAL"),
        ("l_extendedprice", "DOUBLE", "REAL"),
        ("l_discount",      "DOUBLE", "REAL"),
        ("l_tax",           "DOUBLE", "REAL"),
        ("l_returnflag",    "STRING", "TEXT"),
        ("l_linestatus",    "STRING", "TEXT"),
        ("l_shipdate",      "STRING", "TEXT"),   # DATE
        ("l_commitdate",    "STRING", "TEXT"),   # DATE
        ("l_receiptdate",   "STRING", "TEXT"),   # DATE
        ("l_shipinstruct",  "STRING", "TEXT"),
        ("l_shipmode",      "STRING", "TEXT"),
        ("l_comment",       "STRING", "TEXT"),
    ],
}

# Load order: parents before children, so a referential-integrity check can run
# incrementally and so SQLite's inserts never dangle.
TPCH_LOAD_ORDER = ["region", "nation", "part", "supplier", "partsupp",
                   "customer", "orders", "lineitem"]

# Base cardinalities at SF=1. nation and region DO NOT SCALE -- a generator that
# multiplies every count by SF produces a 0-row nation at SF=0.01 and then
# Q5/Q7/Q8 silently return nothing.
TPCH_BASE_ROWS = {
    "region":    5,          # fixed
    "nation":    25,         # fixed
    "supplier":  10_000,
    "customer":  150_000,
    "part":      200_000,
    "partsupp":  800_000,    # 4 per part
    "orders":    1_500_000,
    "lineitem":  6_001_215,  # ~4 per order, actual count is data-dependent
}
TPCH_FIXED_TABLES = {"region", "nation"}

# The .tbl format, as one object. Mirrors FileFormat's three fields in
# src/catalog/table_metadata.h; absent from a catalog entry means CSV.
TBL_FORMAT = {"delimiter": "|", "header": False, "trailing_delimiter": True}


def swiftql_columns(table):
    """[(name, swiftql_type)] — what catalog.json needs."""
    return [(name, sq) for name, sq, _ in TPCH_TABLES[table]]


def sqlite_ddl(table):
    """CREATE TABLE text for the oracle mirror."""
    cols = ", ".join(f"{name} {lite}" for name, _, lite in TPCH_TABLES[table])
    return f"CREATE TABLE {table} ({cols})"


def expected_rows(table, scale):
    """Row count for `table` at `scale`, honouring the two fixed tables.

    lineitem is data-dependent (1-7 lines per order), so this is the SF=1 figure
    scaled — a sanity bound, not an equality. Callers check the others exactly.
    """
    if table in TPCH_FIXED_TABLES:
        return TPCH_BASE_ROWS[table]
    return int(round(TPCH_BASE_ROWS[table] * scale))


if __name__ == "__main__":
    # Transcription check: column counts are the cheapest thing that catches a
    # dropped or duplicated line in the tables above.
    EXPECTED_WIDTHS = {"region": 3, "nation": 4, "part": 9, "supplier": 7,
                       "partsupp": 5, "customer": 8, "orders": 9, "lineitem": 16}
    bad = []
    for t, cols in TPCH_TABLES.items():
        if len(cols) != EXPECTED_WIDTHS[t]:
            bad.append(f"{t}: {len(cols)} columns, expected {EXPECTED_WIDTHS[t]}")
        names = [c[0] for c in cols]
        if len(set(names)) != len(names):
            bad.append(f"{t}: duplicate column name")
    if set(TPCH_TABLES) != set(TPCH_LOAD_ORDER):
        bad.append("TPCH_LOAD_ORDER disagrees with TPCH_TABLES")
    if bad:
        raise SystemExit("schema transcription errors:\n  " + "\n  ".join(bad))
    total = sum(len(c) for c in TPCH_TABLES.values())
    print(f"ok: {len(TPCH_TABLES)} tables, {total} columns")
