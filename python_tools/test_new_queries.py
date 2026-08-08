#!/usr/bin/env python3
"""
test_new_queries.py — regression queries not covered by existing unit tests or compare_against_sqlite.py.

Covers query-observable SQL surface implemented in Weeks 1-21 (README plan): comparison/OR/AND
predicates, multi-column projection, all aggregates, single/multi GROUP BY, HAVING, DISTINCT,
ORDER BY, LIMIT, IS NULL / IS NOT NULL (Week 6), JOIN ... ON (Week 11), and table aliases /
qualified columns (Week 16). Weeks 8-15 and 17-20 are storage/execution/optimizer internals that
must not change results — exercised implicitly by every query above.

Week 21 (predicate pushdown + selection-vector cascade) is an optimizer rewrite that only runs on
the columnar/vectorized path, so the whole suite is also run in --execution vectorized --storage
columnar mode, and a dedicated WEEK21_QUERIES block exercises the pushdown-specific shapes (WHERE
predicates on one or both join sides, mixed cross-relation residuals, OR pushed as a unit, and
multi-conjunct scan-local filters that the executor cascades). The optimizer must not change any
result vs SQLite or vs --no-optimize.

Week 22 (cost-based physical join selection) picks the hash-join build side from filtered
cardinality estimates instead of raw table size, so a WEEK22_QUERIES block exercises joins whose
selective single-side WHERE makes the larger table the smaller *filtered* input — flipping the
build side to the side the pre-Week-22 row-count heuristic would never build. The build side is
internal and result-invariant, so these queries assert the same two properties as Week 21: output
matches SQLite, and optimized output matches --no-optimize (which still uses raw counts).

Skills applied:
  - execution-state-simulation: traced Volcano open/next/close for each query category
  - invariant-extraction: verified schema width, row-count monotonicity, iterator lifecycle
  - semantic-drift-detection: targeted OR semantics, multi-col GROUP BY, COUNT(col) vs COUNT(*),
                              multiple aggregates on empty group key, NULL pass-through, drivers table
  - minimal-fix-strategy: bugs found here will be fixed at the smallest causal stage
"""

import subprocess
import sqlite3
import csv
import sys
import re
import os

SWIFTQL_BIN = "./build/swiftql"
CATALOG_PATH = "catalog.json"
LAPS_CSV = "data/laps.csv"
DRIVERS_CSV = "data/drivers.csv"

# ─── 30 new queries ─────────────────────────────────────────────────────────
# Each entry: (label, query)
# Selection rationale per skill:
#   OR queries            → semantic-drift: OR short-circuit in FilterNode
#   != / <= / >=          → semantic-drift: comparison operators with real data
#   Multi-col SELECT      → invariant-extraction: schema width = N columns
#   Multiple agg, no GB   → execution-state-simulation: empty group key path
#   GROUP BY 2 cols       → invariant-extraction: composite group key hashing
#   HAVING COUNT / SUM    → execution-state-simulation: post-agg filter
#   HAVING with AND       → semantic-drift: AND inside HAVING predicate
#   COUNT(col)            → semantic-drift: COUNT(col) skips NULLs vs COUNT(*)
#   drivers table         → invariant-extraction: catalog lookup, schema mismatch risk
#   MIN/MAX on INT/STRING → semantic-drift: aggregate type compatibility

QUERIES = [
    # ── OR in WHERE ──────────────────────────────────────────────────────────
    ("OR_two_string_values",
     "SELECT COUNT(*) FROM laps WHERE team = 'Ferrari' OR team = 'RedBull'"),

    ("OR_two_season_values",
     "SELECT COUNT(*) FROM laps WHERE season = 2022 OR season = 2025"),

    ("OR_mixed_columns",
     "SELECT COUNT(*) FROM laps WHERE team = 'Ferrari' OR speed > 340"),

    # ── != / <= / >= comparisons ─────────────────────────────────────────────
    ("NEQ_string_where",
     "SELECT COUNT(*) FROM laps WHERE team != 'Ferrari'"),

    ("NEQ_with_season_filter",
     "SELECT COUNT(*) FROM laps WHERE team != 'Ferrari' AND season = 2025"),

    ("LTE_speed",
     "SELECT COUNT(*) FROM laps WHERE speed <= 285"),

    ("GTE_speed",
     "SELECT COUNT(*) FROM laps WHERE speed >= 340"),

    ("LTE_season",
     "SELECT COUNT(*) FROM laps WHERE season <= 2023"),

    # ── multi-column SELECT, no aggregates ──────────────────────────────────
    ("multi_col_select_no_agg",
     "SELECT lap_id, team, season FROM laps LIMIT 5"),

    ("select_sector_columns",
     "SELECT lap_id, sector_1, sector_2, sector_3 FROM laps LIMIT 5"),

    # ── multiple aggregates, no GROUP BY (empty group key path) ─────────────
    ("multi_agg_no_group_by",
     "SELECT COUNT(*), MIN(speed), MAX(speed), AVG(speed) FROM laps"),

    ("sum_and_avg_no_group_by",
     "SELECT SUM(speed), AVG(speed) FROM laps WHERE season = 2024"),

    # ── GROUP BY two columns ─────────────────────────────────────────────────
    ("group_by_two_cols",
     "SELECT team, season, COUNT(*) FROM laps GROUP BY team, season ORDER BY team, season LIMIT 8"),

    ("group_by_team_round",
     "SELECT team, round, COUNT(*) FROM laps GROUP BY team, round ORDER BY team, round LIMIT 8"),

    # ── HAVING with COUNT(*) threshold ──────────────────────────────────────
    ("having_count_threshold",
     "SELECT team, COUNT(*) FROM laps GROUP BY team HAVING COUNT(*) > 100 ORDER BY team"),

    ("having_count_low_threshold",
     "SELECT team, COUNT(*) FROM laps GROUP BY team HAVING COUNT(*) > 40 ORDER BY team"),

    # ── HAVING with AND ──────────────────────────────────────────────────────
    ("having_and_avg_range",
     "SELECT team, AVG(speed) FROM laps GROUP BY team HAVING AVG(speed) > 308 AND AVG(speed) < 314 ORDER BY team"),

    # ── HAVING with SUM ──────────────────────────────────────────────────────
    ("having_sum_threshold",
     "SELECT team, SUM(speed) FROM laps GROUP BY team HAVING SUM(speed) > 70000 ORDER BY team"),

    # ── COUNT(col) vs COUNT(*) ───────────────────────────────────────────────
    ("count_col_vs_star",
     "SELECT COUNT(driver_id) FROM laps"),

    ("count_col_with_filter",
     "SELECT COUNT(speed) FROM laps WHERE season = 2025"),

    # ── MIN / MAX on integer column ──────────────────────────────────────────
    ("min_max_int_season",
     "SELECT MIN(season), MAX(season) FROM laps"),

    ("min_max_int_round",
     "SELECT MIN(round), MAX(round) FROM laps"),

    # ── DISTINCT + WHERE + ORDER BY ──────────────────────────────────────────
    ("distinct_where_order",
     "SELECT DISTINCT team FROM laps WHERE speed > 320 ORDER BY team"),

    ("distinct_round_where_season",
     "SELECT DISTINCT round FROM laps WHERE season = 2025 ORDER BY round"),

    # ── ORDER BY numeric column (no GROUP BY) ────────────────────────────────
    #
    # `, lap_id` is not decoration. Written as `ORDER BY speed LIMIT 5` this
    # entry had two rows tied at speed=280.01 INSIDE the cut, so SQL left their
    # relative order unspecified and the entry passed only because SwiftQL and
    # SQLite happened to break the tie the same way -- both by input/rowid
    # order. Seam audit pass 2 gave the sort a deterministic tie-break
    # (src/execution/sort_comparator.h), which broke that coincidence: SwiftQL
    # now answers 3882 before 5275 where SQLite answers 5275 first. Both are
    # correct; neither is an oracle for the other.
    #
    # No gate could see it -- this query is not in compare_against_sqlite.py,
    # and normalize() below sorts unconditionally -- which is exactly why it is
    # worth fixing rather than leaving. The entry was written to exercise
    # ORDER BY + LIMIT, not to pin a tie SQL does not define, and it still does
    # that with a total order. Queries that DO deliberately depend on a tie at
    # the cut live in compare_against_sqlite.py's ENGINE_AGREEMENT_QUERIES,
    # where the oracle is the other SwiftQL modes rather than SQLite.
    ("order_by_speed_asc",
     "SELECT lap_id, speed FROM laps ORDER BY speed, lap_id LIMIT 5"),

    # ── drivers table: not tested anywhere ───────────────────────────────────
    ("drivers_simple_filter",
     "SELECT name FROM drivers WHERE age > 35 ORDER BY name"),

    ("drivers_group_by_nationality",
     "SELECT nationality, COUNT(*) FROM drivers GROUP BY nationality ORDER BY nationality"),

    ("drivers_min_max_age",
     "SELECT MIN(age), MAX(age) FROM drivers"),

    ("drivers_group_by_team",
     "SELECT team, COUNT(*) FROM drivers GROUP BY team ORDER BY team"),

    ("drivers_having_avg_age",
     "SELECT team, AVG(age) FROM drivers GROUP BY team HAVING AVG(age) > 30 ORDER BY team"),

    # ── scalar aggregate over empty filter result ───────────────────────────
    # execution-state-simulation: no GROUP BY + zero input rows must still emit
    # one row with COUNT=0. season=1999 matches no rows. (The SUM/AVG/MIN/MAX=NULL
    # case is asserted in the C++ test HashAggregateNode.ScalarAggregateOverEmptyInput...;
    # it is omitted here because the vectorized engine has no null bitmap and renders
    # empty SUM/MIN/MAX as 0.0 sentinels — a separate, pre-existing limitation.)
    ("empty_scalar_count",
     "SELECT COUNT(*) FROM laps WHERE season = 1999"),

    # ── IS NULL / IS NOT NULL (Week 6) ───────────────────────────────────────
    # invariant-extraction: null-aware predicate path; CSV holds no nulls, so
    # IS NOT NULL passes every row and IS NULL passes none — both must match SQLite.
    ("is_not_null_speed",
     "SELECT COUNT(*) FROM laps WHERE speed IS NOT NULL"),

    ("is_null_no_rows",
     "SELECT lap_id FROM laps WHERE speed IS NULL"),

    ("is_null_scalar_count",
     "SELECT COUNT(*) FROM laps WHERE speed IS NULL"),

    # ── plain < in WHERE (Week 6 comparison completeness) ────────────────────
    ("lt_speed_where",
     "SELECT COUNT(*) FROM laps WHERE speed < 300"),

    # ── JOIN ... ON (Week 11 hash join execution) ────────────────────────────
    # 1:1 join on driver_id → row count preserved. GROUP BY resolves against the
    # merged FROM+JOIN schema, so grouping on a joined-table column is supported.
    ("join_count_all",
     "SELECT COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id"),

    ("join_group_by_from_col",
     "SELECT l.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY l.team ORDER BY l.team"),

    ("join_group_by_joined_col",
     "SELECT nationality, COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id GROUP BY nationality ORDER BY nationality"),

    ("join_filtered_avg",
     "SELECT AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id WHERE l.season = 2025"),

    ("join_count_with_filter",
     "SELECT COUNT(*) FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id WHERE laps.speed > 340"),

    # ── table aliases + qualified columns (Week 16 binder) ───────────────────
    ("qualified_col_in_where",
     "SELECT COUNT(*) FROM laps WHERE laps.season = 2025"),

    ("alias_projection",
     "SELECT l.team, l.speed FROM laps l WHERE l.speed >= 344"),

    ("alias_drivers_filter",
     "SELECT d.name, d.age FROM drivers d WHERE d.age > 36 ORDER BY d.name"),
]


# ─── Week 21: predicate pushdown + selection-vector cascade ──────────────────
# These specifically exercise the Week 21 optimizer and are run on the
# vectorized path (where pushdown/cascade live) against SQLite AND against the
# same query with --no-optimize (result-preserving invariant).
#   both-sides pushdown   → checkpoint: both join inputs filtered before the join
#   from-only / join-only → single-relation predicate lands on its own scan
#   mixed residual        → cross-relation conjunct stays above the join
#   OR pushed as a unit    → an OR referencing one relation is still single-slot
#   multi-conjunct scan   → executor cascades the ordered conjuncts
#   self-join + WHERE      → slot routing pushes each side to the right scan
WEEK21_QUERIES = [
    ("pd_both_sides",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 AND d.age > 35 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_from_only",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_join_only",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE d.age > 35 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_mixed_residual",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 AND l.speed > d.age ORDER BY l.team, d.name LIMIT 25"),

    ("pd_or_one_side",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE (l.season = 2024 OR l.season = 2025) AND d.age > 35 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_three_conjuncts",
     "SELECT l.team, d.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 AND l.speed > 340 AND d.age > 35 ORDER BY l.team, d.name LIMIT 25"),

    ("pd_agg_after_pushdown",
     "SELECT l.team, AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 AND d.age > 35 GROUP BY l.team ORDER BY l.team"),

    ("cascade_single_table_and",
     "SELECT team, speed FROM laps WHERE season = 2025 AND speed > 340 ORDER BY team, speed LIMIT 25"),

    ("cascade_triple_and",
     "SELECT team FROM laps WHERE season = 2025 AND speed > 330 AND speed < 360 ORDER BY team LIMIT 25"),

    ("self_join_where_both",
     "SELECT l1.team FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.lap_id "
     "WHERE l1.season = 2025 AND l2.speed > 340 ORDER BY l1.team LIMIT 25"),
]


# ─── Week 22: cost-based physical join selection ─────────────────────────────
# Each join pairs laps (10k rows) with drivers (20 rows). A highly selective
# filter on the laps side drops its *estimated* rows below 20, so the cost model
# builds the hash table on filtered laps — the reverse of the raw-count heuristic
# (which always builds the 20-row drivers side). Results are build-side-invariant,
# so these must match SQLite and match --no-optimize (raw counts, no flip).
#   flip_from_side       → laps is FROM; filter flips build to filtered laps
#   flip_join_side       → laps is JOIN; filter flips build to filtered laps
#   no_flip_control       → weak filter keeps drivers the smaller side (no flip)
#   flip_with_aggregate  → flipped build side feeding a GROUP BY still correct
#   flip_both_filtered   → both sides filtered; FROM still smaller, builds
WEEK22_QUERIES = [
    ("w22_flip_from_side",
     "SELECT drivers.name, laps.speed FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id "
     "WHERE laps.speed > 344.94 ORDER BY drivers.name, laps.speed"),

    ("w22_flip_join_side",
     "SELECT drivers.name, laps.speed FROM drivers JOIN laps ON drivers.driver_id = laps.driver_id "
     "WHERE laps.speed > 344.94 ORDER BY drivers.name, laps.speed"),

    ("w22_no_flip_control",
     "SELECT l.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 GROUP BY l.team ORDER BY l.team"),

    ("w22_flip_with_aggregate",
     "SELECT d.nationality, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.speed > 344.9 GROUP BY d.nationality ORDER BY d.nationality"),

    ("w22_flip_both_filtered",
     "SELECT drivers.name, laps.speed FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id "
     "WHERE laps.speed > 344.9 AND drivers.age > 25 ORDER BY drivers.name, laps.speed"),
]


# Week 23.5 selects a join ALGORITHM per join (hash vs SIMD loop) from the same
# estimates. The choice is a result-invariant internal, so these queries assert
# output equality across optimized / --no-optimize (hash-only) / SQLite while
# steering the planner to each algorithm:
#   simd_unfiltered_join → 20-row drivers build side: SIMD loop selected
#   simd_filtered_probe  → selective filter shrinks the probe; SIMD still correct
#   simd_swapped_build   → filtered laps becomes a tiny build side (flip + SIMD)
#   hash_string_key      → STRING join key: SIMD ineligible, hash join runs
WEEK23_5_QUERIES = [
    ("w23_5_simd_unfiltered_join",
     "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id "
     "ORDER BY laps.lap_id LIMIT 50"),

    ("w23_5_simd_filtered_probe",
     "SELECT drivers.name, laps.speed FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id "
     "WHERE laps.speed > 340 ORDER BY drivers.name, laps.speed"),

    ("w23_5_simd_swapped_build",
     "SELECT drivers.name, laps.speed FROM drivers JOIN laps ON drivers.driver_id = laps.driver_id "
     "WHERE laps.speed > 344.94 ORDER BY drivers.name, laps.speed"),

    ("w23_5_hash_string_key",
     "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers ON laps.team = drivers.team "
     "WHERE laps.speed > 344.9 ORDER BY laps.lap_id, drivers.name"),
]

# Phase 4 audit fixes: shapes both engines used to get silently wrong, so the
# vec≡volcano equivalence could never catch them — SQLite is the oracle here.
#   fix_c4_*  self-join aggregates over the same column collapsed to one value
#             (asymmetric join key so the two averages genuinely differ)
#   fix_c3_*  qualified GROUP BY silently grouped by the FROM side
#   fix_m1_*  aggregates referenced only in HAVING / ORDER BY failed at execution
AUDIT_FIXES_QUERIES = [
    ("fix_c4_selfjoin_dup_aggs",
     "SELECT AVG(l1.speed), AVG(l2.speed) FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.driver_id"),

    ("fix_c3_qualified_group_by_join_side",
     "SELECT d.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id GROUP BY d.team"),

    ("fix_c3_qualified_group_by_selfjoin",
     "SELECT l2.team, COUNT(*) FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.driver_id GROUP BY l2.team"),

    ("fix_m1_having_only_aggregate",
     "SELECT team FROM laps GROUP BY team HAVING COUNT(*) > 1000"),

    ("fix_m1_order_by_only_aggregate",
     "SELECT team FROM laps GROUP BY team ORDER BY COUNT(*) DESC, team LIMIT 3"),

    ("fix_m1_hidden_aggs_mixed",
     "SELECT team, MIN(speed) FROM laps GROUP BY team HAVING AVG(speed) > 250 "
     "ORDER BY MAX(speed) DESC, team"),

    ("fix_c4_c3_m1_combined",
     "SELECT l1.team, AVG(l1.speed), AVG(l2.speed) FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.driver_id "
     "GROUP BY l1.team HAVING COUNT(*) > 2 ORDER BY AVG(l2.speed) DESC"),

    # cross-relation OR: indivisible, spans both slots -> stays above the join
    # as an intact residual (audit follow-up; also pinned by a plan-shape test)
    ("fix_or_cross_relation_residual",
     "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.season = 2025 OR d.age > 30"),
]

# Week 24 general expressions: arithmetic precedence, unary minus, aliases,
# expressions inside aggregate arguments, expressions over aggregate results,
# and expression / alias GROUP BY keys. All queries are division-safe (no
# x/0): a NULL is unrepresentable in a materialized DataChunk column —
# ColumnVector has no null mask, so the vectorized path degrades NULL to a
# 0/"NULL" sentinel that legitimately differs from SQLite's NULL. NULL
# semantics are pinned by Volcano-level and VecProjectNode unit tests
# instead. SQLite is the semantics oracle (INT/INT truncates).
WEEK24_QUERIES = [
    ("w24_arith_projection",
     "SELECT lap_id, speed * 2 + 1 FROM laps WHERE lap_id < 6 ORDER BY lap_id"),

    ("w24_arith_precedence",
     "SELECT lap_id, speed + sector_1 * 2, (speed + sector_1) * 2 FROM laps "
     "WHERE lap_id < 6 ORDER BY lap_id"),

    ("w24_unary_minus",
     "SELECT lap_id, -speed, 0 - lap_id FROM laps WHERE lap_id < 6 ORDER BY lap_id"),

    ("w24_int_division_truncates",
     "SELECT season, season / 4 FROM laps WHERE lap_id < 6 ORDER BY lap_id"),

    ("w24_arith_in_where",
     "SELECT lap_id FROM laps WHERE speed * 2 > 660 AND season - 1 = 2024 ORDER BY lap_id LIMIT 10"),

    # the Week 24 checkpoint shape: TPC-H revenue expression + alias ORDER BY
    ("w24_sum_over_expression",
     "SELECT team, SUM(speed * (1 - sector_1 / 100)) AS revenue FROM laps "
     "GROUP BY team ORDER BY revenue DESC"),

    ("w24_expr_over_aggregates",
     "SELECT team, SUM(speed) / COUNT(*) AS manual_avg, AVG(speed) FROM laps "
     "GROUP BY team ORDER BY team"),

    ("w24_alias_order_by",
     "SELECT team, AVG(speed) AS avg_speed FROM laps GROUP BY team ORDER BY avg_speed DESC LIMIT 3"),

    ("w24_group_by_alias_expression",
     "SELECT season - 2020 AS era, COUNT(*) FROM laps GROUP BY era ORDER BY era"),

    ("w24_group_by_expr_having",
     "SELECT season - 2020, AVG(speed) FROM laps GROUP BY season - 2020 "
     "HAVING season - 2020 >= 3 ORDER BY season - 2020"),

    ("w24_having_expr_aggregate",
     "SELECT team, COUNT(*) FROM laps GROUP BY team HAVING SUM(speed * 2) > 900000 ORDER BY team"),

    ("w24_join_arith",
     "SELECT d.name, l.speed - d.age FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.lap_id < 6 ORDER BY d.name, l.speed - d.age"),
]

# The join operator each steering comment above claims — asserted from
# --explain's Physical Plan (see run_join_steering), so the steering stays
# true as data stats or cost constants drift instead of silently all
# degrading to one algorithm while the output checks keep passing.
WEEK23_5_EXPECTED_JOIN = {
    "w23_5_simd_unfiltered_join": "VecSimdLoopJoin",
    "w23_5_simd_filtered_probe":  "VecSimdLoopJoin",
    "w23_5_simd_swapped_build":   "VecSimdLoopJoin",
    "w23_5_hash_string_key":      "VecHashJoin",
}


def load_sqlite():
    conn = sqlite3.connect(":memory:")
    conn.row_factory = sqlite3.Row
    conn.execute("""
        CREATE TABLE laps (
            lap_id INTEGER, driver_id INTEGER, team TEXT, speed REAL,
            sector_1 REAL, sector_2 REAL, sector_3 REAL, season INTEGER, round INTEGER
        )
    """)
    with open(LAPS_CSV) as f:
        for row in csv.DictReader(f):
            conn.execute("INSERT INTO laps VALUES (?,?,?,?,?,?,?,?,?)", (
                int(row['lap_id']), int(row['driver_id']), row['team'],
                float(row['speed']), float(row['sector_1']),
                float(row['sector_2']), float(row['sector_3']),
                int(row['season']), int(row['round'])
            ))
    conn.execute("""
        CREATE TABLE drivers (
            driver_id INTEGER, name TEXT, nationality TEXT, team TEXT, age INTEGER
        )
    """)
    with open(DRIVERS_CSV) as f:
        for row in csv.DictReader(f):
            conn.execute("INSERT INTO drivers VALUES (?,?,?,?,?)", (
                int(row['driver_id']), row['name'], row['nationality'],
                row['team'], int(row['age'])
            ))
    conn.commit()
    return conn


def parse_swiftql_output(output: str):
    lines = [l for l in output.strip().split('\n') if l.strip()]
    if len(lines) < 2:
        return []
    # split on 2+ spaces like the value rows: Week 24 expression columns print
    # names containing single spaces, e.g. "((speed * 2) + 1)"
    headers = re.split(r'  +', lines[0].strip())
    rows = []
    for line in lines[2:]:
        if line.startswith('(') and line.endswith(')'):
            break
        values = re.split(r'  +', line.strip())
        if len(values) == len(headers):
            rows.append(dict(zip(headers, values)))
    return rows


def normalize(rows):
    # Week 29: NULL canonicalization, mirroring compare_against_sqlite.py's
    # normalize(). SwiftQL's aligned printer emits the literal "NULL" while
    # SQLite's driver returns Python None, and str(None) is "None" — so before
    # this, every SwiftQL NULL compared unequal to the SQLite NULL beside it.
    # It went unnoticed because nothing here could PRODUCE a NULL from catalog
    # data (invariant 14); a LEFT JOIN can. Same documented limitation as the
    # sibling harness: a genuine string value "NULL" maps to the token too,
    # since SwiftQL's text output carries no type information.
    NULL_TOKEN = "\x00NULL"

    def coerce(v):
        if v is None or v == "NULL":
            return NULL_TOKEN
        try:
            return round(float(v), 6)
        except (ValueError, TypeError):
            return str(v)
    return sorted(tuple(coerce(v) for v in row.values()) for row in rows)


def run_swiftql(query: str, extra_args=None):
    args = [SWIFTQL_BIN, "--catalog", CATALOG_PATH, "--no-cache", "--query", query]
    if extra_args:
        args = args[:4] + extra_args + args[4:]  # insert after --no-cache
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"SwiftQL error: {result.stderr.strip()}")
    return parse_swiftql_output(result.stdout)


def run_suite(conn, queries, mode_label, extra_args=None):
    """Compare each query's SwiftQL output against SQLite. Returns (passed, failed, errors, fail_list)."""
    passed, failed, errors = 0, 0, 0
    fail_list = []
    print(f"\n--- {mode_label} ---")
    for label, query in queries:
        try:
            swift_rows = run_swiftql(query, extra_args)
            cur = conn.execute(query)
            cols = [d[0] for d in cur.description]
            sqlite_rows = [dict(zip(cols, r)) for r in cur.fetchall()]

            sw_norm = normalize(swift_rows)
            sq_norm = normalize(sqlite_rows)

            if sw_norm == sq_norm:
                print(f"  PASS  [{label}]  {query[:66]}")
                passed += 1
            else:
                print(f"  FAIL  [{label}]  {query[:66]}")
                print(f"    SwiftQL ({len(swift_rows)} rows): {sw_norm[:3]}")
                print(f"    SQLite  ({len(sqlite_rows)} rows): {sq_norm[:3]}")
                failed += 1
                fail_list.append((f"{mode_label}:{label}", query, sw_norm[:3], sq_norm[:3]))
        except Exception as e:
            print(f"  ERROR [{label}]  {query[:66]}")
            print(f"    {e}")
            errors += 1
            fail_list.append((f"{mode_label}:{label}", query, str(e), ""))
    print(f"  {passed} passed, {failed} failed, {errors} errors")
    return passed, failed, errors, fail_list


def normalize_ordered(rows):
    """normalize() WITHOUT the final sort — rows compared in emission order.

    Seam audit pass 3 (B.4.1): `normalize()` ends in `sorted(...)`, so every
    divergence that is an ORDER difference and not a SET difference is invisible
    to the invariant by construction. That is exactly half of blocker B3-1: a
    reordered join permutes the sort input's schema, the positional tie-break in
    `sort_comparator.h` reads that schema, and the two legs emit the same rows in
    a different order. Without a LIMIT to turn the order difference into a set
    difference, the sorted comparison passes it.

    Used only where the query DECLARES an order (`ORDER BY`), because that is
    the only case where SQL gives emission order any meaning at all. See
    `_invariant_compare` for the split and for what it deliberately still cannot
    see.
    """
    NULL_TOKEN = "\x00NULL"

    def coerce(v):
        if v is None or v == "NULL":
            return NULL_TOKEN
        try:
            return round(float(v), 6)
        except (ValueError, TypeError):
            return str(v)
    return [tuple(coerce(v) for v in row.values()) for row in rows]


def _invariant_compare(query, rows):
    """Pick the comparison the query's own text licenses.

    ORDER BY present  → ORDERED (positional). Strictly stronger; measured
                        against the whole suite before it was turned on, and all
                        65 pre-existing ORDER BY entries already agreed under it,
                        so it costs nothing and closes B.4's first obligation.
    no ORDER BY       → SET (sorted), unchanged. A query with no declared order
                        has no specified emission order, and a reordered join
                        legitimately emits in a different one.

    WHAT THIS STILL CANNOT SEE, stated rather than implied: a divergence that
    the LIMIT happens not to expose. `b31_tie_order_only_no_set_change` is the
    entry that pins the ordered half against exactly that; without the ordered
    comparison it would pass under any comparator at all.
    """
    if re.search(r"\bORDER\s+BY\b", query, re.IGNORECASE):
        return normalize_ordered(rows)
    return normalize(rows)


# What `optimized == --no-optimize` does and does not certify. Printed with the
# result, because a number reported without its scope gets read as a bigger claim
# than it is — which is how B3-1 stayed invisible behind "119 checks, 0
# divergences".
#
# Established by seam audit pass 3, A.3 and B.4, and re-checked against
# src/cli/main.cc:566 and :134 while this banner was written:
#
#   * `--no-optimize` gates EXACTLY THREE passes — PredicatePushdown,
#     JoinEnumeration, CardinalityEstimator (main.cc:566-...). Those three, and
#     only those, are what the two legs differ by.
#   * SIX further passes run in both legs. FIVE of them — foldConstants,
#     lowerInSubqueries, lowerExistsSubqueries, lowerCorrelatedScalars, derived
#     normalization — run strictly BEFORE the gate on an input the flag cannot
#     have touched, so their output is identical in both legs BY CONSTRUCTION.
#     This differential is blind to a bug in any of them: both legs would be
#     wrong identically. compare_against_sqlite.py is the check that sees those.
#   * THE SIXTH IS NOT LIKE THE OTHER FIVE. `materializeSubqueries`
#     (main.cc:500-543) EXECUTES the nested query and threads the flag into the
#     nested runner (`runVectorizedToRows(..., args.no_optimize)`, main.cc:521),
#     so the body is optimized in one leg and not in the other. It is ungated but
#     its OUTPUT is gate-dependent, which is deliberate — a runner that always
#     optimized would hand both legs the same sub-result and quietly stop testing
#     the sub-plan. The consequence is that this differential DOES reach inside a
#     subquery body, and a plan-dependent body turns into a different
#     materialized constant (b31_subquery_* below).
INVARIANT_SCOPE = (
    "  scope: --no-optimize gates exactly 3 passes (pushdown, join enumeration,\n"
    "         estimation). 5 further passes are identical in both legs BY\n"
    "         CONSTRUCTION and are therefore NOT tested here — a bug in those is\n"
    "         wrong in both legs and only the SQLite oracle can see it. The 6th,\n"
    "         materializeSubqueries, threads the flag into the nested runner, so\n"
    "         subquery BODIES are covered. ORDER BY queries compare positionally;\n"
    "         the rest compare as sets.")


def run_optimizer_invariant(queries):
    """Week 21: the optimizer must be result-preserving — vectorized output must be
    identical with and without --no-optimize. Compares SwiftQL to itself, no oracle."""
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    passed, failed, errors = 0, 0, 0
    fail_list = []
    print(f"\n--- Optimizer invariant (vectorized: optimized == --no-optimize) ---")
    print(INVARIANT_SCOPE)
    for label, query in queries:
        try:
            opt = _invariant_compare(query, run_swiftql(query, VEC))
            noopt = _invariant_compare(query, run_swiftql(query, VEC + ["--no-optimize"]))
            if opt == noopt:
                print(f"  PASS  [{label}]  {query[:66]}")
                passed += 1
            else:
                print(f"  FAIL  [{label}]  {query[:66]}")
                print(f"    optimized:    {opt[:3]}")
                print(f"    --no-optimize:{noopt[:3]}")
                failed += 1
                fail_list.append((f"invariant:{label}", query, opt[:3], noopt[:3]))
        except Exception as e:
            print(f"  ERROR [{label}]  {query[:66]}\n    {e}")
            errors += 1
            fail_list.append((f"invariant:{label}", query, str(e), ""))
    print(f"  {passed} passed, {failed} failed, {errors} errors")
    return passed, failed, errors, fail_list


def run_join_steering(queries):
    """Week 23.5: assert each query's planned join operator matches what its
    steering comment claims, read from --explain's Physical Plan section."""
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    passed, failed = 0, 0
    fail_list = []
    print(f"\n--- Week 23.5 join-algorithm steering (--explain) ---")
    for label, query in queries:
        expected = WEEK23_5_EXPECTED_JOIN[label]
        other = "VecSimdLoopJoin" if expected == "VecHashJoin" else "VecHashJoin"
        args = [SWIFTQL_BIN, "--catalog", CATALOG_PATH, "--no-cache",
                *VEC, "--explain", "--query", query]
        result = subprocess.run(args, capture_output=True, text=True)
        physical = result.stdout.split("=== Physical Plan ===")[-1]
        if result.returncode == 0 and expected in physical and other not in physical:
            print(f"  PASS  [{label}]  plans {expected}")
            passed += 1
        else:
            planned = other if other in physical else "neither operator"
            print(f"  FAIL  [{label}]  expected {expected}, planned {planned}")
            failed += 1
            fail_list.append((f"steering:{label}", query, planned, expected))
    print(f"  {passed} passed, {failed} failed, 0 errors")
    return passed, failed, 0, fail_list

# ─── Week 27: multi-way join execution ──────────────────────────────────────
# Three or more relations execute on the VECTORIZED path only (Planner::plan
# builds one join; row/Volcano never gains multi-way execution), so this block is
# added to the vectorized run and the optimizer-invariant run — never to the
# default row/Volcano run, where it would be a refusal rather than a result.
# The multi-KEY and residual-ON shapes are not vectorized-only and live in
# compare_against_sqlite.py's main suite, which runs all four modes.
WEEK27_QUERIES = [
    # the slot query: the third join's key `team` is at relation slot 1 while the
    # merged left schema holds `team` at slot 0 first, so a bare-name lookup
    # joins the wrong relation's column and returns a plausible wrong count
    ("w27_three_way_slot_exact_key",
     "SELECT COUNT(*) FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.driver_id "
     "JOIN drivers d ON l2.team = d.team"),

    ("w27_three_way_projection",
     "SELECT l.team, d.name, d2.name FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team WHERE l.lap_id < 20 "
     "ORDER BY l.team, d.name, d2.name"),

    # predicates on three different relations, plus a residual ON conjunct:
    # pushdown routes each conjunct down the left-deep spine by relation slot
    ("w27_three_way_predicates_per_relation",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team AND d2.age > 25 "
     "WHERE l.season = 2024 AND d.nationality IN ('British','German')"),

    # group key on the MIDDLE relation, whose column name also exists at slot 0
    ("w27_three_way_group_by_middle_relation",
     "SELECT d.team, COUNT(*), MIN(l.speed) FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team GROUP BY d.team ORDER BY d.team"),

    # a residual belonging to an earlier relation, on a later join's ON clause:
    # distribute() has to walk past the join that owns the clause
    ("w27_residual_for_an_earlier_relation",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team AND d.age > 30"),

    # four relations: nothing in lowering is 3-specific
    ("w27_four_way_join",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.driver_id = d2.driver_id "
     "JOIN laps l2 ON d2.driver_id = l2.driver_id WHERE l.lap_id < 5"),
]


# ─── Week 28: cost-based join ordering ───────────────────────────────────────
# Three-relation-plus shapes where more than one order is legal, so the search
# has a real choice. Vectorized-only (multi-way execution has been, since Week
# 27), and bounded so the suite does not materialize a five-million-row
# intermediate. A join order is a result-invariant internal, so these assert
# output equality across optimized / --no-optimize / SQLite; the ORDER itself is
# asserted separately from --explain (invariant 13), and the WORK it saves is
# measured separately still, from --explain-analyze.
#
# The same query texts are in compare_against_sqlite.py, which is the external
# oracle for them; this file adds the two decision assertions on top.
WEEK28_QUERIES = [
    # star centred on l: drivers (20 rows) is adjacent to both laps scans, so
    # joining it second avoids an intermediate of 10000*10000/20 rows
    ("w28_star_pivot_small",
     "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id "
     "JOIN drivers d ON l.driver_id = d.driver_id WHERE l.lap_id < 200"),

    # the SAME join graph, written in the good order already. Under enumeration
    # the two must execute identically — that is the pair run_join_order_work
    # measures.
    ("w28_star_written_good",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN laps l2 ON l.driver_id = l2.driver_id WHERE l.lap_id < 200"),

    # the search leads with drivers@1, so relation 0 is NOT at the bottom of the
    # spine — the case the merged-schema stamping and the bottom-join
    # from_slot = 0 rewrite exist for, and one no written-order tree can produce
    ("w28_nonzero_leftmost",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team"),

    # same reordering, projecting columns from relations 0 and 2: column identity
    # has to survive a merged schema rebuilt in a new order
    ("w28_nonzero_leftmost_projection",
     "SELECT l.team, d2.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team WHERE l.season = 2022 AND l.round < 3 "
     "ORDER BY l.team, d2.name"),

    # triangle: every order legal, and the last relation added carries TWO keys
    # where the first carried one — the ordering decides which join is composite
    ("w28_triangle",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team AND l.driver_id = d2.driver_id"),

    # a residual ON conjunct and a pushed WHERE on a reordered tree: predicate
    # assignment runs BEFORE enumeration and every conjunct must survive the fold
    ("w28_residual_and_pushed_where",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team AND d2.age > 25 WHERE l.season = 2024"),

    # aggregation over a reordered tree, grouped by a middle relation's column
    ("w28_aggregate_over_reordered",
     "SELECT d.nationality, COUNT(*), MIN(l.speed) FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team GROUP BY d.nationality ORDER BY d.nationality"),

    # CONTROL: same shape as w28_nonzero_leftmost_projection but with a selective
    # filter on laps. That filter drops laps below drivers, so leading with laps
    # is now correct and the search KEEPS the written order — proving the decision
    # reacts to filtered cardinality rather than to table size, and that
    # "reordered" is not simply what this pass always does.
    ("w28_selective_filter_keeps_written",
     "SELECT l.team, d2.name FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team WHERE l.lap_id < 30 ORDER BY l.team, d2.name"),

    # four relations: nothing in the DP is 3-specific
    ("w28_four_way",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.driver_id = d2.driver_id "
     "JOIN laps l2 ON d2.driver_id = l2.driver_id WHERE l.lap_id < 5"),
]

# ─── Week 29: outer joins ───────────────────────────────────────────────────
# Result-preservation is the property at risk here, not plan shape: predicate
# pushdown must not move a WHERE conjunct onto the null-supplying side, and join
# enumeration must not reorder across an outer join. Both bugs are invisible to a
# single run — they show up as optimized != --no-optimize, which is what
# run_optimizer_invariant compares. The ROWS themselves are diffed against SQLite
# in compare_against_sqlite.py; this block exists for the optimizer half.
#
# Three-or-more-relation shapes are vectorized-only for the pre-existing Week 27
# reason, which is also why this block joins the vectorized runs only.
WEEK29_QUERIES = [
    # a WHERE conjunct on the null-supplying side. Pushed onto the laps scan it
    # produces null-extended rows the WHERE existed to remove -- MORE rows, no
    # error, and only the --no-optimize leg can see it.
    ("w29_where_on_null_supplying_side",
     "SELECT COUNT(*) FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
     "WHERE l.season = 2024"),

    # ...and the preserved-side control, which must STILL be pushed: a guard that
    # declines both sides is correct and silently costs every outer join its
    # pushdown, which no result test can see.
    ("w29_where_on_preserved_side",
     "SELECT COUNT(*) FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
     "WHERE d.age > 30"),

    # both positions plus an ON residual, in one query
    ("w29_all_three_predicate_positions",
     "SELECT d.name, COUNT(*) FROM drivers d LEFT JOIN laps l "
     "ON d.driver_id = l.driver_id AND l.round < 5 "
     "WHERE d.age > 25 GROUP BY d.name ORDER BY d.name"),

    # THE unmatched-heavy shape: lap_id runs to 10000, driver_id stops at 20, so
    # almost every probe row is null-extended
    ("w29_mostly_unmatched",
     "SELECT COUNT(*) FROM laps l LEFT JOIN drivers d ON l.lap_id = d.driver_id"),

    # TPC-H Q13's semantic: COUNT(col) is 0 for an unmatched row, COUNT(*) is 1
    ("w29_q13_count_shape",
     "SELECT d.name, COUNT(l.lap_id), COUNT(*) FROM drivers d "
     "LEFT JOIN laps l ON d.driver_id = l.driver_id AND l.speed > 400 "
     "GROUP BY d.name ORDER BY d.name"),

    # three relations with an outer join: enumeration must DECLINE the whole tree,
    # so optimized and --no-optimize execute the same order and agree
    ("w29_three_way_outer_last",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "LEFT JOIN drivers d2 ON d.team = d2.team AND d2.age > 90"),

    # the outer join FIRST, an inner one above it: the null-extended rows become a
    # probe input, where their NULL key must stay unmatchable
    ("w29_three_way_outer_first",
     "SELECT COUNT(*) FROM laps l LEFT JOIN drivers d ON l.lap_id = d.driver_id "
     "JOIN laps l2 ON l.driver_id = l2.driver_id WHERE l.lap_id < 30"),

    # ORDER BY over null-extended rows on a reorder-declining tree
    ("w29_order_by_nulls",
     "SELECT l.lap_id, d.name FROM laps l LEFT JOIN drivers d "
     "ON l.lap_id = d.driver_id WHERE l.lap_id < 25 ORDER BY d.name, l.lap_id"),
]

# Week 29: the decline, asserted on the checkpoint surface. An outer join anywhere
# in the tree means the search never ran, so there is no order= line to print —
# and printing one would claim a decision that was never made. Paired with a
# control in WEEK28_QUERIES (w29_outer_decline_control) whose all-inner form of the
# same shape MUST still show method=dp: a containsOuterJoin that fires too eagerly
# would turn off join ordering for the whole project, and no result test can see it.
WEEK29_NO_ORDER_DECISION = [
    ("w29_three_way_outer_last",
     "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "LEFT JOIN drivers d2 ON d.team = d2.team AND d2.age > 90"),
    ("w29_three_way_outer_first",
     "SELECT COUNT(*) FROM laps l LEFT JOIN drivers d ON l.lap_id = d.driver_id "
     "JOIN laps l2 ON l.driver_id = l2.driver_id WHERE l.lap_id < 30"),
]


def run_outer_join_decline(queries):
    """Week 29: an outer join in the tree means join enumeration declined, so
    --explain must show NO order= line — there was no decision. Asserted on
    --explain for the same reason the Week 28 steering checks are (invariant 13):
    a planner decision that never changes rows has no result surface."""
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    passed, failed = 0, 0
    fail_list = []
    print(f"\n--- Week 29 outer-join reorder decline (--explain) ---")
    for label, query in queries:
        args = [SWIFTQL_BIN, "--catalog", CATALOG_PATH, "--no-cache",
                *VEC, "--explain", "--query", query]
        result = subprocess.run(args, capture_output=True, text=True)
        section = result.stdout.split("=== Optimized Logical Plan ===")[-1] \
                              .split("=== Physical Plan ===")[0]
        order = re.search(r"order=(\S+)", section)
        # and the plan must actually BE an outer join, or this asserts nothing
        is_outer = "LogicalLeftJoin" in section
        # the decline is REPORTED rather than silent (a decision was available and
        # was refused), but deliberately not spelled `order=`: no order was chosen,
        # and that token has to keep meaning "the search ran"
        reported = "join-ordering=skipped (outer join)" in section
        if result.returncode == 0 and order is None and is_outer and reported:
            print(f"  PASS  [{label}]  skipped, reported, no order= decision")
            passed += 1
        else:
            got = (order.group(1) if order else
                   "no LogicalLeftJoin in plan" if not is_outer else
                   "decline not reported" if not reported else result.stderr.strip())
            print(f"  FAIL  [{label}]  {got}")
            failed += 1
            fail_list.append((f"decline:{label}", query, got,
                              "join-ordering=skipped and no order=, on a LogicalLeftJoin plan"))
    print(f"  {passed} passed, {failed} failed, 0 errors")
    return passed, failed, 0, fail_list


# Hand-derived from the shipped statistics (laps 10000 rows, drivers 20,
# NDV(driver_id) = 20, NDV(team) = 10) — an expectation nobody can re-derive is
# an expectation nobody can debug. `table@slot` because two relations can share a
# table name.
#   star_pivot_small:  l(199 after pushdown) x l2 estimates 199*10000/20 ~ 99500;
#     l x d estimates 199*20/20 = 199 first, reaching the same final count with an
#     intermediate 500x smaller. drivers must come SECOND.
#   nonzero_leftmost:  d x d2 on team estimates 20*20/10 = 40 rows before laps is
#     touched at all, against 10000 for l x d. A drivers relation must LEAD, which
#     puts a relation other than slot 0 at the bottom of the spine.
#   selective_filter_keeps_written: lap_id < 30 pushes laps to ~29 rows, below
#     drivers' 20+20 — so the written order is now the cheap one and is kept.
# Week 30 (binder scope resolution). These shapes were REFUSED on every path
# before this week — `Error: unknown table qualifier: 'drivers'` — because
# re-binding an already-bound alias clone took the qualified path against a range
# table keyed on ref names. They return rows now, so they belong in the oracle
# suite AND in the optimizer invariant: both are joins, and the alias
# substitution feeds the ORDER BY / GROUP BY that the optimizer reshapes under.
WEEK30_QUERIES = [
    ("w30_order_by_unqualified_select_alias",
     "SELECT name AS n FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "ORDER BY n LIMIT 10"),
    ("w30_group_by_unqualified_select_alias",
     "SELECT name AS n, COUNT(*) AS c FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id GROUP BY n ORDER BY n LIMIT 10"),
    ("w30_group_by_alias_beside_an_aggregate_alias",
     "SELECT nationality AS nat, AVG(speed) AS a FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id GROUP BY nat ORDER BY a DESC, nat LIMIT 5"),
]

# Week 31: uncorrelated subqueries execute by materialization — the body runs
# once, before planning, and the node becomes a constant.
#
# These are here for the OPTIMIZER INVARIANT specifically (optimized ==
# --no-optimize), which is the property the substitution could break in a way the
# SQLite oracle would not localize: the pass runs above the optimizer, and the
# nested query is planned by the same passes in the same order as a top-level
# one, honouring --no-optimize. If the runner ignored that flag, both legs would
# share one optimized sub-result and this suite would silently stop testing the
# sub-plan. Their answers are diffed against SQLite in compare_against_sqlite.py.
WEEK31_QUERIES = [
    ("w31_scalar_in_where",
     "SELECT COUNT(*) FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)"),
    ("w31_scalar_in_having",
     "SELECT team, AVG(speed) AS a FROM laps GROUP BY team "
     "HAVING AVG(speed) > (SELECT AVG(speed) FROM laps) ORDER BY team"),
    ("w31_scalar_under_arithmetic",
     "SELECT COUNT(*) FROM laps WHERE speed > 0.5 * (SELECT AVG(speed) FROM laps)"),
    ("w31_scalar_empty_is_null",
     "SELECT COUNT(*) FROM laps WHERE speed > (SELECT speed FROM laps WHERE lap_id = -1)"),
    ("w31_exists",
     "SELECT COUNT(*) FROM drivers WHERE EXISTS (SELECT * FROM laps WHERE speed > 340)"),
    ("w31_not_exists_empty",
     "SELECT COUNT(*) FROM drivers WHERE NOT EXISTS (SELECT * FROM laps WHERE speed > 99999)"),
    ("w31_in_subquery",
     "SELECT name FROM drivers WHERE driver_id IN (SELECT driver_id FROM laps) ORDER BY name"),
    ("w31_not_in_subquery",
     "SELECT name FROM drivers WHERE driver_id NOT IN "
     "(SELECT driver_id FROM laps WHERE speed > 340) ORDER BY name"),
    # a NULL in the materialized set: NOT IN is never TRUE
    ("w31_not_in_null_bearing_set",
     "SELECT COUNT(*) FROM drivers WHERE driver_id NOT IN "
     "(SELECT l.driver_id FROM drivers d LEFT JOIN laps l "
     " ON d.driver_id = l.driver_id AND l.speed > 99999)"),
    # the outer query still joins, so pushdown, enumeration and the restored
    # projection narrowing all run above a materialized constant
    ("w31_subquery_over_a_join",
     "SELECT l.team, COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.speed > (SELECT AVG(speed) FROM laps) GROUP BY l.team ORDER BY l.team"),
    # the body itself joins three relations: vectorized-only, which is the mode
    # this suite runs in
    ("w31_multi_relation_body",
     "SELECT COUNT(*) FROM laps WHERE speed > "
     "(SELECT AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     " JOIN drivers d2 ON d.driver_id = d2.driver_id)"),
]

WEEK28_EXPECTED_ORDER = {
    "w28_star_pivot_small": "laps@0,drivers@2,laps@1",
    "w28_star_written_good": "laps@0,drivers@1,laps@2",
    "w28_nonzero_leftmost": "drivers@1,drivers@2,laps@0",
    "w28_nonzero_leftmost_projection": "drivers@1,drivers@2,laps@0",
    "w28_triangle": "drivers@1,drivers@2,laps@0",
    "w28_residual_and_pushed_where": "drivers@1,drivers@2,laps@0",
    "w28_aggregate_over_reordered": "drivers@1,drivers@2,laps@0",
    "w28_selective_filter_keeps_written": "laps@0,drivers@1,drivers@2",
    "w28_four_way": "laps@0,drivers@1,drivers@2,laps@3",
}

# The same join graph written two ways. Under enumeration both must execute
# identically — join order is CHOSEN, not inherited from how the user typed it.
WEEK28_ORDER_EQUIVALENT_PAIRS = [
    ("w28_star_pivot_small", "w28_star_written_good"),
]


def run_join_order_steering(queries):
    """Week 28: assert each query's chosen join order matches the hand-derived
    expectation, read from --explain's Optimized Logical Plan section. Mirrors
    run_join_steering (Week 23.5): a cost decision is result-invariant, so
    --explain is the only surface it can be asserted on (invariant 13)."""
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    passed, failed, errors = 0, 0, 0
    fail_list = []
    print(f"\n--- Week 28 join-order steering (--explain) ---")
    for label, query in queries:
        expected = WEEK28_EXPECTED_ORDER[label]
        args = [SWIFTQL_BIN, "--catalog", CATALOG_PATH, "--no-cache",
                *VEC, "--explain", "--query", query]
        result = subprocess.run(args, capture_output=True, text=True)
        section = result.stdout.split("=== Optimized Logical Plan ===")[-1] \
                              .split("=== Physical Plan ===")[0]
        match = re.search(r"order=(\S+)", section)
        chosen = match.group(1) if match else "no order= decision"
        # The written order is always legal and always inside the search space, so
        # a chosen cost above it means the search installed a plan its own model
        # scores worse -- which no result test can see, since reordering never
        # changes rows. Asserted on every steering query rather than on one, since
        # the trigger (a sub-1-row intermediate) is not a property of the shape.
        cw = re.search(r"cost=([\d.]+) \(written=([\d.]+)\)", section)
        # Week 29: `cost <= written` holds BY CONSTRUCTION -- reorder() clamps
        # chosen_cost = min(chosen_cost, written_cost) two statements before it
        # builds this string -- so on its own it can never fail. Reintroduce the
        # Week 28 floor defect and the DP again returns an order costed 666 against
        # the written 629; reorder() silently downgrades to method=written-floor,
        # prints cost=629 (written=629), and the old assertion still passed while
        # the optimizer had stopped optimizing this shape. `method=dp` is the
        # statement with content: the search did not need the bound. Both shipped
        # tables carry full statistics, so the DP is exact on every steering query.
        m = re.search(r"method=(\w+)", section)
        cost_ok = (cw is not None and float(cw.group(1)) <= float(cw.group(2))
                   and m is not None and m.group(1) == "dp")
        if result.returncode == 0 and chosen == expected and cost_ok:
            print(f"  PASS  [{label}]  order {chosen}")
            passed += 1
        elif result.returncode == 0 and chosen == expected:
            got = (f"cost={cw.group(1)} > written={cw.group(2)}" if cw and
                   float(cw.group(1)) > float(cw.group(2))
                   else f"method={m.group(1)}" if m else "no cost= pair")
            print(f"  FAIL  [{label}]  order {chosen} but {got}")
            failed += 1
            fail_list.append((f"order:{label}", query, got, "cost <= written and method=dp"))
        else:
            print(f"  FAIL  [{label}]  expected {expected}, chose {chosen}")
            failed += 1
            fail_list.append((f"order:{label}", query, chosen, expected))
    print(f"  {passed} passed, {failed} failed, {errors} errors")
    return passed, failed, errors, fail_list


def joinRowsMaterialized(query, extra_args):
    """Total rows emitted by every join node, read from --explain-analyze. This is
    the quantity join ordering exists to minimize and the one the data-volume cost
    term models — a MEASURED number, not the belief that a plan is good."""
    args = [SWIFTQL_BIN, "--catalog", CATALOG_PATH, "--no-cache",
            *extra_args, "--explain-analyze", "--query", query]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(result.stderr.strip())
    total = 0
    for line in result.stdout.splitlines():
        if "Join" not in line:
            continue
        m = re.search(r"rows_out=(\d+)", line)
        if m:
            total += int(m.group(1))
    return total


def run_join_order_work(pairs, queries):
    """Week 28, measured rather than believed: two spellings of the SAME join
    graph must do the SAME work once the order is chosen by cost instead of
    inherited from the written order — and the chosen order must materialize
    strictly fewer join rows than the written one, which is the claim the search
    is making. The second assertion is what keeps the first from passing
    vacuously if enumeration silently became a no-op."""
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    by_label = dict(queries)
    passed, failed, errors = 0, 0, 0
    fail_list = []
    print(f"\n--- Week 28 join-order work (--explain-analyze, measured) ---")
    for a, b in pairs:
        try:
            opt_a = joinRowsMaterialized(by_label[a], VEC)
            opt_b = joinRowsMaterialized(by_label[b], VEC)
            written_a = joinRowsMaterialized(by_label[a], VEC + ["--no-optimize"])
            written_b = joinRowsMaterialized(by_label[b], VEC + ["--no-optimize"])
            same = opt_a == opt_b
            # the two spellings must diverge WITHOUT the optimizer, or the
            # equality above proves nothing about the search
            sensitive = written_a != written_b
            cheaper = opt_a < max(written_a, written_b)
            if same and sensitive and cheaper:
                print(f"  PASS  [{a} == {b}]  {opt_a} join rows "
                      f"(written: {written_a} / {written_b})")
                passed += 1
            else:
                print(f"  FAIL  [{a} == {b}]  optimized {opt_a}/{opt_b}, "
                      f"written {written_a}/{written_b}")
                failed += 1
                fail_list.append((f"order-work:{a}", by_label[a],
                                  f"opt {opt_a}/{opt_b}", f"written {written_a}/{written_b}"))
        except Exception as e:
            print(f"  ERROR [{a} == {b}]  {e}")
            errors += 1
            fail_list.append((f"order-work:{a}", by_label[a], str(e), ""))
    print(f"  {passed} passed, {failed} failed, {errors} errors")
    return passed, failed, errors, fail_list



# A ten-way self-join is inside MAX_DP_RELATIONS, so this is a configuration the
# join enumerator advertises rather than an exotic one. Its estimates exceed
# int64_t, and std::llround outside that range is undefined -- it printed
# `est=-9223372036854775808` on the checkpoint surface. Estimates are doubles
# everywhere decisions are made, so this was display only, but a negative row
# count is exactly what --explain exists to prevent.
WIDE_SELF_JOIN = "SELECT COUNT(*) FROM laps a0 " + " ".join(
    f"JOIN laps a{i} ON a0.driver_id = a{i}.driver_id" for i in range(1, 10))


def run_explain_estimate_format():
    """Week 28: no --explain estimate may render as a negative number."""
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    print(f"\n--- Week 28 estimate rendering (--explain) ---")
    args = [SWIFTQL_BIN, "--catalog", CATALOG_PATH, "--no-cache",
            *VEC, "--explain", "--query", WIDE_SELF_JOIN]
    result = subprocess.run(args, capture_output=True, text=True)
    bad = re.findall(r"est=-\d+", result.stdout)
    if result.returncode == 0 and not bad:
        print(f"  PASS  [w28_wide_self_join_estimate]  no negative est=")
        print(f"  1 passed, 0 failed, 0 errors")
        return 1, 0, 0, []
    print(f"  FAIL  [w28_wide_self_join_estimate]  {bad[:2] or result.stderr.strip()[:80]}")
    print(f"  0 passed, 1 failed, 0 errors")
    return 0, 1, 0, [("estimate-format:w28_wide_self_join_estimate",
                      WIDE_SELF_JOIN, str(bad[:2]), "no negative est=")]



# ─── Week 37 / seam pass 3 B3-1: the tie that straddles the LIMIT cut ───────
#
# WHY THIS BLOCK EXISTS. Three independent auditors found a blocker in the join
# order DP — `JoinEnumeration::rebuild` builds the merged join schema in the
# CHOSEN order, the sort tie-break in `src/execution/sort_comparator.h` compares
# the row POSITIONALLY in schema order, so the two legs impose two different
# total orders on rows the declared keys leave tied — and every gate stayed green
# through it. This block is why it can be seen.
#
# WHAT THE CLAIM "no oracle query reaches join enumeration" GOT WRONG, checked
# rather than inherited. `catalog.json` has two tables, and `reorder` declines
# below MIN_ENUMERATED_RELATIONS = 3 (join_enumeration.h:99). But a SELF-JOIN is
# three RELATIONS over two TABLES, and WEEK27/WEEK28_QUERIES are full of them:
# `--explain` on w28_nonzero_leftmost_projection prints
# `order=drivers@1,drivers@2,laps@0 cost=1692 (written=2045) method=dp`. The DP
# has been running against the oracle catalog since Week 28. A THIRD TABLE WAS
# NOT WHAT WAS MISSING and none is added — see the note at the end of this block.
#
# What was missing is the DEFECT SHAPE, which needs four things at once, all of
# which the existing 3-relation entries have three of:
#
#   (1) >= 3 relations, so the DP runs at all;
#   (2) the DP actually PICKS a different order (`order=` != written) — proved
#       per entry by run_join_order_steering's sibling assertion, and visible in
#       `--explain`;
#   (3) the sort sits DIRECTLY above that join — no GROUP BY in between, because
#       `buildAggregateSchema`'s column order is a function of the query, not of
#       the plan, and immunises everything above it;
#   (4) the ORDER BY is NOT a total order on the surviving columns, and the tie
#       is MATERIAL — the tied rows differ in some other projected column.
#
# (4) is the part a fixture has to earn, so: WHY THIS DATA PRODUCES MATERIAL
# TIES. `data/laps.csv` is 10000 rows over 20 drivers; `season` takes 4 distinct
# values and `round` about 24, so any prefix of laps has thousands of rows per
# season and hundreds per round, and those rows carry DIFFERENT lap_id, speed and
# sector values. `drivers.csv` is 20 rows over 8 teams, so `team` and
# `nationality` repeat 2-3x with different name/age beside them. Every ORDER BY
# key below is one of those columns, and every projection carries at least one
# column that differs across the tie — so which tied row survives the cut is
# OBSERVABLE, which is the whole point. A tie whose rows are identical in every
# projected column is an "immaterial tie" and cannot fail this check no matter
# how broken the comparator is.
#
# THESE ARE INVARIANT-ONLY, NOT SQLITE-ORACLE ENTRIES, and that is a real
# limitation rather than a convenience. `ORDER BY <non-unique> LIMIT n` has no
# single correct answer in SQL — SQLite picks a tied row by its own plan, SwiftQL
# picks one by the tie-break rule it declares. Diffing them would fail for a
# reason that is not a bug. What IS assertable, and is what the project asserts
# (sort_comparator.h: "The project asserts optimized == --no-optimize, so that is
# a defect even though every one of those answers is legal SQL"), is that
# SwiftQL's own answer must not depend on which passes ran. So these go to
# run_optimizer_invariant and nowhere else.
#
# Vectorized-only for the standing Week 27 reason: Volcano refuses multi-way
# joins outright, in both legs.
TIE_STRADDLE_QUERIES = [
    # ── shape 1: ORDER BY a non-unique key, LIMIT cuts inside the tie ────────
    # The canonical B3-1 repro on the oracle catalog. `l.season` ties across
    # every row of the lap_id<100 prefix that shares a season; LIMIT 5 cuts
    # inside the 2022 group; the tied rows carry different lap_id and different
    # driver names. Measured pre-fix: 4 of 5 rows differ between the legs.
    ("b31_tie_int_key_limit_cut",
     "SELECT l.lap_id, d.name, d2.name FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team "
     "WHERE l.lap_id < 100 ORDER BY l.season LIMIT 5"),

    # the same with a STRING key, so the tie-break's string path
    # (compareForTieBreak's number-before-string rule) is on the hook too, not
    # just its integer path
    ("b31_tie_string_key_limit_cut",
     "SELECT l.lap_id, d.name, d2.name FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team "
     "WHERE l.lap_id < 100 ORDER BY l.team LIMIT 5"),

    # DESC. The tie-break is documented as ALWAYS ASCENDING regardless of the
    # declared direction, so this pins that the direction of the declared key
    # does not smuggle plan-dependence back in.
    ("b31_tie_desc_limit_cut",
     "SELECT l.lap_id, d.name, d2.age FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team "
     "WHERE l.lap_id < 100 ORDER BY l.round DESC LIMIT 5"),

    # a key on the MIDDLE relation rather than the leading one: the DP moves
    # drivers@1 to the bottom of the spine, so this is the column whose position
    # in the merged schema moves furthest
    ("b31_tie_middle_relation_key",
     "SELECT l.lap_id, d.name, d2.name FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team "
     "WHERE l.lap_id < 100 ORDER BY d.nationality LIMIT 5"),

    # ── shape 1b: the ORDER-ONLY divergence, with the same rows on both sides ─
    # Two declared keys, still not total. The LIMIT does NOT straddle here: both
    # legs return the SAME SEVEN ROWS in a DIFFERENT ORDER. The pre-existing
    # sorted comparison passes this entry no matter what the comparator does;
    # only normalize_ordered() can see it. It is here to keep that half of the
    # harness honest — if someone re-sorts the invariant comparison, this entry
    # is the one that goes quiet.
    ("b31_tie_order_only_no_set_change",
     "SELECT l.lap_id, d.name, d2.name FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team "
     "WHERE l.lap_id < 80 ORDER BY l.season, d.team LIMIT 7"),

    # ── shape 3: the cut inside a SCALAR SUBQUERY — a wrong VALUE, not a
    # ── wrong order ─────────────────────────────────────────────────────────
    # This is the shape that makes the severity unarguable, and the reason it
    # works is A.3's sixth pass: materializeSubqueries EXECUTES the body and
    # threads --no-optimize into the nested runner, so the body's LIMIT 1 cut
    # lands on a different tied row per leg and the Literal it folds to differs.
    # The OUTER query has no ORDER BY and no LIMIT and nothing unspecified about
    # it — a plain COUNT(*) that comes back with two different numbers.
    ("b31_subquery_cut_becomes_a_constant",
     "SELECT COUNT(*) FROM laps WHERE lap_id > "
     "(SELECT l.lap_id FROM laps l "
     " JOIN drivers d ON l.driver_id = d.driver_id "
     " JOIN drivers d2 ON d.team = d2.team "
     " WHERE l.lap_id < 100 ORDER BY l.season LIMIT 1)"),

    # the same, reaching the third relation's column through the cut, and with
    # an aggregate pair outside so a single wrong constant moves two numbers
    ("b31_subquery_cut_third_relation_column",
     "SELECT COUNT(*), MAX(speed) FROM laps WHERE driver_id = "
     "(SELECT d2.driver_id FROM laps l "
     " JOIN drivers d ON l.driver_id = d.driver_id "
     " JOIN drivers d2 ON d.team = d2.team "
     " WHERE l.lap_id < 100 ORDER BY l.round DESC LIMIT 1)"),

    # ...and grouped, so the wrong constant redistributes rows across groups
    ("b31_subquery_cut_under_group_by",
     "SELECT team, COUNT(*) FROM laps WHERE speed > "
     "(SELECT l.speed FROM laps l "
     " JOIN drivers d ON l.driver_id = d.driver_id "
     " JOIN drivers d2 ON d.team = d2.team "
     " WHERE l.lap_id < 100 ORDER BY l.season LIMIT 1) "
     "GROUP BY team ORDER BY team"),

    # ── shape 2: a plain `LIMIT` with NO `ORDER BY` at all ───────────────────
    # There is no sort node in this plan, so the sort tie-break cannot reach it:
    # the rows come out in the top join's probe order and the DP changes which
    # relation probes. It was an OPEN defect when this block was first written
    # and was closed separately (the LIMIT now cuts a determined order), which is
    # why it sits beside the sort shapes rather than inside them — one symptom,
    # two mechanisms, and a fix to either one alone leaves the other live.
    #
    # WHY THIS JOIN KEY AND NOT `l.driver_id = l2.driver_id`. Both shapes
    # discriminate, and the obvious one is 25x MORE EXPENSIVE THAN THE WHOLE
    # REST OF THIS BLOCK: `--no-optimize` does not push the WHERE, so a
    # driver_id self-join hands the LIMIT the full 10000x10000/20 product, and
    # once the LIMIT is given a determined order that product must be ordered
    # rather than streamed — measured 47.3 s in the `--no-optimize` leg alone
    # (0.27 s before that change). `l.lap_id = l2.driver_id` matches each of the
    # 10000 probe rows to exactly ONE row, so the unfiltered join is 10000 rows
    # and the same entry costs 1.8 s. Measured on both binaries, not reasoned.
    ("b31_plain_limit_no_order_by",
     "SELECT l.lap_id, l2.speed, d.name FROM laps l "
     "JOIN laps l2 ON l.lap_id = l2.driver_id "
     "JOIN drivers d ON l2.driver_id = d.driver_id LIMIT 5"),

    # ...and the TWO-relation instance of the same shape, which arrives here
    # from the other side: `compare_against_sqlite.py`'s join-projection entry
    # was a bare `LIMIT 5` diffed against SQLite, which is a test of a
    # non-guarantee. That entry was given a total order; the unordered shape
    # belongs here, where the oracle is SwiftQL's own other leg.
    #
    # !! IT IS NOT THAT ENTRY'S TEXT, AND THE DIFFERENCE IS THE WHOLE POINT.
    # The original — `laps JOIN drivers ON driver_id` with no WHERE — CANNOT
    # DISCRIMINATE. Measured: with the LIMIT removed, so nothing sorts, the two
    # legs emit all 10000 rows in BYTE-IDENTICAL ORDER despite planning
    # different join algorithms (VecSimdLoopJoin optimized, VecHashJoin under
    # --no-optimize). Both probe `laps`, both emit probe-major in storage order,
    # so any `LIMIT n` cuts the same n rows in both legs whether or not the
    # determinism fix exists. Copied across verbatim it would have been a green
    # tick that proves nothing — the exact failure mode this block was built to
    # end.
    #
    # `WHERE l.lap_id < 15` is what makes it real: the filtered cardinality (14)
    # drops below `drivers` (20), so the optimized leg builds on the OTHER side
    # from the `--no-optimize` leg and the two probe orders genuinely differ.
    # Reconstructing the pre-fix cut exactly — pre-fix the LIMIT only truncated
    # the stream, so the first 5 rows of the unsorted output ARE what it
    # returned — the legs differ in FOUR of five rows:
    #   optimized      2022 332.72 Driver_2 | 2024 287.99 Driver_2  | 2025 305.32 …
    #   --no-optimize  2022 332.72 Driver_2 | 2022 320.38 Driver_20 | 2022 293.12 …
    # With the deterministic cut they are identical.
    ("b31_plain_limit_two_relations",
     "SELECT l.season, l.speed, d.name FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE l.lap_id < 15 LIMIT 5"),

    # ── Wave-B optimizer fixes: conjunct ordering, and pushdown into a derived
    # ── body ────────────────────────────────────────────────────────────────
    # Invariant-only by nature. Each asserts that a pass which REORDERS or MOVES
    # work did not change the answer, and the failure mode is a divergence
    # between the legs rather than a disagreement with SQLite.
    #
    # `run_optimizer_invariant` records an exception from either leg as an ERROR,
    # so the sibling shape whose correct post-fix answer is "both legs error"
    # (`SUBSTRING(team, lap_id - lap_id, 2)` with no eliminating conjunct) is NOT
    # here — it is a rejection entry in compare_against_sqlite.py, where an error
    # is the assertion rather than a failure.
    #
    # THE TOTAL-SUBSTRING CONTROL. `SUBSTRING(team, 1, 3)` is well-defined on
    # every row, so this query must keep ANSWERING `Ferrari`. A blanket refusal
    # of SUBSTRING-in-WHERE — the cheapest way to make the throwing shapes go
    # away — would still pass an invariant that only compared two legs, so the
    # entry is here to make sure the fix narrowed the rule instead of the
    # surface. Verified non-empty: 1 row, both legs.
    ("w37_substring_total_still_answers",
     "SELECT DISTINCT team FROM laps WHERE SUBSTRING(team, 1, 3) = 'Fer' "
     "AND speed > 344.5"),

    # pushdown INTO a derived body: the conjuncts must reach the inner scan and
    # the answer must not move. 7 groups, both legs.
    ("w37_pushdown_into_derived_body",
     "SELECT d.team, COUNT(*) FROM (SELECT team, speed, season FROM laps) d "
     "WHERE d.speed > 344.5 AND d.season = 2023 GROUP BY d.team"),

    # ...and into a derived body that itself contains a JOIN, where the pushed
    # conjunct has to be routed to the right side of the inner join. 82, both legs.
    ("w37_pushdown_into_derived_join_body",
     "SELECT COUNT(*) FROM (SELECT l.team AS t, l.speed AS sp FROM laps l "
     "JOIN drivers dr ON l.driver_id = dr.driver_id) d WHERE d.sp > 344.5"),

    # THE MASKING PAIR, and they are NOT vacuous despite returning zero rows.
    # `SUBSTRING(l.team, l.lap_id - l.lap_id, 2)` asks for start position 0 and
    # THROWS wherever it is evaluated — verified, it is a hard error in all four
    # modes when nothing eliminates the row first. Here `d.nationality = 'Zzz'`
    # matches nothing, so the correct behaviour is zero rows and NO error, in
    # both legs. The assertion is therefore "neither leg threw", which
    # run_optimizer_invariant enforces by recording an exception as an ERROR —
    # so a conjunct reordering that hoists the SUBSTRING ahead of the eliminating
    # predicate fails this entry loudly. Empty output is the answer, not an
    # absence of one.
    ("w37_masking_conjunct_inner_join",
     "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE d.nationality = 'Zzz' "
     "AND SUBSTRING(l.team, l.lap_id - l.lap_id, 2) = 'x'"),

    # the same over LEFT JOIN, where the null-extended rows are the ones a
    # careless pushdown would evaluate the throwing conjunct against
    ("w37_masking_conjunct_left_join",
     "SELECT l.team FROM laps l LEFT JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE d.nationality = 'Zzz' "
     "AND SUBSTRING(l.team, l.lap_id - l.lap_id, 2) = 'x'"),

    # ── CONTROLS. Without these the block proves only that SOME 3-relation
    # ── query diverges, which is not the claim. Each removes exactly one of the
    # ── four preconditions from b31_tie_int_key_limit_cut and must be SAME both
    # ── before and after any fix. Measured SAME pre-fix.
    #
    # (4) removed: the declared key list is made total, so the tie-break is never
    # reached. Same query, same data, same reordered plan.
    ("b31_control_total_order_is_immune",
     "SELECT l.lap_id, d.name, d2.name FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team "
     "WHERE l.lap_id < 100 ORDER BY l.season, l.lap_id, d.name, d2.name LIMIT 5"),

    # (3) removed: a GROUP BY between the join and the sort. buildAggregateSchema
    # emits a plan-independent column order, so everything above it is immune —
    # which is also the reason 20 of the 22 TPC-H queries cannot exhibit B3-1.
    ("b31_control_aggregate_between_is_immune",
     "SELECT d2.team, COUNT(*) FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id "
     "JOIN drivers d2 ON d.team = d2.team "
     "WHERE l.lap_id < 100 GROUP BY d2.team ORDER BY COUNT(*) LIMIT 3"),
]

# WHY NO THIRD TABLE. The brief for this work assumed the oracle catalog's two
# tables put join enumeration out of reach and asked for a third. Checked, and
# it does not: three RELATIONS is what MIN_ENUMERATED_RELATIONS counts, a
# self-join supplies them over two tables, the DP has been reordering oracle
# queries since Week 28 (`method=dp` on w28_*), and every one of the three defect
# shapes above reproduces on `catalog.json` unchanged. A third table would have
# added a catalog-wide change — new row counts feeding CardinalityEstimator, new
# join-order decisions on existing w28 entries, a new file for every harness that
# loads the catalog into SQLite — to buy coverage the fixture already had. It
# would also have been WEAKER in one respect: a self-join is the harder case for
# this defect, because the merged schema then carries several columns of the SAME
# NAME at different relation slots, which is exactly the confusion a positional
# tie-break trips over.


# The `LIMIT` with no `ORDER BY` shape was carried here for one round as an
# EXPECTED-DIVERGENCE entry, because no comparator fix could reach it and this
# harness is not the place to decide an open question. It was closed while this
# block was being written (the LIMIT is now given a determined order), the
# expected-divergence check FAILED with "LEGS NOW AGREE — promote this entry",
# which is what it was built to do, and the entry moved into
# TIE_STRADDLE_QUERIES as `b31_plain_limit_no_order_by`. The mechanism is gone
# with it rather than left standing empty.


def main():
    conn = load_sqlite()
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    all_queries = QUERIES + WEEK21_QUERIES + WEEK22_QUERIES + WEEK23_5_QUERIES + AUDIT_FIXES_QUERIES + WEEK24_QUERIES + WEEK27_QUERIES + WEEK28_QUERIES + WEEK29_QUERIES + WEEK30_QUERIES + WEEK31_QUERIES

    # existing surface on the default row/Volcano path (audit fixes and
    # Week 24 expressions affected both engines, so they run here too)
    r1 = run_suite(conn, QUERIES + AUDIT_FIXES_QUERIES + WEEK24_QUERIES, "Default (row storage, Volcano)")
    # whole surface + Week 21 shapes on the vectorized path (where the optimizer runs)
    r2 = run_suite(conn, all_queries, "Vectorized (columnar, optimizer ON)", extra_args=VEC)
    # Week 21 result-preserving invariant. TIE_STRADDLE_QUERIES joins HERE and
    # not in r2: `ORDER BY <non-unique> LIMIT n` has no single correct answer, so
    # SQLite is not an oracle for it — see the block's header.
    r3 = run_optimizer_invariant(all_queries + TIE_STRADDLE_QUERIES)
    # Week 23.5 plan-shape steering
    r4 = run_join_steering(WEEK23_5_QUERIES)
    # Week 28 join-order steering: the chosen order, from --explain
    r5 = run_join_order_steering(WEEK28_QUERIES)
    # Week 28 join-order work: the same graph written two ways must execute
    # identically, measured from --explain-analyze rather than assumed
    r6 = run_join_order_work(WEEK28_ORDER_EQUIVALENT_PAIRS, WEEK28_QUERIES)
    # Week 28 estimate rendering: a wide self-join overflows int64_t
    r7 = run_explain_estimate_format()
    # Week 29: an outer join in the tree means enumeration declined, so no
    # order= line may be printed — a decision that never happened
    r8 = run_outer_join_decline(WEEK29_NO_ORDER_DECISION)

    passed = r1[0] + r2[0] + r3[0] + r4[0] + r5[0] + r6[0] + r7[0] + r8[0]
    failed = r1[1] + r2[1] + r3[1] + r4[1] + r5[1] + r6[1] + r7[1] + r8[1]
    errors = r1[2] + r2[2] + r3[2] + r4[2] + r5[2] + r6[2] + r7[2] + r8[2]
    fail_list = r1[3] + r2[3] + r3[3] + r4[3] + r5[3] + r6[3] + r7[3] + r8[3]

    print(f"\n{'='*70}")
    print(f"{passed} passed, {failed} failed, {errors} errors "
          f"({len(QUERIES) + len(AUDIT_FIXES_QUERIES) + len(WEEK24_QUERIES)} default + {len(all_queries)} vectorized "
          f"+ {len(all_queries) + len(TIE_STRADDLE_QUERIES)} invariant (of which "
          f"{len(TIE_STRADDLE_QUERIES)} tie-straddle) "
          f"+ {len(WEEK23_5_QUERIES)} algorithm steering + {len(WEEK28_QUERIES)} order steering "
          f"+ {len(WEEK28_ORDER_EQUIVALENT_PAIRS)} order work + 1 estimate rendering "
          f"+ {len(WEEK29_NO_ORDER_DECISION)} outer-join decline)")

    if fail_list:
        print(f"\n{'='*70}")
        print("FAILING QUERIES:")
        for label, query, actual, expected in fail_list:
            print(f"\n  [{label}]")
            print(f"  Query:    {query}")
            if isinstance(actual, str):
                print(f"  Error:    {actual}")
            else:
                print(f"  SwiftQL:  {actual}")
                print(f"  Expected: {expected}")

    return 1 if (failed > 0 or errors > 0) else 0


if __name__ == "__main__":
    sys.exit(main())
