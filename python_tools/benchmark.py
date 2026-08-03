"""
benchmark.py — Phase 2/3/4 performance harness.

Runs 5 benchmark queries across row+Volcano, columnar+Volcano, and
columnar+Vectorized, reports latency (ms, avg of 5 runs) and input throughput.

Phase 4 (Week 23) section: the same vectorized configuration with and without
--no-optimize, plus per-node cardinality estimation accuracy (q-error) parsed
from the est=/rows_out= pairs in --explain-analyze output.
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

# Week 23: optimizer on/off, vectorized mode only (the optimizer never runs elsewhere).
# Query 2 is expected to show ~1.0x: with no filter, estimates track raw table sizes,
# so the cost model and the pre-Week-22 heuristic pick the same build side.
VEC_MODE = ["--storage", "columnar", "--execution", "vectorized"]

OPTIMIZER_QUERIES = [
    ("Conjunct ordering (single table)",
     "SELECT AVG(speed) FROM laps WHERE season = 2025 AND speed > 300"),

    ("Join build-side selection (no filter)",
     "SELECT laps.team, COUNT(*) FROM laps JOIN drivers "
     "ON laps.driver_id = drivers.driver_id GROUP BY laps.team"),

    ("Pushdown below join (filtered both sides)",
     "SELECT laps.team, COUNT(*) FROM laps JOIN drivers "
     "ON laps.driver_id = drivers.driver_id "
     "WHERE laps.season = 2025 AND drivers.age > 30 GROUP BY laps.team"),
]

# main.cc printAligned emits "rows_out=48203   est=51200" per node line
EST_RE = re.compile(r'rows_out=(\d+)\s+est=(\d+)')


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


def q_error(est: int, actual: int) -> float:
    """Standard cardinality-estimation error: max/min ratio, >= 1.0, symmetric."""
    lo, hi = min(est, actual), max(est, actual)
    if lo == 0:
        # 0/0 is a perfect estimate; a one-sided zero is penalized (hi+1), never
        # scored 1.0 — est=0 vs actual=1 used to read as a perfect estimate
        return 1.0 if hi == 0 else float(hi + 1)
    return hi / lo


def estimate_errors(binary, catalog, query):
    """Per-node q-errors from one optimized --explain-analyze run.

    Estimates only exist in optimized mode — never point this at --no-optimize
    (no est= tokens there; an empty list would read as "no error").
    Estimates are deterministic per process (stats recomputed at load), so one
    run suffices.
    """
    cmd = [binary, "--catalog", catalog] + VEC_MODE + ["--explain-analyze", "--query", query]
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        return []
    return [q_error(int(e), int(a)) for a, e in EST_RE.findall(result.stdout)]


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

    # Phase 4 (Week 23): optimizer impact + estimation accuracy
    print("\n\n=== Optimizer Impact (Col + Vectorized, avg of 5 runs) ===\n")
    header = (f"{'Query':<{col_w}} {'No-Opt (ms)':>12} {'Opt (ms)':>10} {'Speedup':>8}"
              f" {'Max q-err':>10} {'Med q-err':>10}")
    print(header)
    print("-" * len(header))

    for label, query in OPTIMIZER_QUERIES:
        noopt_ms, _ = benchmark_query(args.binary, args.catalog, VEC_MODE + ["--no-optimize"], label, query)
        opt_ms, _   = benchmark_query(args.binary, args.catalog, VEC_MODE, label, query)
        qerrs = sorted(estimate_errors(args.binary, args.catalog, query))
        if noopt_ms and opt_ms:
            max_q = f"{qerrs[-1]:>9.1f}x" if qerrs else f"{'N/A':>10}"
            med_q = f"{qerrs[len(qerrs)//2]:>9.1f}x" if qerrs else f"{'N/A':>10}"
            print(f"{label:<{col_w}} {noopt_ms:>12.1f} {opt_ms:>10.1f}"
                  f" {noopt_ms/opt_ms:>7.2f}x {max_q} {med_q}")
        else:
            print(f"{label:<{col_w}} {'ERR':>12} {'ERR':>10} {'N/A':>8} {'N/A':>10} {'N/A':>10}")


if __name__ == "__main__":
    main()
