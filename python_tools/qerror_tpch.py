#!/usr/bin/env python3
"""
qerror_tpch.py — cardinality-estimation accuracy on TPC-H.

    .venv/bin/python python_tools/qerror_tpch.py \
        --catalog data/tpch/dbgen-sf0.1/catalog.json --json docs/week-37-qerror-sf0.1.json

WHAT q-ERROR IS. For one plan node, max(est, actual) / min(est, actual): the
factor by which the estimate is wrong, in whichever direction. 1.0 is exact, 2.0
means "out by 2x either way". It is symmetric on purpose — an estimator that
under-counts by 10x and one that over-counts by 10x are equally wrong, and a
signed error would let them cancel in an average.

WHY THE GEOMETRIC MEAN AND THE MAX, NEVER THE ARITHMETIC MEAN. q-errors are
ratios spanning orders of magnitude; an arithmetic mean over them is dominated by
the single worst node and reports a number no individual estimate resembles. The
max is reported separately because it is the one that actually decides plans:
a join whose input is under-estimated 1000x gets the wrong build side.

ESTIMATES ONLY EXIST IN OPTIMIZED MODE. `--no-optimize` emits no `est=` tokens at
all, so pointing this at it yields an empty list, which would read as "no error"
rather than "not measured". The mode is fixed here rather than exposed.

WHAT THIS MEASURES AND WHAT IT DOES NOT. Estimates are deterministic per process
(statistics are recomputed at load), so one run per query suffices and there is
no repetition or warmup. This says nothing about whether a mis-estimate changed
the chosen plan — only how wrong the numbers are. A node can be 100x out and
still yield the right plan if the ordering is unaffected.
"""

import argparse
import json
import math
import os
import re
import statistics
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tpch_queries import QUERY_IDS, render  # noqa: E402

VEC_MODE = ["--storage", "columnar", "--execution", "vectorized"]

# `rows_out=N est=M` as --explain-analyze prints it, per node.
EST_RE = re.compile(r"rows_out=(\d+)\s+est=(\d+)")
NODE_RE = re.compile(r"^\s*(\w+)\s.*?rows_out=(\d+)\s+est=(\d+)", re.M)


def q_error(est, actual):
    """max/min ratio, >= 1.0, symmetric.

    A one-sided zero is penalized as hi+1 rather than scored 1.0: est=0 against
    actual=1 is a real failure (it is what makes a join pick the wrong build
    side), and 0/0 is the only case that deserves a perfect score.
    """
    lo, hi = min(est, actual), max(est, actual)
    if lo == 0:
        return 1.0 if hi == 0 else float(hi + 1)
    return hi / lo


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalog", default="data/tpch/dbgen-sf0.1/catalog.json")
    ap.add_argument("--binary", default="./build-release/swiftql")
    ap.add_argument("--queries", default="")
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    qids = [q.strip() for q in args.queries.split(",") if q.strip()] or list(QUERY_IDS)

    per_query, all_errors, record = {}, [], {}
    print(f"catalog: {args.catalog}")
    print(f"{'query':<6} {'nodes':>6} {'geomean':>9} {'max':>12}   worst node")
    print("-" * 62)
    for qid in qids:
        r = subprocess.run([args.binary, "--catalog", args.catalog] + VEC_MODE
                           + ["--explain-analyze", "--query", render(qid)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"{qid:<6} ERROR: {r.stderr.strip().splitlines()[-1][:60]}")
            continue
        nodes = [(name, int(a), int(e)) for name, a, e in NODE_RE.findall(r.stdout)]
        if not nodes:
            print(f"{qid:<6} no est= tokens (did the optimizer run?)")
            continue
        errs = [(name, q_error(est, act), act, est) for name, act, est in nodes]
        vals = [e for _, e, _, _ in errs]
        worst = max(errs, key=lambda t: t[1])
        gm = math.exp(statistics.fmean([math.log(v) for v in vals]))
        per_query[qid] = {"nodes": len(vals), "geomean": gm, "max": max(vals),
                          "worst_node": worst[0], "worst_actual": worst[2],
                          "worst_est": worst[3]}
        record[qid] = [{"node": n, "qerror": e, "actual": a, "est": s}
                       for n, e, a, s in errs]
        all_errors += vals
        print(f"{qid:<6} {len(vals):>6} {gm:>9.2f} {max(vals):>12.1f}   "
              f"{worst[0]} (actual={worst[2]}, est={worst[3]})")

    if all_errors:
        gm = math.exp(statistics.fmean([math.log(v) for v in all_errors]))
        print("-" * 62)
        print(f"{len(all_errors)} nodes across {len(per_query)} queries")
        print(f"  geometric mean q-error : {gm:.2f}")
        print(f"  median                 : {statistics.median(all_errors):.2f}")
        print(f"  90th percentile        : {sorted(all_errors)[int(len(all_errors)*0.9)]:.1f}")
        print(f"  max                    : {max(all_errors):.1f}")
        within = lambda k: sum(1 for v in all_errors if v <= k) / len(all_errors) * 100
        print(f"  within 2x / 10x / 100x : {within(2):.0f}% / {within(10):.0f}% / {within(100):.0f}%")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"catalog": args.catalog, "binary": args.binary,
                       "mode": " ".join(VEC_MODE),
                       "summary": per_query, "nodes": record}, f, indent=2,
                      sort_keys=True)
        print(f"\nwrote {args.json}")


if __name__ == "__main__":
    main()
