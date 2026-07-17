#!/usr/bin/env python3
"""
compare_against_sqlite.py - runs queries against SwiftQL and SQLite, diffs results.
Usage: python3 compare_against_sqlite.py
"""

import subprocess
import sqlite3
import csv
import sys
import os

SWIFTQL_BIN = "./build/swiftql"
CATALOG_PATH = "catalog.json"
LAPS_CSV = "data/laps.csv"
DRIVERS_CSV = "data/drivers.csv"

# README Phase 2 / Week 12 benchmark queries. (SwiftQL now parses table aliases;
# the qualified form here matches benchmark.py and stays alias-free for parity.)
PHASE2_WEEK12_BENCHMARK_QUERIES = [
    "SELECT AVG(speed) FROM laps",
    "SELECT COUNT(*) FROM laps WHERE season = 2025",
    "SELECT team, speed FROM laps WHERE speed > 300",
    "SELECT team, COUNT(*) FROM laps GROUP BY team",
    "SELECT laps.team, AVG(laps.speed) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id GROUP BY laps.team",
]
PHASE2_WEEK12_BENCHMARK_QUERY_SET = set(PHASE2_WEEK12_BENCHMARK_QUERIES)

# Broader regression queries.
REGRESSION_QUERIES = [
    "SELECT team FROM laps LIMIT 5",
    "SELECT COUNT(*) FROM laps",
    "SELECT AVG(speed) FROM laps",
    "SELECT MIN(speed), MAX(speed) FROM laps",
    "SELECT team, COUNT(*) FROM laps GROUP BY team",
    "SELECT team, AVG(speed) FROM laps GROUP BY team ORDER BY team",
    "SELECT * FROM laps WHERE season = 2025 LIMIT 10",
    "SELECT team, speed FROM laps WHERE speed > 330",
    "SELECT team FROM laps WHERE speed IS NULL",
    "SELECT team FROM laps WHERE speed IS NOT NULL LIMIT 5",
    "SELECT DISTINCT team FROM laps ORDER BY team",
    "SELECT DISTINCT season FROM laps ORDER BY season",
    "SELECT team, AVG(speed) FROM laps GROUP BY team HAVING AVG(speed) > 310",
    "SELECT team, COUNT(*) FROM laps WHERE season = 2025 GROUP BY team",
    "SELECT team, SUM(speed) FROM laps GROUP BY team ORDER BY team LIMIT 3",
    "SELECT team, MIN(speed), MAX(speed) FROM laps GROUP BY team ORDER BY team",
    "SELECT COUNT(*) FROM laps WHERE season = 2024 AND speed > 300",
    "SELECT DISTINCT team FROM laps WHERE season = 2025 ORDER BY team",
    "SELECT team, AVG(speed) FROM laps WHERE season = 2025 GROUP BY team ORDER BY team",
    "SELECT DISTINCT team, season FROM laps ORDER BY team, season LIMIT 10",
    "SELECT team, COUNT(*) FROM laps GROUP BY team ORDER BY COUNT(*) LIMIT 5",
    "SELECT team, AVG(speed) FROM laps WHERE speed IS NOT NULL GROUP BY team HAVING AVG(speed) > 305 ORDER BY team",
    "SELECT season, speed FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id LIMIT 5",
    "SELECT season, AVG(speed) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id GROUP BY season ORDER BY season",
    "SELECT season, COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id WHERE speed > 300 GROUP BY season ORDER BY season",
]

# Week 6 checkpoint query: full operator pipeline in one statement
# (WHERE + IS NOT NULL + GROUP BY + HAVING + DISTINCT + ORDER BY + LIMIT).
WEEK6_CHECKPOINT_QUERIES = [
    "SELECT DISTINCT team, AVG(speed) FROM laps WHERE season = 2025 AND speed IS NOT NULL "
    "GROUP BY team HAVING AVG(speed) > 300 ORDER BY team LIMIT 10",
]

# Week 10 zone-map pruning: chunk-boundary predicates the pruner must evaluate
# without dropping matching rows (`<` was previously untested; only `=`/`>` were covered).
# Week 16 zero-row plan reporting: predicate matches nothing in any chunk.
ZONE_MAP_QUERIES = [
    "SELECT COUNT(*) FROM laps WHERE speed < 290",
    "SELECT team FROM laps WHERE speed < 290 ORDER BY team LIMIT 5",
    "SELECT team FROM laps WHERE season = 1900",
]

# Self-join + qualified-column disambiguation (Week 16 alias/binder work).
# Self-joins use the unique key lap_id (1:1) so output stays bounded, then
# aggregate down. Aggregate/projected columns have distinct names so the
# harness compares them positionally (duplicate names collapse in its dict-based
# row model). Correctness of distinct same-named side values is locked by unit
# tests (SelfJoin.DistinctSideValuesResolveIndependently).
SELF_JOIN_QUERIES = [
    "SELECT COUNT(*) FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.lap_id",
    "SELECT l1.season, COUNT(*) FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.lap_id GROUP BY l1.season ORDER BY l1.season",
    "SELECT l1.team, AVG(l2.speed) FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.lap_id GROUP BY l1.team ORDER BY l1.team",
    # qualified column disambiguation: drivers.team resolves to the JOIN side
    "SELECT drivers.team, COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id GROUP BY drivers.team ORDER BY drivers.team",
]

QUERIES = PHASE2_WEEK12_BENCHMARK_QUERIES + [
    query for query in REGRESSION_QUERIES
    if query not in PHASE2_WEEK12_BENCHMARK_QUERY_SET
] + WEEK6_CHECKPOINT_QUERIES + ZONE_MAP_QUERIES + SELF_JOIN_QUERIES

# SQLite setup
def load_sqlite():
    """Load CSVs into an in-memory SQLite database."""
    conn = sqlite3.connect(":memory:")
    conn.row_factory = sqlite3.Row

    # create and load laps
    conn.execute("""
        CREATE TABLE laps (
            lap_id INTEGER, driver_id INTEGER, team TEXT, speed REAL,
            sector_1 REAL, sector_2 REAL, sector_3 REAL, season INTEGER, round INTEGER
        )
    """)
    with open(LAPS_CSV) as f:
        reader = csv.DictReader(f)
        for row in reader:
            conn.execute("INSERT INTO laps VALUES (?,?,?,?,?,?,?,?,?)", (
                int(row['lap_id']), int(row['driver_id']), row['team'],
                float(row['speed']), float(row['sector_1']),
                float(row['sector_2']), float(row['sector_3']),
                int(row['season']), int(row['round'])
            ))

    # create and load drivers
    conn.execute("""
        CREATE TABLE drivers (
            driver_id INTEGER, name TEXT, nationality TEXT, team TEXT, age INTEGER
        )
    """)
    with open(DRIVERS_CSV) as f:
        reader = csv.DictReader(f)
        for row in reader:
            conn.execute("INSERT INTO drivers VALUES (?,?,?,?,?)", (
                int(row['driver_id']), row['name'], row['nationality'],
                row['team'], int(row['age'])
            ))
    conn.commit()
    return conn


def parse_swiftql_output(output: str):
    """Parse aligned table output into list of dicts."""
    lines = [l for l in output.strip().split('\n') if l.strip()]
    if len(lines) < 3:
        return []  # header + separator + at least one row

    headers = lines[0].split()
    rows = []
    for line in lines[2:]:  # skip header and separator
        if line.startswith('(') and line.endswith(')'):
            break  # row count footer
        # split on 2+ spaces to handle aligned columns
        import re
        values = re.split(r'  +', line.strip())
        if len(values) == len(headers):
            rows.append(dict(zip(headers, values)))
    return rows


def normalize(rows):
    """Sort rows and coerce numbers for stable comparison."""
    def coerce(v):
        try: return round(float(v), 6)
        except (ValueError, TypeError): return str(v)

    normalized = []
    for row in rows:
        normalized.append(tuple(coerce(v) for v in row.values()))
    return sorted(normalized)


def rows_equal(a, b):
    """Compare two sorted normalized row lists, using epsilon tolerance for floats."""
    if len(a) != len(b):
        return False
    for row_a, row_b in zip(a, b):
        if len(row_a) != len(row_b):
            return False
        for x, y in zip(row_a, row_b):
            if isinstance(x, float) and isinstance(y, float):
                if abs(x - y) > 1e-5:
                    return False
            else:
                if x != y:
                    return False
    return True


def run_swiftql(query: str, extra_args: list = None):
    args = [SWIFTQL_BIN, "--catalog", CATALOG_PATH, "--no-cache", "--query", query]
    if extra_args:
        args = args[:4] + extra_args + args[4:]  # insert after --no-cache
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())
    return parse_swiftql_output(result.stdout)


def run_query_suite(conn, queries, label: str, extra_args: list = None):
    passed = failed = errors = 0
    print(f"\n--- {label} ---")
    for query in queries:
        try:
            swift_rows = run_swiftql(query, extra_args)
            sqlite_cursor = conn.execute(query)
            cols = [d[0] for d in sqlite_cursor.description]
            sqlite_rows = [dict(zip(cols, r)) for r in sqlite_cursor.fetchall()]

            if rows_equal(normalize(swift_rows), normalize(sqlite_rows)):
                print(f"  PASS  {query[:70]}")
                passed += 1
            else:
                print(f"  FAIL  {query[:70]}")
                print(f"    SwiftQL: {normalize(swift_rows)[:3]}")
                print(f"    SQLite:  {normalize(sqlite_rows)[:3]}")
                failed += 1
        except Exception as e:
            print(f"  ERROR {query[:70]}\n    {e}")
            errors += 1
    print(f"{passed} passed, {failed} failed, {errors} errors")
    return passed, failed, errors


# main
def main():
    conn = load_sqlite()

    p1, f1, e1 = run_query_suite(conn, QUERIES, "Default (row storage, Volcano)")
    p2, f2, e2 = run_query_suite(
        conn, QUERIES, "Volcano (columnar storage, Week 8 storage-mode checkpoint)",
        extra_args=["--storage", "columnar"],
    )
    p3, f3, e3 = run_query_suite(
        conn, QUERIES, "Vectorized (columnar storage, vec path)",
        extra_args=["--execution", "vectorized", "--storage", "columnar"],
    )

    total_passed = p1 + p2 + p3
    total_failed = f1 + f2 + f3
    total_errors = e1 + e2 + e3
    print(f"\nTotal: {total_passed} passed, {total_failed} failed, {total_errors} errors")
    if total_failed > 0 or total_errors > 0:
        sys.exit(1)

if __name__ == "__main__":
    main()
