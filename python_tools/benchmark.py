"""
benchmark.py — Phase 2/3 performance harness.

Runs 5 benchmark queries across row+Volcano, columnar+Volcano, and
columnar+Vectorized, reports latency (ms, avg of 5 runs) and input throughput.
"""

import argparse
import re
import subprocess
import sys
import os

SWIFTQL_BIN = "./build/swiftql"
CATALOG_PATH = "catalog.json"
RUNS = 5
TABLE_ROWS = 1_000_000   # laps table input size — throughput denominator

# The 5 queries from README Week 12 benchmark table, each testing a specific investment
BENCHMARK_QUERIES = [
    ("Full scan aggregate",
     "SELECT AVG(speed) FROM laps"),

    ("Selective filter + zone-map pruning",
     "SELECT COUNT(*) FROM laps WHERE season = 2025"),

    ("Projection pushdown (2 of 9 cols)",
     "SELECT team, speed FROM laps WHERE speed > 300"),

    ("GROUP BY dictionary-encoded string",
     "SELECT team, COUNT(*) FROM laps GROUP BY team"),

    ("Hash join + aggregate",
     "SELECT laps.team, AVG(laps.speed) FROM laps JOIN drivers "
     "ON laps.driver_id = drivers.driver_id GROUP BY laps.team"),
]


def run_once(binary: str, catalog: str, mode_args: list, query: str):
    """
    Call the binary once and return execution time in µs, or None on error.

    --explain-analyze already bypasses the in-process result cache.
    The binary is a fresh process each call, so cross-run caching is impossible.
    """
    cmd = [binary, "--catalog", catalog] + mode_args + ["--explain-analyze", "--query", query]
    result = subprocess.run(cmd, capture_output=True, text=True)

    if result.returncode != 0:
        print(f"  FAILED: {result.stderr.strip()}", file=sys.stderr)
        return None

    # main.cc format: "Execution: 72000.0µs"
    # µ is U+00B5, encoded as \xc2\xb5 in UTF-8. Python re handles it fine.
    match = re.search(r'Execution:\s+([\d.]+)µs', result.stdout)
    if not match:
        # Debug aid: uncomment if the regex doesn't fire
        # print(repr(result.stdout[-200:]), file=sys.stderr)
        print("  Could not parse Execution line", file=sys.stderr)
        return None

    return float(match.group(1))   # µs


def benchmark_query(binary, catalog, mode_args, label, query):
    """Average RUNS executions. Returns (avg_ms, rows_per_sec) or (None, None)."""
    timings_us = []
    for run_num in range(RUNS):
        us = run_once(binary, catalog, mode_args, query)
        if us is None:
            return None, None
        timings_us.append(us)

    avg_us = sum(timings_us) / len(timings_us)
    avg_ms = avg_us / 1000.0
    rows_per_sec = TABLE_ROWS / (avg_us / 1_000_000)   # input throughput
    return avg_ms, rows_per_sec


def main():
    parser = argparse.ArgumentParser(description="Phase 2/3 benchmark harness")
    parser.add_argument("--binary", default=SWIFTQL_BIN)
    parser.add_argument("--catalog", default=CATALOG_PATH)
    args = parser.parse_args()

    MODES = [
        ("row", ["--storage", "row"]),
        ("col", ["--storage", "columnar"]),
        ("vec", ["--storage", "columnar", "--execution", "vectorized"]),
    ]
    results = {}   # (label, mode_name) -> (avg_ms, rows_per_sec)

    for name, mode_args in MODES:
        print(f"\n=== {name} ===")
        for label, query in BENCHMARK_QUERIES:
            print(f"  {label}... ", end="", flush=True)
            avg_ms, rps = benchmark_query(args.binary, args.catalog, mode_args, label, query)
            if avg_ms is not None:
                results[(label, name)] = (avg_ms, rps)
                print(f"{avg_ms:.1f}ms  ({rps / 1e6:.2f}M rows/sec)")
            else:
                results[(label, name)] = (None, None)
                print("FAILED")

    # Comparison table
    print("\n\n=== Phase 3 Benchmark Results (1M rows, avg of 5 runs) ===\n")
    col_w = 42
    header = (f"{'Query':<{col_w}} {'Row (ms)':>10} {'Col (ms)':>10} {'Vec (ms)':>10}"
              f" {'Col/Row':>8} {'Vec/Row':>8}")
    print(header)
    print("-" * len(header))

    for label, _ in BENCHMARK_QUERIES:
        row_ms, _ = results.get((label, "row"), (None, None))
        col_ms, _ = results.get((label, "col"), (None, None))
        vec_ms, _ = results.get((label, "vec"), (None, None))
        if row_ms and col_ms and vec_ms:
            print(f"{label:<{col_w}} {row_ms:>10.1f} {col_ms:>10.1f} {vec_ms:>10.1f}"
                  f" {row_ms/col_ms:>7.2f}x {row_ms/vec_ms:>7.2f}x")
        else:
            print(f"{label:<{col_w}} {'ERR':>10} {'ERR':>10} {'ERR':>10} {'N/A':>8} {'N/A':>8}")


if __name__ == "__main__":
    main()
