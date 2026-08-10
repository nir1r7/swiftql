#!/usr/bin/env python3
"""
compare_engines.py — TPC-H latency for SwiftQL against SQLite, DuckDB and Postgres.

    .venv/bin/python python_tools/compare_engines.py \
        --catalog data/tpch/dbgen-sf0.1/catalog.json --reps 5

WHAT MAKES THIS A COMPARISON RATHER THAN FOUR UNRELATED NUMBERS

  1. ONE query text. Every engine runs the string `tpch_queries.render(qid)`
     produces -- the SwiftQL dialect port, with the same validation parameters
     the correctness harness uses. No engine gets a rewrite tuned to it. This is
     the anchor, and it cuts against SwiftQL: the port exists because SwiftQL
     cannot parse the spec's own text, so the other three are being handed a
     query shaped by a dialect they do not need.

  2. ONE type mapping. All four get tpch_schema.py's SQLite declarations
     (INTEGER/REAL/TEXT), so a DATE is a lexicographically-compared string
     everywhere, exactly as src/common/date_util.h defines it. Letting Postgres
     use a native DATE and a NUMERIC would measure a different query.

  3. LOAD IS EXCLUDED, for all four, and they exclude it differently. SQLite,
     DuckDB and Postgres load once into a persistent database and are then timed
     on execution alone. SwiftQL has no persistence at all -- every invocation
     re-reads the .tbl files -- so it is timed by parsing its own
     `--explain-analyze` footer, whose `Execution:` line excludes the load by
     construction. That asymmetry is the honest one to draw: including SwiftQL's
     load would measure a CSV parser against three storage engines.

  4. TWO SwiftQL numbers, not one. `Execution:` excludes parse and plan; the
     other three engines are timed with a call that includes theirs. So both are
     reported -- `exec` for the operator comparison and `total` (parse + plan +
     exec) for the like-for-like one. Quote `total` in a headline; `exec` only
     beside the word "execution".

WHAT THIS DOES NOT MEASURE. Cold start, concurrency, write throughput,
larger-than-memory operation, and index selection. SwiftQL has no write path,
no persistence and no indexes, so three of those are not comparisons it can
enter. The number here is single-query, single-threaded, warm-cache latency on
one machine -- which is the only claim the paper should make from it.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tpch_queries import QUERY_IDS, render  # noqa: E402

# CMakeLists.txt sets no default CMAKE_BUILD_TYPE, so a plain `cmake ..` build
# compiles at -O0. Measured: q18 runs 1568ms there against 72ms from a Release
# build of the same commit -- a 21x difference that has nothing to do with the
# engine. The default points at build-release/ so a benchmark cannot silently
# quote the unoptimized binary.
SWIFTQL_BIN = "./build-release/swiftql"

# The mode every one of the 22 queries answers in. row-volcano and col-volcano
# answer 5 of 22 (Planner::plan builds exactly one join), so timing the engine
# in either would compare a 5-query subset against three full ones.
SWIFTQL_MODE = ["--storage", "columnar", "--execution", "vectorized"]

# The SAME engine with the optimizer off, so optimizer impact is a column in the
# comparison table rather than a separate measurement in a separate file. It is
# the honest pairing: both legs are the same binary, the same storage and the
# same executor, differing only in whether the logical plan was rewritten.
# row-volcano and col-volcano are deliberately absent -- they answer 5 of the 22,
# so a column for either would be mostly blank and would invite a reader to
# compare a 5-query engine against four 22-query ones.
SWIFTQL_NOOPT_MODE = SWIFTQL_MODE + ["--no-optimize"]

# tpch_schema.py's SQLite column types, mapped per engine. TEXT for dates in all
# three: see the header's point 2.
TYPE_MAP = {
    "duckdb":   {"INTEGER": "BIGINT", "REAL": "DOUBLE", "TEXT": "VARCHAR"},
    "postgres": {"INTEGER": "BIGINT", "REAL": "DOUBLE PRECISION", "TEXT": "TEXT"},
}

# The footer's UNIT VARIES per line -- main.cc scales it, so a 75µs parse sits
# above a 254812.9µs execution in the same block. Matching a hard-coded "ms"
# silently failed on every query rather than mis-scaling one, which is the only
# reason it was caught.
SWIFTQL_TIMES = re.compile(
    r"Parse:\s+([\d.]+)(µs|ms|s)\s+"
    r"Plan:\s+([\d.]+)(µs|ms|s)\s+"
    r"Execution:\s+([\d.]+)(µs|ms|s)")

TO_MS = {"µs": 1e-3, "ms": 1.0, "s": 1e3}

PG_BIN = "/opt/homebrew/opt/postgresql@17/bin"

# INDEXES: exactly the PRIMARY and FOREIGN keys the TPC-H specification itself
# declares, transcribed from `dbgen/dss.ri` in the official V3.0.1 kit. Nothing
# else.
#
# WHY THIS SET AND NOT ANOTHER. An index set chosen by looking at the 22 queries
# would be tuning, and tuning one engine and not the others is how a benchmark
# stops meaning anything. The spec's own referential-integrity declarations are
# the one index set that is (a) written by the benchmark's authors rather than
# by us, (b) what any DBA creates before running anything, and (c) query-blind:
# `dss.ri` was fixed before we chose a single query. Composite keys keep the
# spec's column ORDER, since a (partkey, suppkey) index is not the same object
# as (suppkey, partkey).
#
# WHY IT MATTERS. Without indexes SQLite runs q21's correlated NOT EXISTS as a
# nested scan: 19.7s at SF=0.01, and unfinished after 28 minutes at SF=1.
# Reporting SwiftQL beating an unindexed row store is a weak claim -- a reviewer
# will assume the competitor was configured properly, and here it now is.
#
# WHO GETS THEM. SQLite and Postgres, the two row stores. NOT DuckDB and NOT
# SwiftQL: both are columnar and prune with automatic min/max zone maps instead
# of user-declared indexes, and DuckDB's own guidance is that ART indexes rarely
# help analytical scans. So the comparison is each engine in the configuration
# its own design calls for, which is a fairer question than forcing one shape on
# all four.
TPCH_INDEXES = {
    "region":   [["r_regionkey"]],
    "nation":   [["n_nationkey"], ["n_regionkey"]],
    "part":     [["p_partkey"]],
    "supplier": [["s_suppkey"], ["s_nationkey"]],
    "partsupp": [["ps_partkey", "ps_suppkey"], ["ps_suppkey"], ["ps_partkey"]],
    "customer": [["c_custkey"], ["c_nationkey"]],
    "orders":   [["o_orderkey"], ["o_custkey"]],
    "lineitem": [["l_orderkey", "l_linenumber"], ["l_orderkey"],
                 ["l_partkey", "l_suppkey"], ["l_partkey"], ["l_suppkey"]],
}


def index_statements(table):
    """CREATE INDEX text for `table`, or nothing if the schema is not TPC-H."""
    for i, cols in enumerate(TPCH_INDEXES.get(table, [])):
        yield "CREATE INDEX idx_{0}_{1} ON {0}({2})".format(
            table, i, ", ".join(cols))

# SQLite and DuckDB get a file inside the catalog's own directory, so a second
# scale factor cannot collide with the first. Postgres has no such file -- its
# database is a name on a shared server -- so the name carries the scale factor
# instead. A fixed name meant "tables already present, skip the load", which for
# a second catalog is the sf0.01 data answering sf1's queries with no error
# anywhere.
def pg_dbname(catalog_path, index=True):
    """Database name for this catalog AND this index configuration.

    The suffix is not cosmetic. Without it, an indexed and an unindexed run of
    the same scale factor share one database, so the second run either reuses
    the first's configuration silently or forces a full reload of 6M rows to get
    it back. Two names means both stay loaded and either can be re-run cheaply.
    """
    tag = os.path.basename(os.path.dirname(os.path.abspath(catalog_path)))
    return ("swiftql_" + re.sub(r"[^a-z0-9_]", "_", tag.lower())
            + ("" if index else "_noidx"))


# ---------------------------------------------------------------- catalog

def read_catalog(catalog_path):
    """[(table, [(col, sqlite_type)], abs_path)] in the catalog's own order."""
    base = os.path.dirname(os.path.abspath(catalog_path))
    with open(catalog_path) as f:
        spec = json.load(f)
    sqlite_type = {"INT": "INTEGER", "DOUBLE": "REAL", "STRING": "TEXT"}
    return [(t["name"],
             [(c["name"], sqlite_type[c["type"]]) for c in t["columns"]],
             os.path.join(base, t["file"]))
            for t in spec["tables"]]


def ddl(table, cols, engine):
    """CREATE TABLE with a trailing dummy column.

    dbgen writes a trailing '|' on every line, so each row carries one more
    field than the table has columns. Every bulk loader here counts fields, so
    the extra one is declared and dropped rather than stripped by rewriting
    hundreds of megabytes of input.
    """
    tmap = TYPE_MAP.get(engine, {})
    body = ", ".join(f"{n} {tmap.get(t, t)}" for n, t in cols)
    return f"CREATE TABLE {table} ({body}, tbl_trailing_dummy {tmap.get('TEXT', 'TEXT')})"


# ---------------------------------------------------------------- loaders

def load_sqlite(catalog_path, workdir, rebuild, index):
    import sqlite3
    path = os.path.join(workdir, "tpch.sqlite" if index else "tpch-noidx.sqlite")
    if rebuild and os.path.exists(path):
        os.remove(path)
    if os.path.exists(path):
        return path
    conn = sqlite3.connect(path)
    for table, cols, tbl in read_catalog(catalog_path):
        conn.execute(ddl(table, cols, "sqlite"))
        placeholders = ",".join("?" * (len(cols) + 1))
        with open(tbl) as f:
            conn.executemany(f"INSERT INTO {table} VALUES ({placeholders})",
                             (line.rstrip("\n").split("|") for line in f if line.strip()))
        conn.execute(f"ALTER TABLE {table} DROP COLUMN tbl_trailing_dummy")
        if index:
            for stmt in index_statements(table):
                conn.execute(stmt)
    if index:
        conn.execute("ANALYZE")     # sqlite_stat1; without it a 6-table join
                                    # order is chosen from static heuristics
    conn.commit()
    conn.close()
    return path


def load_duckdb(catalog_path, workdir, rebuild):
    import duckdb
    path = os.path.join(workdir, "tpch.duckdb")
    if rebuild and os.path.exists(path):
        os.remove(path)
    if os.path.exists(path):
        return path
    con = duckdb.connect(path)
    for table, cols, tbl in read_catalog(catalog_path):
        con.execute(ddl(table, cols, "duckdb"))
        con.execute(f"COPY {table} FROM '{tbl}' (DELIMITER '|', HEADER false)")
        con.execute(f"ALTER TABLE {table} DROP COLUMN tbl_trailing_dummy")
    con.close()
    return path


def pg_running():
    return subprocess.run([f"{PG_BIN}/pg_isready", "-q"]).returncode == 0


def load_postgres(catalog_path, workdir, rebuild, index):
    import psycopg2
    db = pg_dbname(catalog_path, index)
    if not pg_running():
        sys.exit(f"postgres is not accepting connections. Start it with:\n"
                 f"  brew services start postgresql@17")
    admin = psycopg2.connect(dbname="postgres")
    admin.autocommit = True
    with admin.cursor() as cur:
        cur.execute("SELECT 1 FROM pg_database WHERE datname = %s", (db,))
        exists = cur.fetchone() is not None
        if exists and rebuild:
            cur.execute(f'DROP DATABASE "{db}"')
            exists = False
        if not exists:
            cur.execute(f'CREATE DATABASE "{db}"')
    admin.close()

    con = psycopg2.connect(dbname=db)
    with con.cursor() as cur:
        cur.execute("SELECT count(*) FROM information_schema.tables "
                    "WHERE table_schema = 'public'")
        if cur.fetchone()[0] == 0:
            for table, cols, tbl in read_catalog(catalog_path):
                cur.execute(ddl(table, cols, "postgres"))
                with open(tbl) as f:
                    cur.copy_expert(
                        f"COPY {table} FROM STDIN WITH (FORMAT csv, DELIMITER '|', QUOTE E'\\b')", f)
                cur.execute(f"ALTER TABLE {table} DROP COLUMN tbl_trailing_dummy")
                if index:
                    for stmt in index_statements(table):
                        cur.execute(stmt)
            con.commit()
            cur.execute("ANALYZE")
            con.commit()
    con.close()
    return db


# ---------------------------------------------------------------- timing

def time_swiftql(binary, catalog, sql, mode):
    """{"ms": parse+plan+exec, "exec_ms": exec, "rows": n} for one invocation.

    Two numbers because they answer two questions -- see the header's point 4.
    A cold process each time is unavoidable: SwiftQL has no server mode.
    """
    r = subprocess.run([binary, "--catalog", catalog] + mode
                       + ["--explain-analyze", "--query", sql],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None
    m = SWIFTQL_TIMES.search(r.stdout)
    if not m:
        return None
    g = m.groups()
    parse, plan, exec_ms = (float(g[i]) * TO_MS[g[i + 1]] for i in (0, 2, 4))
    rows = re.search(r"Rows returned:\s+(\d+)", r.stdout)
    return {"ms": parse + plan + exec_ms, "exec_ms": exec_ms,
            "rows": int(rows.group(1)) if rows else None}


def time_dbapi(connect, sql, setup=()):
    """{"ms": elapsed, "rows": n} for one fully-materialized execution."""
    con = connect()
    cur = con.cursor()
    # Session settings applied BEFORE the timer: they configure the engine, they
    # are not part of the query's work.
    for stmt in setup:
        cur.execute(stmt)
    t0 = time.perf_counter()
    cur.execute(sql)
    out = cur.fetchall()
    elapsed = (time.perf_counter() - t0) * 1000.0
    cur.close()
    con.close()
    return {"ms": elapsed, "rows": len(out)}


def measure(fn, warmups, reps):
    """Median over `reps` after `warmups` discarded, or None if any run failed.

    Every per-run key is aggregated the same way, so SwiftQL's second number
    (exec_ms) needs no special case here and cannot silently go unreported.
    """
    for _ in range(warmups):
        if fn() is None:
            return None
    runs = []
    for _ in range(reps):
        got = fn()
        if got is None:
            return None
        runs.append(got)
    out = {"rows": runs[-1]["rows"]}
    for key in ("ms", "exec_ms"):
        if key in runs[0]:
            vals = [r[key] for r in runs]
            out[key] = statistics.median(vals)
            out[f"min_{key}"] = min(vals)
    return out


# ---------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalog", default="data/tpch/dbgen-sf0.1/catalog.json")
    ap.add_argument("--queries", default="", help="comma-separated qids; default all")
    ap.add_argument("--engines",
                    default="swiftql,swiftql-noopt,sqlite,duckdb,postgres")
    ap.add_argument("--swiftql-bin", default=SWIFTQL_BIN,
                    help="path to the swiftql binary; MUST be an optimized build")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--warmups", type=int, default=1)
    ap.add_argument("--workdir", default="", help="where the loaded databases live; "
                                                  "default <catalog dir>/.engines")
    ap.add_argument("--rebuild", action="store_true",
                    help="drop and reload every database first")
    ap.add_argument("--pg-single-threaded", action="store_true",
                    help="SET max_parallel_workers_per_gather = 0 per connection. "
                         "Postgres defaults to 2 workers and every TPC-H table over "
                         "8MB qualifies, so at SF=1 it runs on up to 3 processes "
                         "while SwiftQL runs on 1. This flag makes the comparison "
                         "engine-vs-engine rather than 3-cores-vs-1")
    ap.add_argument("--no-index", action="store_true",
                    help="build SQLite/Postgres WITHOUT the spec's PK/FK indexes. "
                         "Off by default: an unindexed row store is not a "
                         "configuration anyone would run, and at SF=1 it does not "
                         "finish. Kept because the unindexed number is the "
                         "like-for-like one against SwiftQL, which has no indexes")
    ap.add_argument("--json", default="", help="write the per-query record here")
    args = ap.parse_args()

    qids = [q.strip() for q in args.queries.split(",") if q.strip()] or list(QUERY_IDS)
    engines = [e.strip() for e in args.engines.split(",") if e.strip()]
    catalog = args.catalog
    workdir = args.workdir or os.path.join(os.path.dirname(os.path.abspath(catalog)),
                                           ".engines")
    os.makedirs(workdir, exist_ok=True)

    runners = {}
    if "swiftql" in engines:
        if not os.path.exists(args.swiftql_bin):
            sys.exit(f"{args.swiftql_bin} not found. Build an OPTIMIZED binary:\n"
                     f"  cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release\n"
                     f"  cmake --build build-release -j")
        runners["swiftql"] = lambda sql, b=args.swiftql_bin: time_swiftql(
            b, catalog, sql, SWIFTQL_MODE)
    if "swiftql-noopt" in engines:
        if not os.path.exists(args.swiftql_bin):
            sys.exit(f"{args.swiftql_bin} not found -- build Release first")
        runners["swiftql-noopt"] = lambda sql, b=args.swiftql_bin: time_swiftql(
            b, catalog, sql, SWIFTQL_NOOPT_MODE)
    # Each path is bound as a DEFAULT ARGUMENT, not captured. A plain closure
    # over a shared `p` reads it at call time, so the second loader's path
    # silently became the first engine's too -- sqlite opening the duckdb file.
    if "sqlite" in engines:
        import sqlite3
        print("loading sqlite...", flush=True)
        sqlite_path = load_sqlite(catalog, workdir, args.rebuild, not args.no_index)
        runners["sqlite"] = lambda sql, p=sqlite_path: time_dbapi(
            lambda: sqlite3.connect(p), sql)
    if "duckdb" in engines:
        import duckdb
        print("loading duckdb...", flush=True)
        duckdb_path = load_duckdb(catalog, workdir, args.rebuild)  # columnar: zone maps, no indexes
        runners["duckdb"] = lambda sql, p=duckdb_path: time_dbapi(
            lambda: duckdb.connect(p, read_only=True), sql)
    if "postgres" in engines:
        import psycopg2
        print("loading postgres...", flush=True)
        pg_db = load_postgres(catalog, workdir, args.rebuild, not args.no_index)
        pg_setup = (("SET max_parallel_workers_per_gather = 0",)
                    if args.pg_single_threaded else ())
        runners["postgres"] = lambda sql, d=pg_db, su=pg_setup: time_dbapi(
            lambda: psycopg2.connect(dbname=d), sql, su)

    print(f"\ncatalog: {catalog}")
    print(f"indexes:  {'NONE (--no-index)' if args.no_index else 'TPC-H spec PK/FK on sqlite+postgres, ANALYZE'}")
    print(f"engines: {', '.join(runners)}   "
          f"median of {args.reps} after {args.warmups} warmup\n")

    header = f"{'query':<6}" + "".join(f"{n:>14}" for n in runners) + "   rows"
    print(header)
    print("-" * len(header))

    results = {}
    for qid in qids:
        sql = render(qid)
        cells, row_counts = [], set()
        for name, run in runners.items():
            got = measure(lambda: run(sql), args.warmups, args.reps)
            results[f"{qid}|{name}"] = got
            cells.append(f"{'ERR':>14}" if got is None
                         else f"{got['ms']:>13.1f}ms")
            if got is not None and got["rows"] is not None:
                row_counts.add(got["rows"])
        # Row counts are not the correctness oracle -- run_tpch.py is -- but four
        # engines disagreeing on how many rows they returned makes any latency
        # comparison meaningless, so it is shown rather than assumed.
        note = (str(row_counts.pop()) if len(row_counts) == 1
                else f"DISAGREE {sorted(row_counts)}")
        print(f"{qid:<6}" + "".join(cells) + f"   {note}", flush=True)

    if args.json:
        with open(args.json, "w") as f:
            # The index configuration is part of WHAT WAS MEASURED, not of how
            # it was invoked. A file recording 22 latencies without saying
            # whether the row stores had their PK/FK indexes describes two very
            # different experiments with the same numbers -- measured, the
            # difference is 1.52x vs 0.87x against Postgres at SF=1.
            json.dump({"catalog": catalog, "reps": args.reps,
                       "warmups": args.warmups, "engines": list(runners),
                       "indexes": ("none (--no-index)" if args.no_index else
                                   "TPC-H spec PK/FK on sqlite+postgres, ANALYZE"),
                       "postgres_parallelism": ("disabled "
                                                "(max_parallel_workers_per_gather=0)"
                                                if args.pg_single_threaded
                                                else "server default"),
                       "swiftql_bin": args.swiftql_bin,
                       "results": results}, f, indent=2, sort_keys=True)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
