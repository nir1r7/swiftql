#!/usr/bin/env python3
"""
validate_answers.py — SwiftQL against the OFFICIAL TPC-H answer set.

    .venv/bin/python python_tools/validate_answers.py \
        --catalog data/tpch/dbgen-sf1/catalog.json

WHY THIS EXISTS, AND WHY IT DID NOT UNTIL NOW. Every correctness claim this
project has made says "matches SQLite", never "correct", because the published
answer set is defined for SF=1 data produced by `dbgen` and this project's data
was produced by a hand-written generator. That reasoning was right and it stopped
being right the moment `dbgen` V3.0.1 was built and SF=1 data generated from it:
`dbgen/answers/*.out` IS the answer for exactly this data. The old caveat was
carried forward without re-checking its premise.

An independent oracle matters here beyond provenance. SQLite and SwiftQL can
agree and both be wrong -- they share IEEE-754 doubles, the same textual date
representation, and (since the port) the same query text. The published answers
were computed by neither.

WHAT THE ANSWER FILES ARE. A header line of space-padded column names, then
pipe-delimited rows in the query's ORDER BY order, decimals rounded to 2 places.
They correspond to the spec's QUALIFICATION substitution parameters -- the fixed
validation values -- not to arbitrary ones.

THE TWO QUERIES THAT NEED THEIR PARAMETERS RESTORED. `VALIDATION_PARAMS`
deliberately deviates from the spec on q2 (SIZE 15 -> 1) and q19 (brands
12/23/34 -> 14/34/23), both recorded with measurements: at the spec values those
queries are VACUOUS on the synthetic generator's data, and a vacuous pass
overstates coverage. That reasoning applies to the synthetic data, not to
`dbgen`'s, and comparing against the published answers requires the published
parameters. SPEC_PARAMS restores both. Any query run here uses the spec's own
values or it is not being validated against the spec's own answers.

TOLERANCE. The answers are rounded to 2 decimals, so a value matches when it
agrees to within 0.005 absolute -- half of the last printed place -- OR to 1e-6
relative, whichever is looser. The relative arm exists for the revenue sums,
where 0.005 absolute is below the printing precision of a 1e11 quantity.
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tpch_queries import QUERY_IDS, VALIDATION_PARAMS, render  # noqa: E402

ANSWERS = "tpch-tools/TPC-H V3.0.1/dbgen/answers"
MODE = ["--storage", "columnar", "--execution", "vectorized"]

# The spec's qualification values for the two queries this project deviates on.
# Every other query in VALIDATION_PARAMS already carries the spec's own values.
SPEC_OVERRIDES = {
    "q2":  {"SIZE": "15", "TYPE": "BRASS", "REGION": "EUROPE"},
    "q19": {"BRAND1": "Brand#12", "BRAND2": "Brand#23", "BRAND3": "Brand#34",
            "QTY1": "1", "QTY2": "10", "QTY3": "20"},
}

ABS_TOL = 0.005          # half of the last printed decimal place
REL_TOL = 1e-6


def spec_params(qid):
    p = dict(VALIDATION_PARAMS[qid])
    p.update(SPEC_OVERRIDES.get(qid, {}))
    return p


def read_answer(qid):
    """[[cell, ...], ...] from an answer file, header dropped, cells stripped."""
    path = os.path.join(ANSWERS, f"{qid}.out")
    if not os.path.exists(path):
        return None
    with open(path, encoding="latin-1") as f:
        lines = [l.rstrip("\n") for l in f if l.strip()]
    rows = []
    for line in lines[1:]:
        cells = [c.strip() for c in line.split("|")]
        # dbgen writes a trailing delimiter on some files and not others
        if cells and cells[-1] == "":
            cells.pop()
        rows.append(cells)
    return rows


def read_swiftql(binary, catalog, sql):
    r = subprocess.run([binary, "--catalog", catalog] + MODE
                       + ["--no-cache", "--format", "tsv", "--query", sql],
                       capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr.strip().splitlines() or ["error"])[-1][:90]
    lines = [l for l in r.stdout.split("\n") if l.strip()]
    rows = []
    for line in lines[1:]:
        if line.startswith("(") and line.endswith("rows)"):
            break
        rows.append([c.strip() for c in line.split("\t")])
    return rows, None


def cells_match(mine, theirs):
    """Numeric within tolerance, else exact string after stripping."""
    try:
        a, b = float(mine), float(theirs)
    except ValueError:
        return mine == theirs
    if math.isnan(a) or math.isnan(b):
        return False
    return abs(a - b) <= ABS_TOL or abs(a - b) <= REL_TOL * max(abs(a), abs(b))


def compare(mine, theirs):
    if len(mine) != len(theirs):
        return f"row count {len(mine)} vs {len(theirs)}"
    for i, (r1, r2) in enumerate(zip(mine, theirs)):
        if len(r1) != len(r2):
            return f"row {i}: {len(r1)} columns vs {len(r2)}"
        for j, (c1, c2) in enumerate(zip(r1, r2)):
            if not cells_match(c1, c2):
                return f"row {i} col {j}: {c1!r} vs {c2!r}"
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalog", default="data/tpch/dbgen-sf1/catalog.json")
    ap.add_argument("--binary", default="./build-release/swiftql")
    ap.add_argument("--queries", default="")
    ap.add_argument("--json", default="")
    args = ap.parse_args()

    if "sf1" not in args.catalog:
        print("WARNING: the published answers are defined for SF=1 ONLY. "
              f"This catalog is {args.catalog}; every comparison below is "
              "meaningless unless it is SF=1 dbgen data.\n")

    qids = [q.strip() for q in args.queries.split(",") if q.strip()] or list(QUERY_IDS)
    matched, mismatched, errored, record = [], [], [], {}

    print(f"catalog: {args.catalog}")
    print(f"answers: {ANSWERS}\n")
    for qid in qids:
        theirs = read_answer(qid)
        if theirs is None:
            print(f"{qid:<5} NO ANSWER FILE")
            continue
        mine, err = read_swiftql(args.binary, args.catalog, render(qid, spec_params(qid)))
        if err:
            errored.append(qid)
            record[qid] = {"status": "ERROR", "detail": err}
            print(f"{qid:<5} ERROR        {err}")
            continue
        diff = compare(mine, theirs)
        if diff is None:
            matched.append(qid)
            record[qid] = {"status": "MATCH", "rows": len(mine)}
            print(f"{qid:<5} MATCH        {len(mine)} rows")
        else:
            mismatched.append(qid)
            record[qid] = {"status": "MISMATCH", "detail": diff}
            print(f"{qid:<5} MISMATCH     {diff}")

    n = len(matched) + len(mismatched) + len(errored)
    print("\n" + "-" * 62)
    print(f"MATCHES THE PUBLISHED TPC-H ANSWER SET: {len(matched)}/{n}")
    if mismatched:
        print(f"  mismatched: {mismatched}")
    if errored:
        print(f"  errored:    {errored}")
    print("\nParameters: the spec's qualification values throughout; q2 and q19 "
          "restored from this project's deviations.")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"catalog": args.catalog, "answers": ANSWERS,
                       "abs_tol": ABS_TOL, "rel_tol": REL_TOL,
                       "matched": matched, "mismatched": mismatched,
                       "errored": errored, "per_query": record}, f,
                      indent=2, sort_keys=True)
        print(f"wrote {args.json}")
    sys.exit(0 if not mismatched and not errored else 1)


if __name__ == "__main__":
    main()
