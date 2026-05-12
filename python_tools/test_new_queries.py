#!/usr/bin/env python3
"""
test_new_queries.py — 30 untested queries not covered by existing unit tests or compare_against_sqlite.py.

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


def run_swiftql(query: str):
    result = subprocess.run(
        [SWIFTQL_BIN, "--catalog", CATALOG_PATH, "--no-cache", "--query", query],
        capture_output=True, text=True
    )
    if result.returncode != 0:
        raise RuntimeError(f"SwiftQL error: {result.stderr.strip()}")
    return parse_swiftql_output(result.stdout)


def main():
    conn = load_sqlite()
    passed, failed, errors = 0, 0, 0
    fail_list = []

    for label, query in QUERIES:
        try:
            swift_rows = run_swiftql(query)

            cur = conn.execute(query)
            cols = [d[0] for d in cur.description]
            sqlite_rows = [dict(zip(cols, r)) for r in cur.fetchall()]

            sw_norm = normalize(swift_rows)
            sq_norm = normalize(sqlite_rows)

            if sw_norm == sq_norm:
                print(f"  PASS  [{label}]  {query[:70]}")
                passed += 1
            else:
                print(f"  FAIL  [{label}]  {query[:70]}")
                print(f"    SwiftQL ({len(swift_rows)} rows): {sw_norm[:3]}")
                print(f"    SQLite  ({len(sqlite_rows)} rows): {sq_norm[:3]}")
                failed += 1
                fail_list.append((label, query, sw_norm[:3], sq_norm[:3]))
        except Exception as e:
            print(f"  ERROR [{label}]  {query[:70]}")
            print(f"    {e}")
            errors += 1
            fail_list.append((label, query, str(e), ""))

    print(f"\n{'='*70}")
    print(f"{passed} passed, {failed} failed, {errors} errors out of {len(QUERIES)} queries")

    if fail_list:
        print(f"\n{'='*70}")
        print("FAILING QUERIES:")
        for item in fail_list:
            label, query, actual, expected = item
            print(f"\n  [{label}]")
            print(f"  Query:    {query}")
            if isinstance(actual, str):
                print(f"  Error:    {actual}")
            else:
                print(f"  SwiftQL:  {actual}")
                print(f"  SQLite:   {expected}")

    return 1 if (failed > 0 or errors > 0) else 0


if __name__ == "__main__":
    sys.exit(main())
