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
    # SELECT * + JOIN regression (Week 18): the columnar Volcano path used to
    # narrow the join-side scan by the FROM table's column names, silently
    # dropping drivers.name/nationality/age from the output. Bounded via a
    # WHERE (not LIMIT) so both engines produce the same row set.
    "SELECT * FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id WHERE lap_id < 6",
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

# Week 24 general expressions — the checkpoint shape (TPC-H revenue idiom:
# expression inside the aggregate + alias + ORDER BY alias) and an
# expression-over-aggregates projection.
WEEK24_EXPRESSION_QUERIES = [
    "SELECT team, SUM(speed * (1 - sector_1 / 100)) AS revenue FROM laps GROUP BY team ORDER BY revenue DESC",
    "SELECT team, SUM(speed) / COUNT(*) AS manual_avg FROM laps GROUP BY team ORDER BY team",
]

# NULL on the vectorized path. ColumnVector used to have no validity mask, so
# every NULL degraded to a 0 / "NULL" sentinel that was indistinguishable from a
# genuine zero — which blocks left-outer-join semantics (TPC-H Q13). These pin
# the mask down: NULL through a projection, through the blocking operators
# (ORDER BY, DISTINCT), and NULL-skipping inside the aggregates.
# Every computed column is aliased: parse_swiftql_output() splits the header
# line on whitespace, so an unaliased "(season / 0)" would parse as three
# columns and the row would be dropped.
NULL_SEMANTICS_QUERIES = [
    "SELECT season / 0 AS n FROM laps LIMIT 5",
    "SELECT DISTINCT round / (round - round) AS n FROM laps",
    "SELECT speed / 0 AS n FROM laps ORDER BY n LIMIT 5",
    "SELECT COUNT(*) AS c, COUNT(season / 0) AS cn, SUM(season / 0) AS s, AVG(season / 0) AS a FROM laps",
    "SELECT team, COUNT(season / 0) AS cn FROM laps GROUP BY team ORDER BY team",
    "SELECT team, MIN(speed / 0) AS mn, MAX(speed / 0) AS mx FROM laps GROUP BY team ORDER BY team",
]

# MIN/MAX are order statistics: they return an element of the input domain.
# Typing every non-COUNT aggregate DOUBLE made MIN(team) throw bad_variant_access.
MIN_MAX_TYPE_QUERIES = [
    "SELECT MIN(team), MAX(team) FROM laps",
    "SELECT MIN(season), MAX(season), MIN(speed), MAX(speed) FROM laps",
    "SELECT team, MIN(team), MAX(season) FROM laps GROUP BY team ORDER BY team",
]

# Expression group keys are matched by slot-based identity, not by the as-typed
# qualifier, so qualifying in one clause but not another agrees with SQLite.
# All four directions used to error.
GROUP_KEY_QUALIFIER_QUERIES = [
    "SELECT laps.season - 1 AS s, COUNT(*) AS c FROM laps GROUP BY season - 1 ORDER BY s",
    "SELECT season - 1 AS s, COUNT(*) AS c FROM laps GROUP BY laps.season - 1 ORDER BY s",
    "SELECT season - 1 AS s, COUNT(*) AS c FROM laps GROUP BY season - 1 HAVING laps.season - 1 > 2021 ORDER BY s",
    "SELECT season - 1 AS s, COUNT(*) AS c FROM laps GROUP BY season - 1 ORDER BY laps.season - 1",
    "SELECT laps.season - 1 AS s, SUM(laps.speed * (1 - laps.sector_1 / 100)) AS rev FROM laps GROUP BY season - 1 ORDER BY s",
]

# Constant folding runs before validation, so the folded predicate must produce
# the same rows as the literal it folds to — in every mode, optimizer on or off.
# The win is that folding restores zone-map pruning and the comparison fast path.
CONSTANT_FOLDING_QUERIES = [
    "SELECT COUNT(*) AS c FROM laps WHERE season = 2020 + 4",
    "SELECT COUNT(*) AS c FROM laps WHERE season = 2024 * 1",
    "SELECT COUNT(*) AS c FROM laps WHERE season > 2 * (1000 + 10)",
    "SELECT COUNT(*) AS c FROM laps WHERE season > -(-2023)",
    "SELECT COUNT(*) AS c FROM laps WHERE season = 2020 + 4 AND speed > 100 * 3",
]

# Expressions in the WHERE and projection positions now compile to the
# chunk-at-a-time executor instead of a per-row evaluate(). Same answers required.
# Three-valued AND/OR. evaluate() propagated NULL through the connectives, so
# Volcano and the vectorized path returned different answers for the same query —
# 0 rows vs 10000. TPC-H Q19 is an OR chain over nullable columns.
THREE_VALUED_LOGIC_QUERIES = [
    "SELECT COUNT(*) AS c FROM laps WHERE (season / 0) > 0 OR 1 = 1",
    "SELECT COUNT(*) AS c FROM laps WHERE (season / 0) > 0 OR 1 = 0",
    "SELECT COUNT(*) AS c FROM laps WHERE (season / 0) > 0 AND 1 = 1",
    "SELECT COUNT(*) AS c FROM laps WHERE (season / 0) > 0 AND 1 = 0",
    "SELECT COUNT(*) AS c FROM laps WHERE speed > 300 OR (season / 0) > 0",
    "SELECT COUNT(*) AS c FROM laps WHERE speed > 300 AND (season / 0) > 0",
    "SELECT COUNT(*) AS c FROM laps WHERE (round / (round - 1)) > 1 OR speed > 340",
    "SELECT team, COUNT(*) AS c FROM laps WHERE (round / (round - 1)) > 1 OR season = 2024 GROUP BY team ORDER BY team",
]

EXPRESSION_POSITION_QUERIES = [
    "SELECT COUNT(*) AS c FROM laps WHERE speed * 2 > 600",
    "SELECT COUNT(*) AS c FROM laps WHERE speed * (1 - sector_1 / 100) > 200",
    "SELECT COUNT(*) AS c FROM laps WHERE season - 1 = 2023 AND speed * 2 > 600",
    "SELECT COUNT(*) AS c FROM laps WHERE round + 0 > 10 OR speed * 2 > 690",
    "SELECT speed * 2 AS d FROM laps ORDER BY d LIMIT 10",
    "SELECT team, speed * (1 - sector_1 / 100) AS adj FROM laps ORDER BY adj, team LIMIT 10",
    "SELECT -speed AS n, round * 2 AS r FROM laps ORDER BY n, r LIMIT 10",
    "SELECT DISTINCT season - 1 AS s FROM laps ORDER BY s",
]

# ORDER BY over a NULLABLE sort key. Comparing with the SQL operators is not a
# strict weak ordering once a key can be NULL (every comparison against NULL is
# false, so NULL is equivalent to every value and equivalence is non-transitive),
# which is undefined behaviour in std::stable_sort — it inverted the NON-NULL keys
# and dropped rows under LIMIT, not merely misplaced the NULLs. SQLite orders NULL
# first ascending, last descending. These only bite because the harness now
# compares ORDER BY results in emitted order.
NULL_ORDERING_QUERIES = [
    "SELECT round / (round - 1) AS g, COUNT(*) AS c FROM laps GROUP BY round / (round - 1) ORDER BY g",
    "SELECT round / (round - 1) AS g, COUNT(*) AS c FROM laps GROUP BY round / (round - 1) ORDER BY g DESC",
    "SELECT lap_id, speed / (round - 1) AS n FROM laps ORDER BY n DESC, lap_id LIMIT 12",
    "SELECT lap_id, speed / (round - 1) AS n FROM laps ORDER BY n, lap_id LIMIT 12",
    "SELECT lap_id, round / (round - 1) AS g FROM laps ORDER BY g, lap_id LIMIT 20",
    # NULL as a secondary key, and a NULL-bearing key alongside a non-NULL one
    "SELECT team, round / (round - 1) AS g FROM laps ORDER BY team, g, round LIMIT 20",
    "SELECT DISTINCT round / (round - 1) AS g FROM laps ORDER BY g",
]

# Week 25 predicates. BETWEEN is desugared in the parser into two comparisons,
# so these also pin down that the rewrite agrees with SQLite — including the
# precedence case, where a trailing AND must belong to the WHERE and not to the
# range. LIKE is ASCII case-insensitive to match SQLite's default, which the
# lowercase-pattern query below is here to prove.
WEEK25_PREDICATE_QUERIES = [
    "SELECT COUNT(*) AS c FROM laps WHERE season BETWEEN 2021 AND 2023",
    "SELECT COUNT(*) AS c FROM laps WHERE season NOT BETWEEN 2021 AND 2023",
    "SELECT COUNT(*) AS c FROM laps WHERE season BETWEEN 2021 AND 2023 AND speed > 340",
    "SELECT COUNT(*) AS c FROM laps WHERE speed BETWEEN 300 - 10 AND 300 + 10",
    "SELECT team, COUNT(*) AS c FROM laps WHERE team BETWEEN 'A' AND 'M' GROUP BY team ORDER BY team",
    "SELECT COUNT(*) AS c FROM laps WHERE season IN (2021, 2023)",
    "SELECT COUNT(*) AS c FROM laps WHERE season NOT IN (2021, 2023)",
    "SELECT COUNT(*) AS c FROM laps WHERE team IN ('Ferrari', 'McLaren')",
    "SELECT COUNT(*) AS c FROM laps WHERE season IN (2021.0, 2023)",
    "SELECT COUNT(*) AS c FROM laps WHERE (season / 0) IN (1, 2)",
    "SELECT COUNT(*) AS c FROM laps WHERE team LIKE 'Fer%'",
    "SELECT COUNT(*) AS c FROM laps WHERE team NOT LIKE 'Fer%'",
    "SELECT COUNT(*) AS c FROM laps WHERE team LIKE '%rrar%'",
    "SELECT COUNT(*) AS c FROM laps WHERE team LIKE '_erra_i'",
    "SELECT COUNT(*) AS c FROM laps WHERE team LIKE 'ferrari'",
    "SELECT team, COUNT(*) AS c FROM laps WHERE team LIKE '%a%' GROUP BY team ORDER BY team",
]

# CASE. The conditional-aggregate shape is TPC-H Q8/Q12/Q14's whole structure.
# A missing ELSE must yield NULL (which SUM skips), not 0, and a NULL condition
# must fall through rather than count as true.
WEEK25_CASE_QUERIES = [
    "SELECT SUM(CASE WHEN season = 2024 THEN speed ELSE 0 END) AS s FROM laps",
    "SELECT SUM(CASE WHEN season = 2024 THEN speed END) AS s FROM laps",
    "SELECT team, SUM(CASE WHEN season = 2024 THEN 1 ELSE 0 END) AS n FROM laps GROUP BY team ORDER BY team",
    "SELECT COUNT(*) AS c FROM laps WHERE CASE WHEN season > 2022 THEN 1 ELSE 0 END = 1",
    "SELECT COUNT(*) AS c FROM laps WHERE CASE WHEN (season / 0) > 0 THEN 1 ELSE 0 END = 0",
    "SELECT DISTINCT CASE WHEN season > 2022 THEN 'late' ELSE 'early' END AS era FROM laps ORDER BY era",
    # mixed INT/DOUBLE branches: inferExprType says DOUBLE while evaluate()
    # returns an INT from the INT branch, so this pins the widening path
    "SELECT lap_id, CASE WHEN season = 2024 THEN 1 ELSE 2.5 END AS m FROM laps ORDER BY m, lap_id LIMIT 10",
]

# SUBSTRING. SQLite accepts only the comma form (the SQL-standard
# `SUBSTRING(x FROM a FOR b)` is a syntax error there), so the FROM/FOR form is
# covered by unit tests instead. The IN query is TPC-H Q22's exact shape.
WEEK25_SUBSTRING_QUERIES = [
    "SELECT SUBSTRING(team, 1, 3) AS t, COUNT(*) AS c FROM laps GROUP BY SUBSTRING(team, 1, 3) ORDER BY t",
    "SELECT SUBSTRING(team, 2) AS t FROM laps ORDER BY t LIMIT 10",
    "SELECT COUNT(*) AS c FROM laps WHERE SUBSTRING(team, 1, 3) IN ('Fer', 'McL')",
    "SELECT COUNT(*) AS c FROM laps WHERE SUBSTRING(team, 1, 3) LIKE 'Fe%'",
]

# Week 25 predicates below a join. collectSlots/restampSlots are dispatch sites
# development.md's checklist omits, and missing them costs pushdown silently —
# the answers stay right, so only a plan check or a benchmark would notice.
# These at least lock the answers down across all four modes.
WEEK25_JOIN_QUERIES = [
    "SELECT COUNT(*) AS c FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id "
    "WHERE laps.team LIKE 'Fer%' AND drivers.nationality IN ('British','German')",
    "SELECT drivers.team AS dt, COUNT(*) AS c FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id "
    "WHERE laps.season BETWEEN 2021 AND 2023 GROUP BY drivers.team ORDER BY dt",
]

# Week 26 (multi-way join language + binding). Nothing this week adds a query
# that RETURNS rows: multi-way and multi-key joins bind and build a logical join
# tree, but lowering them is Week 27, so there is nothing to diff against SQLite.
# What can be pinned is the refusal — every one of these must fail with its own
# message rather than return a wrong answer, which is the property the whole
# dialect's error handling exists to protect. Checked in all four modes, since
# the refusal comes from a different place on the Volcano and vectorized paths.
#
# Each entry: (query, required substring of the error).
WEEK26_REJECTED_QUERIES = [
    # planned and bound this week, executable in Week 27
    ("SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.driver_id = d2.driver_id",
     "multi-way joins are planned but not yet executable"),
    ("SELECT l.team FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id AND l.team = d.team",
     "multi-key equi-joins are planned but not yet executable"),

    # still rejected on shape: non-equality ON conjuncts become residuals in
    # Week 27, and an OR in ON is not a key list at all
    ("SELECT l.team FROM laps l JOIN drivers d ON l.driver_id < d.driver_id",
     "non-equality"),
    ("SELECT l.team FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id AND l.speed > d.age",
     "non-equality"),
    ("SELECT l.team FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id OR l.team = d.team",
     "AND-chain of equalities"),

    # a second, independent fault must win over the temporary refusal, in every
    # mode: the multi-key guard is deferred past the plan-time type checks
    # because the merged schema does not depend on the key count
    ("SELECT l.team FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id AND l.team = d.team WHERE l.team + 1 > 0",
     "requires numeric operands"),

    # a query that is BOTH multi-way and multi-key must report the same reason
    # in every mode — the two engines check in different places, so only a query
    # with both properties can catch them disagreeing
    ("SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.driver_id = d2.driver_id AND d.team = d2.team",
     "multi-way joins are planned but not yet executable"),

    # only reachable once a third relation exists: d2 is not in the tree yet
    ("SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d2.driver_id "
     "JOIN drivers d2 ON d.driver_id = d2.driver_id",
     "table being joined"),
    # ...and the same forward reference with the operands swapped, which passes
    # the "references the table being joined" test and needs the other half of
    # the rule to reject it. Accepting it silently rewires the join key to a
    # column of the left tree and renders a plan that looks correct
    ("SELECT l.team FROM laps l JOIN drivers d ON d.driver_id = d2.driver_id "
     "JOIN drivers d2 ON l.driver_id = d2.driver_id",
     "joined later"),
    ("SELECT l.team FROM laps l JOIN drivers d ON d.driver_id = d2.age "
     "JOIN drivers d2 ON l.driver_id = d2.driver_id",
     "joined later"),

    # N-relation range table: the clash is between entries 0 and 2, which the
    # old pairwise [0]/[1] check never compared
    ("SELECT laps.team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id "
     "JOIN laps ON drivers.driver_id = laps.driver_id",
     "self-join requires table aliases"),
    # two DIFFERENT tables under one name -> the duplicate-alias message, not
    # the self-join one; the clash is again entries 0 and 2
    ("SELECT x.team FROM laps x JOIN drivers d ON x.driver_id = d.driver_id "
     "JOIN drivers x ON d.driver_id = x.driver_id",
     "duplicate table alias"),
    # the same table twice under one ALIAS: every relation is aliased, so the
    # advice is "pick distinct names", not "add aliases you already wrote"
    ("SELECT d.name FROM drivers d JOIN laps l ON d.driver_id = l.driver_id "
     "JOIN drivers d ON l.driver_id = d.driver_id",
     "duplicate table alias"),

    # `team` exists on both relations — ambiguity is correct SQL behaviour and
    # gets more likely as relations are added
    ("SELECT team FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id",
     "ambiguous column reference"),
]

# Week 26 regression: Binder::resolveColumnRef rewrites an unqualified ON
# reference's table_name to the TABLE name of the relation it resolved to, so a
# relation aliased to exactly that name shadows it. Site 18 matched on that name
# and checked the column against the wrong schema, rejecting these legal,
# fully-executable single-join queries in all four modes. SQLite accepts both,
# so the rows are the oracle.
WEEK26_ALIAS_SHADOW_QUERIES = [
    "SELECT x.name AS n, drivers.lap_id AS lid FROM drivers x JOIN laps drivers "
    "ON age = drivers.round ORDER BY n, lid LIMIT 10",
    # the mirror: the shadowing alias is on the FROM side and the unqualified
    # column belongs to the joined relation
    "SELECT y.name AS n, drivers.lap_id AS lid FROM laps drivers JOIN drivers y "
    "ON drivers.round = age ORDER BY n, lid LIMIT 10",
]

QUERIES = PHASE2_WEEK12_BENCHMARK_QUERIES + [
    query for query in REGRESSION_QUERIES
    if query not in PHASE2_WEEK12_BENCHMARK_QUERY_SET
] + WEEK6_CHECKPOINT_QUERIES + ZONE_MAP_QUERIES + SELF_JOIN_QUERIES \
  + WEEK24_EXPRESSION_QUERIES + NULL_SEMANTICS_QUERIES + MIN_MAX_TYPE_QUERIES \
  + GROUP_KEY_QUALIFIER_QUERIES + CONSTANT_FOLDING_QUERIES + EXPRESSION_POSITION_QUERIES \
  + NULL_ORDERING_QUERIES + THREE_VALUED_LOGIC_QUERIES \
  + WEEK25_PREDICATE_QUERIES + WEEK25_CASE_QUERIES + WEEK25_SUBSTRING_QUERIES \
  + WEEK25_JOIN_QUERIES + WEEK26_ALIAS_SHADOW_QUERIES

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


def normalize(rows, preserve_order=False):
    """Sort rows and coerce numbers for stable comparison.

    NULL canonicalization: SwiftQL's aligned printer emits the literal "NULL",
    SQLite's driver returns Python None. Both map to the same token so a NULL
    compares equal across the two engines. Limitation: a genuine string value
    "NULL" in the data would also map to it — SwiftQL's text output carries no
    type information, so the two are indistinguishable here.

    preserve_order: when true, rows are compared in the order the engine emitted
    them instead of being sorted first. Sorting BOTH sides makes every ORDER BY
    defect undetectable by construction, which is exactly how a broken sort
    comparator (NULL keys, see compareForSort in value.h) passed this harness.
    Callers set it for any query containing ORDER BY; without ORDER BY, SQL does
    not specify row order and sorting is the only fair comparison.
    """
    NULL_TOKEN = "\x00NULL"

    def coerce(v):
        if v is None or v == "NULL": return NULL_TOKEN
        try: return round(float(v), 6)
        except (ValueError, TypeError): return str(v)

    normalized = []
    for row in rows:
        normalized.append(tuple(coerce(v) for v in row.values()))
    return normalized if preserve_order else sorted(normalized)


def rows_equal(a, b):
    """Compare two normalized row lists, using epsilon tolerance for floats."""
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


def run_rejection_suite(queries, label: str, extra_args: list = None):
    """Assert each query fails, and fails for the stated reason.

    SQLite is not an oracle here — it accepts all of these. The property under
    test is SwiftQL's own: unsupported SQL is a clean, specific error rather
    than a wrong answer. Matching the message (not just "it failed") is what
    stops an unrelated failure from passing this suite, the same rule the C++
    tests follow.
    """
    passed = failed = errors = 0
    print(f"\n--- {label} ---")
    for query, expected in queries:
        try:
            run_swiftql(query, extra_args)
            print(f"  FAIL  {query[:70]}\n    expected a rejection, got rows")
            failed += 1
        except RuntimeError as e:
            if expected in str(e):
                print(f"  PASS  {query[:70]}")
                passed += 1
            else:
                print(f"  FAIL  {query[:70]}\n    expected: {expected}\n    actual:   {e}")
                failed += 1
        except Exception as e:
            print(f"  ERROR {query[:70]}\n    {e}")
            errors += 1
    print(f"{passed} passed, {failed} failed, {errors} errors")
    return passed, failed, errors


def run_query_suite(conn, queries, label: str, extra_args: list = None):
    passed = failed = errors = 0
    print(f"\n--- {label} ---")
    for query in queries:
        try:
            swift_rows = run_swiftql(query, extra_args)
            sqlite_cursor = conn.execute(query)
            cols = [d[0] for d in sqlite_cursor.description]
            sqlite_rows = [dict(zip(cols, r)) for r in sqlite_cursor.fetchall()]

            # ORDER BY makes row order part of the answer, so compare it
            ordered = "ORDER BY" in query.upper()
            if rows_equal(normalize(swift_rows, ordered), normalize(sqlite_rows, ordered)):
                print(f"  PASS  {query[:70]}")
                passed += 1
            else:
                print(f"  FAIL  {query[:70]}")
                print(f"    ordered comparison: {ordered}")
                print(f"    SwiftQL: {normalize(swift_rows, ordered)[:4]}")
                print(f"    SQLite:  {normalize(sqlite_rows, ordered)[:4]}")
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
    # The optimizer-off path shares the binder, the logical planner, and the
    # physical plan builder with the optimized one, so plan-time typing and the
    # now-unconditional constant folding run either way — but nothing gated it.
    p4, f4, e4 = run_query_suite(
        conn, QUERIES, "Vectorized, optimizer off (--no-optimize)",
        extra_args=["--execution", "vectorized", "--storage", "columnar", "--no-optimize"],
    )

    # Week 26: the same four modes, for SQL that must be refused rather than
    # answered. The refusal is raised from a different place on each path
    # (Planner::plan on Volcano, VectorizedPlanBuilder on the vec path), so all
    # four are checked.
    modes = [
        ("Rejections (row storage, Volcano)", None),
        ("Rejections (columnar storage, Volcano)", ["--storage", "columnar"]),
        ("Rejections (columnar storage, vec path)",
         ["--execution", "vectorized", "--storage", "columnar"]),
        ("Rejections (vectorized, optimizer off)",
         ["--execution", "vectorized", "--storage", "columnar", "--no-optimize"]),
    ]
    r_passed = r_failed = r_errors = 0
    for label, extra in modes:
        rp, rf, re_ = run_rejection_suite(WEEK26_REJECTED_QUERIES, label, extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_

    total_passed = p1 + p2 + p3 + p4 + r_passed
    total_failed = f1 + f2 + f3 + f4 + r_failed
    total_errors = e1 + e2 + e3 + e4 + r_errors
    print(f"\nTotal: {total_passed} passed, {total_failed} failed, {total_errors} errors")
    if total_failed > 0 or total_errors > 0:
        sys.exit(1)

if __name__ == "__main__":
    main()
