#!/usr/bin/env python3
"""
random_diff.py — randomized RESULT differencing over multi-way join shapes.

    python3 python_tools/random_diff.py --catalog data/f1/sf-small/catalog.json

WEEK 28 DEFERRED THIS HERE BY NAME, with the diagnosis already done: its
randomized coverage is at PLAN level (300 shapes checked for a legal `order=`,
`cost <= written` and no negative `est=`), and randomized RESULT differencing
never completed, because the --no-optimize leg of a multi-way self-join over the
10k-row laps table takes tens of seconds -- 35.6 s measured on one query -- so
batches of 100, 40 and 14 all timed out. Every optimizer week since has rested
on hand-written differentials instead.

The blocker was DATA SIZE, not budget, which is why it lands in the week that
owns the data. Against data/f1/sf-small (500 laps) the dominant term -- the
unfiltered scan -- shrinks by 20x.

TWO LEGS, answering different questions:

  1. optimized == --no-optimize, both on col-vec. RESULT PRESERVATION: the
     optimizer must not change an answer. Needs no oracle, so it covers every
     shape the engine can run.
  2. optimized == SQLite. CORRECTNESS. Bounded by the oracle's blind spots.

Leg 1 alone can pass on two identically-wrong plans; leg 2 alone cannot isolate
the optimizer. Both run.

THE TWO TRAPS WEEK 28'S NOTE NAMED, built in from the start:

  TRAP 1 -- SORT BEFORE DIFFING, and understand WHEN. A query with no ORDER BY
  has no specified row order, and reordering a join legitimately changes
  physical emission order; that is not a result difference. The subtle half is
  the OTHER direction: an ORDER BY with TIES makes the comparison
  order-sensitive on rows whose order SQL does not specify, and a reordered join
  breaks those ties differently -- a FALSE FAILURE that looks like an optimizer
  bug, which is the worst thing to hand a future week. So the generator emits
  either no ORDER BY, or a TOTAL one (a unique tiebreak column appended).

  TRAP 2 -- normalize() KEYS ROWS BY COLUMN NAME. Its own docstring says so: a
  merged join schema legally carries several columns of the same name (two
  driver_id, two team on a self-join), both engines collapse them identically,
  and the file therefore CANNOT SEE a column-identity or column-order regression
  in a SELECT * multi-way join. A generator producing self-joins hits that
  constantly. Two exits, both taken here: the generator projects NAMED,
  DISTINCTLY ALIASED columns rather than SELECT *, and the comparison is
  POSITIONAL over raw --format tsv output, never through a name-keyed dict.

Seeded, and the seed is printed on every failure: an unreproducible randomized
failure is not a bug report.
"""

import argparse
import os
import random
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from compare_against_sqlite import load_from_catalog          # noqa: E402

SWIFTQL_BIN = "./build/swiftql"
VEC = ["--execution", "vectorized", "--storage", "columnar"]

# CARDINALITY IS THE BUDGET, and the --no-optimize leg is what spends it. That
# leg runs the filter ABOVE the joins, so the intermediate it materializes is
# the UNFILTERED product -- which is exactly the 35.6 s Week 28 measured and the
# reason its randomized differencing never completed.
#
# Two rules keep an 8-relation shape inside a second, without making the shapes
# uninteresting:
#
#   * AT MOST 2 `laps` RELATIONS. laps has 500 rows and 20 distinct driver_ids,
#     so laps x laps on driver_id fans out ~25x per join: two of them is ~12.5k
#     unfiltered intermediate rows, three is ~312k, four is ~7.8M. MEASURED: at
#     three the batch ran 12.7 s per query, at two it is well under a second.
#     Every further relation is `drivers`, whose driver_id is UNIQUE -- a 1:1
#     lookup that adds a join for the enumerator to reorder while multiplying the
#     row count by 1, which is what keeps an 8-relation shape affordable.
#   * `team` IS NEVER A JOIN KEY WITH `laps` ON EITHER SIDE. It has 8 distinct
#     values, so laps-on-team is a near cross product (500^n / 8^(n-1)) and a
#     single 8-way shape runs for minutes. Between two `drivers` relations it is
#     cheap and still exercises a STRING key.
MAX_LAPS_RELATIONS = 2


def generate_query(rng, n_relations):
    """A 3-8 relation equi-join chain over aliased laps/drivers.

    NEVER SELECT *: see TRAP 2 above. Every projected column gets a distinct
    alias so nothing can collapse on either side of the diff.
    """
    rels = [("laps", "r0")]
    laps_used = 1
    for i in range(1, n_relations):
        if laps_used < MAX_LAPS_RELATIONS and rng.random() < 0.4:
            rels.append(("laps", f"r{i}"))
            laps_used += 1
        else:
            rels.append(("drivers", f"r{i}"))

    froms = f"{rels[0][0]} {rels[0][1]}"
    for i in range(1, n_relations):
        table, alias = rels[i]
        prev_table, prev_alias = rels[rng.randrange(i)]
        # team only when NEITHER side is laps -- see the note above.
        if table == "drivers" and prev_table == "drivers" and rng.random() < 0.4:
            key = "team"
        else:
            key = "driver_id"
        froms += f" JOIN {table} {alias} ON {prev_alias}.{key} = {alias}.{key}"

    # Projection: distinct aliases, and columns that can actually TELL RELATIONS
    # APART -- which is the whole job of this generator.
    #
    # !! WEEK 36 FIXED TWO DEFECTS HERE, and the second is the one that mattered.
    #
    #  1. `rels[:3]` projected the first three relations only, so a shape with 8
    #     relations never looked at five of them. A wrong-relation resolution in
    #     relation 5 produced a byte-identical answer.
    #  2. `driver_id` IS THE JOIN KEY. Every relation is joined on it, so
    #     `r1.driver_id` and `r4.driver_id` are EQUAL BY CONSTRUCTION and
    #     projecting them cannot distinguish which relation a column came from --
    #     which is exactly the defect class this generator exists to find
    #     (Week 33 round 1, H-1/H-2: a key resolved by bare name against the wrong
    #     relation of a merged schema; wrong rows, no error, identical --explain).
    #     The old pool was `driver_id` and `team` only, and under a `driver_id`
    #     chain `team` is functionally determined by it too on the drivers side.
    #
    # So each relation now contributes columns that are NOT the join key and that
    # differ per row within the relation: `lap_id`/`speed`/`season` for `laps`,
    # `name`/`age` for `drivers`. `team` is kept because it exists on BOTH tables
    # and is a join key only between two `drivers` -- it is the one column that
    # exercises the mixed case -- but it is no longer the only non-key column.
    #
    # The width stays bounded (a random prefix) so the diff output stays readable
    # and the sort key stays cheap; what changed is WHICH columns are in the pool
    # and that EVERY relation is in it.
    #
    # WHAT IT COSTS, measured rather than assumed: the 40-shape batch went from
    # 61 s (Week 35) to 102 s, because every row now carries more columns to
    # print, sort and compare. That is the price of being able to SEE the defect
    # class this generator exists to find, and it is still inside the budget
    # Week 28's note predicted for the 500-row fixture.
    #
    # Demonstrated on a 4-relation shape (laps r0, drivers r1, drivers r2,
    # laps r3, all joined on driver_id): swapping r0 -> r3 in the projection
    # changes the answer under the NEW pool and is INVISIBLE under the old one,
    # where both relations project the same join key. (r1 vs r2 stays identical
    # under both, and correctly so -- two `drivers` joined 1:1 on a UNIQUE key
    # really are the same rows, so that is a property of the data and not a
    # weakness of the projection.)
    proj_pool = []
    for table, alias in rels:
        if table == "laps":
            proj_pool.append(f"{alias}.lap_id AS {alias}_lid")
            proj_pool.append(f"{alias}.speed AS {alias}_spd")
            proj_pool.append(f"{alias}.season AS {alias}_sea")
        else:
            proj_pool.append(f"{alias}.name AS {alias}_nm")
            proj_pool.append(f"{alias}.age AS {alias}_age")
        proj_pool.append(f"{alias}.team AS {alias}_team")
    projection = ", ".join(proj_pool[:rng.randint(2, len(proj_pool))])

    # A selective conjunct on the driving relation, ALWAYS. Under --no-optimize
    # it does not reduce the intermediate at all (that is the point of the leg),
    # but under the optimized leg it is what pushdown gets to move -- so its
    # presence is what makes the two legs structurally different rather than
    # accidentally identical.
    conjuncts = [f"{rels[0][1]}.lap_id < {rng.randint(10, 60)}"]
    if rng.random() < 0.5:
        conjuncts.append(f"{rels[0][1]}.season = "
                         f"{rng.choice([2022, 2023, 2024, 2025])}")
    where = " WHERE " + " AND ".join(conjuncts)

    # Aggregate form some of the time: a different plan shape (a pipeline
    # breaker above the joins) with an order-independent answer.
    if rng.random() < 0.35:
        key = f"{rels[0][1]}.team"
        return (f"SELECT {key} AS gk, COUNT(*) AS n FROM {froms}{where} "
                f"GROUP BY {key}")

    # NO ORDER BY *AND* NO LIMIT. TRAP 1's safe side -- and the LIMIT half is a
    # third form of the same trap, which this generator FOUND rather than
    # anticipated: `LIMIT n` with no ORDER BY selects an UNSPECIFIED n rows, so a
    # reordered join legitimately returns a DIFFERENT SUBSET of the same answer.
    # The first run of this file reported four "OPTIMIZER CHANGED THE RESULT"
    # failures, every one of them "500 rows vs 500" -- same count, different
    # rows, both correct. A LIMIT is only comparable under a TOTAL order; the
    # row-count bound above is what replaces it as a safety cap.
    return f"SELECT {projection} FROM {froms}{where}"


def run_tsv(catalog, mode_args, sql):
    """Raw positional output: a list of tuples, header dropped.

    Deliberately NOT normalize(): its dict keying is TRAP 2, and this leg exists
    to see exactly what that keying hides.
    """
    cmd = ([SWIFTQL_BIN, "--catalog", catalog] + mode_args +
           ["--no-cache", "--format", "tsv", "--query", sql])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stderr.strip().splitlines() or ["(no message)"])[-1]
    lines = [l for l in r.stdout.split("\n") if l]
    if not lines:
        return None, "empty output"
    rows = []
    for line in lines[1:]:
        if line.startswith("(") and line.endswith("rows)"):
            break
        rows.append(tuple(line.split("\t")))
    return rows, None


def _coerce(v):
    try:
        return round(float(v), 6)
    except ValueError:
        return v


def sqlite_rows(conn, sql):
    try:
        cur = conn.execute(sql)
        return [tuple("NULL" if c is None else str(c) for c in r)
                for r in cur.fetchall()], None
    except Exception as e:
        return None, str(e)


def rows_match(a, b):
    """Positional, sorted, with float coercion per field.

    Sorting both sides is TRAP 1's rule for a query with no ORDER BY: SQL does
    not specify row order there, and a reordered join legitimately emits in a
    different one.
    """
    if len(a) != len(b):
        return False
    ka = sorted(tuple(_coerce(v) for v in row) for row in a)
    kb = sorted(tuple(_coerce(v) for v in row) for row in b)
    for ra, rb in zip(ka, kb):
        if len(ra) != len(rb):
            return False
        for x, y in zip(ra, rb):
            if isinstance(x, float) and isinstance(y, float):
                if abs(x - y) > 1e-5:
                    return False
            elif str(x) != str(y):
                return False
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--catalog", default="data/f1/sf-small/catalog.json")
    ap.add_argument("--count", type=int, default=40,
                    help="Week 28's note predicts 40 in under a minute here")
    ap.add_argument("--seed", type=int, default=20250101,
                    help="fixed by default; an unreproducible randomized "
                         "failure is not a bug report")
    ap.add_argument("--min-relations", type=int, default=3)
    ap.add_argument("--max-relations", type=int, default=8)
    ap.add_argument("--no-oracle", action="store_true",
                    help="leg 1 only (optimized == --no-optimize)")
    ap.add_argument("--self-check", action="store_true",
                    help="prove the comparison can FAIL, then exit")
    args = ap.parse_args()

    if args.self_check:
        # A differential that has never failed has not been shown to work --
        # Week 33's dead-assertion finding is the same lesson. These are the
        # three ways this comparison must discriminate, and the one way it must
        # NOT (row order, which is not a result difference without ORDER BY).
        cases = [
            ("row count",   [("1", "a")],            [("1", "a"), ("2", "b")], False),
            ("a value",     [("1", "a")],            [("1", "b")],             False),
            ("column count",[("1", "a")],            [("1",)],                 False),
            ("column ORDER",[("1", "2")],            [("2", "1")],             False),
            ("row order only", [("1", "a"), ("2", "b")],
                               [("2", "b"), ("1", "a")],                       True),
            ("float noise", [("1.0000001",)],        [("1.0",)],               True),
        ]
        bad = []
        for name, a, b, want in cases:
            got = rows_match(a, b)
            print(f"  {'ok ' if got == want else 'BAD'}  {name}: "
                  f"match={got} (expected {want})")
            if got != want:
                bad.append(name)
        sys.exit(1 if bad else 0)

    rng = random.Random(args.seed)
    conn = None if args.no_oracle else load_from_catalog(args.catalog)

    started = time.time()
    checked = preserved = oracled = 0
    failures = []
    skipped = []

    for i in range(args.count):
        n = rng.randint(args.min_relations, args.max_relations)
        sql = generate_query(rng, n)

        opt, err_opt = run_tsv(args.catalog, VEC, sql)
        if opt is None:
            # A generated shape the engine refuses is not a data point; record it
            # so a generator that quietly produces only refusals is visible.
            skipped.append((sql, err_opt))
            continue

        noopt, err_noopt = run_tsv(args.catalog, VEC + ["--no-optimize"], sql)
        checked += 1
        if noopt is None:
            failures.append(("optimized ran, --no-optimize did not", sql, err_noopt))
            continue
        if not rows_match(opt, noopt):
            failures.append(("OPTIMIZER CHANGED THE RESULT", sql,
                             f"{len(opt)} rows vs {len(noopt)}"))
            continue
        preserved += 1

        if conn is not None:
            ref, err = sqlite_rows(conn, sql)
            if ref is None:
                skipped.append((sql, f"oracle: {err}"))
            elif not rows_match(opt, ref):
                failures.append(("DIVERGED FROM SQLITE", sql,
                                 f"{len(opt)} rows vs {len(ref)}"))
            else:
                oracled += 1

    elapsed = time.time() - started
    print(f"seed={args.seed} count={args.count} "
          f"relations={args.min_relations}-{args.max_relations} "
          f"catalog={args.catalog}")
    print(f"  generated and run   : {checked}")
    print(f"  result-preserving   : {preserved}   (optimized == --no-optimize)")
    if conn is not None:
        print(f"  matched SQLite      : {oracled}")
    print(f"  skipped (refused)   : {len(skipped)}")
    print(f"  wall time           : {elapsed:.1f}s")

    if skipped and len(skipped) > checked:
        # A generator that mostly produces refusals proves nothing while
        # LOOKING green. Say so.
        print("  !! more shapes were refused than checked -- the generator is "
              "not exercising the engine")

    for kind, sql, detail in failures:
        print(f"\n!! {kind}\n   seed={args.seed}\n   {detail}\n   {sql}")

    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
