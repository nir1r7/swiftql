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
  physical emission order; that is not a result difference. Both legs are
  therefore sorted before comparison in that case, and always have been.

  !! WHAT THIS PARAGRAPH USED TO SAY, AND WHY IT WAS WRONG BY WEEK 37. It read:

      "an ORDER BY with TIES makes the comparison order-sensitive on rows whose
       order SQL does not specify, and a reordered join breaks those ties
       differently -- a FALSE FAILURE ... So the generator emits either no ORDER
       BY, or a TOTAL one (a unique tiebreak column appended)."

  That was TRUE WHEN WRITTEN and it stopped being true the moment a
  DETERMINISTIC TIE-BREAK landed (`src/execution/sort_comparator.h`), which
  states the rule in its own header: when every declared key ties, compare the
  whole row column by column, so the surviving row is a function of the row's
  VALUES ALONE and NOT of the plan. Under that rule a tied ORDER BY is no longer
  unspecified WITHIN SWIFTQL: the two legs are REQUIRED to agree, and a
  divergence is a real bug, not a false failure. The header says as much --
  "The project asserts optimized == --no-optimize, so that is a defect even
  though every one of those answers is legal SQL."

  The cost of leaving the paragraph in place was total. A tied ORDER BY with a
  LIMIT that cuts inside the tie is EXACTLY the shape of seam-audit pass 3's
  blocker B3-1 (the join-order DP permutes the merged schema; the tie-break reads
  that schema positionally; two legs, two different total orders, two different
  first-n rows) -- and this file, the one tool whose whole job is to find that
  class by brute force, generated it ZERO TIMES in every run it has ever made.
  It was not that the generator missed the shape; it was forbidden from emitting
  it, in writing, for a reason that had expired.

  SO, NOW: the generator emits a TIED `ORDER BY ... LIMIT n` form on purpose (see
  FORM_TIED_LIMIT), over columns chosen for having FEW DISTINCT VALUES in the
  fixture -- laps.season (4 values over 500 rows), laps.round (24), laps.team
  (7), drivers.nationality (6), drivers.age (11) -- beside a projection that
  carries per-row-distinct columns, so the tie is MATERIAL and which tied row
  survives the cut is observable.

  THE ONE THING THAT IS STILL TRUE, and is why the form changes the COMPARISON
  and not just the SQL: SQLite has no such rule. Its answer among tied rows is
  its own plan's business. So the tied form runs LEG 1 ONLY (optimized ==
  --no-optimize, ORDERED rather than sorted, because a deterministic tie-break
  makes emission order meaningful) and is EXCLUDED from LEG 2 (the SQLite
  oracle), where it would fail for a reason that is not a bug. That exclusion is
  a real loss of oracle coverage on that form, stated here rather than hidden:
  leg 1 alone cannot tell two identically-wrong plans apart.

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

# The three forms the generator emits. Returned alongside the SQL because the
# form -- not a guess from the query text -- decides HOW the rows are compared
# and WHETHER the SQLite oracle is a valid judge of them. See TRAP 1.
FORM_PLAIN = "plain"            # no ORDER BY, no LIMIT: sorted, both legs
FORM_AGGREGATE = "aggregate"    # GROUP BY: order-independent answer, both legs
FORM_TIED_LIMIT = "tied_limit"  # tied ORDER BY + LIMIT: ORDERED, leg 1 only

# Columns with FEW DISTINCT VALUES in data/f1/sf-small, measured from the CSVs
# rather than assumed: laps has 500 rows / 4 seasons / 24 rounds / 7 teams,
# drivers has 20 rows / 6 nationalities / 11 ages / 7 teams. Sorting on any of
# these leaves large groups of rows tied, and those rows differ in lap_id, speed
# and name -- so the tie is MATERIAL and a LIMIT cutting into it is observable.
#
# NOT in this pool, deliberately: lap_id, speed, sector_*, name, driver_id. Each
# is unique or near-unique in its table, so an ORDER BY on it is effectively a
# total order, the tie-break is never reached, and the shape would look like
# coverage while testing nothing -- which is the failure mode this whole change
# exists to undo.
TIE_KEYS = {
    "laps": ["season", "round", "team"],
    "drivers": ["nationality", "age", "team"],
}


def generate_query(rng, n_relations):
    """A 3-8 relation equi-join chain over aliased laps/drivers.

    Returns (sql, form). The join chain always has >= 3 relations, which is what
    MIN_ENUMERATED_RELATIONS counts (join_enumeration.h) -- three RELATIONS, not
    three TABLES -- so every shape this emits reaches the join-order DP even
    though the fixture has only two tables. Confirmed by --explain: these print
    `method=dp` with an `order=` that differs from the written one.

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

    # Aggregate form: a different plan shape (a pipeline breaker above the joins)
    # with an order-independent answer. An aggregate between the join and any
    # sort also IMMUNISES a query against B3-1 -- buildAggregateSchema's column
    # order is a function of the query, not of the plan -- so this form is the
    # generator's built-in control, not just variety.
    if rng.random() < 0.30:
        key = f"{rels[0][1]}.team"
        return (f"SELECT {key} AS gk, COUNT(*) AS n FROM {froms}{where} "
                f"GROUP BY {key}"), FORM_AGGREGATE

    # THE TIED `ORDER BY ... LIMIT n` FORM. See TRAP 1 for why this used to be
    # forbidden and why that reason expired.
    #
    # Three things have to line up for the shape to discriminate, and all three
    # are constructed rather than hoped for:
    #   * the key is drawn from TIE_KEYS, so it has 4-24 distinct values over
    #     500 (or 20) rows and leaves large tied groups;
    #   * the LIMIT is SMALL relative to the result, so the cut lands INSIDE a
    #     tied group rather than past the end of the answer -- a LIMIT larger
    #     than the result set is a no-op and would make the entry vacuous;
    #   * the projection (built above) always carries per-row-distinct columns,
    #     so tied rows are DISTINGUISHABLE and which one survives is visible.
    # DESC half the time: the tie-break documents itself as always ascending
    # regardless of the declared direction, so both directions must agree.
    if rng.random() < 0.35:
        table, alias = rng.choice(rels)
        key = f"{alias}.{rng.choice(TIE_KEYS[table])}"
        direction = " DESC" if rng.random() < 0.5 else ""
        limit = rng.randint(3, 12)
        return (f"SELECT {projection} FROM {froms}{where} "
                f"ORDER BY {key}{direction} LIMIT {limit}"), FORM_TIED_LIMIT

    # NO ORDER BY *AND* NO LIMIT. TRAP 1's safe side -- and `LIMIT n` with NO
    # ORDER BY is a third form of the same trap, which this generator FOUND
    # rather than anticipated: it selects an UNSPECIFIED n rows, so a reordered
    # join returns a DIFFERENT SUBSET of the same answer. The first run of this
    # file reported four "OPTIMIZER CHANGED THE RESULT" failures, every one of
    # them "500 rows vs 500" -- same count, different rows.
    #
    # That form is STILL not generated here, and the reason is now narrower than
    # it was: no sort node exists in such a plan, so the deterministic tie-break
    # cannot reach it and nothing has changed about its unspecified-ness. It is
    # not simply dropped, though -- two hand-written instances of it are pinned
    # in test_new_queries.py's KNOWN_DIVERGENCES, which asserts they still
    # diverge, so the open defect is recorded instead of being generated at
    # random into a stream of unactionable failures.
    return f"SELECT {projection} FROM {froms}{where}", FORM_PLAIN


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


def rows_match(a, b, ordered=False):
    """Positional within a row, with float coercion per field.

    ordered=False (default): rows SORTED before comparison. TRAP 1's rule for a
    query with no ORDER BY -- SQL does not specify row order there, and a
    reordered join legitimately emits in a different one.

    ordered=True: rows compared IN EMISSION ORDER. Used for FORM_TIED_LIMIT,
    where SwiftQL's declared tie-break makes emission order a function of the
    row VALUES and therefore something the two legs must agree on. Strictly
    stronger: it catches a divergence whose row SET happens to be unchanged,
    which is precisely the half of B3-1 a sorted comparison passes.
    """
    if len(a) != len(b):
        return False
    if ordered:
        ka = [tuple(_coerce(v) for v in row) for row in a]
        kb = [tuple(_coerce(v) for v in row) for row in b]
    else:
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
            ("row count",   [("1", "a")],            [("1", "a"), ("2", "b")], False, False),
            ("a value",     [("1", "a")],            [("1", "b")],             False, False),
            ("column count",[("1", "a")],            [("1",)],                 False, False),
            ("column ORDER",[("1", "2")],            [("2", "1")],             False, False),
            ("row order only", [("1", "a"), ("2", "b")],
                               [("2", "b"), ("1", "a")],                       True,  False),
            ("float noise", [("1.0000001",)],        [("1.0",)],               True,  False),
            # ordered=True is the FORM_TIED_LIMIT comparison. It must reject the
            # one case the sorted comparison accepts -- same rows, different
            # order -- and must still accept float noise, or the tied form would
            # fail on rounding rather than on the defect.
            ("ORDERED: row order only", [("1", "a"), ("2", "b")],
                                        [("2", "b"), ("1", "a")],              False, True),
            ("ORDERED: identical",      [("1", "a"), ("2", "b")],
                                        [("1", "a"), ("2", "b")],              True,  True),
            ("ORDERED: float noise",    [("1.0000001",)], [("1.0",)],          True,  True),
        ]
        bad = []
        for name, a, b, want, ordered in cases:
            got = rows_match(a, b, ordered=ordered)
            print(f"  {'ok ' if got == want else 'BAD'}  {name}: "
                  f"match={got} (expected {want})")
            if got != want:
                bad.append(name)
        sys.exit(1 if bad else 0)

    rng = random.Random(args.seed)
    conn = None if args.no_oracle else load_from_catalog(args.catalog)

    started = time.time()
    checked = preserved = oracled = 0
    by_form = {FORM_PLAIN: 0, FORM_AGGREGATE: 0, FORM_TIED_LIMIT: 0}
    failures = []
    skipped = []

    for i in range(args.count):
        n = rng.randint(args.min_relations, args.max_relations)
        sql, form = generate_query(rng, n)

        opt, err_opt = run_tsv(args.catalog, VEC, sql)
        if opt is None:
            # A generated shape the engine refuses is not a data point; record it
            # so a generator that quietly produces only refusals is visible.
            skipped.append((sql, err_opt))
            continue

        noopt, err_noopt = run_tsv(args.catalog, VEC + ["--no-optimize"], sql)
        checked += 1
        by_form[form] += 1
        if noopt is None:
            failures.append(("optimized ran, --no-optimize did not", sql, err_noopt))
            continue
        # A tied ORDER BY is order-SIGNIFICANT inside SwiftQL (the tie-break makes
        # it a function of the values), so its two legs are compared IN ORDER.
        if not rows_match(opt, noopt, ordered=(form == FORM_TIED_LIMIT)):
            failures.append(("OPTIMIZER CHANGED THE RESULT", sql,
                             f"{len(opt)} rows vs {len(noopt)}"))
            continue
        preserved += 1

        # LEG 2 SKIPS THE TIED FORM. SQLite breaks ties by its own plan, so a
        # mismatch there would be a property of the oracle, not a bug. Counted as
        # skipped rather than silently passed, so the loss of oracle coverage on
        # this form stays visible in the summary.
        if conn is not None and form == FORM_TIED_LIMIT:
            skipped.append((sql, "oracle: tied ORDER BY has no single correct "
                                 "answer; leg 1 only (see TRAP 1)"))
        elif conn is not None:
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
    print(f"    plain / aggregate / tied ORDER BY+LIMIT : "
          f"{by_form[FORM_PLAIN]} / {by_form[FORM_AGGREGATE]} / "
          f"{by_form[FORM_TIED_LIMIT]}")
    print(f"  result-preserving   : {preserved}   (optimized == --no-optimize)")
    if conn is not None:
        print(f"  matched SQLite      : {oracled}   "
              f"(tied form excluded by design)")
    print(f"  skipped (refused)   : {len(skipped)}")
    print(f"  wall time           : {elapsed:.1f}s")

    if skipped and len(skipped) > checked:
        # A generator that mostly produces refusals proves nothing while
        # LOOKING green. Say so.
        print("  !! more shapes were refused than checked -- the generator is "
              "not exercising the engine")

    if checked and by_form[FORM_TIED_LIMIT] == 0:
        # THE EXACT FAILURE THIS FILE JUST GOT FIXED FOR: a run that emits the
        # tied form zero times looks identical to a clean run. Never let that be
        # silent again.
        print("  !! ZERO tied ORDER BY+LIMIT shapes generated -- the B3-1 shape "
              "was not exercised by this run; raise --count or change --seed")

    for kind, sql, detail in failures:
        print(f"\n!! {kind}\n   seed={args.seed}\n   {detail}\n   {sql}")

    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
