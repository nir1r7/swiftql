#!/usr/bin/env python3
"""
run_tpch.py — the TPC-H harness. Correctness across the four execution modes,
reported honestly, plus timings.

    python3 python_tools/run_tpch.py --catalog data/tpch/sf0.01/catalog.json
    python3 python_tools/run_tpch.py --catalog ... --time --reps 5

WHY THIS FILE REFUSES TO PRINT "22/22".

Three facts already recorded in the README make an unqualified 22/22 false, and
this harness is what Week 36's claims will rest on:

 1. TWO CAPABILITY BOUNDARIES ARE OPEN. Planner::plan builds exactly one
    HashJoinNode out of stmt.joins and runs no LogicalPlanBuilder, so the
    Volcano path can hold neither a second join nor a relation that is itself a
    plan. Multi-way joins, IN (subquery), correlated subqueries and derived
    tables are therefore REFUSED there, by message. Most TPC-H queries are
    multi-way joins, so most of the 22 are two-mode queries. A report that says
    "22/22" without saying IN HOW MANY MODES converts a documented, deliberate
    limitation into a silent claim of full coverage.

 2. A REFUSAL IS NOT A PASS. The oracle cannot hold a query that errors, so
    "refused on Volcano" is a THIRD outcome -- and it counts as coverage only
    when the refusal was asserted BY MESSAGE, which is the discipline
    compare_against_sqlite.py's run_rejection_suite already enforces.

 3. WHAT THE ORACLE CANNOT CHECK is printed with every run, not buried.

The output is therefore a PAIR: how many queries answered correctly, and in how
many modes each. The two-mode / four-mode census is COMPUTED from the run, never
typed.

TIMING. --time parses the Execution: line out of --explain-analyze rather than
using wall clock, exactly as benchmark.py does. main.cc reloads every table a
query touches on EVERY invocation (table_rows is declared inside the per-query
loop), and the README's methodology excludes CSV load from all timers.
--no-cache is passed unconditionally: CacheKey is {query, storage, execution,
no_optimize}, so without it repetition 2 onward measures an unordered_map lookup
and reports it as query latency.
"""

import argparse
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from tpch_queries import QUERY_IDS, VALIDATION_PARAMS, render          # noqa: E402
from compare_against_sqlite import load_from_catalog, normalize, rows_equal  # noqa: E402

SWIFTQL_BIN = "./build/swiftql"

# Named exactly as compare_against_sqlite.py::main names them, so the two files
# talk about the same four modes.
MODES = [
    ("row-volcano",   []),
    ("col-volcano",   ["--storage", "columnar"]),
    ("col-vec",       ["--execution", "vectorized", "--storage", "columnar"]),
    ("col-vec-noopt", ["--execution", "vectorized", "--storage", "columnar",
                       "--no-optimize"]),
]

# Boundaries this project has already decided and already pins by message
# elsewhere. Substrings, matched the way run_rejection_suite matches them:
# "it failed" is not the property under test, "it failed for the stated reason"
# is. Keep these in sync with the *_VOLCANO_REJECTED suites.
VOLCANO_BOUNDARIES = [
    "multi-way joins are not supported on the Volcano path",
    "IN (subquery) is lowered to a semi-join",
    "IN subquery",
    "correlated subqueries are decorrelated to a semi-join",
    "derived tables (FROM (subquery)) are not supported on the Volcano path",
    "not supported on the Volcano path",
]

# TPC-H tolerance. Derived in rows_equal's docstring: 1e-9 relative is four
# orders of magnitude above %.15g printing noise and summation-order noise, and
# many orders below any arithmetic defect this engine could have.
TPCH_REL_TOL = 1e-9
TPCH_ABS_TOL = 1e-6

# Outcome vocabulary. Six states, because collapsing any two of them is how a
# TPC-H report starts lying.
MATCH = "MATCH"                            # counts as correctness coverage
MISMATCH = "MISMATCH"                      # a defect
REFUSED_EXPECTED = "REFUSED_EXPECTED"      # boundary coverage, NOT correctness
REFUSED_UNEXPECTED = "REFUSED_UNEXPECTED"  # unknown -- always loud
UNPORTED = "UNPORTED"                      # not answered: a parse/dialect gap,
                                           # or a refusal this project has
                                           # already documented BY NAME (e.g.
                                           # "correlated subquery: ..."). Both
                                           # are Week 36 input; the printed
                                           # REASON separates them.
ORACLE_BLIND = "ORACLE_BLIND"              # SwiftQL answered, SQLite cannot ask

# Errors that mean "the dialect cannot express this yet" rather than "this
# capability is Volcano-only". Distinguishing them is what makes the UNPORTED
# list an actionable Week 36 worklist instead of a pile.
UNPORTED_MARKERS = [
    "Parse error",
    "unexpected trailing input",
    "is supported only",
    "are supported in WHERE and HAVING only",
    "DISTINCT is supported inside COUNT only",
    "NOT is supported only",
    "correlated subquery:",
    "scalar subquery",
    "column not found",
    "unknown table qualifier",
    "Table not found",
    "ambiguous",
]

PLAN_NODE_KINDS = ["LogicalSemiJoin", "LogicalAntiJoin", "LogicalLeftJoin",
                   "LogicalDerived", "LogicalJoin"]


def run_swiftql(catalog, mode_args, sql, extra=None):
    cmd = ([SWIFTQL_BIN, "--catalog", catalog] + mode_args +
           ["--no-cache", "--format", "tsv"] + (extra or []) + ["--query", sql])
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, r.stdout, r.stderr


def parse_tsv(out):
    """Parse --format tsv output into a list of dicts.

    The '(N rows)' terminator is what makes a genuinely empty result
    distinguishable from a truncated or failed run -- the aligned parser
    conflates the two and returns [] for both.
    """
    lines = out.split("\n")
    if not lines or not lines[0]:
        return None
    headers = lines[0].split("\t")
    rows = []
    saw_terminator = False
    for line in lines[1:]:
        if not line:
            continue
        if line.startswith("(") and line.endswith("rows)"):
            saw_terminator = True
            break
        fields = line.split("\t")
        if len(fields) != len(headers):
            # Never silently drop: a field-count mismatch under TSV means the
            # data itself contains a tab, which is a finding, not a row to skip.
            raise ValueError(
                f"tsv row has {len(fields)} fields, header has {len(headers)}")
        rows.append(dict(zip(headers, fields)))
    if not saw_terminator:
        raise ValueError("tsv output has no '(N rows)' terminator -- truncated?")
    return rows


def plan_fingerprint(catalog, mode_args, sql):
    """The multiset of logical node kinds in --explain, as {kind: count}.

    LogicalJoin::explain carries the join's semantics in the node NAME on
    purpose (Weeks 29 and 32 both chose a distinct name over a suffix), so
    "which rewrite fired" is read off a plan rather than argued from the query
    text. This is how Week 34's Q22 hand-forward -- "verify it against the
    ported query in Week 36 and record which half was which" -- becomes
    mechanical.
    """
    rc, out, _ = run_swiftql(catalog, mode_args, sql, extra=["--explain"])
    if rc != 0:
        return None
    counts = {}
    for line in out.split("\n"):
        stripped = line.strip()
        for kind in PLAN_NODE_KINDS:
            if stripped.startswith(kind + " ") or stripped.startswith(kind + "["):
                # LogicalSemiJoin/LogicalAntiJoin/LogicalLeftJoin all contain
                # "Join" but are distinct names; match the longest first.
                counts[kind] = counts.get(kind, 0) + 1
                break
    return counts


def error_message(stderr):
    """Extract the ACTUAL error from stderr.

    A columnar run prints its per-column encoding report to stderr before any
    error, so stderr.splitlines()[0] is "c_acctbal: 11.7 KB -> 11.7 KB" rather
    than the diagnosis -- which made a real internal error read as encoding
    noise. Prefer the line main.cc's handler writes; fall back to the last
    non-empty line, never the first.
    """
    lines = [l.strip() for l in stderr.splitlines() if l.strip()]
    if not lines:
        return ""
    for line in lines:
        if line.startswith("Error:"):
            return line[len("Error:"):].strip()
    return lines[-1]


def classify(rc, stdout, stderr, oracle_rows, ordered):
    if rc != 0:
        err = error_message(stderr)
        # An internal error is never a boundary and never a dialect gap: it is a
        # defect, and it is checked FIRST so no substring below can absorb it.
        if err.startswith("internal:"):
            return REFUSED_UNEXPECTED, err[:200]
        for msg in VOLCANO_BOUNDARIES:
            if msg in err:
                return REFUSED_EXPECTED, msg
        for marker in UNPORTED_MARKERS:
            if marker in err:
                return UNPORTED, err[:200]
        return REFUSED_UNEXPECTED, (err[:200] if err else "(no message)")

    try:
        rows = parse_tsv(stdout)
    except ValueError as e:
        return REFUSED_UNEXPECTED, f"unparseable output: {e}"

    if oracle_rows is None:
        return ORACLE_BLIND, "SQLite could not run this query"

    ok = rows_equal(normalize(rows, ordered), normalize(oracle_rows, ordered),
                    rel_tol=TPCH_REL_TOL, abs_tol=TPCH_ABS_TOL)
    return (MATCH if ok else MISMATCH), (None if ok else
                                         f"{len(rows)} rows vs {len(oracle_rows)}")


def oracle_answer(conn, sql):
    """Run the query on SQLite. None means the oracle cannot express it."""
    try:
        cur = conn.execute(sql)
        cols = [d[0] for d in cur.description]
        return [dict(zip(cols, r)) for r in cur.fetchall()]
    except Exception:
        return None


def time_once(catalog, mode_args, sql):
    rc, out, _ = run_swiftql(catalog, mode_args, sql, extra=["--explain-analyze"])
    if rc != 0:
        return None
    m = re.search(r"Execution:\s+([\d.]+)", out)
    return float(m.group(1)) if m else None


def time_query(catalog, mode_args, sql, warmups, reps):
    for _ in range(warmups):
        time_once(catalog, mode_args, sql)   # discarded; each run is a fresh
                                             # process, so this warms the OS
                                             # page cache and nothing else
    samples = []
    for _ in range(reps):
        t = time_once(catalog, mode_args, sql)
        if t is not None:
            samples.append(t)
    if not samples:
        return None
    samples.sort()
    return {"median_us": samples[len(samples) // 2],
            "min_us": samples[0], "n": len(samples)}


def render_report(qids, cells, details, fingerprints, empties, catalog, provenance):
    print("=" * 78)
    print("TPC-H correctness report")
    print("=" * 78)
    print(f"catalog: {catalog}")
    for line in provenance:
        print(f"  {line}")
    print()

    # Per-query grid. Printed BEFORE any summary, because the summary is a lossy
    # view and the loss should be visible.
    width = max(len(m) for m, _ in MODES)
    print(f"{'query':<6} " + " ".join(f"{m:<{width}}" for m, _ in MODES))
    for qid in qids:
        row = " ".join(f"{cells[(qid, m)][:width]:<{width}}" for m, _ in MODES)
        print(f"{qid:<6} {row}")
    print()

    correct_modes = {q: sum(1 for m, _ in MODES if cells[(q, m)] == MATCH)
                     for q in qids}
    answered = [q for q, n in correct_modes.items() if n > 0]
    by_count = {}
    for q, n in correct_modes.items():
        if n:
            by_count.setdefault(n, []).append(q)

    unported = [q for q in qids
                if any(cells[(q, m)] == UNPORTED for m, _ in MODES)
                and correct_modes[q] == 0]
    mismatched = sorted({q for q in qids
                         if any(cells[(q, m)] == MISMATCH for m, _ in MODES)})
    blind = sorted({q for q in qids
                    if any(cells[(q, m)] == ORACLE_BLIND for m, _ in MODES)})
    pinned = sum(1 for v in cells.values() if v == REFUSED_EXPECTED)
    unknown = sorted(k for k, v in cells.items() if v == REFUSED_UNEXPECTED)

    print("-" * 78)
    print(f"answered correctly:            {len(answered)}/{len(qids)}")
    for n in sorted(by_count, reverse=True):
        label = {4: "in all four modes", 2: "in the two vectorized modes only"}.get(
            n, f"in {n} mode(s)")
        print(f"  {label:<30} {len(by_count[n]):>3}   {sorted(by_count[n])}")
    print(f"not answered (gap or refusal): {len(unported):>3}   {unported}")
    for q in unported:
        # The REASON, not just the id: this list IS Week 36's worklist and a bare
        # id makes it a pile instead.
        for mode, _ in MODES:
            if cells[(q, mode)] == UNPORTED:
                print(f"      {q}: {details[(q, mode)]}")
                break
    if mismatched:
        print(f"!! WRONG ANSWERS:              {len(mismatched):>3}   {mismatched}")
    print()
    print(f"query x mode cells:            {len(cells)}")
    print(f"  Volcano refusals pinned by message: {pinned}")
    if unknown:
        print(f"  !! UNEXPLAINED ERRORS ({len(unknown)}):")
        for qid, mode in unknown:
            print(f"       {qid} [{mode}]: {details[(qid, mode)]}")
    print()

    # The blind spots, printed with every run rather than buried in a doc. A
    # TPC-H report that does not name what it cannot check invites a false
    # reading of its own headline.
    print("WHAT THIS HARNESS CANNOT CHECK")
    print("  * A query that ERRORS has no rows to diff, so the oracle is silent")
    print("    on every refusal. Refusals are covered only by message-matching,")
    print("    on a separate line above -- never counted as correctness.")
    print("  * SQLite cannot parse a derived-table column alias list")
    print("    (FROM (SELECT ...) AS d (c1, c2)), so that Week 34 feature has")
    print("    C++ coverage only. No template above uses the alias-list form.")
    if blind:
        print(f"  * ORACLE_BLIND this run: {blind}")
    if empties:
        print("  * VACUOUS PASSES -- these matched with ZERO rows on both sides,")
        print("    so they assert only that both engines found nothing:")
        print(f"      {sorted(empties)}")
    print("  * The data is synthetic unless PROVENANCE says otherwise; the")
    print("    published TPC-H answer set does not apply to it.")
    print()

    # Q22's provenance, mechanically. Week 34: "Q22 is NOT claimed closed on the
    # strength of the correlated-scalar rewrite alone: verify it against the
    # ported query in Week 36 and record which half was which."
    fp = fingerprints.get(("q22", "col-vec"))
    print("Q22 PROVENANCE (Week 34 hand-forward: which half is which)")
    if "q22" not in qids:
        print("  q22 was not in this run")
    elif fp is None:
        print(f"  q22 does not plan in col-vec: {details.get(('q22','col-vec'))}")
    else:
        print(f"  plan fingerprint: {fp}")
        anti = fp.get("LogicalAntiJoin", 0)
        derived = fp.get("LogicalDerived", 0)
        left = fp.get("LogicalLeftJoin", 0)
        print(f"  correlated half  : {'ANTI-JOIN (Week 33 NOT EXISTS)' if anti else 'NOT the Week 33 anti-join'}")
        print(f"  custsale half    : {'LogicalDerived (Week 34)' if derived else 'no derived relation'}")
        print(f"  correlated-scalar rewrite present: {'yes' if left else 'no'}")
    print("=" * 78)

    return len(mismatched) == 0 and not unknown


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalog", default="data/tpch/sf0.01/catalog.json")
    ap.add_argument("--queries", default="", help="comma-separated qids; default all")
    ap.add_argument("--time", action="store_true", help="also measure latency")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--warmups", type=int, default=1)
    ap.add_argument("--json", default="", help="write the per-query record here")
    args = ap.parse_args()

    qids = [q.strip() for q in args.queries.split(",") if q.strip()] or QUERY_IDS

    provenance = []
    prov_path = os.path.join(os.path.dirname(os.path.abspath(args.catalog)),
                             "PROVENANCE.txt")
    if os.path.exists(prov_path):
        provenance = [l.rstrip() for l in open(prov_path) if l.strip()]

    conn = load_from_catalog(args.catalog)

    cells, details, fingerprints, timings = {}, {}, {}, {}
    empties = set()

    for qid in qids:
        sql = render(qid)
        ordered = "ORDER BY" in sql.upper()
        oracle = oracle_answer(conn, sql)
        if oracle is not None and len(oracle) == 0:
            # A zero-row answer on both sides is a PASS that asserted nothing.
            # Naming them is the difference between a harness and a rubber stamp.
            empties.add(qid)

        for mode, extra in MODES:
            rc, out, err = run_swiftql(args.catalog, extra, sql)
            outcome, detail = classify(rc, out, err, oracle, ordered)
            cells[(qid, mode)] = outcome
            details[(qid, mode)] = detail
            if rc == 0:
                fp = plan_fingerprint(args.catalog, extra, sql)
                if fp is not None:
                    fingerprints[(qid, mode)] = fp
            print(f"  {qid:<5} {mode:<14} {outcome}"
                  + (f"  ({detail})" if detail else ""))

            if args.time and outcome == MATCH:
                t = time_query(args.catalog, extra, sql, args.warmups, args.reps)
                if t:
                    timings[(qid, mode)] = t

    print()
    ok = render_report(qids, cells, details, fingerprints, empties,
                       args.catalog, provenance)

    if args.time:
        print("\nLATENCY (engine Execution: line, median of "
              f"{args.reps} after {args.warmups} warmup; CSV load excluded)")
        for (qid, mode), t in sorted(timings.items()):
            print(f"  {qid:<5} {mode:<14} median={t['median_us']:>12.1f}us "
                  f"min={t['min_us']:>12.1f}us")

    if args.json:
        record = {
            "catalog": args.catalog,
            "provenance": provenance,
            "tolerance": {"rel": TPCH_REL_TOL, "abs": TPCH_ABS_TOL},
            "modes": [m for m, _ in MODES],
            "cells": {f"{q}|{m}": cells[(q, m)] for (q, m) in cells},
            "details": {f"{q}|{m}": details[(q, m)] for (q, m) in details
                        if details[(q, m)]},
            "fingerprints": {f"{q}|{m}": fingerprints[(q, m)]
                             for (q, m) in fingerprints},
            "vacuous": sorted(empties),
            "timings": {f"{q}|{m}": timings[(q, m)] for (q, m) in timings},
        }
        with open(args.json, "w") as f:
            json.dump(record, f, indent=2, sort_keys=True)
        print(f"\nwrote {args.json}")

    # A wrong answer or an unexplained error fails the run. A dialect gap does
    # not: it is Week 36's work, recorded rather than hidden.
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
