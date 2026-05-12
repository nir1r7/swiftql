#!/usr/bin/env python3
"""
run_queries.py - run a batch of SQL queries against the SwiftQL binary and print results.

Usage:
  python3 python_tools/run_queries.py                        # run default query set and output results
  python3 python_tools/run_queries.py --queries my.sql       # load queries from file and output results
  python3 python_tools/run_queries.py --explain              # show plan tree only
  python3 python_tools/run_queries.py --explain-analyze      # show annotated plan after exec

  # --binary specifies path to executable to run query against (default: ./build/swiftql)
  # --catalog specifies path to catalog JSON file (default: catalog.json)
  python3 python_tools/run_queries.py --catalog catalog.json --binary ./build/swiftql 

  custom query:
  ./build/swiftql --catalog catalog.json --explain-analyze --query "SELECT DISTINCT team FROM laps ORDER BY team"
"""

import argparse
import subprocess
import sys
import time

SWIFTQL_BIN = "./build/swiftql"
CATALOG_PATH = "catalog.json"

DEFAULT_QUERIES = [
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
]


def load_queries_from_file(path: str) -> list[str]:
    with open(path) as f:
        text = f.read()
    # split on semicolons, strip whitespace, drop empty entries
    return [q.strip() for q in text.split(";") if q.strip()]


def run_query(binary: str, catalog: str, query: str, extra_flags: list[str]) -> tuple[str, str, int, float]:
    cmd = [binary, "--catalog", catalog, "--no-cache", "--query", query] + extra_flags
    t0 = time.monotonic()
    result = subprocess.run(cmd, capture_output=True, text=True)
    elapsed = time.monotonic() - t0
    return result.stdout, result.stderr, result.returncode, elapsed


def main():
    parser = argparse.ArgumentParser(description="Run SQL queries against SwiftQL.")
    parser.add_argument("--catalog", default=CATALOG_PATH)
    parser.add_argument("--binary", default=SWIFTQL_BIN)
    parser.add_argument("--queries", metavar="FILE", help="SQL file (semicolon-separated queries)")
    parser.add_argument("--explain", action="store_true")
    parser.add_argument("--explain-analyze", action="store_true")
    args = parser.parse_args()

    queries = load_queries_from_file(args.queries) if args.queries else DEFAULT_QUERIES

    extra_flags = []
    if args.explain:
        extra_flags.append("--explain")
    if args.explain_analyze:
        extra_flags.append("--explain-analyze")

    passed = 0
    failed = 0
    total_wall = 0.0

    for i, query in enumerate(queries, 1):
        print(f"\n[{i}/{len(queries)}] {query}")
        print("-" * 72)

        stdout, stderr, rc, elapsed = run_query(args.binary, args.catalog, query, extra_flags)
        total_wall += elapsed

        if rc != 0:
            print(f"ERROR ({elapsed*1e6:.1f}µs): {stderr.strip()}")
            failed += 1
        else:
            print(stdout, end="")
            print(f"Process Overhead: {elapsed*1e6:.1f}µs \n")
            passed += 1

    print("\n" + "=" * 72)
    print(f"{passed} ok, {failed} failed | {len(queries)} queries | {total_wall*1e6:.1f}µs total")
    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
