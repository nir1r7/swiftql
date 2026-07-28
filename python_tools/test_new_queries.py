#!/usr/bin/env python3
"""
test_new_queries.py — regression queries not covered by existing unit tests or compare_against_sqlite.py.

Covers query-observable SQL surface implemented in Weeks 1-21 (README plan): comparison/OR/AND
predicates, multi-column projection, all aggregates, single/multi GROUP BY, HAVING, DISTINCT,
ORDER BY, LIMIT, IS NULL / IS NOT NULL (Week 6), JOIN ... ON (Week 11), and table aliases /
qualified columns (Week 16). Weeks 8-15 and 17-20 are storage/execution/optimizer internals that
must not change results — exercised implicitly by every query above.

Week 21 (predicate pushdown + selection-vector cascade) is an optimizer rewrite that only runs on
the columnar/vectorized path, so the whole suite is also run in --execution vectorized --storage
columnar mode, and a dedicated WEEK21_QUERIES block exercises the pushdown-specific shapes (WHERE
predicates on one or both join sides, mixed cross-relation residuals, OR pushed as a unit, and
multi-conjunct scan-local filters that the executor cascades). The optimizer must not change any
result vs SQLite or vs --no-optimize.

Skills applied:
  - execution-state-simulation: traced Volcano open/next/close for each query category
  - invariant-extraction: verified schema width, row-count monotonicity, iterator lifecycle
  - semantic-drift-detection: targeted OR semantics, multi-col GROUP BY, COUNT(col) vs COUNT(*),
                              multiple aggregates on empty group key, NULL pass-through, drivers table
  - minimal-fix-strategy: bugs found here will be fixed at the smallest causal stage
"""

import subprocess
import sqlite3
import csv
import sys
import re
import os

SWIFTQL_BIN = "./build/swiftql"
CATALOG_PATH = "catalog.json"
LAPS_CSV = "data/laps.csv"
DRIVERS_CSV = "data/drivers.csv"

# ─── 30 new queries ─────────────────────────────────────────────────────────
# Each entry: (label, query)
# Selection rationale per skill:
#   OR queries            → semantic-drift: OR short-circuit in FilterNode
#   != / <= / >=          → semantic-drift: comparison operators with real data
#   Multi-col SELECT      → invariant-extraction: schema width = N columns
#   Multiple agg, no GB   → execution-state-simulation: empty group key path
#   GROUP BY 2 cols       → invariant-extraction: composite group key hashing
#   HAVING COUNT / SUM    → execution-state-simulation: post-agg filter
#   HAVING with AND       → semantic-drift: AND inside HAVING predicate
#   COUNT(col)            → semantic-drift: COUNT(col) skips NULLs vs COUNT(*)
#   drivers table         → invariant-extraction: catalog lookup, schema mismatch risk
#   MIN/MAX on INT/STRING → semantic-drift: aggregate type compatibility

QUERIES = [
    # ── OR in WHERE ──────────────────────────────────────────────────────────
    ("OR_two_string_values",
     "SELECT COUNT(*) FROM laps WHERE team = 'Ferrari' OR team = 'RedBull'"),

    ("OR_two_season_values",
     "SELECT COUNT(*) FROM laps WHERE season = 2022 OR season = 2025"),

    ("OR_mixed_columns",
     "SELECT COUNT(*) FROM laps WHERE team = 'Ferrari' OR speed > 340"),

    # ── != / <= / >= comparisons ─────────────────────────────────────────────
    ("NEQ_string_where",
     "SELECT COUNT(*) FROM laps WHERE team != 'Ferrari'"),

    ("NEQ_with_season_filter",
     "SELECT COUNT(*) FROM laps WHERE team != 'Ferrari' AND season = 2025"),

    ("LTE_speed",
     "SELECT COUNT(*) FROM laps WHERE speed <= 285"),

    ("GTE_speed",
     "SELECT COUNT(*) FROM laps WHERE speed >= 340"),

    ("LTE_season",
     "SELECT COUNT(*) FROM laps WHERE season <= 2023"),

    # ── multi-column SELECT, no aggregates ──────────────────────────────────
    ("multi_col_select_no_agg",
     "SELECT lap_id, team, season FROM laps LIMIT 5"),

    ("select_sector_columns",
     "SELECT lap_id, sector_1, sector_2, sector_3 FROM laps LIMIT 5"),

    # ── multiple aggregates, no GROUP BY (empty group key path) ─────────────
    ("multi_agg_no_group_by",
     "SELECT COUNT(*), MIN(speed), MAX(speed), AVG(speed) FROM laps"),

    ("sum_and_avg_no_group_by",
     "SELECT SUM(speed), AVG(speed) FROM laps WHERE season = 2024"),

    # ── GROUP BY two columns ─────────────────────────────────────────────────
    ("group_by_two_cols",
     "SELECT team, season, COUNT(*) FROM laps GROUP BY team, season ORDER BY team, season LIMIT 8"),

    ("group_by_team_round",
     "SELECT team, round, COUNT(*) FROM laps GROUP BY team, round ORDER BY team, round LIMIT 8"),

    # ── HAVING with COUNT(*) threshold ──────────────────────────────────────
    ("having_count_threshold",
     "SELECT team, COUNT(*) FROM laps GROUP BY team HAVING COUNT(*) > 100 ORDER BY team"),

    ("having_count_low_threshold",
     "SELECT team, COUNT(*) FROM laps GROUP BY team HAVING COUNT(*) > 40 ORDER BY team"),

    # ── HAVING with AND ──────────────────────────────────────────────────────
    ("having_and_avg_range",
     "SELECT team, AVG(speed) FROM laps GROUP BY team HAVING AVG(speed) > 308 AND AVG(speed) < 314 ORDER BY team"),

    # ── HAVING with SUM ──────────────────────────────────────────────────────
    ("having_sum_threshold",
     "SELECT team, SUM(speed) FROM laps GROUP BY team HAVING SUM(speed) > 70000 ORDER BY team"),

    # ── COUNT(col) vs COUNT(*) ───────────────────────────────────────────────
    ("count_col_vs_star",
     "SELECT COUNT(driver_id) FROM laps"),

    ("count_col_with_filter",
     "SELECT COUNT(speed) FROM laps WHERE season = 2025"),

    # ── MIN / MAX on integer column ──────────────────────────────────────────
    ("min_max_int_season",
     "SELECT MIN(season), MAX(season) FROM laps"),

    ("min_max_int_round",
     "SELECT MIN(round), MAX(round) FROM laps"),

    # ── DISTINCT + WHERE + ORDER BY ──────────────────────────────────────────
    ("distinct_where_order",
     "SELECT DISTINCT team FROM laps WHERE speed > 320 ORDER BY team"),

    ("distinct_round_where_season",
     "SELECT DISTINCT round FROM laps WHERE season = 2025 ORDER BY round"),

    # ── ORDER BY numeric column (no GROUP BY) ────────────────────────────────
    ("order_by_speed_asc",
     "SELECT lap_id, speed FROM laps ORDER BY speed LIMIT 5"),

    # ── drivers table: not tested anywhere ───────────────────────────────────
    ("drivers_simple_filter",
     "SELECT name FROM drivers WHERE age > 35 ORDER BY name"),

    ("drivers_group_by_nationality",
     "SELECT nationality, COUNT(*) FROM drivers GROUP BY nationality ORDER BY nationality"),

    ("drivers_min_max_age",
     "SELECT MIN(age), MAX(age) FROM drivers"),

    ("drivers_group_by_team",
     "SELECT team, COUNT(*) FROM drivers GROUP BY team ORDER BY team"),

    ("drivers_having_avg_age",
     "SELECT team, AVG(age) FROM drivers GROUP BY team HAVING AVG(age) > 30 ORDER BY team"),

    # ── scalar aggregate over empty filter result ───────────────────────────
    # execution-state-simulation: no GROUP BY + zero input rows must still emit
    # one row with COUNT=0. season=1999 matches no rows. (The SUM/AVG/MIN/MAX=NULL
    # case is asserted in the C++ test HashAggregateNode.ScalarAggregateOverEmptyInput...;
    # it is omitted here because the vectorized engine has no null bitmap and renders
    # empty SUM/MIN/MAX as 0.0 sentinels — a separate, pre-existing limitation.)
    ("empty_scalar_count",
     "SELECT COUNT(*) FROM laps WHERE season = 1999"),

    # ── IS NULL / IS NOT NULL (Week 6) ───────────────────────────────────────
    # invariant-extraction: null-aware predicate path; CSV holds no nulls, so
    # IS NOT NULL passes every row and IS NULL passes none — both must match SQLite.
    ("is_not_null_speed",
     "SELECT COUNT(*) FROM laps WHERE speed IS NOT NULL"),

    ("is_null_no_rows",
     "SELECT lap_id FROM laps WHERE speed IS NULL"),

    ("is_null_scalar_count",
     "SELECT COUNT(*) FROM laps WHERE speed IS NULL"),

    # ── plain < in WHERE (Week 6 comparison completeness) ────────────────────
    ("lt_speed_where",
     "SELECT COUNT(*) FROM laps WHERE speed < 300"),

    # ── JOIN ... ON (Week 11 hash join execution) ────────────────────────────
    # 1:1 join on driver_id → row count preserved. GROUP BY resolves against the
    # merged FROM+JOIN schema, so grouping on a joined-table column is supported.
    ("join_count_all",
     "SELECT COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id"),

    ("join_group_by_from_col",
     "SELECT l.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team ORDER BY l.team"),

    ("join_group_by_joined_col",
     "SELECT nationality, COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id GROUP BY nationality ORDER BY nationality"),

    ("join_filtered_avg",
     "SELECT AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id WHERE l.season = 2025"),

    ("join_count_with_filter",
     "SELECT COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id WHERE laps.speed > 340"),

    # ── table aliases + qualified columns (Week 16 binder) ───────────────────
    ("qualified_col_in_where",
     "SELECT COUNT(*) FROM laps WHERE laps.season = 2025"),

    ("alias_projection",
     "SELECT l.team, l.speed FROM laps l WHERE l.speed >= 344"),

    ("alias_drivers_filter",
     "SELECT d.name, d.age FROM drivers d WHERE d.age > 36 ORDER BY d.name"),
]


# ─── Week 21: predicate pushdown + selection-vector cascade ──────────────────
# These specifically exercise the Week 21 optimizer and are run on the
# vectorized path (where pushdown/cascade live) against SQLite AND against the
# same query with --no-optimize (result-preserving invariant).
#   both-sides pushdown   → checkpoint: both join inputs filtered before the join
#   from-only / join-only → single-relation predicate lands on its own scan
#   mixed residual        → cross-relation conjunct stays above the join
#   OR pushed as a unit    → an OR referencing one relation is still single-slot
#   multi-conjunct scan   → executor cascades the ordered conjuncts
#   self-join + WHERE      → slot routing pushes each side to the right scan
WEEK21_QUERIES = [
    ("pd_both_sides",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 AND d.age > 35 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_from_only",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_join_only",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE d.age > 35 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_mixed_residual",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 AND l.speed > d.age ORDER BY l.team, d.name LIMIT 25"),

    ("pd_or_one_side",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE (l.season = 2024 OR l.season = 2025) AND d.age > 35 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_three_conjuncts",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 AND l.speed > 340 AND d.age > 35 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_agg_after_pushdown",
     "SELECT l.team, AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 AND d.age > 35 GROUP BY l.team ORDER BY l.team"),

    ("cascade_single_table_and",
     "SELECT team, speed FROM laps WHERE season = 2025 AND speed > 340 ORDER BY team, speed LIMIT 25"),

    ("cascade_triple_and",
     "SELECT team FROM laps WHERE season = 2025 AND speed > 330 AND speed < 360 ORDER BY team LIMIT 25"),

    ("self_join_where_both",
     "SELECT l1.team FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.lap_id "
     "WHERE l1.season = 2025 AND l2.speed > 340 ORDER BY l1.team LIMIT 25"),
]


def load_sqlite():
    conn = sqlite3.connect(":memory:")
    conn.row_factory = sqlite3.Row
    conn.execute("""
        CREATE TABLE laps (
            lap_id INTEGER, driver_id INTEGER, team TEXT, speed REAL,
            sector_1 REAL, sector_2 REAL, sector_3 REAL, season INTEGER, round INTEGER
        )
    """)
    with open(LAPS_CSV) as f:
        for row in csv.DictReader(f):
            conn.execute("INSERT INTO laps VALUES (?,?,?,?,?,?,?,?,?)", (
                int(row['lap_id']), int(row['driver_id']), row['team'],
                float(row['speed']), float(row['sector_1']),
                float(row['sector_2']), float(row['sector_3']),
                int(row['season']), int(row['round'])
            ))
    conn.execute("""
        CREATE TABLE drivers (
            driver_id INTEGER, name TEXT, nationality TEXT, team TEXT, age INTEGER
        )
    """)
    with open(DRIVERS_CSV) as f:
        for row in csv.DictReader(f):
            conn.execute("INSERT INTO drivers VALUES (?,?,?,?,?)", (
                int(row['driver_id']), row['name'], row['nationality'],
                row['team'], int(row['age'])
            ))
    conn.commit()
    return conn


def parse_swiftql_output(output: str):
    lines = [l for l in output.strip().split('\n') if l.strip()]
    if len(lines) < 2:
        return []
    headers = lines[0].split()
    rows = []
    for line in lines[2:]:
        if line.startswith('(') and line.endswith(')'):
            break
        values = re.split(r'  +', line.strip())
        if len(values) == len(headers):
            rows.append(dict(zip(headers, values)))
    return rows


def normalize(rows):
    def coerce(v):
        try:
            return round(float(v), 6)
        except (ValueError, TypeError):
            return str(v)
    return sorted(tuple(coerce(v) for v in row.values()) for row in rows)


def run_swiftql(query: str, extra_args=None):
    args = [SWIFTQL_BIN, "--catalog", CATALOG_PATH, "--no-cache", "--query", query]
    if extra_args:
        args = args[:4] + extra_args + args[4:]  # insert after --no-cache
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"SwiftQL error: {result.stderr.strip()}")
    return parse_swiftql_output(result.stdout)


def run_suite(conn, queries, mode_label, extra_args=None):
    """Compare each query's SwiftQL output against SQLite. Returns (passed, failed, errors, fail_list)."""
    passed, failed, errors = 0, 0, 0
    fail_list = []
    print(f"\n--- {mode_label} ---")
    for label, query in queries:
        try:
            swift_rows = run_swiftql(query, extra_args)
            cur = conn.execute(query)
            cols = [d[0] for d in cur.description]
            sqlite_rows = [dict(zip(cols, r)) for r in cur.fetchall()]

            sw_norm = normalize(swift_rows)
            sq_norm = normalize(sqlite_rows)

            if sw_norm == sq_norm:
                print(f"  PASS  [{label}]  {query[:66]}")
                passed += 1
            else:
                print(f"  FAIL  [{label}]  {query[:66]}")
                print(f"    SwiftQL ({len(swift_rows)} rows): {sw_norm[:3]}")
                print(f"    SQLite  ({len(sqlite_rows)} rows): {sq_norm[:3]}")
                failed += 1
                fail_list.append((f"{mode_label}:{label}", query, sw_norm[:3], sq_norm[:3]))
        except Exception as e:
            print(f"  ERROR [{label}]  {query[:66]}")
            print(f"    {e}")
            errors += 1
            fail_list.append((f"{mode_label}:{label}", query, str(e), ""))
    print(f"  {passed} passed, {failed} failed, {errors} errors")
    return passed, failed, errors, fail_list


def run_optimizer_invariant(queries):
    """Week 21: the optimizer must be result-preserving — vectorized output must be
    identical with and without --no-optimize. Compares SwiftQL to itself, no oracle."""
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    passed, failed, errors = 0, 0, 0
    fail_list = []
    print(f"\n--- Optimizer invariant (vectorized: optimized == --no-optimize) ---")
    for label, query in queries:
        try:
            opt = normalize(run_swiftql(query, VEC))
            noopt = normalize(run_swiftql(query, VEC + ["--no-optimize"]))
            if opt == noopt:
                print(f"  PASS  [{label}]  {query[:66]}")
                passed += 1
            else:
                print(f"  FAIL  [{label}]  {query[:66]}")
                print(f"    optimized:    {opt[:3]}")
                print(f"    --no-optimize:{noopt[:3]}")
                failed += 1
                fail_list.append((f"invariant:{label}", query, opt[:3], noopt[:3]))
        except Exception as e:
            print(f"  ERROR [{label}]  {query[:66]}\n    {e}")
            errors += 1
            fail_list.append((f"invariant:{label}", query, str(e), ""))
    print(f"  {passed} passed, {failed} failed, {errors} errors")
    return passed, failed, errors, fail_list


def main():
    conn = load_sqlite()
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    all_queries = QUERIES + WEEK21_QUERIES

    # existing surface on the default row/Volcano path
    r1 = run_suite(conn, QUERIES, "Default (row storage, Volcano)")
    # whole surface + Week 21 shapes on the vectorized path (where the optimizer runs)
    r2 = run_suite(conn, all_queries, "Vectorized (columnar, optimizer ON)", extra_args=VEC)
    # Week 21 result-preserving invariant
    r3 = run_optimizer_invariant(all_queries)

    passed = r1[0] + r2[0] + r3[0]
    failed = r1[1] + r2[1] + r3[1]
    errors = r1[2] + r2[2] + r3[2]
    fail_list = r1[3] + r2[3] + r3[3]

    print(f"\n{'='*70}")
    print(f"{passed} passed, {failed} failed, {errors} errors "
          f"({len(QUERIES)} default + {len(all_queries)} vectorized + {len(all_queries)} invariant)")

    if fail_list:
        print(f"\n{'='*70}")
        print("FAILING QUERIES:")
        for label, query, actual, expected in fail_list:
            print(f"\n  [{label}]")
            print(f"  Query:    {query}")
            if isinstance(actual, str):
                print(f"  Error:    {actual}")
            else:
                print(f"  SwiftQL:  {actual}")
                print(f"  Expected: {expected}")

    return 1 if (failed > 0 or errors > 0) else 0


if __name__ == "__main__":
    sys.exit(main())
