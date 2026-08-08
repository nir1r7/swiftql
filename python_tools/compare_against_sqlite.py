#!/usr/bin/env python3
"""
compare_against_sqlite.py - runs queries against SwiftQL and SQLite, diffs results.
Usage: python3 compare_against_sqlite.py
"""

import subprocess
import sqlite3
import csv
import json
import math
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

    # !! Week 37. Folding in the ORDER BY / GROUP BY positions, which is where it
    # used to change the OUTCOME rather than the value: the folded node is a
    # Literal, the Validator's column-ordinal rule tested Literal-ness, and every
    # one of these was REFUSED as an ordinal the user had not typed
    # ("ORDER BY 1 + 1" -> "ORDER BY 2: column ordinals are not supported").
    # SQLite accepts all of them, and BINARY ARITHMETIC is exactly the boundary:
    # to SQLite an ordinal is an integer literal under any number of parens and
    # unary signs, so `(1)` and `- -1` ARE ordinal 1 and stay refused (they are
    # in WEEK37_COLUMN_ORDINAL_REFUSED), while `1 + 1` is a constant expression
    # on which every row ties. This distinction is only visible on data whose
    # insert order, column-1 order and column-2 order all differ; a first pass
    # at this suite put `(1)` and `- -1` here and the diff caught it.
    #
    # The diffed suite could not hold any of these until now, because it cannot
    # hold a query that errors. Each carries a unique tie-break so a constant
    # primary sort key still leaves a total order to compare in emitted order.
    "SELECT team, speed FROM laps ORDER BY 1 + 1, lap_id LIMIT 10",
    "SELECT team, speed FROM laps ORDER BY 2 * 1, lap_id LIMIT 10",
    "SELECT team, speed FROM laps ORDER BY 1 + 0, lap_id LIMIT 10",
    "SELECT COUNT(*) AS c FROM laps GROUP BY 1 + 1",
    # ...and the same manufactured Literal from the OTHER rewrite that runs
    # before the Validator: the binder substitutes a clone of the select-list
    # expression for an ORDER BY alias, so `one` became Literal(1) and the query
    # was refused as "ORDER BY 1". Nothing was folded here at all.
    "SELECT 1 AS one, team FROM laps ORDER BY one, lap_id LIMIT 10",
]

# The other half of Week 37, and the half that keeps the guard honest: what an
# ordinal REFUSAL still is, now that the rule tests the parser's record of what
# the user wrote instead of Literal-ness of the tree. Pinned here because the
# diffed oracle cannot hold a query that errors, and because narrowing a guard
# without pinning what survives is how a guard gets deleted by accident.
#
# The message must quote the text AS TYPED. That is the concrete defect the
# audit found: with the rule reading a folded tree, `ORDER BY 1 + 1` was refused
# as "ORDER BY 2", an ordinal that appears nowhere in the query.
#
# `-1` and `0` are ordinals to SQLite too — it answers "1st ORDER BY term out of
# range" rather than sorting by a constant — so refusing them is agreement, and
# they cannot go in the diffed suite because SQLite errors on them as well.
WEEK37_COLUMN_ORDINAL_REFUSED = [
    ("SELECT team, speed FROM laps ORDER BY 1 LIMIT 10",
     "ORDER BY 1: column ordinals are not supported"),
    ("SELECT team, speed FROM laps ORDER BY 2 LIMIT 10",
     "ORDER BY 2: column ordinals are not supported"),
    # not the first ORDER BY item: the two parse sites are separate code
    ("SELECT team, speed FROM laps ORDER BY team, 2 LIMIT 10",
     "ORDER BY 2: column ordinals are not supported"),
    ("SELECT team, speed FROM laps ORDER BY -1 LIMIT 10",
     "ORDER BY -1: column ordinals are not supported"),
    ("SELECT team, speed FROM laps ORDER BY 0 LIMIT 10",
     "ORDER BY 0: column ordinals are not supported"),
    ("SELECT team, COUNT(*) FROM laps GROUP BY 1",
     "GROUP BY 1: column ordinals are not supported"),
    ("SELECT COUNT(*) FROM laps GROUP BY -1",
     "GROUP BY -1: column ordinals are not supported"),
    # Parentheses and unary signs are TRANSPARENT to SQLite's ordinal rule, so
    # these are ordinal 1 / ordinal 2 there and belong on this side of the line,
    # not in the diffed suite. The message reports the ordinal the term denotes,
    # which is what SQLite itself resolves it to.
    ("SELECT team, speed FROM laps ORDER BY (1) LIMIT 10",
     "ORDER BY 1: column ordinals are not supported"),
    ("SELECT team, speed FROM laps ORDER BY - -1 LIMIT 10",
     "ORDER BY 1: column ordinals are not supported"),
    ("SELECT team, speed FROM laps ORDER BY ((2)) LIMIT 10",
     "ORDER BY 2: column ordinals are not supported"),
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

# SQL that must be REFUSED in every mode rather than answered. Week 26 filled
# this list with shapes that were merely unimplemented; Week 27 executes those,
# so what remains is SQL SwiftQL genuinely does not support (an ON clause with no
# equi-join key), plus binder and type faults that must outrank everything else.
# Every entry must fail with its own message rather than return a wrong answer,
# which is the property the whole dialect's error handling exists to protect.
# Checked in all four modes: the refusals are raised from different places on the
# Volcano and vectorized paths.
#
# Each entry: (query, required substring of the error).
WEEK26_REJECTED_QUERIES = [
    # An ON clause that yields NO equi-join key is a cross product with a filter
    # on top, and SwiftQL has no cross-product operator. Week 26 refused these
    # for a narrower reason ("non-equality" / "AND-chain"); Week 27 makes the
    # non-equality conjunct legal ALONGSIDE a key, so what is left to refuse is
    # exactly the missing key. SQLite answers all three — the property under test
    # is a clean refusal rather than a cartesian product nobody asked for.
    # Week 29: a join key comparing a STRING column with a numeric one. The key is
    # compared as TEXT, which carries no type tag, so "7" matched the INT 7 while
    # "007" did not — half a match, while the identical predicate in a WHERE clause
    # throws. SQLite answers all of these (its affinity rules convert), so the
    # property under test is again SwiftQL's own refusal rather than a half-answer.
    # Under an outer join the unmatched half comes back null-extended, which is why
    # this is closed now.
    ("SELECT l.team FROM laps l JOIN drivers d ON d.team = l.lap_id",
     "cannot join a STRING column with a numeric one"),
    ("SELECT l.team FROM laps l LEFT JOIN drivers d ON l.speed = d.name",
     "cannot join a STRING column with a numeric one"),
    ("SELECT l.team FROM laps l JOIN drivers d ON l.driver_id < d.driver_id",
     "at least one equality"),
    ("SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = 5",
     "at least one equality"),
    # an OR is one indivisible conjunct, so it contributes no key even though it
    # contains equalities
    ("SELECT l.team FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id OR l.team = d.team",
     "at least one equality"),

    # a genuine query defect still outranks everything else, in every mode: the
    # plan-time type checks that used to precede the multi-key refusal must keep
    # firing now that the refusal is gone
    ("SELECT l.team FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id AND l.team = d.team WHERE l.team + 1 > 0",
     "requires numeric operands"),

    # ...including a fault in the SELECT list, which buildProjectSchema
    # type-checks last
    ("SELECT l.team + 1 FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id AND l.team = d.team",
     "requires numeric operands"),

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

# Week 30 (binder scope resolution). Both of these returned
# `Error: unknown table qualifier: 'drivers'` on every path, at two relations:
# Binder::resolveColumnRef rewrites an unqualified ref's table_name to the TABLE
# name while the range table is keyed on the REF name, so re-binding an
# already-bound clone — which is what GROUP BY <alias> and ORDER BY <alias> both
# do — took the qualified path and looked for a relation called `drivers` among
# `l` and `d`. Binding is idempotent now. SQLite answers both, so the rows are
# the oracle; the fix must also NOT rename any output column, which is pinned on
# the C++ side by BinderTest.AliasFixDoesNotMoveAggregateOutputNames (the harness
# normalizes rows through a dict keyed by column name and would not notice).
WEEK30_ALIAS_REBIND_QUERIES = [
    "SELECT name AS n FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "ORDER BY n LIMIT 10",
    "SELECT name AS n, COUNT(*) AS c FROM laps l JOIN drivers d "
    "ON l.driver_id = d.driver_id GROUP BY n ORDER BY n LIMIT 10",
    # the same rebind through a GROUP BY alias beside an aggregate alias in
    # ORDER BY. `nationality` and `speed` each live in exactly one relation, so
    # the unqualified references are legal — `team` would be ambiguous
    "SELECT nationality AS nat, AVG(speed) AS a FROM laps l JOIN drivers d "
    "ON l.driver_id = d.driver_id GROUP BY nat ORDER BY a DESC, nat LIMIT 5",
]

# Week 30 (subquery parsing + binding), SPLIT IN WEEK 31.
#
# Week 30 ended in one refusal and this was a rejection suite: reaching the
# refusal was the assertion, because nothing returned rows to diff. Week 31
# executes the UNCORRELATED forms, so the same queries split three ways and each
# part now carries a different kind of evidence:
#
#   WEEK31_SUBQUERY_QUERIES  — uncorrelated, diffed against SQLite in all four
#                              modes. The first subquery ROWS this project has
#                              produced, and the only oracle for the NULL rules.
#   WEEK31_SUBQUERY_VEC_ONLY — uncorrelated with a 3+ relation body: diffed in
#                              the two vectorized modes, refused in the two
#                              Volcano ones. An ordinary capability difference,
#                              identical in shape to MULTIWAY_QUERIES.
#   WEEK33_CORRELATED_BINDS  — correlated: still a rejection suite, now naming
#                              Week 33. Every scope-resolution property Week 30
#                              bought is still asserted by reaching that refusal.
#
# Splitting rather than deleting is the point: the correlated half keeps proving
# that nested scope resolution, correlation detection and per-scope validation
# all still happen, and the uncorrelated half proves the answers are right.

# Uncorrelated subqueries EXECUTE as of Week 31: materialized once, before
# planning, and substituted as a constant. Diffed in all four modes, which is
# what proves the pass sits above both engines rather than being reimplemented
# in each.
WEEK31_SUBQUERY_QUERIES = [
    # --- scalar (TPC-H Q22's uncorrelated half, Q11's HAVING shape) ---
    "SELECT COUNT(*) FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)",
    "SELECT team, AVG(speed) FROM laps GROUP BY team "
    "HAVING AVG(speed) > (SELECT AVG(speed) FROM laps) ORDER BY team",
    # the scalar composes with arithmetic, which is why it lives at the primary
    # level — and re-folding must put the predicate back into the
    # ColumnRef-op-Literal shape three fast paths pattern-match on
    "SELECT COUNT(*) FROM laps WHERE speed > 0.5 * (SELECT AVG(speed) FROM laps)",
    # a subquery over a table the outer query never names: before Week 31 the
    # loader walked from_table + joins only and this died in std::out_of_range
    "SELECT COUNT(*) FROM laps WHERE speed > (SELECT AVG(age) FROM drivers)",
    # SEAM AUDIT pass 2, B-1's fourth lowering. `SELECT *` beside a MATERIALIZED
    # subquery: the node is replaced by a literal and no relation is added, so
    # the star's domain is untouched and the answer is 5 columns. That was true
    # before the B-1 fix and is true after it — this entry is the immunity lock
    # for it, and it lives HERE rather than in the vectorized-only seam suite
    # because an uncorrelated scalar runs on Volcano too, which exercises the
    # SECOND copy of the star expansion (Planner::plan's). The three lowerings
    # that are vectorized-only are locked in SEAM2_STAR_OVER_LOWERED_SUBQUERY.
    # Partial row set (8 of 20) on purpose: an all-rows or no-rows entry
    # compares few or no columns.
    "SELECT * FROM drivers d WHERE d.age > (SELECT AVG(l.speed) / 10 FROM laps l) "
    "ORDER BY d.driver_id",

    # --- the NULL cases, which are the ones a materialization gets wrong ---
    # ZERO ROWS: the scalar is NULL, the comparison UNKNOWN, the answer no rows.
    # It is also the first constant NULL this engine has ever had, so it is the
    # test that inferExprType answers from Literal::null_type instead of throwing
    "SELECT COUNT(*) FROM laps WHERE speed > (SELECT speed FROM laps WHERE lap_id = -1)",
    # ONE NULL ROW, which is a DIFFERENT result set with the same answer: an
    # aggregate over an empty selection returns one row holding NULL
    "SELECT COUNT(*) FROM laps WHERE speed > (SELECT AVG(speed) FROM laps WHERE season = 9999)",
    # a scalar whose value is NULL rather than absent, from arithmetic (x/0)
    "SELECT COUNT(*) FROM laps WHERE speed > (SELECT speed / 0 FROM laps LIMIT 1)",
    # an empty INNER RELATION on the EXISTS and IN paths
    "SELECT COUNT(*) FROM drivers WHERE EXISTS (SELECT * FROM laps WHERE speed > 99999)",

    # --- EXISTS / NOT EXISTS, both truth values ---
    "SELECT COUNT(*) FROM drivers WHERE EXISTS (SELECT * FROM laps WHERE speed > 340)",
    "SELECT COUNT(*) FROM drivers WHERE NOT EXISTS (SELECT * FROM laps WHERE speed > 99999)",
    "SELECT COUNT(*) FROM drivers WHERE age > 30 AND NOT EXISTS "
    "(SELECT * FROM laps WHERE speed > 99999)",


    # --- the walker (dispatch site 19) must reach a subquery inside a container ---
    "SELECT COUNT(*) FROM laps WHERE "
    "CASE WHEN speed > (SELECT AVG(speed) FROM laps) THEN 1 ELSE 0 END = 1",
    # BETWEEN clones its left operand before binding and cloneExpr SHARES the
    # statement, so this is TWO SubqueryExpr nodes over ONE SelectStatement: one
    # run, two substitutions
    "SELECT COUNT(*) FROM laps WHERE (SELECT MAX(age) FROM drivers) BETWEEN 1 AND 99",

    # --- scope resolution, kept from Week 30 and now returning rows ---
    # an inner alias shadowing an outer one: the inner block wins, and that is
    # not an ambiguity
    "SELECT COUNT(*) FROM laps x WHERE EXISTS (SELECT * FROM drivers x WHERE x.age > 30)",
    # an unqualified name present in BOTH blocks resolves to the inner one
    "SELECT COUNT(*) FROM laps l WHERE EXISTS "
    "(SELECT * FROM drivers d WHERE team = 'Ferrari')",
    # a local expression group key inside the body must still satisfy its own
    # select item
    "SELECT COUNT(*) FROM laps l WHERE EXISTS "
    "(SELECT driver_id + 1 FROM drivers d GROUP BY driver_id + 1)",
    # a JOIN in the outer query, so pushdown, join enumeration and the restored
    # projection narrowing all run above a materialized constant
    "SELECT l.team, COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "WHERE l.speed > (SELECT AVG(speed) FROM laps) GROUP BY l.team ORDER BY l.team",
]

# A three-relation body executes wherever a three-relation query does, which is
# the vectorized path only (Week 27). The nested query runs on the SAME engine as
# the query containing it, so this is the pre-existing capability difference
# rather than a new one — and asserting both halves is what keeps it deliberate.
# Week 32 — SET-MEMBERSHIP LOWERING, diffed in the VECTORIZED modes only.
#
# An uncorrelated `IN (subquery)` is no longer materialized into a literal list:
# it is lowered to a hash SEMI join (`NOT IN` to an ANTI join), which Volcano
# cannot hold — Planner::plan builds exactly one HashJoinNode out of stmt.joins
# and a semi-join is a second one. That refusal is pinned below; these are the
# queries whose ANSWERS are compared, and they are compared in two modes rather
# than four. Every one of them was diffed in four modes in Week 31, so the loss
# is real and is recorded here rather than implied by an absence.
WEEK32_SEMI_JOIN_VEC_ONLY = [
    # --- the checkpoint query, and the one Week 31 could NOT answer at all ---
    # 10000 distinct values, i.e. 1e8 Value comparisons under the old linear
    # InExpr scan, which is why Week 31 capped it at 1024 and pinned the REFUSAL
    # instead of the answer. The cap is gone because nothing is materialized, so
    # the query MOVES here — a rejection entry deleted without a diff entry
    # added would quietly reduce coverage exactly where it matters most.
    "SELECT COUNT(*) FROM laps WHERE lap_id IN (SELECT lap_id FROM laps)",

    # --- IN / NOT IN (Q16/Q18's shape) ---
    "SELECT name FROM drivers WHERE driver_id IN (SELECT driver_id FROM laps) ORDER BY name",
    "SELECT name FROM drivers WHERE driver_id NOT IN "
    "(SELECT driver_id FROM laps WHERE speed > 340) ORDER BY name",

    # --- DUPLICATE BUILD KEYS: the single most valuable query in the week ---
    # `team` has 20 rows over a handful of distinct values on the build side. An
    # inner join with a projection on top would emit each outer row once PER
    # match; a semi-join emits it once. The two answers differ by a factor of
    # five, and only SQLite says which is right.
    "SELECT COUNT(*) FROM laps WHERE team IN (SELECT team FROM drivers)",
    "SELECT team, COUNT(*) FROM laps WHERE team IN (SELECT team FROM drivers) "
    "GROUP BY team ORDER BY team",

    # --- EMPTY BUILD SIDE, both polarities ---
    #   x IN ()     is FALSE for every x       -> no rows
    #   x NOT IN () is TRUE for every x        -> every row
    # Both probe drivers.driver_id, which is never NULL over the shipped data —
    # so these two do NOT cover the "NULL x included" asymmetry. That needs a
    # NULL probe key AND an empty build at once, which is the third query.
    "SELECT COUNT(*) FROM drivers WHERE driver_id IN "
    "(SELECT driver_id FROM laps WHERE speed > 99999)",
    "SELECT COUNT(*) FROM drivers WHERE driver_id NOT IN "
    "(SELECT driver_id FROM laps WHERE speed > 99999)",
    # `NULL NOT IN ()` is TRUE — the one asymmetry in the whole NULL table, and
    # the branch VecHashJoinNode reaches by testing build_keys_.empty() INSIDE
    # the failed-key path. Held only by a unit test until now.
    "SELECT COUNT(*) FROM drivers d LEFT JOIN laps l "
    "ON d.driver_id = l.driver_id AND l.speed > 99999 "
    "WHERE l.driver_id NOT IN (SELECT driver_id FROM laps WHERE speed > 99999)",

    # --- THREE-VALUED IN: a NULL on the BUILD side ---
    # The sharpest correctness item in the week, and the one the shipped CSVs
    # cannot express — ColumnarTable cannot hold a NULL, so a LEFT JOIN inside
    # the body is how one is constructed (invariant 14). The rules differ:
    #   NOT IN over a set containing a NULL is NEVER TRUE -> no rows at all,
    #     which is why the build phase carries build_had_null_key_ out as a flag
    #     and the ANTI probe short-circuits on it;
    #   IN over the same set keeps the non-null matches, because a NULL simply
    #     never matches.
    "SELECT COUNT(*) FROM drivers WHERE driver_id NOT IN "
    "(SELECT l.driver_id FROM drivers d LEFT JOIN laps l "
    " ON d.driver_id = l.driver_id AND l.speed > 99999)",
    "SELECT COUNT(*) FROM drivers WHERE driver_id IN "
    "(SELECT l.driver_id FROM drivers d LEFT JOIN laps l "
    " ON d.driver_id = l.driver_id AND l.driver_id < 5)",

    # --- a NULL on the PROBE side ---
    # `NULL IN S` and `NULL NOT IN S` are both UNKNOWN for a non-empty S, and a
    # WHERE keeps neither. The ANTI leg is the discriminating one — a naive
    # "no match, so emit" gives every row instead of none. The SEMI leg answers
    # 0 whichever way a plausible implementation gets there, so it is a
    # consistency check rather than a trap; kept as the pair's other half.
    "SELECT COUNT(*) FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
    "AND l.speed > 99999 WHERE l.driver_id IN (SELECT driver_id FROM laps)",
    "SELECT COUNT(*) FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
    "AND l.speed > 99999 WHERE l.driver_id NOT IN (SELECT driver_id FROM laps)",

    # --- the semi-join sits ABOVE a JOIN SPINE, so the operand's binder slot is
    # a real slot rather than 0, and pushdown/enumeration both meet the node ---
    "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "WHERE l.season IN (SELECT season FROM laps l2)",
    "SELECT COUNT(*) FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
    "WHERE d.age IN (SELECT season FROM laps l2)",

    # --- other conjuncts survive in the WHERE filter and still push to the scan ---
    "SELECT COUNT(*) FROM laps WHERE speed > 300 AND driver_id IN "
    "(SELECT driver_id FROM drivers WHERE age > 30) AND season = 2024",
    # two IN conjuncts -> two stacked semi-joins
    "SELECT COUNT(*) FROM laps WHERE driver_id IN (SELECT driver_id FROM drivers) "
    "AND season IN (SELECT season FROM laps l2 WHERE l2.speed > 340)",
    # a GROUP BY inside the body: its keys are level 0 against the BODY's range
    # table, so buildAggregateSchema's Week 30 tripwire must NOT fire even though
    # the body is now planned by a nested builder call rather than as its own
    # top-level statement (docs/week-32-plan.md 0)
    "SELECT COUNT(*) FROM laps WHERE driver_id IN "
    "(SELECT driver_id FROM drivers GROUP BY driver_id)",
    # a SUBQUERY INSIDE THE IN BODY. Week 31 answered this shape by
    # materializing both nodes; Week 32 routes the outer one to the lowering,
    # and the body's own scalar is STILL materialization's business. The first
    # cut returned from the walker before the "innermost first" recursion, so
    # this query died at dispatch site 12 with an internal-invariant message
    # while the suite stayed green — a capability regressing silently is exactly
    # what the oracle is for, so the shape is pinned here as well as in
    # tests/test_subquery.cc.
    "SELECT COUNT(*) FROM drivers WHERE driver_id IN "
    "(SELECT driver_id FROM laps WHERE speed > (SELECT AVG(speed) FROM laps))",
    # ...and the same shape as a ROW SET rather than a scalar. COUNT(*) alone
    # compares one number, so any defect that preserves cardinality while
    # returning the WRONG drivers — a nested threshold off by one, or the semi
    # join matching on the wrong column — diffs identically against SQLite.
    # Naming the rows and ordering them closes that at no cost.
    "SELECT name FROM drivers WHERE driver_id IN "
    "(SELECT driver_id FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)) "
    "ORDER BY name",
    "SELECT name FROM drivers WHERE driver_id IN "
    "(SELECT driver_id FROM drivers WHERE age > 30) ORDER BY name",
]

# The four Volcano CAPABILITY refusals, each named by the guard that raises it.
#
# Every Volcano rejection suite used to pin the shared tail, "not supported on
# the Volcano path". Four structurally different guards in Planner::plan end with
# that phrase, so the needle could not say WHICH one fired: a suite asserting the
# IN refusal passed unchanged when the multi-way guard caught the query first,
# and would keep passing if the IN guard were deleted outright. That is the same
# "passes regardless of whether the feature works" class as the two-key oracle
# entry above, and it covered 40 entries.
#
# It was not hypothetical. Pinning the specific message showed one entry of
# WEEK34_DERIVED_TABLE_VOLCANO_REJECTED never reaches the derived-table guard at
# all (its outer query joins twice, so multi-way fires first) and three of
# WEEK34_CORRELATED_SCALAR_VOLCANO_REJECTED reach the IN guard rather than the
# correlated one. Both are correct behaviour and both were invisible.
#
# The ORDER these fire in is planner.cc's, top to bottom: multi-way, then IN,
# then correlated, then derived. Where a suite's entries split across guards, the
# split below is written as that ordering rule, not as a list of indices, so an
# entry added later is classified rather than mislabelled.
VOLCANO_MULTIWAY   = "multi-way joins are not supported on the Volcano path"
VOLCANO_IN         = ("IN subqueries are lowered to a semi-join and are not "
                      "supported on the Volcano path")
VOLCANO_CORRELATED = ("correlated subqueries are decorrelated to a semi-join and "
                      "are not supported on the Volcano path")
VOLCANO_DERIVED    = ("derived tables (FROM (subquery)) are not supported on the "
                      "Volcano path")

WEEK32_SEMI_JOIN_VOLCANO_REJECTED = [
    (query, VOLCANO_IN)
    for query in WEEK32_SEMI_JOIN_VEC_ONLY
]

# Shapes the lowering cannot express. Each is legal SQL that SQLite answers, so
# none can live in a diffed suite — the diffed suite cannot hold a query that
# errors — and each is a stated row in README -> Syntax Deliberately Not
# Supported. Refusing beats falling back to Week 31's materialization: two
# productions that must agree on NULL semantics is the drift this codebase has
# had to undo three times.
#
# VECTORIZED MODES ONLY, and the message is why. These three diagnostics come
# from the lowering (LogicalPlanBuilder), which the Volcano path never reaches:
# Planner::plan refuses every IN subquery up front — not for a generic reason,
# but *because it is an IN subquery* it has no second join node to hold. So on
# Volcano these queries are refused for a genuinely different, and genuinely
# correct, reason, and asserting the lowering's wording there would be asserting
# a message the engine has no business producing.
#
# The four-mode coverage is NOT dropped: the same three queries are asserted in
# the two Volcano modes via WEEK32_LOWERING_REFUSED_VOLCANO below, against the
# message that path really emits. Each query is still pinned in all four modes —
# each against the refusal that mode actually owns.
# Week 33, Task 7(1). Audit round 4 left refuseUnloweredIn's call sites in
# LogicalPlanBuilder::build as an explicit HUNCH: it never confirmed the tripwire
# runs on EVERY entry to build() rather than once at the top, which decides
# whether an IN nested inside an IN body's HAVING gets its own diagnostic or dies
# at dispatch site 12 with an internal-defect message. build() recurses (the
# lowerings plan the body through it), so the claim is directly checkable — and a
# read is not a test. This is the test.
WEEK33_NESTED_TRIPWIRE_REFUSED = [
    # This entry used to write `(SELECT 1 WHERE 1 = 1)`. SwiftQL has no FROM-less
    # SELECT, so it died at PARSE and never reached the tripwire it was written
    # to test — it asserted nothing for two rounds. Rewritten to the same claim
    # in syntax the engine has: an IN nested in the IN BODY's HAVING, whose
    # operand is an aggregate rather than a column (the other spelling below).
    ("SELECT lap_id FROM laps WHERE driver_id IN "
     "(SELECT driver_id FROM laps GROUP BY driver_id "
     " HAVING COUNT(*) IN (SELECT age FROM drivers))",
     "IN subquery"),
    ("SELECT lap_id FROM laps WHERE driver_id IN "
     "(SELECT l.driver_id FROM laps l GROUP BY l.driver_id "
     " HAVING MAX(l.speed) > 0 AND l.driver_id IN (SELECT driver_id FROM drivers))",
     "HAVING"),
]

WEEK32_LOWERING_REFUSED = [
    # A COMPUTED OPERAND. The grammar allows `additive [NOT] IN (select_stmt)`,
    # so this parses — but JoinKey holds column NAMES, and there is no
    # computed-key join in this engine. Diffed in four modes in Week 31.
    ("SELECT COUNT(*) FROM laps WHERE season * (2 + 3) IN (SELECT driver_id FROM drivers)",
     "must be a column reference"),
    # AN IN UNDER AN OR. A semi-join is a whole-conjunct construct; there is no
    # disjunctive semi-join here. No TPC-H query writes one.
    ("SELECT COUNT(*) FROM laps WHERE driver_id IN (SELECT driver_id FROM drivers) "
     "OR speed > 340",
     "whole top-level WHERE conjunct (found one in a non-top-level position)"),
    # AN IN IN HAVING. The join would have to sit above LogicalAggregate. Legal,
    # but no TPC-H query needs it (Q11's HAVING subquery is scalar), so lowering
    # only WHERE is the minimum code that solves the problem.
    # The PARENTHETICAL is part of the needle, here and above. Without it the two
    # entries pin one string that both diagnostics satisfy, so a HAVING subquery
    # collapsing into the generic non-top-level branch (or the reverse) is
    # invisible — the same "cannot say which guard fired" defect as the Volcano
    # needles above, at the one place it survives inside a single guard.
    ("SELECT team, COUNT(*) FROM laps GROUP BY team "
     "HAVING COUNT(*) IN (SELECT driver_id FROM drivers)",
     "whole top-level WHERE conjunct (found one in HAVING)"),
]

# The Volcano half of the same three queries. Derived from the list above rather
# than retyped, so a shape added there cannot quietly lose its Volcano coverage.
# Note the ordering this pins: Planner::plan's IN guard fires BEFORE any
# lowering-shape check, so an unlowerable shape is still refused as "no semi-join
# on Volcano" — the capability boundary outranks the expressibility one. If a
# later week gives Volcano a semi-join, these entries start failing, which is the
# signal to move each query to whichever refusal then applies.
WEEK32_LOWERING_REFUSED_VOLCANO = [
    (query, VOLCANO_IN)
    for query, _ in WEEK32_LOWERING_REFUSED
]

WEEK31_SUBQUERY_VEC_ONLY = [
    "SELECT COUNT(*) FROM laps WHERE speed > "
    "(SELECT AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    " JOIN drivers d2 ON d.driver_id = d2.driver_id)",
]
# VOLCANO_MULTIWAY, not VOLCANO_IN: the refusal these entries reach is the
# multi-way one, raised on the SUBQUERY BODY's three relations, which is what the
# comment above and the MULTIWAY_VOLCANO_REJECTED parallel already claim. The
# generic needle could not show that, and so could not show it if it changed.
WEEK31_SUBQUERY_VOLCANO_REJECTED = [
    (query, VOLCANO_MULTIWAY) for query in WEEK31_SUBQUERY_VEC_ONLY
]

# The two queries where materialization DIVERGES FROM SQLITE. Both are legal SQL
# that SQLite answers, so they cannot live in the diffed suite above — but a
# divergence the oracle never executes is one nobody notices going wrong. Pinning
# them here makes each a recorded decision with a message under test, in exactly
# the shape WEEK30_REJECTED_QUERIES already uses for "SQLite answers it; the
# property under test is SwiftQL's own refusal". Both are stated in README ->
# Syntax Deliberately Not Supported.
WEEK31_MATERIALIZATION_REFUSED = [
    # CARDINALITY. SQLite returns the FIRST row of a multi-row scalar subquery
    # (verified: `SELECT (SELECT a FROM t)` over 1,2,3 gives 1); SwiftQL raises,
    # which is what standard SQL requires and what a materialized constant can
    # honestly represent — there is no "first row" without an ORDER BY, so
    # SQLite's answer depends on a scan order this engine is free to change.
    ("SELECT COUNT(*) FROM laps WHERE speed > (SELECT speed FROM laps)",
     "scalar subquery returned more than one row"),
    # THE IN CAP entry lived here until Week 32 and was MOVED, not deleted:
    # `lap_id IN (SELECT lap_id FROM laps)` is answerable now that nothing is
    # materialized, so it is diffed against SQLite in WEEK32_SEMI_JOIN_VEC_ONLY
    # above. Deleting it outright would have removed the one query that best
    # demonstrates the week's checkpoint at the moment it finally has an answer.
]

# Correlated subqueries are Week 33: their value depends on the outer row, so
# there is no constant to substitute. Reaching THIS refusal still asserts
# everything Week 30 built — nested scopes, correlation detection, per-scope
# validation, the level carried on every ref — because all of it has to succeed
# to get here.
# Week 33 executes the decorrelatable shapes, so this is no longer ONE message.
# The shapes below are refused by decorrelation itself ("correlated subquery:
# <what it declined>") or, on Volcano, by the capability refusal ("correlated
# subqueries are decorrelated to a semi-join and are not supported..."). The
# common prefix is what every one of them shares; a per-shape expectation would
# be better and is Week 34's to add when the shapes stop moving.
WEEK33_CORRELATED_EXPECT = "correlated subquer"
# Week 33 — DECORRELATED correlated subqueries: these EXECUTE and are diffed
# against SQLite. Vectorized only, because Planner::plan runs no
# LogicalPlanBuilder and therefore cannot decorrelate (the same capability
# difference Week 32's IN lowering has, refused in the two Volcano modes below).
WEEK33_DECORRELATED_VEC_ONLY = [
    # EXISTS, correlated (Q4/Q21) — `select *` inside must stay legal
    "SELECT d.name FROM drivers d WHERE EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 340) "
    "ORDER BY d.name",
    # NOT EXISTS (Q21): the leading-NOT production, lowered to an ANTI join
    "SELECT d.name FROM drivers d WHERE NOT EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id) ORDER BY d.name",
    # ...and in the middle of an AND chain, so the conjunct extraction is not
    # relying on the subquery being the whole WHERE
    "SELECT d.name FROM drivers d WHERE d.age > 30 AND NOT EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id) ORDER BY d.name",
    # TWO correlated keys: the rewrite must produce a multi-key join, not stop
    # at the first equality it finds
    "SELECT l.lap_id FROM laps l WHERE EXISTS "
    "(SELECT * FROM laps l2 WHERE l2.driver_id = l.driver_id "
    " AND l2.season = l.season AND l2.speed > 340) ORDER BY l.lap_id LIMIT 50",
    # correlated across a JOIN in the outer query: the ref names relation 1, so
    # from_slot must come from the OUTER range table and not the body's
    "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "WHERE EXISTS (SELECT * FROM laps l2 WHERE l2.team = d.team) "
    "ORDER BY l.team LIMIT 50",
    # ANTI JOIN AND NULL — the trap NOT IN was in Week 32, asserted rather than
    # argued. EXISTS is a pure existence test and is never UNKNOWN, so a NULL on
    # either side of the key simply fails to match and the outer row SURVIVES.
    # That is the opposite of NOT IN, where one NULL in the set makes the whole
    # predicate never-true. `sector_1` is nullable in the generated data, so this
    # is a real NULL-keyed body and not a synthetic one.
    "SELECT d.driver_id FROM drivers d WHERE NOT EXISTS "
    "(SELECT * FROM laps l WHERE l.sector_1 = d.age) ORDER BY d.driver_id",
    "SELECT d.driver_id FROM drivers d WHERE EXISTS "
    "(SELECT * FROM laps l WHERE l.sector_1 = d.age) ORDER BY d.driver_id",
    # Week 33 round 2 — BOTH DIRECTIONS of the NULL rule NOT EXISTS must not
    # have, pinned against SQLite instead of argued in a header. Until the ANTI /
    # ANTI_NOT_IN split, a decorrelated NOT EXISTS was lowered to the same
    # enumerator NOT IN uses and inherited its three-valued rule.
    #
    # (a) NULL on the PROBE (correlated) side. Week 29's LEFT JOIN null-extends
    #     `l`, so `l.lap_id` is NULL for the 16 drivers with no lap under 5. The
    #     correlated equality is UNKNOWN for every body row, so the body yields
    #     none, EXISTS is FALSE and NOT EXISTS is TRUE: SQLite returns those 16.
    #     SwiftQL returned 0 — the NULL probe key was dropped because the build
    #     side was non-empty, which is `NULL NOT IN S`'s rule, not this one.
    "SELECT COUNT(*) FROM drivers d LEFT JOIN laps l "
    "ON d.driver_id = l.driver_id AND l.lap_id < 5 "
    "WHERE NOT EXISTS (SELECT * FROM laps l2 WHERE l2.lap_id = l.lap_id)",
    # (b) NULL on the BUILD (body) side. `y.lap_id` is NULL for every body row,
    #     so the body is empty for every outer row and all 20 drivers survive.
    #     SwiftQL returned 0 for the WHOLE query: one unmatchable build key set
    #     the NOT IN short-circuit and every probe chunk was skipped.
    "SELECT COUNT(*) FROM drivers d WHERE NOT EXISTS "
    "(SELECT * FROM drivers x LEFT JOIN laps y "
    " ON x.driver_id = y.driver_id AND y.lap_id < 0 WHERE y.lap_id = d.driver_id)",
    # ...and the positive form of (a), which must be unaffected: a NULL probe key
    # matches nothing, so EXISTS is FALSE and those 16 rows are the ones dropped.
    "SELECT COUNT(*) FROM drivers d LEFT JOIN laps l "
    "ON d.driver_id = l.driver_id AND l.lap_id < 5 "
    "WHERE EXISTS (SELECT * FROM laps l2 WHERE l2.lap_id = l.lap_id)",
    # Round 1 H-2 — an ALIAS in the body's SELECT list shadowing the join key.
    # The key was resolved by name against the body's OUTPUT schema, which
    # buildProjectSchema names by alias, so this probed d.driver_id against
    # laps.speed: 0 rows where SQLite returns 20.
    "SELECT COUNT(*) FROM drivers d WHERE EXISTS "
    "(SELECT l.speed AS driver_id FROM laps l WHERE l.driver_id = d.driver_id)",
    # Round 1 M-3 — the two most idiomatic EXISTS bodies in SQL. Both used to
    # die with "join key 'driver_id' not found on the joined relation", because
    # the correlated conjunct is removed before the body is planned and
    # buildScanSchema then narrowed the key column away.
    "SELECT COUNT(*) FROM laps l WHERE EXISTS "
    "(SELECT 1 FROM drivers d WHERE d.driver_id = l.driver_id)",
    "SELECT COUNT(*) FROM laps l WHERE EXISTS "
    "(SELECT d.name FROM drivers d WHERE d.driver_id = l.driver_id)",
    # Round 1 H-1 — a JOIN body, whose merged schema holds `team` TWICE
    # (invariant 3 makes that legal). indexOf took the first match.
    "SELECT COUNT(*) FROM drivers d WHERE EXISTS "
    "(SELECT * FROM laps l JOIN drivers t ON l.lap_id = t.driver_id "
    " WHERE t.team = d.team)",
    "SELECT COUNT(*) FROM drivers d WHERE NOT EXISTS "
    "(SELECT * FROM laps l JOIN drivers t ON l.lap_id = t.driver_id "
    " WHERE t.team = d.team)",
    # Round 2 left this unaudited: a body ORDER BY must outlive the select-list
    # replacement the H-1/H-2/M-3 fix performs. It does -- Sort is placed BELOW
    # Project so the sort key resolves against the pre-projection schema -- and
    # it can only change the ANSWER together with a LIMIT, which
    # requireDecorrelatableBody refuses. Diffed rather than argued.
    "SELECT COUNT(*) FROM drivers d WHERE EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id ORDER BY l.speed)",
    "SELECT COUNT(*) FROM drivers d WHERE EXISTS "
    "(SELECT l.lap_id FROM laps l WHERE l.driver_id = d.driver_id ORDER BY l.speed)",
    "SELECT COUNT(*) FROM drivers d WHERE NOT EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id AND l.season = 1900 "
    " ORDER BY l.sector_1)",
]

# The other half of that capability difference, asserted so the boundary cannot
# drift silently: the diffed oracle cannot hold a query that errors, so without
# this nothing tests that Volcano REFUSES rather than quietly answering.
WEEK33_DECORRELATED_VOLCANO_REJECTED = [
    (query, VOLCANO_CORRELATED)
    for query in WEEK33_DECORRELATED_VEC_ONLY
]

# Nested two deep, the inner one correlated to the MIDDLE block (Q20). The TOP
# node is uncorrelated, so this is the query that proves the statement flag
# propagates outward. It is OUT of WEEK33_CORRELATED_BINDS because it is the one
# entry whose refusal is NOT the same on all four modes: the two Volcano modes
# refuse it for the OUTER IN — a genuinely different and genuinely correct
# capability boundary, hit before correlation can be diagnosed at all — and
# asserting the correlation wording there would be asserting a message that path
# has no business producing. Same split, and the same reason, as
# WEEK32_LOWERING_REFUSED / WEEK32_LOWERING_REFUSED_VOLCANO.
#
# Round 2 R2-C1 joins it, for the same mode reason: a DIRECTLY correlated IN is
# refused on the vec path by decorrelation ("a correlated IN / NOT IN is not
# lowered") and on Volcano by the IN capability boundary, which is hit first.
# Before the fix, lowerInSubqueries consumed the node THIRTEEN LINES before
# refuseUnloweredCorrelated could see it and lowered it to a ONE-KEY semi-join,
# discarding the correlation: `l.team = d.team` was planned inside the body,
# where the outer ref fell back to bare name and became the tautology
# `laps.team = laps.team`. Measured on the committed data: SwiftQL 20 where
# SQLite says 6, and 0 where SQLite says 14. This file held no directly
# correlated IN at all, which is exactly why every mode passed.
WEEK33_CORRELATED_IN_SHAPES = [
    # MOVED to WEEK34_CORRELATED_SCALAR_VEC_ONLY. Its old comment here called it
    # "nested two deep, the inner one correlated to the MIDDLE block", and that
    # description is what kept it looking unsupported. Re-read: the IN body
    # references only its OWN relation `l`, so the IN is UNCORRELATED and the
    # scalar is correlated ONE level, to the IN body's own block. It is therefore
    # two INDEPENDENT mechanisms in two separate blocks — Week 32's semi-join
    # lowering for the IN, and Week 34's scalar decorrelation running inside the
    # body's own LogicalPlanBuilder::build — not a two-level correlation at all.
    # Verified against SQLite before the move, including a variant that returns a
    # proper subset (19 of 20 drivers) and the NOT IN / anti-join form.
    # R2-C1: SwiftQL 20, SQLite 6
    "SELECT COUNT(*) FROM drivers d WHERE d.driver_id IN "
    "(SELECT l.lap_id FROM laps l WHERE l.team = d.team)",
    # R2-C1, negated: SwiftQL 0, SQLite 14
    "SELECT COUNT(*) FROM drivers d WHERE d.driver_id NOT IN "
    "(SELECT l.lap_id FROM laps l WHERE l.team = d.team)",
    # ...and the shape where the two engines happened to AGREE (0 = 0) while the
    # body was still a tautology, so a matching row count was never evidence
    "SELECT COUNT(*) FROM drivers d WHERE d.age IN "
    "(SELECT l.season FROM laps l WHERE l.driver_id = d.driver_id)",
]
WEEK33_CORRELATED_NESTED_VEC = [
    (q, "correlated subquer") for q in WEEK33_CORRELATED_IN_SHAPES]
# VOLCANO_IN, not VOLCANO_CORRELATED: the correlation is INSIDE an IN body, and
# planner.cc checks has_in before has_correlated. Pinning it is what makes that
# ordering an assertion rather than an accident.
WEEK33_CORRELATED_NESTED_VOLCANO = [
    (q, VOLCANO_IN) for q in WEEK33_CORRELATED_IN_SHAPES]

WEEK33_CORRELATED_BINDS = [
    # MOVED to WEEK34_CORRELATED_SCALAR_VEC_ONLY. This is Q17 exactly — a
    # correlated scalar as an operand of ARITHMETIC — and Week 34's
    # lowerCorrelatedScalars makes it legal: the node is not a whole conjunct, so
    # it is replaced in place through forEachSubquery while the GROUP BY join is
    # grafted onto the spine. Verified against SQLite before the move: 2084 rows
    # both ways at a coefficient that actually discriminates, exact set match,
    # optimized and --no-optimize. Kept as a comment so the move is visible here
    # and not only in a diff — nothing leaves a rejection suite without arriving
    # in a diffed one.
    # a correlated ref inside the NESTED query's own ON clause
    "SELECT lap_id FROM laps l WHERE EXISTS "
    "(SELECT 1 FROM drivers d JOIN drivers d2 ON d.driver_id = d2.driver_id "
    " AND d.age = l.lap_id)",
    # a REAL inner key beside a correlated residual is legal, and the residual
    # must not be what saves it from the cross-product refusal
    "SELECT lap_id FROM laps l WHERE EXISTS "
    "(SELECT 1 FROM drivers d JOIN laps p ON d.driver_id = p.driver_id "
    " AND p.speed > l.speed)",
    # a correlated GROUP BY key is legal SQL, in both spellings
    "SELECT lap_id FROM laps l WHERE EXISTS "
    "(SELECT COUNT(*) FROM drivers d GROUP BY season)",
    "SELECT l.lap_id FROM laps l JOIN drivers dd ON l.driver_id = dd.driver_id "
    "WHERE EXISTS (SELECT COUNT(*) FROM drivers d GROUP BY season)",
    # BETWEEN's clone shares the statement, so BOTH nodes must stay marked
    # correlated — the second one was left uncorrelated until Week 30 round 1
    "SELECT lap_id FROM laps l WHERE "
    "(SELECT MAX(d.age) FROM drivers d WHERE d.driver_id = l.driver_id) "
    "BETWEEN 1 AND 99",
    # a LEGAL correlated aggregate argument: the type check that moved to the
    # Binder must not have become a blanket refusal
    "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "WHERE EXISTS (SELECT SUM(d.age) FROM drivers x)",
    # the two Week 31 tripwire inputs. Both still only BIND: the guards behind
    # them (ChunkPruner declines, buildAggregateSchema throws) stay ARMED and
    # unreached, because Week 31 lowers no correlated reference. `team` exists in
    # both tables, which is what makes each failure a silent hit on the wrong
    # relation rather than a miss
    "SELECT lap_id FROM laps l WHERE EXISTS "
    "(SELECT 1 FROM drivers d WHERE l.team = 'Ferrari')",
    "SELECT lap_id FROM laps l WHERE EXISTS "
    "(SELECT COUNT(*) FROM drivers d GROUP BY l.team)",
    # Week 33 round 2 — a correlated ref in the BODY's ON clause. splitCorrelation
    # reads body.where only, so this one survived into the body's plan and
    # resolved by BARE NAME against the body's merged schema: `d2.team = d.team`
    # became `d2.team = laps.team`. It returned 5 where SQLite returns 20, with
    # no error. Refused by name now, which is why it belongs in this suite and
    # not in the diffed one.
    "SELECT COUNT(*) FROM drivers d WHERE EXISTS "
    "(SELECT * FROM laps l JOIN drivers d2 ON l.lap_id = d2.driver_id "
    " AND d2.team = d.team WHERE d2.driver_id = d.driver_id)",
    # ...and the same guard over the body's SELECT list, which used to reach
    # buildProjectSchema and report an INTERNAL defect for a query SQLite answers
    "SELECT COUNT(*) FROM drivers d WHERE EXISTS "
    "(SELECT d.name FROM laps l WHERE l.driver_id = d.driver_id)",
]

# Each of these must fail EARLIER than the refusal above, and for its own stated
# reason. Without them the Week 31 refusal becomes a catch-all that hides a real
# defect behind a temporary one — the discipline that placed Week 26's multi-key
# refusal past the plan-time type checks.
# Week 34 — COUNT(DISTINCT x). Diffed in ALL FOUR modes, unlike everything else
# this week: both HashAggregateNode (plan_nodes.cc) and VecHashAggregateNode
# implement it, so Volcano stays the correctness baseline (invariant 6) instead
# of refusing. This is the one Week 34 deliverable that RESTORES coverage rather
# than narrowing it, which is why it does not get a *_VEC_ONLY twin.
# EVERY COLUMN IS ALIASED, and that is a harness constraint rather than style:
# parse_swiftql_output() reads the aligned printer's header line with
# `lines[0].split()`, so a column NAME containing a space becomes two headers and
# the whole result fails to parse. `COUNT(DISTINCT driver_id)` is the first
# output name in the engine's history to contain a space at the TOP level of a
# select item, so this constraint had never bitten. Aliasing also makes both
# engines name the column identically, which normalize() keys rows on.
# Week 34 — DERIVED TABLES (FROM (subquery)). Diffed in the two VECTORIZED modes
# only: Planner::plan builds its scan from a catalog table name and exactly one
# HashJoinNode out of stmt.joins, so there is no plan shape there that can hold a
# relation which is itself a PLAN, and that path does not run LogicalPlanBuilder,
# where the graft happens. Third family of query the four-mode oracle does not
# cover (after Week 32's IN and Week 33's correlated); the count is in README
# Limitations so the week it tips is visible.
#
# Every output column is ALIASED — see WEEK34_DISTINCT_AGG_QUERIES for why
# (parse_swiftql_output splits the header line on whitespace).
# Week 35 — a SUBQUERY INSIDE A DERIVED-TABLE BODY. Found by the TPC-H harness
# on Q22, which is the first query in the project with this shape.
#
# materializeSubqueries never descended into a derived body, and main.cc guarded
# the whole materialization block with `stmt.has_subquery` -- a PER-BLOCK flag
# (ast.h documents why it must stay per-block: propagating it would turn
# projection pushdown off for every derived-table query and give the wrong
# Volcano refusal message). So an outer query whose only subquery lived inside a
# derived body had the flag clear, skipped materialization entirely, and the node
# reached inferExprType and raised
#   "internal: a subquery reached type inference without being materialized".
# An INTERNAL error surfaced to the user on a legitimate query.
#
# Two walkers over one structure, one of them updated when Week 34 landed derived
# tables: collectQueryTables, ten lines above materializeSubqueries in the SAME
# FILE, WAS extended to recurse into a body. That is the exact shape the standing
# sweep rule exists for.
#
# Vectorized-only, like every derived-table query, and diffed rather than pinned:
# the fix must produce the RIGHT ROWS, not merely stop throwing.
WEEK35_SUBQUERY_IN_DERIVED_BODY_VEC_ONLY = [
    # the minimal repro: an uncorrelated scalar subquery in the body's WHERE
    "SELECT d.c AS c FROM (SELECT driver_id AS c FROM drivers "
    "WHERE age > (SELECT AVG(age) FROM drivers)) AS d ORDER BY c",
    # in the body's HAVING, so the materialized constant has to survive
    # aggregation rather than just a scan filter
    "SELECT d.t AS t, d.n AS n FROM (SELECT team AS t, COUNT(*) AS n FROM laps "
    "GROUP BY team HAVING COUNT(*) > (SELECT COUNT(*) / 20 FROM laps)) AS d "
    "ORDER BY t",
    # an EXISTS in the body: a different Kind, same walk
    "SELECT d.c AS c FROM (SELECT driver_id AS c FROM drivers "
    "WHERE EXISTS (SELECT * FROM laps WHERE speed > 340)) AS d ORDER BY c",
    # the derived table in a JOIN position rather than FROM -- stmt.joins is a
    # separate recursion site and a fix that only handled `from` would pass the
    # three above and still fail this one
    "SELECT l.lap_id AS lid FROM laps l "
    "JOIN (SELECT driver_id AS c FROM drivers "
    "WHERE age > (SELECT AVG(age) FROM drivers)) AS d ON l.driver_id = d.c "
    "WHERE l.lap_id < 40 ORDER BY lid",
    # TWO derived relations, only the second holding a subquery: proves the walk
    # does not stop at the first body
    "SELECT a.c AS ac, b.c AS bc FROM (SELECT driver_id AS c FROM drivers) AS a "
    "JOIN (SELECT driver_id AS c FROM drivers "
    "WHERE age > (SELECT MIN(age) FROM drivers)) AS b ON a.c = b.c ORDER BY ac",
]

WEEK35_SUBQUERY_IN_DERIVED_BODY_VOLCANO_REJECTED = [
    (q, "not supported on the Volcano path")
    for q in WEEK35_SUBQUERY_IN_DERIVED_BODY_VEC_ONLY
]

WEEK34_DERIVED_TABLE_VEC_ONLY = [
    # the plain shape: a derived relation as the only relation
    "SELECT d.team AS t FROM (SELECT team FROM laps) AS d ORDER BY t LIMIT 5",
    # a derived relation with a WHERE above it — the conjunct lands as a filter
    # ABOVE the LogicalDerived, which is what filterOnto's wrapping gives for free
    "SELECT d.t AS t, d.s AS s FROM (SELECT team AS t, AVG(speed) AS s FROM laps "
    "GROUP BY team) AS d WHERE d.s > 300 ORDER BY t",
    # Q15's shape: a derived AGGREGATE relation joined to a base relation. This is
    # the case the slot-0 normalization exists for on the join side.
    "SELECT dr.name AS n, d.s AS s FROM (SELECT driver_id, AVG(speed) AS s FROM laps "
    "GROUP BY driver_id) AS d JOIN drivers dr ON d.driver_id = dr.driver_id "
    "ORDER BY n LIMIT 10",
    # a derived relation in the JOIN position rather than the FROM one
    "SELECT dr.name AS n, x.c AS c FROM drivers dr JOIN (SELECT driver_id, COUNT(*) AS c "
    "FROM laps GROUP BY driver_id) AS x ON x.driver_id = dr.driver_id ORDER BY n LIMIT 10",
    # a body that JOINS, so its own output schema carries slots 0 AND 1 before
    # normalization — the shape that would put two numbering domains in one schema
    "SELECT x.t AS t, x.nm AS nm FROM (SELECT l.team AS t, d.name AS nm FROM laps l "
    "JOIN drivers d ON l.driver_id = d.driver_id) AS x ORDER BY t, nm LIMIT 10",
    # Q13's shape: a LEFT JOIN inside the body plus a regroup above the derived
    # relation. THE COLUMN ALIAS LIST IS DELIBERATELY NOT HERE — `AS c (nm, k)`
    # is standard SQL that SQLITE DOES NOT PARSE (`near "(": syntax error`), so
    # the oracle cannot hold a query using it in EITHER direction: it is neither
    # diffable nor refusable. This is the mirror image of the blind spot Week 30
    # named — the oracle cannot hold a query SwiftQL rejects, and it equally
    # cannot hold one SQLITE rejects. The feature is covered by
    # LogicalPlanTest DerivedTable.ColumnAliasListRenamesPositionally and its
    # arity twin instead, and that is the whole coverage it can have.
    "SELECT c.k AS k, COUNT(*) AS n FROM (SELECT dr.name AS nm, COUNT(l.lap_id) AS k "
    "FROM drivers dr LEFT JOIN laps l ON dr.driver_id = l.driver_id GROUP BY dr.name) "
    "AS c GROUP BY c.k ORDER BY k",
    # three relations, one of them derived: exercises join enumeration over a
    # relation with no TableStats (joinCardinality's non-multiplicative branch)
    "SELECT dr.name AS n, d.s AS s FROM (SELECT driver_id, AVG(speed) AS s FROM laps "
    "GROUP BY driver_id) AS d JOIN drivers dr ON d.driver_id = dr.driver_id "
    "JOIN laps l2 ON l2.driver_id = dr.driver_id WHERE l2.season = 2024 "
    "ORDER BY n, s LIMIT 10",
    # a derived table nested inside a derived table
    "SELECT y.t AS t FROM (SELECT x.t AS t FROM (SELECT team AS t FROM laps) AS x) AS y "
    "ORDER BY t LIMIT 5",
    # a derived table inside a SUBQUERY body — two nesting mechanisms at once,
    # which is the shape Week 32 shipped a regression in that no suite could see
    "SELECT COUNT(*) AS n FROM laps WHERE speed > (SELECT AVG(x.s) FROM "
    "(SELECT AVG(speed) AS s FROM laps GROUP BY team) AS x)",
    # SELECT * over a derived relation. Aliased in the body rather than by a
    # column alias list, for the SQLite reason given above.
    "SELECT * FROM (SELECT team AS a, speed AS b FROM laps WHERE speed > 340) AS d "
    "ORDER BY a, b",
]

# One entry here joins TWICE in its outer query, so planner.cc's multi-way guard
# fires before it ever reaches the derived-table one — correct, and completely
# hidden by the generic needle. The split is written as the guard-ordering rule
# rather than as an index, so a query added later is classified, not mislabelled.
WEEK34_DERIVED_TABLE_VOLCANO_REJECTED = [
    (query,
     VOLCANO_MULTIWAY if query.upper().count(" JOIN ") > 1 else VOLCANO_DERIVED)
    for query in WEEK34_DERIVED_TABLE_VEC_ONLY
]

# LANGUAGE refusals: they fire identically on every path, so all four modes.
# The diffed oracle cannot hold a query that errors, which is why each message is
# pinned here rather than left to a manual check.
WEEK34_DERIVED_REFUSED = [
    # SQLite accepts an unaliased subquery in FROM; SwiftQL requires the alias
    # because Binder::RangeEntry is keyed on the ref name. A divergence, pinned.
    ("SELECT * FROM (SELECT team FROM laps)",
     "a subquery in FROM requires an alias"),
    ("SELECT * FROM (SELECT team FROM laps) AS d (a, b)",
     "column aliases were supplied"),
    # A catalog table cannot produce two columns of one name; a derived table can,
    # and then BOTH indexOf overloads are a coin flip. SQLite answers this query
    # (it disambiguates positionally), so it is a divergence.
    ("SELECT * FROM (SELECT l.team, d.team FROM laps l "
     "JOIN drivers d ON l.driver_id = d.driver_id) AS x",
     "is produced twice"),
    # LATERAL. Refused by the LATERAL message when the enclosing block is itself
    # nested — there is a parent scope for the reference to resolve in and mark
    # the body correlated. At TOP level there is no parent, so the same shape is
    # reported as an ordinary unresolved qualifier: a sibling FROM item genuinely
    # is not in scope and the Binder cannot tell a lateral reference from a typo.
    # Both halves are pinned so neither can drift into silently binding.
    ("SELECT * FROM laps l JOIN (SELECT team FROM drivers d WHERE d.team = l.team) "
     "AS x ON x.team = l.team",
     "unknown table qualifier"),
    ("SELECT COUNT(*) FROM laps o WHERE EXISTS (SELECT 1 FROM "
     "(SELECT team FROM drivers d WHERE d.team = o.team) AS x WHERE x.team = o.team)",
     "LATERAL is not supported"),
]

# Week 34 — CORRELATED SCALAR subqueries, the Q17 shape. Week 33 recorded their
# absence as a checkpoint MISS and handed them here; a decorrelated scalar
# subquery is a derived table with an implicit join, so they run on the machinery
# the suites above exercise. Vectorized only, inheriting Week 33's boundary
# unchanged (Planner::plan runs no LogicalPlanBuilder, where the rewrite grafts).
WEEK34_CORRELATED_SCALAR_VEC_ONLY = [
    # Q17's shape exactly: a correlated AVG compared against an outer column
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > 1.02 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)",
    # the same, regrouped above the join, so the merged schema is read by a
    # GROUP BY and not only by the WHERE
    "SELECT l.team AS t, COUNT(*) AS n FROM laps l WHERE l.speed > 1.05 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) "
    "GROUP BY l.team ORDER BY t",
    # TWO correlation keys, and MAX rather than AVG. Both halves of this entry are
    # load-bearing and neither is obvious, so both are stated.
    #
    # (a) The keys must be INDEPENDENT. `driver_id AND team` — the shape that
    # shipped — cannot discriminate on this data at all: in laps, driver_id
    # functionally determines team (20 drivers, one team each), so the team key is
    # redundant and dropping it changes no answer. `driver_id AND season` is the
    # nearest genuinely two-dimensional key: 20 drivers x 4 seasons = 80 groups,
    # none of them derivable from the other key.
    #
    # (b) The comparison must not be self-satisfying. `l.speed > MAX(...)` over a
    # group that CONTAINS the outer row is false for every row by construction, so
    # it answers 0 whatever the join does — and 0 is also what a total failure
    # answers. The 0.99 coefficient breaks the tie: the outer row's own speed no
    # longer bounds the predicate, so the answer tracks the GROUP the keys select.
    # (Excluding the outer row instead — `AND l2.lap_id <> l.lap_id` — is not
    # available: splitCorrelation refuses a correlated inequality, by design.)
    #
    # Verified against sqlite3 on the shipped data, SwiftQL == SQLite on all three:
    #   both keys (this entry)        n = 623
    #   l2.driver_id = l.driver_id    n = 553
    #   l2.season = l.season          n = 533
    # so losing EITHER key is visible here. This is the only two-key entry in the
    # week, i.e. the only place a splitCorrelation that emitted one join key
    # instead of two would be caught.
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > 0.99 * (SELECT MAX(l2.speed) "
    "FROM laps l2 WHERE l2.driver_id = l.driver_id AND l2.season = l.season)",
    # THE ZERO-ROW CASE, and the reason the join is LEFT rather than INNER. The
    # body's filter leaves many correlation keys with no group at all; SQL says
    # the scalar subquery is NULL there, so the comparison is UNKNOWN and the row
    # is excluded — but it must be EXCLUDED BY THE PREDICATE, not DROPPED BY THE
    # JOIN. An inner join gives the same answer here and a different one below.
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > (SELECT MAX(l2.speed) FROM laps l2 "
    "WHERE l2.driver_id = l.driver_id AND l2.season = 2024)",
    # every group empty: the answer is 0, not an error and not the whole table
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > (SELECT MAX(l2.speed) FROM laps l2 "
    "WHERE l2.driver_id = l.driver_id AND l2.season = 1900)",
    # !! THE QUERY THAT DISTINGUISHES LEFT FROM INNER. Under an inner join the
    # rows with no matching group would be deleted before the OR could rescue
    # them, so this returns FEWER rows than SQLite. It is the only shape here that
    # can see the difference, and it is also the shape that shows a correlated
    # SCALAR may sit under an OR at all: unlike EXISTS and IN, the rewrite
    # preserves the outer row set exactly, so any expression position in WHERE is
    # sound.
    "SELECT COUNT(*) AS n FROM laps l WHERE l.season = 2024 OR l.speed > "
    "(SELECT MAX(l2.speed) FROM laps l2 WHERE l2.driver_id = l.driver_id "
    "AND l2.season = 2024)",
    # ARRIVED FROM WEEK33_CORRELATED_BINDS. Week 33 refused this exact shape and
    # recorded it as its checkpoint miss; it is Q17's, and it is why this suite
    # exists. The 0.2 coefficient is the one Week 33 wrote, and it matches every
    # row - kept verbatim so the moved entry is recognisable - with the
    # discriminating coefficients below carrying the actual oracle weight.
    "SELECT l.lap_id AS id FROM laps l WHERE l.speed > 0.2 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) ORDER BY id LIMIT 10",
    # The correlated scalar in every other ARITHMETIC position: on the left of the
    # comparison, and inside a subtraction. lowerCorrelatedScalars replaces the
    # node through its owning slot, so position within the expression must not
    # matter - these are what say so.
    "SELECT l.lap_id AS id FROM laps l WHERE l.speed > 1.06 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) ORDER BY id",
    "SELECT l.lap_id AS id FROM laps l WHERE "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) * 1.06 < l.speed "
    "ORDER BY id",
    "SELECT l.lap_id AS id FROM laps l WHERE l.speed - "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) > 18 ORDER BY id",
    # TWO correlated scalars in one predicate: two derived relations grafted onto
    # one spine, at two different synthetic slots.
    "SELECT l.lap_id AS id FROM laps l WHERE l.speed > "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) AND l.speed < "
    "(SELECT MAX(l3.speed) FROM laps l3 WHERE l3.team = l.team) ORDER BY id",
    # ---- THE ZERO-ROW GROUP RULE, one query per aggregate kind. ----
    #
    # Week 34 audit round 1, F1: a silent WRONG ANSWER, and the whole reason this
    # block exists rather than one representative query. The rewrite groups the
    # body by the correlation key, so a key with NO matching body rows produces no
    # group row at all and the LEFT join null-extends it. For SUM / AVG / MIN / MAX
    # that IS the right value — those are NULL over an empty set. **COUNT is the
    # exception**: SQL says COUNT over zero rows is 0, so the outer predicate read
    # NULL where it must read 0, and `WHERE d.age > (SELECT COUNT(*) ...)` returned
    # 0 rows against SQLite's 20.
    #
    # Every aggregate kind is pinned, not just COUNT, because the trap is that the
    # two families disagree and a reader checking one learns the wrong rule. The
    # body filter `speed > 999` matches nothing, so EVERY correlation key is a
    # zero-row group and each query below is a direct read of its aggregate's
    # empty-set value.
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)",
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT COUNT(l.lap_id) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)",
    # COUNT(DISTINCT x) is still a COUNT and has the identical 0-over-empty rule,
    # which is why the fix tests the FUNCTION and not the `distinct` flag.
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT COUNT(DISTINCT l.season) FROM laps l WHERE l.driver_id = d.driver_id "
    "AND l.speed > 999)",
    # The four that ARE NULL over an empty set. Each must return zero outer rows:
    # a comparison against NULL is UNKNOWN, so no row qualifies. If one of these
    # ever starts returning rows, the COUNT fix has been over-applied.
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT AVG(l.speed) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)",
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT SUM(l.speed) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)",
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT MIN(l.speed) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)",
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT MAX(l.speed) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)",
    # MIXED, and the one that would still catch the bug if the all-empty queries
    # above were ever weakened: 19 of the 20 drivers appear in the first 60 laps,
    # so exactly one correlation key has no group. Pre-fix this returned 19 rows
    # where SQLite returns 20 — a one-row difference, which is what a partial
    # regression looks like.
    "SELECT d.driver_id AS did, d.age AS age FROM drivers d WHERE d.age > "
    "(SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id AND l.lap_id < 60) "
    "ORDER BY did",
    # And a COUNT body whose groups all EXIST, so the CASE wrapper must be a no-op
    # rather than a change of value.
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id)",

    # ARRIVED FROM WEEK33_CORRELATED_IN_SHAPES. The correlation is INSIDE the IN
    # body and relative to that block, so the IN itself is UNCORRELATED and the
    # scalar rewrite runs during the body's own build() - two lowerings stacked on
    # two different spines in one query, which nothing else here exercises.
    "SELECT d.name AS nm FROM drivers d WHERE d.driver_id IN "
    "(SELECT l.driver_id FROM laps l WHERE l.speed > "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)) ORDER BY nm",
    # ...and the same shape at a coefficient that actually DISCRIMINATES. The
    # entry above returns all 20 drivers, so it would pass against an engine that
    # ignored the inner scalar entirely; this one returns 19, a proper subset, and
    # is the one that can fail. A weak oracle query is the failure Week 32 shipped
    # a regression past.
    "SELECT d.name AS nm FROM drivers d WHERE d.driver_id IN "
    "(SELECT l.driver_id FROM laps l WHERE l.speed > 1.10 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.driver_id = l.driver_id)) ORDER BY nm",
    # The ANTI-JOIN half of the same shape: NOT IN over a body whose own scalar is
    # decorrelated. Week 32's three-valued NOT IN rule and Week 34's LEFT-join
    # null-extension meet here, and nothing else in the suite puts them together.
    "SELECT d.name AS nm FROM drivers d WHERE d.driver_id NOT IN "
    "(SELECT l.driver_id FROM laps l WHERE l.speed > 1.10 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.driver_id = l.driver_id)) ORDER BY nm",

    # WEEK 36 — THE CONSTANT WRAPPER, i.e. TPC-H Q17's own text. Week 34 required
    # the body's select-list item to BE the aggregate and refused `0.2 * AVG(x)`;
    # the wrapper is now lifted out of the body and re-attached around the
    # substituted reference. The pair below is the point: the two forms are
    # semantically identical, only the parenthesis position differs, and after the
    # rewrite they produce the SAME plan. Both are diffed so a divergence between
    # them cannot hide.
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > 0.5 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)",
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > "
    "(SELECT 0.5 * AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)",
    # A coefficient that DISCRIMINATES rather than one that keeps every row: the
    # entries above return all 10000 laps, so they would pass against an engine
    # that dropped the predicate. This one returns a proper subset.
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > "
    "(SELECT 1.02 * AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)",

    # SEAM AUDIT pass 2 — B-2. THE SAME PAIR WITH A COLUMN ALIAS ON THE BODY,
    # which is where the "SAME plan" claim two comments up USED TO BE FALSE.
    #
    # The claim was asserted by this file and by README (twice) and verified in
    # Week 36 on the UNALIASED pair only. With an alias the two forms did not
    # merely differ, one of them DID NOT PLAN AT ALL:
    #
    #   (SELECT AVG(l2.speed) AS a ...)        -> Error: column not found:
    #                                             'AVG(l2.speed)'   [SQLite 4994]
    #   (SELECT 1.02 * AVG(l2.speed) AS a ...) -> 4037 = SQLite
    #
    # because the alias only reaches the moved node in the UNWRAPPED form (in the
    # wrapped form it rides on the BinaryExpr, which is lifted out), so
    # buildProjectSchema renamed the relation's column to `a` while the
    # substituted outer reference still looked for `AVG(l2.speed)`. The lowering
    # now clears the alias off the aggregate before rebuilding the body's select
    # list, and all four forms are byte-identical under --explain again. These
    # entries are what puts the claim under test instead of leaving it asserted.
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > "
    "(SELECT AVG(l2.speed) AS a FROM laps l2 WHERE l2.team = l.team)",
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > 1.02 * "
    "(SELECT AVG(l2.speed) AS a FROM laps l2 WHERE l2.team = l.team)",
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > "
    "(SELECT 1.02 * AVG(l2.speed) AS a FROM laps l2 WHERE l2.team = l.team)",
    # ...and an aliased COUNT body, so the alias is exercised on the OTHER
    # substitution path — the one that wraps the reference in `CASE WHEN ref IS
    # NULL THEN 0 ELSE ref END` rather than assigning it bare. Every group here
    # is empty (`speed > 999`), so the zero-row rule and the alias are both live
    # at once: 20, not 0.
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT COUNT(*) AS c FROM laps l WHERE l.driver_id = d.driver_id "
    "AND l.speed > 999)",
    # ...and an aliased body whose ROWS are compared, not just a count, so an
    # alias that silently selected the WRONG column (rather than no column) would
    # be visible. 66 rows.
    "SELECT l.lap_id AS id FROM laps l WHERE l.speed > "
    "(SELECT MAX(l2.speed) AS m FROM laps l2 WHERE l2.driver_id = l.driver_id "
    "AND l2.season = 2024) ORDER BY id",
    # ...and the alias COLLIDING with the correlation key's own column name. The
    # $kN rename (8a23b9d) makes this safe from the key side; this says so from
    # the alias side, and it is the shape where "resolve by a name derived in one
    # place and looked up in another" could have hit the WRONG column rather than
    # no column. 4994, same as the unaliased form.
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > "
    "(SELECT AVG(l2.speed) AS team FROM laps l2 WHERE l2.team = l.team)",
    # !! THE WRAPPED-COUNT PAIR, and the reason the wrapper is lifted OUT of the
    # body rather than pushed through it. COUNT over an empty group is 0, not
    # NULL, so the substitution site wraps the reference in
    # `CASE WHEN ref IS NULL THEN 0 ELSE ref END`. Lifting puts that CASE at the
    # AGGREGATE'S position inside the wrapper, so `1 + COUNT(*)` over a zero-row
    # group reads 1 + 0 = 1. Keeping the wrapper in the body would have
    # substituted 0 for the WHOLE wrapper and answered 0 — a silent wrong answer
    # of exactly the shape Week 34's audit found (F1), one level further in.
    # `speed > 999` matches nothing, so EVERY correlation key is a zero-row group.
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > 1 + "
    "(SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)",
    # ...and its non-COUNT twin, where the zero-row value IS NULL and the wrapper
    # must propagate it. Pinning only the COUNT half teaches the wrong rule: the
    # two aggregate families disagree here and both must be diffed.
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > 1 + "
    "(SELECT AVG(l.speed) FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 999)",
    # The MIXED case for the wrapped form — one correlation key with no group and
    # nineteen with one — which is what a PARTIAL regression looks like.
    "SELECT d.driver_id AS did, d.age AS age FROM drivers d WHERE d.age > 1 + "
    "(SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id AND l.lap_id < 60) "
    "ORDER BY did",

    # SEAM AUDIT (subquery chain, pass 1) — F1/F2. TWO CORRELATED EQUALITIES ON
    # ONE BODY COLUMN. This used to be REFUSED, with
    #   "derived table '$scalar0': column 'driver_id' is produced twice;
    #    give one of them an alias"
    # — a message naming a relation the user never wrote, advising an alias they
    # cannot spell, for SQL SQLite answers (20 here).
    #
    # It is diffed rather than merely "no longer refused" because the semantics
    # are the point: two keys on one body column mean the join must test BOTH
    # (`d.driver_id = l.driver_id AND d.age = l.driver_id`). Deduplicating the
    # group key down to one and dropping a join key would also stop the refusal
    # and would answer 20 on this data by coincidence — l.driver_id and
    # d.driver_id agreeing is what the second key adds — so a suite entry that
    # only asserted "it runs" would pass a wrong fix.
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id "
    "AND l.driver_id = d.age)",
    # ...and the DISCRIMINATING form of the same shape. Above, the two outer
    # columns are compared against the same body column, so a fix that kept only
    # the first key still answers 20. Here the second key is the only thing that
    # can be wrong: `l.season = d.age` matches nothing (ages are not seasons), so
    # every correlation group is empty, every COUNT is 0, and the answer is 20
    # only if BOTH keys reached the join. Dropping the season key gives a
    # non-empty group per driver and a different answer.
    "SELECT COUNT(*) AS n FROM drivers d WHERE d.age > "
    "(SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id "
    "AND l.season = d.age)",
]

# Three entries wrap their correlated scalar in an IN, and has_in is tested
# before has_correlated, so those reach the IN guard. Same ordering rule as
# WEEK33_CORRELATED_NESTED_VOLCANO, written the same way for the same reason.
WEEK34_CORRELATED_SCALAR_VOLCANO_REJECTED = [
    (query, VOLCANO_IN if " IN (" in query else VOLCANO_CORRELATED)
    for query in WEEK34_CORRELATED_SCALAR_VEC_ONLY
]

# The scalar shapes decorrelation declines, in all four modes. A NON-AGGREGATE
# body is the important one: after the rewrite the GROUP BY makes one row per key
# by construction, so Week 31's runtime `scalar subquery returned more than one
# row` check has nowhere to live — a query SQL calls an error would return an
# arbitrary row. SQLite answers all of these, so each is a divergence.
#
# WEEK 36 NARROWED THE FIRST ONE RATHER THAN REMOVING IT. The rule is no longer
# "the select-list item must BE the aggregate" but "an aggregate, optionally
# wrapped in CONSTANT arithmetic" — because a constant wrapper can be lifted out
# of the body and re-attached outside, and a non-constant one cannot. The
# entries below pin both halves of the new boundary, so a future widening cannot
# pass silently.
WEEK34_CORRELATED_SCALAR_REFUSED = [
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT l2.speed FROM laps l2 WHERE l2.team = l.team)",
     "single aggregate"),
    # SEAM AUDIT pass 2, B-6 — one word, and it is the difference between a pin
    # and a suffix. This used to read "LIMIT cannot be decorrelated", which is a
    # tail of BOTH "a body with LIMIT cannot be decorrelated" (the EXISTS guard,
    # decorr.cc:24) and "a scalar body with LIMIT cannot be decorrelated" (the
    # scalar guard, :404). This is a SCALAR query so it hit the right one, but
    # the assertion would have passed with the scalar guard deleted and the
    # EXISTS one somehow reached. "scalar body with LIMIT" occurs at exactly one
    # refusal in src/.
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team LIMIT 1)",
     "scalar body with LIMIT cannot be decorrelated"),
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.speed > l.speed)",
     "only an equality between two columns can become a join key"),

    # WEEK 36 — TWO AGGREGATES under one wrapper. The lift is written for ONE
    # output column and ONE zero-row rule; two would need two of each, and the
    # COUNT CASE has no way to know which reference it wraps.
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT AVG(l2.speed) / COUNT(*) FROM laps l2 WHERE l2.team = l.team)",
     "may hold ONE aggregate"),
    # WEEK 36 — a wrapper node the lift does not admit. CASE is refused by NAME
    # even though it is constant-valued here: it has no vectorized kernel by
    # design, it raises three-valued questions the arithmetic path does not, and
    # no TPC-H query needs one. The whitelist is Literal + arithmetic, full stop.
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT CASE WHEN AVG(l2.speed) > 1 THEN 1 ELSE 0 END FROM laps l2 "
     "WHERE l2.team = l.team)",
     "wrapped in constant arithmetic"),
    # WEEK 36 — a wrapper naming a BODY COLUMN outside the aggregate. Refused,
    # but NOT by the wrapper rule: the Validator's grouped-reference check runs
    # first and reports the ungrouped column, which is the better diagnostic and
    # is why this entry pins THAT message. Recorded rather than "fixed" — moving
    # the wrapper check earlier would replace a precise message with a vaguer one.
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT AVG(l2.speed) + l2.lap_id FROM laps l2 WHERE l2.team = l.team)",
     "must appear in GROUP BY or be used in an aggregate function"),
]

# WEEK 36 — TPC-H Q21'S SHAPE, pinned on the F1 catalog so the boundary is a
# behavioural fact here and not only a line in the TPC-H report.
#
# Every existing correlated-inequality entry is correlated ONLY by an inequality,
# so it fails the `keys.empty()` test and would STILL be refused by any future
# work. Q21 is the shape nothing covered: a perfectly good correlated EQUALITY
# with an inequality BESIDE it --
#
#     EXISTS (SELECT * FROM lineitem l2
#             WHERE l2.l_orderkey = l1.l_orderkey       <- a join key
#               AND l2.l_suppkey != l1.l_suppkey)       <- a correlated inequality
#
# That inequality would have to ride as an ON RESIDUAL on the semi/anti join.
# It is refused today, and these entries are what a residual implementation
# would have to MOVE rather than delete -- the discipline that nothing leaves a
# rejection suite without arriving in a diffed one.
#
# Both polarities, because SEMI and ANTI part company on the residual: a semi
# join emits the probe row if SOME matching build row passes, an anti join if
# NONE does, and the two are not one boolean apart in the operator.
WEEK36_CORRELATED_RESIDUAL_REFUSED = [
    ("SELECT l.lap_id FROM laps l WHERE EXISTS "
     "(SELECT 1 FROM laps l2 WHERE l2.driver_id = l.driver_id "
     " AND l2.speed != l.speed)",
     "would have to ride as an ON residual"),
    ("SELECT l.lap_id FROM laps l WHERE NOT EXISTS "
     "(SELECT 1 FROM laps l2 WHERE l2.driver_id = l.driver_id "
     " AND l2.speed > l.speed)",
     "would have to ride as an ON residual"),
    # ...and the SCALAR family reaches the same guard, which is worth pinning
    # separately: splitCorrelation is shared, so a residual added for EXISTS
    # would change this query's behaviour too, and it must not do so silently.
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team "
     " AND l2.lap_id != l.lap_id)",
     "would have to ride as an ON residual"),
]

# SEAM AUDIT (subquery chain, pass 2) — B-1. `SELECT *` IN A BLOCK THAT HOLDS A
# LOWERED SUBQUERY.
#
# THE COVERAGE HOLE THIS SUITE EXISTS TO CLOSE, stated first because it is the
# reason a BLOCKER lived through 1336 oracle queries: every one of the 31
# WEEK34_CORRELATED_SCALAR_VEC_ONLY entries NAMES ITS SELECT LIST, and so does
# TPC-H Q17. Not one writes `SELECT *`. The star is the only thing that reads a
# block's whole output schema, so the oracle had no way to observe that
# lowerCorrelatedScalars WIDENS that schema with a synthetic relation
# (`$scalarN` -> `$k0..$k{n-1}` plus the aggregate). It did, and the star
# expanded over them:
#
#     SELECT * FROM drivers d WHERE d.age >
#       (SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id
#                                      AND l.speed > 999)
#     SwiftQL: driver_id name nationality team age $k0 COUNT(*)   -- 7 columns
#     SQLite:  driver_id name nationality team age               -- 5
#
# A WRONG ANSWER, not an error: the extra columns resolve cleanly, both engine
# legs produce them identically, and `optimized == --no-optimize` reported
# agreement. Inside a derived body the same defect became an INTERNAL error
# (`derived table 'x' was bound against a 5-column schema but planned to 7`),
# because blockOutputSchema models no subquery lowering and so never saw them.
#
# WHY EVERY ENTRY HERE IS A DIFF AND NOT A PIN: the leaked columns are named
# `$k0` / `COUNT(*)` / `AVG(...)`, which no other column in these schemas is
# called, so they cannot collapse into another key in normalize()'s dict — an
# extra column changes the row TUPLE LENGTH, and rows_equal rejects on length
# before it compares a value. That is what makes these entries discriminating
# rather than decorative, and it was verified by re-running them against a
# binary with the fix removed (all six of the first group failed).
#
# THE LAST FIVE ARE NOT ABOUT THE BUG — they are the immunity locks. The other
# three lowerings (IN semi join, NOT IN anti join, EXISTS/NOT EXISTS
# decorrelation) set the join's output_schema to children[0]'s and so never
# widen the star's domain, and an uncorrelated scalar is substituted as a
# literal with no relation added at all. They were correct before this fix and
# are correct after it; without an entry each, that is a fact nobody is
# checking. Their row sets are deliberately PARTIAL (17/3/11/9/8 of 20) — an
# all-rows or no-rows entry compares zero or few columns and would pass on an
# engine that emitted the wrong ones.
SEAM2_STAR_OVER_LOWERED_SUBQUERY_VEC_ONLY = [
    # THE BLOCKER, verbatim. COUNT with an unsatisfiable body filter, so every
    # correlation group is empty and the leaked columns arrive as NULLs.
    "SELECT * FROM drivers d WHERE d.age > (SELECT COUNT(*) FROM laps l "
    "WHERE l.driver_id = d.driver_id AND l.speed > 999) ORDER BY d.driver_id",
    # ...and a NON-COUNT aggregate, where the leaked columns carry REAL VALUES
    # rather than NULLs. Worth having both: a fix that dropped null-extended
    # columns rather than synthetic ones would pass the entry above.
    "SELECT * FROM laps l WHERE l.season = 2024 AND l.speed > 1.02 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) ORDER BY l.lap_id",
    # TWO correlated scalars in one block: two synthetic relations, at two
    # different range-table slots, stacked one join above the other. The second
    # lowering's spine is the first lowering's MERGED schema, so this is what
    # says the narrowing is carried through a stack rather than applied once.
    "SELECT * FROM laps l WHERE l.speed > (SELECT AVG(l2.speed) FROM laps l2 "
    "WHERE l2.team = l.team) AND l.speed < 1.02 * (SELECT AVG(l3.speed) "
    "FROM laps l3 WHERE l3.driver_id = l.driver_id) ORDER BY l.lap_id",
    # THE OTHER DIRECTION, and the entry that stops the fix from being "expand
    # the star over relation 0 only": a real JOIN beside the correlated scalar.
    # The star must still emit BOTH user relations and neither synthetic one.
    # (normalize() keys rows by column name, so the two `driver_id`s and the two
    # `team`s collapse on BOTH sides identically — 12 keys, not 14. That blind
    # spot is documented at normalize(); it does not weaken this entry, because
    # the columns under test are named `$k0` and `AVG(l2.speed)` and collapse
    # into nothing.)
    "SELECT * FROM drivers d JOIN laps l ON d.driver_id = l.driver_id "
    "WHERE l.season = 2024 AND l.speed > 1.05 * (SELECT AVG(l2.speed) FROM laps l2 "
    "WHERE l2.team = l.team) ORDER BY l.lap_id",
    # THE DERIVED-BODY SYMPTOM (B-1's second half). Before the fix this was not a
    # wrong answer but an INTERNAL ERROR — buildRelation's drift check caught
    # blockOutputSchema (5) disagreeing with build() (7) and refused legal SQL.
    # It is the one shape that proves the narrowing happens INSIDE the body's own
    # build() rather than at the top level only.
    "SELECT COUNT(*) AS n FROM (SELECT * FROM drivers d WHERE d.age > "
    "(SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id "
    "AND l.speed > 999)) AS x",
    # ...and the same body with its columns READ BY NAME from outside, so the
    # derived RELATION's schema is checked column-by-column and not just by
    # width. `x.name` and `x.age` only resolve if the star published exactly the
    # five columns of `drivers`, in order.
    "SELECT x.name AS nm, x.age AS ag FROM (SELECT * FROM drivers d WHERE d.age > "
    "(SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id "
    "AND l.speed > 900)) AS x ORDER BY nm",

    # --- the immunity locks: the three lowerings that never had the hole ---
    # IN -> SEMI join (output_schema is children[0]'s)
    "SELECT * FROM drivers d WHERE d.driver_id IN (SELECT l.driver_id FROM laps l "
    "WHERE l.speed > 344 AND l.season = 2024) ORDER BY d.driver_id",
    # NOT IN -> ANTI_NOT_IN join
    "SELECT * FROM drivers d WHERE d.driver_id NOT IN (SELECT l.driver_id FROM laps l "
    "WHERE l.speed > 344 AND l.season = 2024) ORDER BY d.driver_id",
    # correlated EXISTS -> decorrelated SEMI join
    "SELECT * FROM drivers d WHERE EXISTS (SELECT 1 FROM laps l "
    "WHERE l.driver_id = d.driver_id AND l.speed > 344.9) ORDER BY d.driver_id",
    # correlated NOT EXISTS -> decorrelated ANTI join
    "SELECT * FROM drivers d WHERE NOT EXISTS (SELECT 1 FROM laps l "
    "WHERE l.driver_id = d.driver_id AND l.speed > 344.9) ORDER BY d.driver_id",
    # (the fourth lowering — an UNCORRELATED scalar, materialized to a literal
    # with no relation added — is NOT here: it runs on Volcano too, so its star
    # entry lives in WEEK31_SUBQUERY_QUERIES and is diffed in all four modes.)
]

# Per-query pins, not a shared tail: these queries reach three DIFFERENT Volcano
# refusals (IN, correlated, derived), and an entry that pinned only "not
# supported on the Volcano path" would pass if the wrong one fired. Tested in
# the order Planner::plan tests them — the IN guard runs before the correlated
# one, and both run before the derived-table one.
def _seam2_volcano_pin(q: str):
    if " IN (" in q or " NOT IN (" in q:  return VOLCANO_IN
    if "FROM (SELECT" in q:               return VOLCANO_DERIVED
    return VOLCANO_CORRELATED

SEAM2_STAR_OVER_LOWERED_SUBQUERY_VOLCANO_REJECTED = [
    (q, _seam2_volcano_pin(q))
    for q in SEAM2_STAR_OVER_LOWERED_SUBQUERY_VEC_ONLY
]

# SEAM AUDIT (subquery chain, pass 2) — B-3. A CORRELATED SUBQUERY NESTED INSIDE
# A CORRELATED BODY.
#
# Every one of these was refused before the fix, with a message about a
# correlated INEQUALITY that none of them contains:
#
#     correlated subquery: only an equality between two columns can become a
#     join key (a correlated inequality has no equi-join to lower to; ...)
#
# splitCorrelation asked "does this conjunct reach outside the body?" as
# `collectSlots(c) contains -1`, and collectSlots has a THIRD producer of -1 its
# comment did not enumerate: a nested CORRELATED SubqueryExpr. That flag means
# "reaches outside the INNER body", and for a one-level reference the block it
# names is the body being split — body-local. The identical nesting under an
# UNCORRELATED IN ran and was right (three entries in
# WEEK34_CORRELATED_SCALAR_VEC_ONLY), which is what proved the shape was
# supportable and only the classification wrong.
#
# NO SUITE HELD THIS SHAPE, in either direction — checked WEEK33_DECORRELATED,
# WEEK33_CORRELATED_BINDS, WEEK34_CORRELATED_SCALAR_*,
# WEEK35_SUBQUERY_IN_DERIVED_BODY. The only nesting anywhere was
# uncorrelated-outer / correlated-inner.
#
# Row sets are PARTIAL by construction (11, 9, 13, 11 of 20; 2329 of 10000), not
# 0 and not all: a nesting that silently dropped the inner subquery would answer
# 20 here, and one that dropped the outer would answer 0.
SEAM2_NESTED_CORRELATION_VEC_ONLY = [
    # correlated EXISTS inside a correlated EXISTS body. The inner one keys on
    # `l.lap_id`, a column of the MIDDLE body's relation, which is what makes it
    # level 1 there and therefore lowerable by the middle body's own build().
    "SELECT d.name AS nm FROM drivers d WHERE EXISTS (SELECT 1 FROM laps l "
    "WHERE l.driver_id = d.driver_id AND EXISTS (SELECT 1 FROM laps l2 "
    "WHERE l2.lap_id = l.lap_id AND l2.speed > 344.9)) ORDER BY nm",
    # ...and the ANTI polarity of the OUTER one. SEMI and ANTI part company in
    # the operator, so a nesting that worked for one is not evidence for the
    # other. 9 rows, the complement of the 11 above.
    "SELECT d.name AS nm FROM drivers d WHERE NOT EXISTS (SELECT 1 FROM laps l "
    "WHERE l.driver_id = d.driver_id AND EXISTS (SELECT 1 FROM laps l2 "
    "WHERE l2.lap_id = l.lap_id AND l2.speed > 344.9)) ORDER BY nm",
    # the inner EXISTS keyed on a DIFFERENT column of the middle relation, with
    # a local predicate beside it in the middle body — so the middle body's own
    # WHERE holds both a lowered subquery and an ordinary conjunct.
    "SELECT d.name AS nm FROM drivers d WHERE EXISTS (SELECT 1 FROM laps l "
    "WHERE l.driver_id = d.driver_id AND l.speed > 344 AND EXISTS "
    "(SELECT 1 FROM laps l2 WHERE l2.driver_id = l.driver_id "
    "AND l2.season = 2024 AND l2.speed > 344.5)) ORDER BY nm",
    # a correlated SCALAR inside a correlated EXISTS body: the other lowering,
    # reached by the same classification. Two different rewrites stacked on two
    # different spines, the inner one running during the middle body's build().
    # 1.104 rather than a round number because 1.09 returns all 20 rows and would
    # pass against an engine that dropped the predicate.
    "SELECT d.name AS nm FROM drivers d WHERE EXISTS (SELECT 1 FROM laps l "
    "WHERE l.driver_id = d.driver_id AND l.speed > 1.104 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)) ORDER BY nm",
    # THE OTHER NESTING ORDER — a correlated EXISTS inside a correlated SCALAR
    # body, which is not the same code path: the scalar lowering rewrites the
    # body's select list and GROUP BY around whatever its WHERE was left holding.
    # DISCRIMINATING: the same query without the inner EXISTS answers 2555, so
    # the 2329 here is evidence the inner subquery ran and changed the groups,
    # not merely that the outer one did.
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > 1.05 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team "
    "AND EXISTS (SELECT 1 FROM drivers d3 WHERE d3.driver_id = l2.driver_id "
    "AND d3.age > 30))",
    # a correlated scalar inside a correlated EXISTS where the inner body reads
    # the middle relation in BOTH the key and a local predicate, and the outer
    # comparison spans two seasons — 9 rows.
    "SELECT d.name AS nm FROM drivers d WHERE EXISTS (SELECT 1 FROM laps l "
    "WHERE l.driver_id = d.driver_id AND l.season = 2024 AND l.speed > "
    "(SELECT MAX(l2.speed) FROM laps l2 WHERE l2.driver_id = l.driver_id "
    "AND l2.season = 2023)) ORDER BY nm",
]

SEAM2_NESTED_CORRELATION_VOLCANO_REJECTED = [
    (q, VOLCANO_CORRELATED) for q in SEAM2_NESTED_CORRELATION_VEC_ONLY
]

# !! THE COUPLED HALF OF B-3, and the entry that matters most in this file.
#
# The depth refusal (`outer_side->id.level() != 1` in splitCorrelation) was
# UNREACHABLE before the fix above — pass 2's B-5.3 enumerated every route to a
# level-2 reference and found each one closed by something earlier, the nearest
# being B-3's own misclassification. Fixing B-3 OPENS that route, and this guard
# is what keeps it safe: levels are NOT decremented when the middle body is
# decorrelated, so a stale level-2 reference would otherwise reach
# leftKeyIndices and be read as a slot of the wrong range table.
#
# It had ZERO coverage — no diffed entry, no rejection entry, nothing — because
# nothing could make it fire. A reachable guard that is untested is strictly
# worse than an unreachable one that is wrong, so these entries are the
# condition on which B-3 closes at all.
#
# THE PIN IS SPECIFIC TO THIS GUARD, not a shared tail: grep says "more than one
# level out" occurs at exactly one refusal in src/. Three distinct states make
# these entries FAIL, which is the question worth asking of a brand-new pin:
#   1. the pre-fix engine — verified: it answers the CORRELATED-INEQUALITY
#      message for both of these, so the pin fails rather than passing for the
#      wrong reason. That is the demonstration, not an argument;
#   2. the guard deleted — the level-2 id reaches
#      `localSlot("splitCorrelation")`, which throws "internal: splitCorrelation
#      read a correlated column reference as a local relation slot (query level
#      2)". Different message, entry fails;
#   3. the query ceasing to BE two levels out (an edit to `d.` here) — it then
#      returns rows and run_rejection_suite reports "expected a rejection, got
#      rows".
#
# Vectorized only, same reason as WEEK34_CORRELATED_SCALAR_REFUSED: the Volcano
# path refuses a correlated subquery outright and would assert the wrong
# refusal. SQLite answers both of these (20 each), so each is a recorded
# divergence, not a claim about SQL.
SEAM2_CORRELATION_DEPTH_REFUSED = [
    # EXISTS nested in EXISTS, inner ref reaching TWO blocks out (`d`, not `l`).
    # One character apart from the first SEAM2_NESTED_CORRELATION entry's family
    # and on the other side of the boundary, which is the point.
    ("SELECT COUNT(*) FROM drivers d WHERE EXISTS "
     "(SELECT 1 FROM laps l WHERE l.driver_id = d.driver_id "
     " AND EXISTS (SELECT 1 FROM laps l2 WHERE l2.driver_id = d.driver_id))",
     "more than one level out"),
    # ...and the SCALAR family, pinned separately for the same reason
    # WEEK36_CORRELATED_RESIDUAL_REFUSED pins its scalar entry separately:
    # splitCorrelation is SHARED, so a future change to the depth rule for
    # EXISTS changes this query's behaviour too and must not do so silently.
    ("SELECT COUNT(*) FROM drivers d WHERE EXISTS "
     "(SELECT 1 FROM laps l WHERE l.driver_id = d.driver_id "
     " AND l.speed > (SELECT AVG(l2.speed) FROM laps l2 "
     "                WHERE l2.driver_id = d.driver_id))",
     "more than one level out"),
]

# ─── B3-2: a STRING join key against a numeric one is REFUSED, at all four
# ─── JoinKey producers ──────────────────────────────────────────────────────
#
# WHAT WAS WRONG. Semi-join keys are matched as SERIALIZED TEXT, which carries no
# type tag. So a STRING column joined against a numeric one matched only when the
# string was already the number's canonical rendering: `'16'` matched the INT 16,
# `'016'` did not, and SQLite's affinity converts and matches BOTH. No error, no
# plan difference — a wrong row count. The fix refuses in all four producers
# rather than adding affinity, via `Validator::validateJoinKeyTypes` walking the
# FINISHED plan at the end of `LogicalPlanBuilder::build`.
#
# THE COVERAGE THIS DELIBERATELY GIVES UP, recorded because giving it up was a
# choice and a later reader is owed the choice rather than the outcome. The
# canonical-text half of the shape — `l.driver_id IN (SELECT '16' ...)` —
# returned 495 and MATCHED SQLITE before this change. It is now REFUSED. That
# answer was correct by accident: it agreed only because the literal happened to
# be the INT's canonical rendering, and its `'016'` twin was silently wrong by
# 495 rows in each direction on this same shipped catalog. Four doors with one
# behaviour was the requirement; leaving the canonical spelling answering would
# have been a fifth behaviour, decided by how the user spelled a literal. So a
# previously-correct answer was traded for a uniform refusal, on purpose.
#
# WHY THE PINS CARRY THEIR FULL CLAUSE UP TO ": cannot join". `"EXISTS
# subquery"` is a SUBSTRING of both `"IN / EXISTS subquery"` and `"NOT EXISTS
# subquery"`, so a bare pin is satisfied by the WRONG message and the entry
# stops discriminating between two different producers. That is the same defect
# class as the shared `"not supported on the Volcano path"` tail the VOLCANO_*
# constants above exist to undo, and it is not hypothetical — it was written and
# caught here. `assert_b32_pins_discriminate()` below re-proves it by execution
# rather than leaving it as a comment.
#
# VECTORIZED-ONLY, and the Volcano twin asserts a DIFFERENT message on purpose:
# `Planner::plan` refuses IN and correlated subqueries by CAPABILITY before a
# join key is ever built, so the four modes do not share one message here. The
# split follows planner.cc's guard order (multi-way, IN, correlated, derived),
# same rule the other _VOLCANO_REJECTED lists follow.
B32_JOIN_KEY_TYPE_REJECTED = [
    ("SELECT COUNT(*) FROM laps l WHERE l.team IN "
     "(SELECT d.driver_id FROM drivers d)",
     "IN / EXISTS subquery: cannot join a STRING column with a numeric one"),

    ("SELECT COUNT(*) FROM laps l WHERE l.team NOT IN "
     "(SELECT d.driver_id FROM drivers d)",
     "NOT IN subquery: cannot join a STRING column with a numeric one"),

    ("SELECT COUNT(*) FROM laps l WHERE EXISTS "
     "(SELECT 1 FROM drivers d WHERE d.driver_id = l.team)",
     "IN / EXISTS subquery: cannot join a STRING column with a numeric one"),

    ("SELECT COUNT(*) FROM laps l WHERE NOT EXISTS "
     "(SELECT 1 FROM drivers d WHERE d.driver_id = l.team)",
     "NOT EXISTS subquery: cannot join a STRING column with a numeric one"),

    # the correlated-scalar producer, which names no subquery form in its message
    ("SELECT COUNT(*) FROM laps l WHERE "
     "(SELECT COUNT(*) FROM drivers d WHERE d.driver_id = l.team) > 0",
     "join key: cannot join a STRING column with a numeric one"),

    # THE ENTRY THAT WAS SILENTLY WRONG. `'016'` is not the INT's canonical
    # rendering, so this returned 0 where SQLite returns 495; its `'16'` twin
    # returned 495 and agreed. Same shape, opposite errors, neither visible.
    # Keep this one under any trimming.
    ("SELECT COUNT(*) FROM laps l WHERE l.driver_id IN "
     "(SELECT '016' AS s FROM drivers d)",
     "IN / EXISTS subquery: cannot join a STRING column with a numeric one"),

    # THE ENTRY THE AST LOOP STRUCTURALLY CANNOT SEE. The offending key is
    # produced behind a LogicalDerived, so only a walk over the FINISHED plan
    # reaches it — which is the whole reason the check moved off the AST. Keep
    # this one under any trimming too.
    ("SELECT COUNT(*) FROM laps l WHERE l.driver_id IN "
     "(SELECT x.c FROM (SELECT d.team AS c FROM drivers d) x)",
     "IN / EXISTS subquery: cannot join a STRING column with a numeric one"),
]

# The same seven queries on Volcano, refused EARLIER and for a different reason.
# Built from the list above BY IDENTITY (index, not re-typed text) so the two
# suites cannot drift apart: an edit to a query above is an edit here.
#
# The guard that fires is planner.cc's, top to bottom. Rows 0, 1, 5, 6 are IN /
# NOT IN forms and hit the IN guard; rows 2, 3, 4 are EXISTS / NOT EXISTS /
# correlated-scalar and hit the correlated one. Written as that classification
# rather than as a hand-kept index list, so a query added above is classified by
# its own text.
def _b32_volcano_expectation(query: str) -> str:
    # `" IN (SELECT"` matches `NOT IN (SELECT` too — there is a space before the
    # `IN` either way — so one test covers both IN forms. Everything else here
    # (EXISTS, NOT EXISTS, correlated scalar) carries a correlated reference and
    # reaches the correlated guard.
    return VOLCANO_IN if " IN (SELECT" in query.upper() else VOLCANO_CORRELATED


B32_JOIN_KEY_TYPE_VOLCANO_REJECTED = [
    (query, _b32_volcano_expectation(query))
    for query, _ in B32_JOIN_KEY_TYPE_REJECTED
]


def assert_b32_pins_discriminate():
    """Prove each B3-2 pin matches ITS OWN message and NO OTHER entry's.

    run_rejection_suite only asks "does the actual message contain the expected
    substring". That is satisfied by a pin so short it also matches a sibling —
    which is exactly how `"EXISTS subquery"` passes against `"IN / EXISTS
    subquery"` and against `"NOT EXISTS subquery"`, three different producers
    behind one green tick. The suite above has four distinct producer messages
    and would not notice three of them being replaced by a fourth.

    So this collects every entry's ACTUAL message once and cross-checks the full
    pin matrix: pin[i] must match message[i] and must NOT match message[j != i]
    unless the two entries genuinely share a producer (rows 0, 2 and 5, 6 do —
    IN and EXISTS are one producer, and the two IN-literal shapes are another
    instance of it). Shared pins are compared as equal-expectation pairs rather
    than exempted by index.
    """
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    actual = []
    findings = []
    for query, expected in B32_JOIN_KEY_TYPE_REJECTED:
        try:
            run_swiftql(query, VEC)
            findings.append(f"expected a rejection, got rows: {query[:60]}")
            actual.append(None)
        except RuntimeError as e:
            actual.append(str(e))
        except Exception as e:                                # pragma: no cover
            findings.append(f"unexpected error kind {e!r}: {query[:60]}")
            actual.append(None)

    for i, (qi, pin_i) in enumerate(B32_JOIN_KEY_TYPE_REJECTED):
        if actual[i] is None:
            continue
        if pin_i not in actual[i]:
            findings.append(f"pin {i} does not match its own message: {pin_i!r}")
        for j, (_qj, pin_j) in enumerate(B32_JOIN_KEY_TYPE_REJECTED):
            if i == j or actual[j] is None or pin_i == pin_j:
                continue
            if pin_i in actual[j]:
                findings.append(
                    f"pin {i} ({pin_i!r}) ALSO matches entry {j}'s message — "
                    f"it cannot tell the two producers apart")

    print(f"\n--- B3-2 join-key pin discrimination ---")
    print(f"{len(B32_JOIN_KEY_TYPE_REJECTED)} pins cross-checked against "
          f"{sum(a is not None for a in actual)} actual messages")
    for f in findings:
        print(f"  FINDING  {f}")
    if not findings:
        print("  clean — every pin matches its own producer and no other")
    return findings

# ─── E-10: the vectorized path REFUSES an INT it cannot materialize into a
# ─── DOUBLE result column, instead of silently changing it ──────────────────
#
# A chunk column holds ONE type, so an expression whose branches mix INTEGER and
# REAL (typically a CASE) is stored as REAL. `appendColumnValue`'s DOUBLE arm
# used to convert the INT silently. It now throws (`narrowToDoubleColumn`,
# src/execution/vec_types.h).
#
# THE BOUND IS 1e15, NOT 2^53, and the difference is the whole reason entry
# `e10_render_witness` exists. TWO things have to survive a round trip, and they
# fail at different magnitudes:
#   * the VALUE — fails above 2^53 (9007199254740993 becomes ...92);
#   * the RENDERING — `Value::toString()` uses `%.15g`, which flips to exponent
#     form at 1e15, so 1000000000000001 prints as `1e+15` while the INT prints
#     all sixteen digits.
# 1e15 < 2^53, so the smaller bound is binding and one comparison covers both.
# A suite that witnessed only 2^53 would miss the render half entirely —
# `e10_render_witness` is BELOW 2^53 and is refused anyway.
#
# THE PAIRING IS THE USUAL `_VEC_ONLY` / `_VOLCANO_REJECTED` SHAPE WITH THE
# SIDES SWAPPED. Everywhere else in this file the vectorized path is the capable
# one and Volcano refuses. Here Volcano is the one that answers — it holds a
# `Value`, not a typed column, so it never narrows — and the two VECTORIZED
# modes are the ones pinned to refuse.
#
# !! THESE ARE NOT THE QUERIES AS FIRST SPECIFIED, AND THE DIFFERENCE IS
# LOAD-BEARING. The first two were handed over as `ORDER BY c LIMIT 2` —
# ASCENDING. Run: they do NOT refuse, and all four modes return `0.5, 0.5`
# matching SQLite. The deterministic LIMIT cut is a BOUNDED TOP-N, so with an
# ascending sort the two smallest rows win and the big integer is discarded
# before anything materializes it. As written they would have been:
#   * dead as refusal pins — pinned to a message that is never raised; and
#   * VACUOUS as diffed entries — the Volcano leg's output is `0.5, 0.5`, which
#     agrees with SQLite whether or not the integer survives, so the entry could
#     not witness the thing it is named for.
# `DESC` fixes both at once: the two LARGEST rows win, so the integer is IN the
# output (the Volcano diff now actually compares 9007199254740993 against
# SQLite's) and it reaches the materialization the vectorized path refuses.
# The ascending form is kept — as `e10_guard_cut_before_materialize` below,
# where it belongs, because the behaviour it pins is real and worth holding.
E10_VOLCANO_ONLY = [
    # THE VALUE WITNESS: above 2^53, so the double cannot hold it.
    "SELECT CASE WHEN round > 10 THEN 9007199254740993 ELSE 0.5 END AS c "
    "FROM laps ORDER BY c DESC LIMIT 2",

    # THE RENDER WITNESS: BELOW 2^53 — the double holds this value exactly — but
    # at/above 1e15, so %.15g renders it `1e+15` instead of all sixteen digits.
    # This is the entry a 2^53-only bound would let through.
    "SELECT CASE WHEN round > 10 THEN 1000000000000001 ELSE 0.5 END AS c "
    "FROM laps ORDER BY c DESC LIMIT 2",

    # THE ROW-COUNT WITNESS: ...93 and ...92 are distinct INTs that collapse to
    # ONE double, so a silent conversion returns 2 rows where SQLite returns 3.
    "SELECT DISTINCT CASE WHEN lap_id=2 THEN 9007199254740993 "
    "WHEN lap_id=8 THEN 9007199254740992 ELSE 0.5 END AS c "
    "FROM laps WHERE lap_id < 10 ORDER BY c",

    # THE SECOND SITE: aggregate materialization, not VecProject. The guard has
    # to be at appendColumnValue for this to be covered by the same fix.
    "SELECT MAX(CASE WHEN lap_id=2 THEN 9007199254740993 ELSE 0.5 END) AS m "
    "FROM laps WHERE lap_id < 10",
]

# Built from the list above BY IDENTITY so the two halves cannot drift: an edit
# to a query there is an edit here. The pin is the stable clause of the message;
# the integer that follows it differs per entry (entry 3 names ...92, not ...93,
# because that is the value the column reaches first).
E10_VECTORIZED_REFUSED = [
    (query, "cannot materialize the integer") for query in E10_VOLCANO_ONLY
]

# ALL FOUR MODES, diffed against SQLite. Without these the block above shows only
# that SOMETHING refuses; these are what show the refusal is exactly one integer
# wide and does not fire where it must not.
E10_BOUNDARY_GUARDS = [
    # ONE BELOW THE BOUND. 999999999999999 is 15 digits, so %.15g still prints it
    # in full and the double holds it exactly — every mode must ANSWER.
    # DESC deliberately, for the same reason as above: ascending would return
    # `0.5, 0.5` and this guard would pass without the integer ever appearing in
    # the output it is meant to be guarding.
    "SELECT CASE WHEN round > 10 THEN 999999999999999 ELSE 0.5 END AS c "
    "FROM laps ORDER BY c DESC LIMIT 2",

    # the ordinary mixed-type CASE, far below any bound: the common case must not
    # have become an error
    "SELECT DISTINCT CASE WHEN lap_id=2 THEN 1 WHEN lap_id=8 THEN 1.0 "
    "ELSE 0.5 END AS c FROM laps WHERE lap_id < 10 ORDER BY c",

    # THE LEG THAT WAS ALREADY CORRECT, and half of a self-contradiction. Before
    # the fix the vectorized path answered `SELECT DISTINCT e` = 2 rows and
    # `COUNT(DISTINCT e)` = 3 for the SAME expression, because COUNT(DISTINCT)
    # keys off the pre-conversion `Value` and never narrows. Post-fix the DISTINCT
    # leg refuses and this one still answers 3: one answer to the question, or
    # none — never two. This must stay 3 in all four modes.
    "SELECT COUNT(DISTINCT CASE WHEN lap_id=2 THEN 9007199254740993 "
    "WHEN lap_id=8 THEN 9007199254740992 ELSE 0.5 END) AS n "
    "FROM laps WHERE lap_id < 10",

    # THE SCOPE OF THE REFUSAL, and the query that was handed over as a refusal
    # pin. Same offending integer as the value witness, ASCENDING: the bounded
    # top-N cuts those rows before anything materializes them, so all four modes
    # answer `0.5, 0.5` and agree with SQLite. The refusal is scoped to values
    # that REACH the output column, which is the correct scope and is worth
    # pinning — a future change that widened it to "any INT anywhere in the
    # expression" would fail here, and a future change that dropped the top-N
    # would turn this into an error.
    "SELECT CASE WHEN round > 10 THEN 9007199254740993 ELSE 0.5 END AS c "
    "FROM laps ORDER BY c LIMIT 2",
]

WEEK34_DISTINCT_AGG_QUERIES = [
    # the plain grouped shape (TPC-H Q16)
    "SELECT team, COUNT(DISTINCT driver_id) AS d FROM laps GROUP BY team ORDER BY team",
    # THE DEDUPE TEST. extractAggregates dedupes specs by aggregateOutputName,
    # which IS exprToString, so an exprToString that forgot DISTINCT collapses
    # these two select items into ONE spec and one output column that both read.
    # Without this query that bug is invisible: every other query here still
    # returns the right single number.
    "SELECT COUNT(driver_id) AS a, COUNT(DISTINCT driver_id) AS b FROM laps",
    # THE KEY-ENCODING TEST, and the reason it is a DOUBLE expression. Week 27
    # measured `sector_1 + sector_2` at 3245 distinct values against 2526
    # distinct %.15g texts over this dataset, so a distinct set keyed on
    # Value::toString() is off by 719 here and by nothing anywhere else.
    "SELECT COUNT(DISTINCT sector_1 + sector_2) AS d FROM laps",
    # global, and empty-input: COUNT(DISTINCT x) over zero rows is 0, not NULL
    "SELECT COUNT(DISTINCT team) AS d FROM laps",
    "SELECT COUNT(DISTINCT team) AS d FROM laps WHERE season = 1900",
    # NULLs are excluded (x / 0 is NULL in this dialect), while COUNT(*) is not
    "SELECT COUNT(*) AS a, COUNT(DISTINCT round / (season - season)) AS b FROM laps",
    # grouped, with the distinct column also grouped on — the degenerate case
    "SELECT team, COUNT(DISTINCT team) AS d FROM laps GROUP BY team ORDER BY team",
]

# Refused in all four modes: these are LANGUAGE refusals, not capability ones,
# so they fire identically on every path. The diffed oracle cannot hold a query
# that errors, which is why the message is pinned here instead.
WEEK34_DISTINCT_AGG_REFUSED = [
    ("SELECT COUNT(DISTINCT *) FROM laps",
     "COUNT(DISTINCT *) is not supported"),
    ("SELECT SUM(DISTINCT speed) FROM laps",
     "DISTINCT is supported inside COUNT only"),
    ("SELECT AVG(DISTINCT speed) FROM laps",
     "DISTINCT is supported inside COUNT only"),
    # MIN(DISTINCT x) is a legal SQL no-op. Refused rather than silently
    # accepted, because accepting it invites the reader to believe SUM/AVG work.
    ("SELECT MIN(DISTINCT speed) FROM laps",
     "DISTINCT is supported inside COUNT only"),
]

WEEK30_REJECTED_QUERIES = [
    # position: WHERE and HAVING only. All three are refused by the fail-closed
    # allow_subqueries flag rather than by enumerating what is allowed
    ("SELECT (SELECT AVG(speed) FROM laps) FROM drivers",
     "SELECT: subqueries are supported in WHERE and HAVING only"),
    ("SELECT team FROM laps GROUP BY (SELECT AVG(speed) FROM laps)",
     "GROUP BY: subqueries are supported in WHERE and HAVING only"),
    ("SELECT team FROM laps ORDER BY (SELECT AVG(speed) FROM laps)",
     "ORDER BY: subqueries are supported in WHERE and HAVING only"),
    # dispatch site 18, in the shape that file already uses for AggregateExpr
    ("SELECT l.team FROM laps l JOIN drivers d "
     "ON l.driver_id = d.driver_id AND EXISTS (SELECT * FROM laps)",
     "JOIN ON: subqueries are not supported in a join condition"),
    # arity is decidable at bind time; cardinality ("more than one row") is
    # Week 31's runtime check
    ("SELECT team FROM laps WHERE speed > (SELECT speed, team FROM laps)",
     "scalar subquery must return exactly one column"),
    ("SELECT team FROM laps WHERE season IN (SELECT * FROM drivers)",
     "IN subquery must return exactly one column"),
    # the NESTED query's own faults outrank the refusal, which is what proves
    # validateExpr handed the body to a fresh validation against its own schema
    # instead of descending with the outer one
    ("SELECT team FROM laps WHERE EXISTS (SELECT * FROM nosuchtable)",
     "Table not found: 'nosuchtable'"),
    ("SELECT team FROM laps WHERE EXISTS (SELECT * FROM drivers WHERE nosuchcol = 1)",
     "column not found: 'nosuchcol'"),
    ("SELECT team FROM laps WHERE EXISTS (SELECT name FROM drivers GROUP BY age)",
     "must appear in GROUP BY"),
    # ambiguity is still decided per scope
    ("SELECT lap_id FROM laps l WHERE EXISTS "
     "(SELECT * FROM laps l2 JOIN drivers d ON l2.driver_id = d.driver_id "
     " WHERE team = 'Ferrari')",
     "ambiguous column reference"),
    # the constant-list IN is untouched: this is still a parse error, from a
    # different production
    ("SELECT team FROM laps WHERE season IN (speed)",
     "IN accepts a list of constant values only"),

    # --- round 1 ---
    # a NESTED key-less inner join. `l` is the OUTER relation, so there is no
    # equality between d and p at all — a cartesian product, which
    # classifyJoinCondition exists to refuse. Treating the correlated ref as a
    # key operand fabricated JoinKey{driver_id, driver_id, from_slot=0}, joining
    # the inner `d` to `p` on a predicate the user never wrote, and a non-empty
    # key list bypassed the refusal. SQLite answers it; the property under test is
    # SwiftQL's own refusal, and from Week 31 the alternative is wrong rows
    ("SELECT lap_id FROM laps l WHERE EXISTS "
     "(SELECT 1 FROM drivers d JOIN laps p ON p.driver_id = l.driver_id)",
     "at least one equality"),
    # the ORDER BY position rule inspected only the ROOT of the expression, so a
    # subquery one level down was invisible. It routes through validateExpr now,
    # whose allow_subqueries=false default is checked at every node
    ("SELECT lap_id FROM laps ORDER BY lap_id + (SELECT MAX(age) FROM drivers)",
     "ORDER BY: subqueries are supported in WHERE and HAVING only"),
    ("SELECT lap_id FROM laps ORDER BY "
     "CASE WHEN lap_id > (SELECT MAX(age) FROM drivers) THEN 1 ELSE 0 END",
     "ORDER BY: subqueries are supported in WHERE and HAVING only"),

    # --- round 2 ---
    # the SUM/AVG type check indexed THIS statement's join list with a slot that
    # belongs to the enclosing one, so the same illegal aggregate was caught or
    # skipped depending on the INNER query's own join order. All three spellings
    # must now give the same answer
    ("SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE EXISTS (SELECT SUM(d.name) FROM laps y JOIN drivers x "
     "ON x.driver_id = y.driver_id)",
     "SUM() requires a numeric column, but 'name' is of type STRING"),
    ("SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE EXISTS (SELECT SUM(d.name) FROM drivers x JOIN laps y "
     "ON x.driver_id = y.driver_id)",
     "SUM() requires a numeric column, but 'name' is of type STRING"),
    ("SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
     "WHERE EXISTS (SELECT SUM(d.name) FROM drivers x)",
     "SUM() requires a numeric column, but 'name' is of type STRING"),
    # exprKey encoded a ColumnRef as slot#name with no level, so a CORRELATED
    # expression group key satisfied an ungrouped LOCAL reference. The
    # plain-column spelling below is round 1's fix; adding `+ 1` to both sides
    # routed the identical pair through exprKey and the refusal disappeared
    ("SELECT lap_id FROM laps l WHERE EXISTS "
     "(SELECT driver_id + 1 FROM drivers d GROUP BY l.driver_id + 1)",
     "SELECT column 'driver_id' must appear in GROUP BY"),
    ("SELECT lap_id FROM laps l WHERE EXISTS "
     "(SELECT driver_id FROM drivers d GROUP BY l.driver_id)",
     "SELECT column 'driver_id' must appear in GROUP BY"),
]

# Week 27 (multi-way join execution). Shapes that Week 26 could only bind now
# return rows, so they move out of the rejection list and are diffed against
# SQLite like everything else. These four run in ALL modes — multi-KEY joins and
# residual ON conjuncts are not vectorized-only; only the relation count is.
WEEK27_JOIN_QUERIES = [
    # multi-key equi-join (TPC-H Q9's shape). Both keys must constrain the match
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d "
    "ON l.driver_id = d.driver_id AND l.team = d.team",
    # a non-equality ON conjunct beside a key: the TPC-H Q21 shape, executed as a
    # post-join residual rather than refused
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d "
    "ON l.driver_id = d.driver_id AND l.speed > d.age",
    # a single-relation residual, which predicate assignment pushes onto the
    # drivers scan — same answer whether or not it is pushed
    "SELECT d.name AS n, COUNT(*) AS c FROM laps l JOIN drivers d "
    "ON l.driver_id = d.driver_id AND d.nationality = 'British' "
    "GROUP BY d.name ORDER BY n",
    # both operands of an ON conjunct in one relation: a local filter, which
    # Week 26 rejected because rerouting it across sides would have changed it
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d "
    "ON l.driver_id = d.driver_id AND l.sector_1 < l.sector_2",
    # two STRING keys. The tuple encoding has to be injective for whatever bytes
    # a CSV cell contains, and a bare per-field sentinel is not: it only
    # separates fields that do not themselves contain it
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d "
    "ON l.driver_id = d.driver_id AND l.team = d.team AND d.name = d.name",
    # DOUBLE keys are compared through their serialized text, so the text has to
    # identify the double rather than merely display it — SQLite compares REALs
    # exactly, and every sector value here is distinct at the 15th digit or not
    # at all
    "SELECT COUNT(*) AS c FROM laps l1 JOIN laps l2 ON l1.sector_1 = l2.sector_1",
    "SELECT COUNT(*) AS c FROM laps l1 JOIN laps l2 "
    "ON l1.sector_1 = l2.sector_1 AND l1.sector_2 = l2.sector_2",
]

# Week 27 round 2: the group-by and dedup key serializers hold the same contract
# the join key does — the serialized text has to IDENTIFY a value, not display it
# — and neither had been brought along. Both of these are wrong answers on the
# shipped 10k-row dataset, in every mode, so only SQLite can see them: the four
# modes agree with each other and disagree with the oracle.
WEEK27_KEY_ENCODING_QUERIES = [
    # `%.15g` renders 3245 distinct doubles as 2526 distinct texts, so DISTINCT
    # and GROUP BY collapse 706 pairs that SQLite (which compares REALs exactly)
    # keeps apart — and every collapsed group's COUNT(*) is wrong with it
    "SELECT DISTINCT sector_1 + sector_2 AS s FROM laps ORDER BY s",
    "SELECT sector_1 + sector_2 AS s, COUNT(*) AS c FROM laps GROUP BY s ORDER BY s",
    # a NULL key member and the literal string 'NULL' must not dedup together.
    # A CASE with no ELSE is the reachable way to get a NULL into a dedup key
    "SELECT DISTINCT CASE WHEN speed > 300 THEN 'NULL' END AS x FROM laps ORDER BY x",
    "SELECT CASE WHEN speed > 300 THEN 'NULL' END AS x, COUNT(*) AS c FROM laps "
    "GROUP BY x ORDER BY x",
    # the same value reached through a group key and through a join key must
    # agree with each other as well as with SQLite
    "SELECT l.sector_1 AS s, COUNT(*) AS c FROM laps l JOIN drivers d "
    "ON l.driver_id = d.driver_id GROUP BY l.sector_1 ORDER BY s LIMIT 50",
]

# Three or more relations execute on the VECTORIZED path only: Planner::plan
# builds exactly one join and row/Volcano never gains multi-way execution
# (README, Week 27). This is the first deliberate per-mode capability difference
# in the project, so it needs both halves — the rows where it runs
# (MULTIWAY_QUERIES, diffed against SQLite in the two vec modes) and the refusal
# where it does not (MULTIWAY_VOLCANO_REJECTED, in the two Volcano modes).
WEEK27_MULTIWAY_QUERIES = [
    # THE slot query. The third join's key is `team` at relation slot 1, while
    # the left input's MERGED schema holds `team` at slot 0 first — so a
    # bare-name lookup joins l1.team instead of l2.team and returns 31440
    # plausible rows instead of 32193. Only SQLite can tell the two apart
    "SELECT COUNT(*) AS c FROM laps l1 JOIN laps l2 ON l1.lap_id = l2.driver_id "
    "JOIN drivers d ON l2.team = d.team",
    # three relations, two tables, projected columns from all three
    "SELECT l.team AS t, d.name AS n, d2.name AS n2 FROM laps l "
    "JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team "
    "WHERE l.lap_id < 20 ORDER BY t, n, n2",
    # a multi-way tree whose predicates land on three different relations, plus
    # a residual ON conjunct on the last one — predicate assignment across a
    # left-deep spine, which is where routing a conjunct to the wrong relation
    # would show up as a wrong count
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team AND d2.age > 25 "
    "WHERE l.season = 2024 AND d.nationality IN ('British','German')",
    # a residual belonging to an EARLIER relation, attached to a LATER join's ON
    # clause: predicate assignment must walk past the join that owns the clause
    # and land the conjunct on relation 1's scan, not relation 2's
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team AND d.age > 30",
    # aggregation over a three-relation join, grouped by a column of the middle
    # relation — the group key has to resolve to slot 1, not slot 0's same name
    "SELECT d.team AS t, COUNT(*) AS c, MIN(l.speed) AS lo FROM laps l "
    "JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team GROUP BY d.team ORDER BY t",
    # four relations: nothing in lowering is 3-specific, and a fourth is what
    # proves the recursion rather than a special case
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.driver_id = d2.driver_id "
    "JOIN laps l2 ON d2.driver_id = l2.driver_id WHERE l.lap_id < 5",
]

# Week 28 gives multi-way joins a cost-chosen order. Reordering changes plan
# shape and never results, so these are diffed against SQLite exactly like the
# Week 27 block — the point is that they are shapes the search actually REORDERS,
# where a mis-oriented key, a mis-stamped merged schema or a lost residual would
# produce plausible rows rather than an error. The suite runs them in both
# vectorized modes, and the second is --no-optimize, which keeps the WRITTEN
# order: that pairing is what makes this file able to catch a reordering that
# changes an answer.
WEEK28_JOIN_ORDER_QUERIES = [
    # Star centred on l: drivers (20 rows) is adjacent to both laps scans.
    # Written order joins the two laps scans first — driver_id NDV 20 over 10k
    # rows each — and the search puts drivers second instead.
    "SELECT COUNT(*) AS c FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id "
    "JOIN drivers d ON l.driver_id = d.driver_id WHERE l.lap_id < 200",
    # THE non-zero-leftmost query: the search leads with drivers@1, so relation 0
    # is NOT at the bottom of the spine. That is the case the merged-schema
    # stamping and the bottom-join from_slot = 0 rewrite exist for, and no
    # written-order tree can produce it. Wrong either way returns rows.
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team",
    # same reordering, but projecting columns from relations 0 and 2 and sorting
    # them: column identity has to survive a merged schema rebuilt in a new order
    "SELECT l.team AS t, d2.name AS n FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team WHERE l.season = 2022 AND l.round < 3 ORDER BY t, n",
    # triangle: every order is legal, and the last relation added carries TWO
    # keys where the first carried one — so the ordering decides which join is
    # composite, exercising the shared key encoding from the other direction
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team AND l.driver_id = d2.driver_id",
    # a residual ON conjunct and a pushed WHERE on a reordered tree: predicate
    # assignment runs BEFORE enumeration and every conjunct must survive the fold
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team AND d2.age > 25 WHERE l.season = 2024",
    # aggregation over a reordered tree, grouped by a middle relation's column
    "SELECT d.nationality AS nat, COUNT(*) AS c, MIN(l.speed) AS lo FROM laps l "
    "JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team GROUP BY d.nationality ORDER BY nat",
    # CONTROL: the same shape as the third query with a selective filter on laps
    # instead. That filter drops laps below drivers, so leading with laps is now
    # correct and the search keeps the written order — proving the decision reacts
    # to FILTERED cardinality rather than to table size, and that "reordered" is
    # not simply what this pass always does.
    "SELECT l.team AS t, d2.name AS n FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "JOIN drivers d2 ON d.team = d2.team WHERE l.lap_id < 30 ORDER BY t, n",
]

# Week 29 — LEFT OUTER JOIN. Every query here exists because a LEFT JOIN that
# silently behaves as an INNER JOIN passes every test that only inspects matched
# rows, so each one is written so that the two differ. SQLite supports LEFT JOIN
# natively, which makes it a true oracle for the first time on NULLs that come
# from ORDINARY CATALOG DATA: invariant 14 says a CSV cannot express a NULL, so
# until this week no query could produce one from a table.
#
# Two harness properties respected throughout: rows are keyed by column NAME by
# normalize(), so these project named aliases rather than SELECT * (the merged
# schema legally repeats names); and NULL is canonicalized across the engines
# ("NULL" text vs Python None), which is what lets a null-extended row diff at all.
#
# Run in all four modes: Volcano implements the same rule (invariant 6), and the
# only per-mode difference is the pre-existing multi-way one.
WEEK29_OUTER_JOIN_QUERIES = [
    # THE shape. laps.lap_id runs to 10000 while drivers.driver_id stops at 20, so
    # the overwhelming majority of probe rows find NOTHING — an inner join returns
    # 20 rows here and the outer join 10000. Nothing else in this file has that
    # ratio, which is what makes an inner-join regression impossible to miss.
    "SELECT COUNT(*) AS c FROM laps l LEFT JOIN drivers d ON l.lap_id = d.driver_id",
    # the null-extended half, projected: the join's columns must be NULL, not the
    # placeholder 0 / '' underneath the validity mask
    "SELECT l.lap_id AS lid, d.name AS n FROM laps l LEFT JOIN drivers d "
    "ON l.lap_id = d.driver_id WHERE l.lap_id < 25 ORDER BY lid",
    # IS NULL over the null-supplying side is the anti-join idiom, and it only
    # answers correctly if the NULL survived materialization
    "SELECT COUNT(*) AS c FROM laps l LEFT JOIN drivers d ON l.lap_id = d.driver_id "
    "WHERE d.driver_id IS NULL",
    # TPC-H Q13's semantic, on this schema: COUNT(col) is 0 for an unmatched row
    # while COUNT(*) is 1. If COUNT counts NULLs, every unmatched row reads as 1.
    "SELECT d.name AS n, COUNT(l.lap_id) AS c, COUNT(*) AS star FROM drivers d "
    "LEFT JOIN laps l ON d.driver_id = l.driver_id AND l.speed > 400 "
    "GROUP BY d.name ORDER BY n",
    # ...and the same query with a residual that DOES match some drivers, so the
    # result mixes joined and null-extended rows in one aggregate
    "SELECT d.name AS n, COUNT(l.lap_id) AS c FROM drivers d "
    "LEFT JOIN laps l ON d.driver_id = l.driver_id AND l.speed > 349 "
    "GROUP BY d.name ORDER BY c DESC, n",
    # ON vs WHERE — the pair that makes predicate placement observable. Under an
    # INNER join these two are the same query; under a LEFT join they are not, and
    # only SQLite can say which number belongs to which.
    "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l "
    "ON d.driver_id = l.driver_id AND l.speed > 400",
    "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l "
    "ON d.driver_id = l.driver_id WHERE l.speed > 400",
    # a WHERE conjunct on the null-supplying side must NOT be pushed onto its
    # scan: pushing it produces null-extended rows the WHERE existed to remove
    "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
    "WHERE l.season = 2024",
    # ...while a preserved-side conjunct still is, and must not change the answer
    "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
    "WHERE d.age > 30",
    # both at once, plus a residual: the three predicate positions in one query
    "SELECT d.name AS n, COUNT(*) AS c FROM drivers d LEFT JOIN laps l "
    "ON d.driver_id = l.driver_id AND l.round < 5 "
    "WHERE d.age > 25 GROUP BY d.name ORDER BY n",
    # ORDER BY over null-extended rows: compareForSort puts NULL first ascending,
    # last descending, and the harness compares ORDER BY queries IN ORDER
    "SELECT l.lap_id AS lid, d.name AS n FROM laps l LEFT JOIN drivers d "
    "ON l.lap_id = d.driver_id WHERE l.lap_id < 25 ORDER BY n, lid",
    "SELECT l.lap_id AS lid, d.name AS n FROM laps l LEFT JOIN drivers d "
    "ON l.lap_id = d.driver_id WHERE l.lap_id < 25 ORDER BY n DESC, lid",
    # DISTINCT over a null-extended column: NULL is one group of its own
    "SELECT DISTINCT d.name AS n FROM laps l LEFT JOIN drivers d "
    "ON l.lap_id = d.driver_id ORDER BY n",
    # a STRING key, so the null-extended block is a STRING column rather than an
    # INT one — the placeholder under a STRING NULL is "" and would diff as a row
    "SELECT COUNT(*) AS c FROM drivers d LEFT JOIN laps l "
    "ON d.nationality = l.team",
    # a composite key on an outer join: the shared key encoding (Week 27) and the
    # null-extension have to compose
    "SELECT COUNT(*) AS c FROM laps l LEFT JOIN drivers d "
    "ON l.driver_id = d.driver_id AND l.team = d.team",
    # a self outer join, where both sides carry the same column names and the
    # merged schema repeats them — resolution is by slot, not by name
    "SELECT COUNT(*) AS c FROM laps l LEFT JOIN laps l2 ON l.lap_id = l2.driver_id",
    # aggregates over the preserved side only, so every unmatched row still
    # contributes its group
    "SELECT d.team AS t, MIN(l.speed) AS lo, MAX(l.speed) AS hi, COUNT(l.lap_id) AS c "
    "FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id AND l.round = 99 "
    "GROUP BY d.team ORDER BY t",
    # an inner join FOLLOWED by an outer one: predicate routing and lowering must
    # keep working per-join rather than per-query (vectorized path only)
]

# Three-relation shapes with an outer join in them — vectorized-only, for the
# pre-existing Week 27 reason (Planner::plan builds exactly one join). They are
# also the queries that prove join enumeration DECLINED: a reordering that moved
# a relation across the outer join would change these answers.
WEEK29_MULTIWAY_OUTER_QUERIES = [
    "SELECT COUNT(*) AS c FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "LEFT JOIN drivers d2 ON d.team = d2.team AND d2.age > 90",
    "SELECT d.name AS n, COUNT(l2.lap_id) AS c FROM drivers d "
    "JOIN laps l ON d.driver_id = l.driver_id "
    "LEFT JOIN laps l2 ON l.lap_id = l2.driver_id "
    "WHERE l.lap_id < 40 GROUP BY d.name ORDER BY n",
    # the outer join FIRST, an inner one on top of it: the null-extended rows flow
    # into another join's probe side, where their NULL key must stay unmatchable
    "SELECT COUNT(*) AS c FROM laps l LEFT JOIN drivers d ON l.lap_id = d.driver_id "
    "JOIN laps l2 ON l.driver_id = l2.driver_id WHERE l.lap_id < 30",
]

MULTIWAY_QUERIES = WEEK27_MULTIWAY_QUERIES + WEEK28_JOIN_ORDER_QUERIES \
    + WEEK29_MULTIWAY_OUTER_QUERIES

MULTIWAY_VOLCANO_REJECTED = [
    (query, VOLCANO_MULTIWAY) for query in MULTIWAY_QUERIES
]

# SEAM AUDIT (phase 5, engine-divergence pass 1) — GROUP BY EMISSION ORDER.
#
# These are NOT diffed against SQLite, and the reason is the point of the suite.
#
# `HashAggregateNode` used to emit its groups by iterating its accumulator
# `unordered_map` directly (hash-bucket order); `VecHashAggregateNode` emits in
# first-encounter order. SQL leaves GROUP BY row order unspecified, so both are
# legal and `normalize`'s sort hides the difference -- EXCEPT under
# `ORDER BY <aggregate> LIMIT n` with a TIE spanning the cut. Both sort nodes are
# `std::stable_sort`, so tied rows are broken by INPUT order, and the two engines
# kept DIFFERENT ROWS. Sorting cannot mask that: the difference is in which rows
# survive, not in their order.
#
# Why SQLite cannot adjudicate it: with a tie at the cut EVERY choice of the tied
# rows is a correct answer, SQLite's included. Putting such a query in QUERIES
# would assert a non-guarantee. The property that IS real is that the four
# SwiftQL modes agree with EACH OTHER and are deterministic -- Volcano is the
# correctness baseline, so a baseline whose row order is decided by a plan
# decision is not one. That is what this suite pins.
#
# CORRECTION, seam audit pass 2. The sentence that stood here said SQLite "picks
# a third set again", and cited that as the reason these queries cannot be
# diffed. That was TRUE when written and is FALSE now. The pass-2 fix gives the
# sort a deterministic tie-break -- when every declared ORDER BY key ties, the
# whole row decides, ascending (src/execution/sort_comparator.h) -- and for a
# GROUP BY tie that sorts the tied groups by their group key, which is also the
# order SQLite's GROUP BY emits. So SwiftQL and SQLite now COINCIDE on both of
# the original entries: audit A7 measured SQLite at AlphaTauri/Alpine/Ferrari
# against SwiftQL's AlphaTauri/Alpine/McLaren, and SwiftQL now answers
# AlphaTauri/Alpine/Ferrari too.
#
# The suite stays here anyway, and deliberately does not diff against SQLite.
# The coincidence is not a guarantee: it holds only while the tie-break's first
# discriminating column is the group key, and it does not hold in general (an
# entry whose sort input is a raw join row is broken by that row's first column,
# not by the group key -- see the DISTINCT entry below, which all four modes
# answer RedBull/AlphaTauri/McLaren while SQLite is free to answer otherwise).
# A suite that quietly started depending on SQLite's arbitrary choice would be
# the vacuous-entry class this file keeps having to fix.
#
# ---------------------------------------------------------------------------
# WHAT EACH ENTRY MUST SATISFY, AND WHY IT IS DATA RATHER THAN A COMMENT
#
# The entire discriminating power of this suite comes from the DATA producing a
# MATERIAL tie spanning the LIMIT cut. Until pass 2 the requirement was written
# in this comment and asserted nowhere: regenerate data/laps.csv with a wider
# season range and every entry would become a vacuous pass with no signal. That
# is finding E-4, and it is the same failure mode as a test that passes for a
# reason its comment DESCRIBES rather than ENFORCES.
#
# So each entry is a dict, and the precondition is a field:
#
#   query      what the four modes run and must agree on
#   tie_probe  the same ORDER BY with the LIMIT removed, run through SQLITE by
#              check_engine_agreement_tie_precondition(). It must expose the
#              ORDER BY key as a column named `k`; the check reads that name, so
#              a probe that drifts away from its query fails loudly instead of
#              silently measuring the wrong column. For an entry whose tie lives
#              in a SUBQUERY BODY, this is the BODY, not the outer query.
#   cut        the LIMIT the tie must span
#
# The check asserts, per entry: rows exist beyond the cut; the tied block
# STRADDLES the cut; and the block holds at least two DISTINCT rows, so that at
# least two different answers are legal. A tie among identical rows satisfies a
# naive "is there a tie" check while asserting nothing at all.
#
# ---------------------------------------------------------------------------
# E-4: THE ORIGINAL TWO ENTRIES HAVE NO `WHERE` CLAUSE, WHICH IS THE ONE
# CONFIGURATION WHERE THE TWO BUILD-SIDE RULES PROVABLY COINCIDE.
#
# Volcano picks a join's build side from RAW table row counts (planner.cc); the
# vectorized builder picks it from POST-PUSHDOWN cardinality ESTIMATES and real
# row widths (vectorized_plan_builder.cc). With no WHERE the estimate equals the
# raw count, so the two rules agree and the seam looks closed. Every entry added
# in pass 2 carries a WHERE whose ESTIMATE crosses the other side's cardinality
# while its actual selectivity does not, which is what makes the two engines
# choose different build sides -- and the build side is what decides probe
# order, group first-encounter order, and which tied row survives the cut.
#
# Each pass-2 entry was RUN against a build of this tree with the tie-break loop
# removed (byte-for-byte the pre-fix semantics) and DIVERGES there across the
# four modes; all four agree at HEAD. The measured pre-fix disagreements are
# quoted per entry.
_TIE_WHERE = " AND ".join(["l.season = 2022"] * 6)

ENGINE_AGREEMENT_QUERIES = [
    # all seven teams tie on MIN(season)=2022, cut at 3 -- four of the seven are
    # discarded purely on emission order
    {"query": "SELECT team, MIN(season) FROM laps GROUP BY team "
              "ORDER BY MIN(season) LIMIT 3",
     "tie_probe": "SELECT team, MIN(season) AS k FROM laps GROUP BY team ORDER BY k",
     "cut": 3},
    # the same exposure one level up: a tie at the cut over a JOIN's output
    {"query": "SELECT d.team, MIN(l.season) FROM laps l JOIN drivers d "
              "ON l.driver_id = d.driver_id GROUP BY d.team ORDER BY MIN(l.season) LIMIT 3",
     "tie_probe": "SELECT d.team, MIN(l.season) AS k FROM laps l JOIN drivers d "
                  "ON l.driver_id = d.driver_id GROUP BY d.team ORDER BY k",
     "cut": 3},

    # --- pass 2: entries whose WHERE makes the two build-side rules DISAGREE ---
    #
    # E-1, the audit's A1-repro. The estimator's AND rule is the textbook
    # independence product, so six copies of one conjunct drive the estimate for
    # `laps` to the floor (10000 * 0.25^6 -> 2) while every one of the 2417
    # matching rows survives. Volcano compares raw 20 < 10000 and probes laps;
    # the optimized vec path compares estimated 20 vs 2 and probes drivers.
    # PRE-FIX: row-volcano / col-volcano / col-vec-noopt returned
    # {AlphaTauri, Alpine, McLaren}; col-vec returned {RedBull, AlphaTauri,
    # McLaren} -- a different row SET, which no sorting in this harness repairs.
    {"query": f"SELECT d.team, MIN(l.season) FROM drivers d JOIN laps l "
              f"ON d.driver_id = l.driver_id WHERE {_TIE_WHERE} "
              f"GROUP BY d.team ORDER BY MIN(l.season) LIMIT 3",
     "tie_probe": f"SELECT d.team, MIN(l.season) AS k FROM drivers d JOIN laps l "
                  f"ON d.driver_id = l.driver_id WHERE {_TIE_WHERE} "
                  f"GROUP BY d.team ORDER BY k",
     "cut": 3},

    # the same shape DESCENDING. The tie-break is NOT a key the user wrote, so
    # its direction is fixed to ascending regardless of `desc`; this entry is
    # what makes that a pinned decision rather than an implementation accident.
    # PRE-FIX: same split as above ({AlphaTauri, Alpine, McLaren} against
    # {RedBull, AlphaTauri, McLaren}).
    {"query": f"SELECT d.team, MIN(l.season) FROM drivers d JOIN laps l "
              f"ON d.driver_id = l.driver_id WHERE {_TIE_WHERE} "
              f"GROUP BY d.team ORDER BY MIN(l.season) DESC LIMIT 3",
     "tie_probe": f"SELECT d.team, MIN(l.season) AS k FROM drivers d JOIN laps l "
                  f"ON d.driver_id = l.driver_id WHERE {_TIE_WHERE} "
                  f"GROUP BY d.team ORDER BY k DESC",
     "cut": 3},

    # DISTINCT, which audit B5 named as inheriting the same exposure and which
    # 87c08a2's reasoning never covered: DISTINCT emits in INPUT order, so it
    # inherits every join-side and join-order decision above it.
    #
    # This is also the only entry whose sort input is a RAW JOIN ROW rather than
    # an aggregate's output, and that is why it was left here as a tripwire for a
    # SECOND asymmetry: row storage handed Volcano's scan the FULL table schema
    # while every columnar mode handed it the narrowed one, so the two legs
    # tie-broke over different column sets and agreed only because the first
    # discriminating column happened to be driver_id in both.
    #
    # THAT ASYMMETRY IS NOW CLOSED, not merely watched. Planner::plan narrows the
    # ROWS as well as the schema (`narrowRows`), so both legs hand SeqScanNode
    # the same buildScanSchema output and the tie-break sees one column set. The
    # entry stays: it is still the only one whose sort input is a raw join row,
    # so it remains the only place a future re-divergence of the two legs' scan
    # schemas would show up here — it is now a regression test rather than a
    # standing hazard.
    # PRE-FIX: {AlphaTauri, Alpine, McLaren} on three modes against
    # {RedBull, AlphaTauri, McLaren} on col-vec.
    {"query": f"SELECT DISTINCT d.team, l.season FROM drivers d JOIN laps l "
              f"ON d.driver_id = l.driver_id WHERE {_TIE_WHERE} "
              f"ORDER BY l.season LIMIT 3",
     "tie_probe": f"SELECT DISTINCT d.team, l.season AS k FROM drivers d JOIN laps l "
                  f"ON d.driver_id = l.driver_id WHERE {_TIE_WHERE} ORDER BY k",
     "cut": 3},

    # E-1b, and the reason E-1 is a BLOCKER rather than a dialect choice.
    # `materializeSubqueries` turns a SCALAR body's first row into a `Literal`,
    # so a tie at the body's cut stops being a row order and becomes a VALUE
    # propagating into the outer query. PRE-FIX this returned COUNT(*) = 977 on
    # row-volcano, col-volcano and col-vec-noopt and 1536 on col-vec -- a single
    # scalar differing by 559 between two modes of the same engine, and a direct
    # violation of `optimized == --no-optimize`.
    #
    # The tie_probe is the BODY with its key exposed, at the body's own cut of 1.
    {"query": f"SELECT COUNT(*) FROM laps WHERE team = "
              f"(SELECT d.team FROM drivers d JOIN laps l ON d.driver_id = l.driver_id "
              f"WHERE {_TIE_WHERE} GROUP BY d.team ORDER BY MIN(l.season) LIMIT 1)",
     "tie_probe": f"SELECT d.team, MIN(l.season) AS k FROM drivers d JOIN laps l "
                  f"ON d.driver_id = l.driver_id WHERE {_TIE_WHERE} "
                  f"GROUP BY d.team ORDER BY k",
     "cut": 1},
]

QUERIES = PHASE2_WEEK12_BENCHMARK_QUERIES + [
    query for query in REGRESSION_QUERIES
    if query not in PHASE2_WEEK12_BENCHMARK_QUERY_SET
] + WEEK6_CHECKPOINT_QUERIES + ZONE_MAP_QUERIES + SELF_JOIN_QUERIES \
  + WEEK24_EXPRESSION_QUERIES + NULL_SEMANTICS_QUERIES + MIN_MAX_TYPE_QUERIES \
  + GROUP_KEY_QUALIFIER_QUERIES + CONSTANT_FOLDING_QUERIES + EXPRESSION_POSITION_QUERIES \
  + NULL_ORDERING_QUERIES + THREE_VALUED_LOGIC_QUERIES \
  + WEEK25_PREDICATE_QUERIES + WEEK25_CASE_QUERIES + WEEK25_SUBSTRING_QUERIES \
  + WEEK25_JOIN_QUERIES + WEEK26_ALIAS_SHADOW_QUERIES + WEEK27_JOIN_QUERIES \
  + WEEK27_KEY_ENCODING_QUERIES + WEEK29_OUTER_JOIN_QUERIES \
  + WEEK30_ALIAS_REBIND_QUERIES + WEEK31_SUBQUERY_QUERIES \
  + WEEK34_DISTINCT_AGG_QUERIES

# SQLite setup
#
# Week 35: CATALOG-DRIVEN. This used to be literal CREATE TABLE / INSERT text for
# laps and drivers, with the CSV paths as module constants. Two hand-maintained
# copies of one schema disagree eventually, and a disagreement HERE reads as an
# engine bug rather than as a harness bug. Now both sides of the diff derive
# from the same catalog.json the engine reads -- including the Week 35 "format"
# object, so the oracle splits a .tbl on the same delimiter, skips the same
# header (none) and drops the same trailing field the C++ loader does.
SQLITE_TYPE = {"INT": "INTEGER", "DOUBLE": "REAL", "STRING": "TEXT"}


def _coerce_field(text, swiftql_type):
    """Type one raw field the way CSVLoader::parseField does.

    Deliberately NOT tolerant: parseField now requires full consumption of a
    numeric field, so a mistyped column throws there. If it threw there and
    quietly became a string here, the two sides would disagree about what the
    data even is.
    """
    if swiftql_type == "INT":
        return int(text)
    if swiftql_type == "DOUBLE":
        return float(text)
    return text


def load_from_catalog(catalog_path):
    """Build an in-memory SQLite mirror of whatever catalog.json describes."""
    base = os.path.dirname(os.path.abspath(catalog_path))
    with open(catalog_path) as f:
        spec = json.load(f)

    conn = sqlite3.connect(":memory:")
    conn.row_factory = sqlite3.Row

    for t in spec["tables"]:
        cols = t["columns"]
        conn.execute("CREATE TABLE {} ({})".format(
            t["name"],
            ", ".join("{} {}".format(c["name"], SQLITE_TYPE[c["type"]]) for c in cols)))

        fmt = t.get("format", {})
        delim = fmt.get("delimiter", ",")
        header = fmt.get("header", True)
        trailer = fmt.get("trailing_delimiter", False)

        path = os.path.join(base, t["file"])
        placeholders = ",".join("?" * len(cols))
        rows = []
        with open(path) as f:
            if header:
                next(f, None)
            for line_no, line in enumerate(f, start=2 if header else 1):
                line = line.rstrip("\n").rstrip("\r")
                if not line:
                    continue
                fields = line.split(delim)
                if trailer and fields and fields[-1] == "":
                    fields.pop()
                if len(fields) != len(cols):
                    raise ValueError(
                        "{} line {}: expected {} fields, got {}".format(
                            path, line_no, len(cols), len(fields)))
                rows.append([_coerce_field(v, c["type"])
                             for v, c in zip(fields, cols)])
        # executemany in one transaction: the old per-row execute was fine for
        # 10k laps and is not for 60k+ lineitem rows at even a small scale factor.
        conn.executemany("INSERT INTO {} VALUES ({})".format(t["name"], placeholders),
                         rows)
    conn.commit()
    return conn


def load_sqlite():
    """Load the default F1 catalog. Kept as the name every caller here uses."""
    return load_from_catalog(CATALOG_PATH)


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

    # !! WEEK 36 — THE TPC-H LEG DEPENDS ON THIS NUMERIC COERCION, deliberately.
    # The dialect has no `extract(year from d)`; the documented rewrite is
    # `SUBSTRING(d, 1, 4)`, which yields a STRING year where the TPC-H text and
    # the SQLite oracle both yield an INTEGER (q7's `l_year`, q8/q9's `o_year`).
    # float() on both sides is what makes '1995' and 1995 compare equal. Week 35
    # recorded that as a COINCIDENCE it did not want to rely on; Week 36 keeps it
    # and states the dependency, rather than growing a numeric conversion in the
    # dialect (a new expression node -- 17 dispatch sites -- for zero additional
    # queries) or comparing the STRING as a STRING (which would fail q7/q8/q9 on
    # a CORRECT answer, since the oracle's column really is an INTEGER).
    #
    # It is SOUND for those three queries because of a property they have and the
    # rewrite does not confer: `l_year` / `o_year` are only ever GROUP BY keys and
    # output columns, never arithmetic operands -- `SUM(SUBSTRING(...))` is
    # rejected at plan time, correctly, because the result is a STRING. And
    # `ORDER BY` on the STRING year matches the numeric order only because TPC-H
    # dates are fixed-width four-digit years; it would not for a two-digit or
    # mixed-width one. Both halves hold because of the data format, not because
    # of the comparison, so they are written down here rather than assumed.
    # check_year_coercion_dependency() below asserts the coercion itself.
    def coerce(v):
        if v is None or v == "NULL": return NULL_TOKEN
        try: return round(float(v), 6)
        except (ValueError, TypeError): return str(v)

    # !! Rows arrive as dicts keyed by column NAME, so duplicate names collapse.
    # A merged join schema legally carries several columns of the same name
    # (invariant 3: two `driver_id`, two `team`, two `name` on a self-join or a
    # laps/drivers join), and BOTH engines' rows collapse identically — so this
    # file cannot see a column-identity or column-order regression in a
    # `SELECT *` multi-way join. Pre-existing since Week 18; the Week 27 and 28
    # query blocks deliberately project named columns instead. The gap is covered
    # on the C++ side by JoinEnumeration.ReorderedPlansReturnTheWrittenOrdersRows,
    # which diffs raw chunk values and therefore does see column order. Read this
    # file's silence on `SELECT *` joins as absence of coverage, not as coverage.
    #
    # Week 35: the blind spot is UNCHANGED HERE and closed ELSEWHERE, and the two
    # must not be confused. python_tools/random_diff.py compares raw --format tsv
    # output POSITIONALLY, never through this dict, precisely so a column-identity
    # or column-order regression in a generated multi-way join is visible to it.
    # That covers the randomized differ only. Every suite in THIS file still goes
    # through the keying above, so the sentence before this one still holds for
    # them.
    normalized = []
    for row in rows:
        normalized.append(tuple(coerce(v) for v in row.values()))
    return normalized if preserve_order else sorted(normalized)


def rows_equal(a, b, rel_tol=0.0, abs_tol=1e-5):
    """Compare two normalized row lists, using epsilon tolerance for floats.

    The DEFAULT is unchanged: absolute 1e-5, no relative component. Every one of
    this file's ~500 existing diffs passes at it, and loosening it globally to
    accommodate TPC-H would silently weaken all of them.

    Week 35 -- why TPC-H callers must pass rel_tol. An absolute 1e-5 fails Q1 on
    a CORRECT answer, for two independent and measurable reasons:

      * PRINTING. Value::toString formats a DOUBLE with "%.15g"
        (src/common/value.cc), one digit short of a round trip, so the text this
        harness receives already carries ~1e-15 RELATIVE error. On a revenue sum
        near 1e9 that alone is ~1e-6 absolute.
      * SUMMATION ORDER. SwiftQL and SQLite accumulate the same sum in different
        orders over tens of thousands of rows, differing by roughly n*eps
        relative -- on that same sum, order 1e-2 absolute.

    1e-2 > 1e-5, so the comparison would reject a correct engine.

    The TPC-H setting is rel_tol=1e-9, abs_tol=1e-6: four orders of magnitude
    above both noise sources, so it never fires spuriously, and many orders below
    any arithmetic defect this engine could have -- a wrong `1 - l_discount` is a
    percent-level error, not a 1e-9 one. That derivation is written down here
    because a tolerance without one gets loosened by the next person who sees a
    red test.

    Week 36 -- NaN AND INFINITY, which the tolerance test alone cannot judge, and
    the hole is bigger than it looks. IEEE 754 makes EVERY comparison against NaN
    False, so `abs(nan - 5.0) > tol` is False and the mismatch branch below was
    never reached: `rows_equal([[nan]], [[5.0]])` returned True. A NaN anywhere in
    a SwiftQL answer was INVISIBLE to this oracle -- not merely "nan compares
    equal to nan", but "nan compares equal to anything". Measured before the fix,
    all four True: nan/5.0, 5.0/nan, nan/nan, inf/inf.

    The rule now: a NaN is a MISMATCH unless BOTH sides are NaN, in which case the
    two engines agree and a diff is not the place to complain about it.
    Infinities compare exactly and by SIGN -- `abs(inf - inf)` is nan, which the
    old test also read as "equal", right by accident rather than by rule.
    """
    if len(a) != len(b):
        return False
    for row_a, row_b in zip(a, b):
        if len(row_a) != len(row_b):
            return False
        for x, y in zip(row_a, row_b):
            if isinstance(x, float) and isinstance(y, float):
                # Non-finite FIRST: the tolerance test below cannot reject a NaN,
                # because every comparison against NaN is False.
                if math.isnan(x) or math.isnan(y):
                    if not (math.isnan(x) and math.isnan(y)):
                        return False
                elif math.isinf(x) or math.isinf(y):
                    if x != y:          # exact, and sign-sensitive
                        return False
                elif abs(x - y) > max(abs_tol, rel_tol * max(abs(x), abs(y))):
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


def run_engine_agreement_suite(queries, label: str, modes):
    """Assert the SwiftQL modes return the SAME rows, in the same order.

    The oracle for this suite is SwiftQL's first mode, not SQLite — see the
    comment on ENGINE_AGREEMENT_QUERIES for why SQLite cannot adjudicate a tie
    at a LIMIT cut. Comparison is ORDERED and unnormalized-by-sort
    (`preserve_order=True`), because an emission-order difference is precisely
    what sorting would hide.

    Entries are dicts, not strings: each carries the tie precondition it rests
    on, which check_engine_agreement_tie_precondition() enforces before any of
    this runs. See the comment on ENGINE_AGREEMENT_QUERIES.
    """
    passed = failed = errors = 0
    print(f"\n--- {label} ---")
    for entry in queries:
        query = entry["query"]
        try:
            baseline_label, baseline_extra = modes[0]
            baseline = normalize(run_swiftql(query, baseline_extra), True)
            mismatch = None
            for mode_label, extra in modes[1:]:
                other = normalize(run_swiftql(query, extra), True)
                if not rows_equal(baseline, other):
                    mismatch = (mode_label, other)
                    break
            if mismatch is None:
                print(f"  PASS  {query[:70]}")
                passed += 1
            else:
                mode_label, other = mismatch
                print(f"  FAIL  {query[:70]}")
                print(f"    {baseline_label}: {baseline[:4]}")
                print(f"    {mode_label}: {other[:4]}")
                failed += 1
        except Exception as e:
            print(f"  ERROR {query[:70]}\n    {e}")
            errors += 1
    print(f"{passed} passed, {failed} failed, {errors} errors")
    return passed, failed, errors


# Week 35, Task 8 — THE BEHAVIOURAL REJECTION SWEEP.
#
# Week 34's lesson, recorded in its own commits: A TEXTUAL CROSS-CHECK CANNOT
# DETECT A SUITE ENTRY WHOSE MOVE HALF-LANDED. An entry moved out of one list and
# not into another, or whose expected message has drifted, still READS correct.
# Two stale entries were caught that week, and the check that found them was
# behavioural -- run every rejection entry and assert it still errors.
#
# Three checks, cheap, composing:
#   1. STRUCTURAL   -- every *_REJECTED / *_REFUSED suite is a non-empty list of
#                      (query, expected_substring) pairs with a non-empty
#                      expectation. Catches a bare string appended to a list of
#                      pairs.
#   2. DISJOINTNESS -- no query text is in both a positive suite and a rejection
#                      suite. THAT IS the half-landed-move signature: the entry
#                      reached its new home and never left its old one, so the
#                      same SQL is asserted both to return rows and to error.
#                      Review will not see it; set intersection will.
#   3. BEHAVIOURAL  -- every entry is EXECUTED and must still error with its
#                      expected substring in at least one mode.
#
# Suites are discovered BY NAME over globals(), never from a curated list: a
# curated list is one more place a half-landed move can hide.
SWEEP_MODES = [
    ("row-volcano", None),
    ("col-volcano", ["--storage", "columnar"]),
    ("col-vec", ["--execution", "vectorized", "--storage", "columnar"]),
]

# Rejection suites whose NAME does not end in _REJECTED / _REFUSED. Listed here
# rather than renamed so the sweep does not silently skip them; each entry is
# (name_of_query_list, name_of_expected_message).
UNCONVENTIONALLY_NAMED_REJECTIONS = [
    ("WEEK33_CORRELATED_BINDS", "WEEK33_CORRELATED_EXPECT"),
]


def _errors_with(query, expected, extra_args):
    try:
        run_swiftql(query, extra_args)
        return False
    except RuntimeError as e:
        return expected in str(e)
    except Exception:
        return False


def sweep_rejection_suites(behavioural=True):
    """Return a list of findings; empty means the sweep is clean."""
    # Match _REJECTED / _REFUSED ANYWHERE in the name, not as a suffix.
    # WEEK26_REJECTED_QUERIES and WEEK30_REJECTED_QUERIES are rejection suites
    # whose names end in _QUERIES, and a suffix test silently skipped both --
    # found by deliberately injecting a stale expectation into one of them and
    # watching this sweep report "clean". A sweep that discovers zero suites is
    # green too, which is why the counts below are printed.
    suites = {}
    for name, obj in globals().items():
        if ("_REJECTED" in name or "_REFUSED" in name) and isinstance(obj, list):
            suites[name] = obj
    for list_name, expect_name in UNCONVENTIONALLY_NAMED_REJECTIONS:
        queries = globals().get(list_name)
        expected = globals().get(expect_name)
        if isinstance(queries, list) and isinstance(expected, str):
            suites[list_name] = [(q, expected) for q in queries]

    findings = []

    # (1) structural
    for name, suite in sorted(suites.items()):
        if not suite:
            findings.append(f"{name}: EMPTY suite")
        for entry in suite:
            if not (isinstance(entry, tuple) and len(entry) == 2
                    and isinstance(entry[0], str) and isinstance(entry[1], str)
                    and entry[1]):
                findings.append(f"{name}: malformed entry {entry!r}")

    # (2) disjointness, against QUERIES ONLY -- and the scope is the whole point.
    #
    # A _VEC_ONLY list and its _VOLCANO_REJECTED counterpart SHARE every query BY
    # DESIGN: the counterpart is literally built from it
    # (`[(q, "...") for q in MULTIWAY_QUERIES]`), because the same query is
    # diffed in the two vectorized modes and asserted-refused in the two Volcano
    # ones. Flagging that overlap would report 88 findings on a healthy tree and
    # train the reader to ignore this check, which is worse than not having it.
    #
    # QUERIES is different: it runs in ALL FOUR modes. A query that is in QUERIES
    # and in any rejection suite is therefore asserted both to return rows and to
    # error IN THE SAME MODE -- a flat contradiction, and exactly the shape a
    # half-landed move leaves behind.
    #
    # WHAT THIS DOES NOT COVER, stated rather than implied: a query moved between
    # two VEC-ONLY suites, or between two rejection suites, is invisible here.
    # Check (3) is what catches those, by running them.
    for name, suite in sorted(suites.items()):
        for q in sorted({q for q, _ in suite} & set(QUERIES)):
            findings.append(
                f"{name}: ALSO IN QUERIES (diffed in all four modes) -- "
                f"a move half-landed: {q[:70]}")

    # (3) behavioural
    checked = 0
    if behavioural:
        for name, suite in sorted(suites.items()):
            for query, expected in suite:
                if not (isinstance(entry := (query, expected), tuple)
                        and isinstance(expected, str) and expected):
                    continue
                checked += 1
                if not any(_errors_with(query, expected, extra)
                           for _, extra in SWEEP_MODES):
                    findings.append(
                        f"{name}: NO LONGER ERRORS with {expected!r}: {query[:70]}")

    print(f"\n--- Rejection-suite sweep ---")
    print(f"{len(suites)} suites, "
          f"{sum(len(s) for s in suites.values())} entries"
          + (f", {checked} executed" if behavioural else ", behavioural leg SKIPPED"))
    for f in findings:
        print(f"  FINDING  {f}")
    if not findings:
        print("  clean")
    return findings


def mode_census():
    """Count what the README states in prose: how many queries are diffed in TWO
    modes rather than four, and how many in all four.

    Computed, never typed. The README's \"56 queries in two modes against 168 in
    four\" was accurate when written and goes stale the moment a suite grows; a
    number in a document about honesty is the worst place for one to rot.
    """
    vec_only = {name: len(obj) for name, obj in globals().items()
                if name.endswith("_VEC_ONLY") and isinstance(obj, list)}
    vec_only["MULTIWAY_QUERIES"] = len(MULTIWAY_QUERIES)
    two_mode = sum(vec_only.values())
    four_mode = len(QUERIES)
    print(f"\n--- Mode census (computed) ---")
    print(f"  diffed in all four modes           : {four_mode}")
    print(f"  diffed in the two vectorized modes : {two_mode}")
    for name in sorted(vec_only):
        print(f"      {name:<44} {vec_only[name]:>4}")
    print(f"    (Volcano refuses these: Planner::plan builds exactly one")
    print(f"     HashJoinNode and runs no LogicalPlanBuilder, so it can hold")
    print(f"     neither a second join nor a relation that is a plan. Both")
    print(f"     halves are asserted, so the boundary cannot drift silently.)")
    return four_mode, two_mode


# main
def check_rows_equal_non_finite():
    """Week 36 -- the comparator's own NaN/inf rule, asserted directly.

    This is not a query suite: it tests the FUNCTION every suite in this file
    passes through. It exists because the pre-Week-36 form silently returned True
    for `nan` against `5.0` -- IEEE 754 makes every comparison against NaN False,
    so the mismatch branch was unreachable and a NaN in any answer was invisible
    to the oracle. A defect that hides defects gets an assertion, not a comment.

    Raises rather than returning a count: a broken comparator invalidates every
    number this file prints, so there is nothing to keep running for.
    """
    nan, inf = float("nan"), float("inf")
    cases = [
        (([[nan]], [[5.0]]), False, "a NaN against an ordinary number"),
        (([[5.0]], [[nan]]), False, "an ordinary number against a NaN"),
        (([[nan]], [[nan]]), True,  "NaN on both sides -- the engines agree"),
        (([[inf]], [[inf]]), True,  "+inf on both sides"),
        (([[inf]], [[-inf]]), False, "+inf against -inf -- sign matters"),
        (([[inf]], [[1e308]]), False, "+inf against a finite number"),
        (([[1.0]], [[1.0 + 1e-9]]), True, "the ordinary tolerance still applies"),
    ]
    for (a, b), expected, why in cases:
        got = rows_equal(a, b, rel_tol=1e-9, abs_tol=1e-6)
        if got is not expected:
            raise AssertionError(
                "rows_equal regression: %s -- expected %s, got %s" % (why, expected, got))
    print("rows_equal non-finite rule: %d cases OK" % len(cases))


def check_year_coercion_dependency():
    """Week 36 -- the STRING-year dependency the TPC-H leg rests on, asserted.

    `SUBSTRING(d, 1, 4)` is the documented rewrite for `extract(year from d)` and
    it yields a STRING. SQLite yields an INTEGER. q7, q8 and q9 compare equal only
    because normalize()'s coerce() runs float() on both sides. Week 35 called that
    a coincidence; an assertion is what turns a coincidence into a contract, so a
    future change to coerce() fails HERE rather than as three unexplained TPC-H
    mismatches five minutes into the fifth gate step.
    """
    swiftql_side = normalize([{"y": "1995", "rev": "1.5"}])
    sqlite_side = normalize([{"y": 1995, "rev": 1.5}])
    if swiftql_side != sqlite_side:
        raise AssertionError(
            "normalize() no longer coerces a numeric-looking STRING to a number: "
            "TPC-H q7/q8/q9 compare a SUBSTRING year against SQLite's INTEGER "
            "and depend on it (%r != %r)" % (swiftql_side, sqlite_side))
    # ...and it must NOT coerce a genuinely non-numeric string, or the equality
    # above would be vacuous.
    if normalize([{"y": "MED BOX"}]) == normalize([{"y": 0}]):
        raise AssertionError("normalize() coerces a non-numeric string to a number")
    print("normalize() STRING-year coercion: contract holds")


def check_engine_agreement_tie_precondition(conn):
    """Seam audit pass 2, E-4 — the precondition ENGINE_AGREEMENT_QUERIES rests on.

    That suite proves something only if its data produces a MATERIAL tie
    spanning each entry's LIMIT cut. Until now the requirement was a sentence in
    a comment and nothing checked it, so regenerating data/laps.csv with a wider
    season range would have turned every entry into a vacuous pass — green, and
    asserting nothing. This file already had the pattern and did not use it
    (check_rows_equal_non_finite, check_year_coercion_dependency).

    Three assertions per entry, and the third is the one that matters:

      1. there are rows BEYOND the cut, so the LIMIT actually discards something;
      2. the tied block STRADDLES the cut, so which rows survive is a choice;
      3. that block holds at least two DISTINCT rows.

    (3) is what makes the tie MATERIAL. A tie among identical rows satisfies
    (1) and (2) and still proves nothing: every choice yields the same answer,
    so the entry would pass whether or not the engine is deterministic. That is
    the exact shape of a test that passes because of what its comment describes
    rather than what it enforces.

    Raises rather than counting: a vacuous entry does not fail, it stops meaning
    anything, so there is nothing to keep running for.
    """
    for entry in ENGINE_AGREEMENT_QUERIES:
        probe, cut = entry["tie_probe"], entry["cut"]
        cur = conn.execute(probe)
        cols = [d[0] for d in cur.description]
        if "k" not in cols:
            raise AssertionError(
                "engine-agreement tie probe does not expose its ORDER BY key as `k` "
                "(columns %r) — the probe has drifted from its query:\n  %s" % (cols, probe))
        ki = cols.index("k")
        rows = cur.fetchall()

        if len(rows) <= cut:
            raise AssertionError(
                "engine-agreement entry has no rows beyond its cut of %d (%d rows) — "
                "the LIMIT discards nothing, so the entry proves nothing:\n  %s"
                % (cut, len(rows), entry["query"]))

        boundary = rows[cut - 1][ki]
        tied = [i for i, r in enumerate(rows) if r[ki] == boundary]
        if max(tied) < cut:
            raise AssertionError(
                "engine-agreement entry has NO TIE spanning its cut of %d "
                "(boundary key %r is not shared across the cut) — the ORDER BY is "
                "total there, so any emission order passes:\n  %s"
                % (cut, boundary, entry["query"]))

        distinct_tied = {tuple(rows[i]) for i in tied}
        if len(distinct_tied) < 2:
            raise AssertionError(
                "engine-agreement entry's tie at the cut of %d is IMMATERIAL — all "
                "%d tied rows are identical (%r), so every choice gives the same "
                "answer and the entry passes whether or not the engines agree:\n  %s"
                % (cut, len(tied), next(iter(distinct_tied)), entry["query"]))

    print("engine-agreement tie precondition: %d entries, all material ties at the cut"
          % len(ENGINE_AGREEMENT_QUERIES))


def main():
    conn = load_sqlite()
    check_rows_equal_non_finite()
    check_year_coercion_dependency()
    check_engine_agreement_tie_precondition(conn)

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

    # Week 27/28: three-or-more-relation joins execute on the vectorized path only,
    # so they are diffed against SQLite in the two vec modes and asserted to be
    # refused in the two Volcano ones. Splitting the suite this way is what keeps
    # a deliberate capability difference from reading as four failures — and
    # keeps the refusal itself under test, so a later refactor cannot delete it
    # silently.
    vec_modes = [
        ("Multi-way joins (columnar storage, vec path)",
         ["--execution", "vectorized", "--storage", "columnar"]),
        ("Multi-way joins (vectorized, optimizer off)",
         ["--execution", "vectorized", "--storage", "columnar", "--no-optimize"]),
    ]
    m_passed = m_failed = m_errors = 0
    for label, extra in vec_modes:
        mp, mf, me = run_query_suite(conn, MULTIWAY_QUERIES, label, extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me

    volcano_modes = [
        ("Multi-way refused (row storage, Volcano)", None),
        ("Multi-way refused (columnar storage, Volcano)", ["--storage", "columnar"]),
    ]
    for label, extra in volcano_modes:
        mp, mf, me = run_rejection_suite(MULTIWAY_VOLCANO_REJECTED, label, extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me

    # Cross-engine agreement on GROUP BY emission order (seam audit, pass 1).
    # Counted with the query suites, not the rejections: these are answers.
    ea_p, ea_f, ea_e = run_engine_agreement_suite(
        ENGINE_AGREEMENT_QUERIES,
        "GROUP BY emission order — the four modes must agree with each other",
        [("row-volcano", None),
         ("col-volcano", ["--storage", "columnar"]),
         ("col-vec", ["--execution", "vectorized", "--storage", "columnar"]),
         ("col-vec --no-optimize",
          ["--execution", "vectorized", "--storage", "columnar", "--no-optimize"])],
    )
    m_passed += ea_p
    m_failed += ea_f
    m_errors += ea_e

    # SQL that must be refused rather than answered, in all four modes. The
    # refusal is raised from a different place on each path (Planner::plan on
    # Volcano, the binder/validator or VectorizedPlanBuilder on the vec path).
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

    # Week 31. The uncorrelated forms now return rows and are diffed as part of
    # QUERIES above, in all four modes. What is left here is the correlated half,
    # which is still a rejection suite — reaching THAT refusal asserts every
    # scope-resolution property Week 30 built, since all of it has to succeed to
    # get there — and the Week 30 rejections, which assert that everything a
    # query can get wrong earlier still outranks the refusal, in the shape that
    # stops it becoming a catch-all.
    #
    # Both run in all four modes: the refusal is one check at the end of
    # Validator::validate and the materialization pass sits above both engines,
    # so running everywhere is what proves the four modes agree rather than
    # asserting it.
    week33_binds = [(q, WEEK33_CORRELATED_EXPECT) for q in WEEK33_CORRELATED_BINDS]
    for label, extra in vec_modes:
        rp, rf, re_ = run_rejection_suite(
            WEEK33_CORRELATED_NESTED_VEC,
            f"Week 33 correlated inside an IN body — {label}", extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_
    for label, extra in volcano_modes:
        rp, rf, re_ = run_rejection_suite(
            WEEK33_CORRELATED_NESTED_VOLCANO,
            f"Week 33 correlated inside an IN body, refused earlier — {label}",
            extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_
    for label, extra in modes:
        for suite, name in ((week33_binds, "Week 33 correlated subqueries refused"),
                            (WEEK34_DISTINCT_AGG_REFUSED,
                             "Week 34 DISTINCT aggregate refusals"),
                            (WEEK34_DERIVED_REFUSED,
                             "Week 34 derived-table refusals"),
                            (WEEK31_MATERIALIZATION_REFUSED,
                             "Week 31 materialization divergences from SQLite"),
                            (WEEK37_COLUMN_ORDINAL_REFUSED,
                             "Week 37 column-ordinal refusals"),
                            (WEEK30_REJECTED_QUERIES, "Week 30 rejections")):
            rp, rf, re_ = run_rejection_suite(
                suite, f"{name} — {label}", extra_args=extra)
            r_passed += rp
            r_failed += rf
            r_errors += re_

    # A subquery whose BODY joins three or more relations executes wherever a
    # three-relation query does — the vectorized path — because the nested query
    # runs on the same engine as the query containing it. Same split, and for the
    # same reason, as MULTIWAY_QUERIES / MULTIWAY_VOLCANO_REJECTED above.
    for label, extra in vec_modes:
        mp, mf, me = run_query_suite(
            conn, WEEK31_SUBQUERY_VEC_ONLY,
            f"Week 31 multi-relation subquery body — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in volcano_modes:
        mp, mf, me = run_rejection_suite(
            WEEK31_SUBQUERY_VOLCANO_REJECTED,
            f"Week 31 multi-relation subquery body refused — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me

    # Week 34 — the scalar shapes decorrelation declines. VECTORIZED ONLY, and
    # the reason is the one WEEK32_LOWERING_REFUSED already documents: on the
    # Volcano path the CAPABILITY refusal ("correlated subqueries are
    # decorrelated to a semi-join and are not supported on the Volcano path")
    # fires first, so asserting these messages there would assert the wrong
    # refusal. The Volcano half is already covered by
    # WEEK34_CORRELATED_SCALAR_VOLCANO_REJECTED.
    for label, extra in vec_modes:
        rp, rf, re_ = run_rejection_suite(
            WEEK34_CORRELATED_SCALAR_REFUSED,
            f"Week 34 shapes scalar decorrelation declines — {label}", extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_

    # Week 36 — TPC-H Q21's shape: a correlated equality WITH an inequality beside
    # it. Vectorized only, for the same reason as the suite above: the Volcano
    # path refuses a correlated subquery outright and would assert the wrong
    # refusal. The Volcano half is covered by the correlated-subquery Volcano
    # entries already.
    for label, extra in vec_modes:
        rp, rf, re_ = run_rejection_suite(
            WEEK36_CORRELATED_RESIDUAL_REFUSED,
            f"Week 36 correlated inequality beside a valid key — {label}",
            extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_

    # Week 33, Task 7(1) — the nested-tripwire hunch, pinned. Vectorized only:
    # the Volcano path refuses an IN subquery before reaching the tripwire at
    # all, so running it there would assert the wrong refusal.
    for label, extra in vec_modes:
        rp, rf, re_ = run_rejection_suite(
            WEEK33_NESTED_TRIPWIRE_REFUSED,
            f"Week 33 nested IN tripwire — {label}", extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_

    # Week 35 — a subquery inside a derived-table body. Diffed, not pinned: the
    # bug was an INTERNAL error, and the fix has to produce the right rows rather
    # than merely stop throwing. Same two-mode split as every derived-table query.
    for label, extra in vec_modes:
        mp, mf, me = run_query_suite(
            conn, WEEK35_SUBQUERY_IN_DERIVED_BODY_VEC_ONLY,
            f"Week 35 subquery inside a derived body — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in volcano_modes:
        mp, mf, me = run_rejection_suite(
            WEEK35_SUBQUERY_IN_DERIVED_BODY_VOLCANO_REJECTED,
            f"Week 35 subquery inside a derived body refused — {label}",
            extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me

    # Week 34 — derived tables. Same split, same reason as Weeks 32 and 33: the
    # capability difference is real, so BOTH halves are asserted and the boundary
    # cannot drift silently.
    for label, extra in vec_modes:
        mp, mf, me = run_query_suite(
            conn, WEEK34_DERIVED_TABLE_VEC_ONLY,
            f"Week 34 derived tables — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in volcano_modes:
        mp, mf, me = run_rejection_suite(
            WEEK34_DERIVED_TABLE_VOLCANO_REJECTED,
            f"Week 34 derived tables refused — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me

    # Week 34 — correlated scalar subqueries (Q17). Week 33's checkpoint miss,
    # closed. Same split, same reason.
    for label, extra in vec_modes:
        mp, mf, me = run_query_suite(
            conn, WEEK34_CORRELATED_SCALAR_VEC_ONLY,
            f"Week 34 correlated scalar subqueries — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in volcano_modes:
        mp, mf, me = run_rejection_suite(
            WEEK34_CORRELATED_SCALAR_VOLCANO_REJECTED,
            f"Week 34 correlated scalar subqueries refused — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me

    # SEAM AUDIT pass 2 — B-1. `SELECT *` in a block that holds a lowered
    # subquery. Same split, same reason as the suite above; every entry here is
    # a DIFF because the defect was a wrong answer, not a refusal.
    for label, extra in vec_modes:
        mp, mf, me = run_query_suite(
            conn, SEAM2_STAR_OVER_LOWERED_SUBQUERY_VEC_ONLY,
            f"Seam pass 2 — SELECT * over a lowered subquery — {label}",
            extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in volcano_modes:
        mp, mf, me = run_rejection_suite(
            SEAM2_STAR_OVER_LOWERED_SUBQUERY_VOLCANO_REJECTED,
            f"Seam pass 2 — SELECT * over a lowered subquery refused — {label}",
            extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me

    # SEAM AUDIT pass 2 — B-3. A correlated subquery nested inside a correlated
    # body: diffed where it now runs, and PINNED where it now refuses. The
    # refusal half is the coupled guard the fix made reachable, and it is
    # vectorized-only for the same reason WEEK34_CORRELATED_SCALAR_REFUSED is —
    # the Volcano capability refusal fires first and would assert the wrong one.
    for label, extra in vec_modes:
        mp, mf, me = run_query_suite(
            conn, SEAM2_NESTED_CORRELATION_VEC_ONLY,
            f"Seam pass 2 — correlated subquery inside a correlated body — {label}",
            extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in volcano_modes:
        mp, mf, me = run_rejection_suite(
            SEAM2_NESTED_CORRELATION_VOLCANO_REJECTED,
            f"Seam pass 2 — correlated subquery inside a correlated body refused "
            f"— {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in vec_modes:
        rp, rf, re_ = run_rejection_suite(
            SEAM2_CORRELATION_DEPTH_REFUSED,
            f"Seam pass 2 — a reference more than one level out — {label}",
            extra_args=extra)
        m_passed += rp
        m_failed += rf
        m_errors += re_

    # Week 33 — decorrelated EXISTS / NOT EXISTS. Same split, same reason as
    # Week 32's: the capability difference is real, so BOTH halves are asserted.
    for label, extra in vec_modes:
        mp, mf, me = run_query_suite(
            conn, WEEK33_DECORRELATED_VEC_ONLY,
            f"Week 33 decorrelated EXISTS — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in volcano_modes:
        mp, mf, me = run_rejection_suite(
            WEEK33_DECORRELATED_VOLCANO_REJECTED,
            f"Week 33 decorrelated EXISTS refused — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me

    # Week 32 — set-membership lowering. Diffed in the two vectorized modes,
    # refused (by message) in the two Volcano ones. Same split, same reason, as
    # MULTIWAY_QUERIES / MULTIWAY_VOLCANO_REJECTED: the capability difference is
    # real, so BOTH halves are asserted and the boundary cannot drift silently.
    for label, extra in vec_modes:
        mp, mf, me = run_query_suite(
            conn, WEEK32_SEMI_JOIN_VEC_ONLY,
            f"Week 32 semi/anti-join subqueries — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in volcano_modes:
        mp, mf, me = run_rejection_suite(
            WEEK32_SEMI_JOIN_VOLCANO_REJECTED,
            f"Week 32 semi/anti-join subqueries refused — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    # The shapes the lowering cannot express, asserted in all four modes but
    # against two different messages — see WEEK32_LOWERING_REFUSED's comment.
    for label, extra in vec_modes:
        rp, rf, re_ = run_rejection_suite(
            WEEK32_LOWERING_REFUSED,
            f"Week 32 shapes lowering cannot express — {label}", extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_
    for label, extra in volcano_modes:
        rp, rf, re_ = run_rejection_suite(
            WEEK32_LOWERING_REFUSED_VOLCANO,
            f"Week 32 shapes lowering cannot express, refused earlier — {label}",
            extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_

    # B3-2 — a STRING join key against a numeric one. Refused in the two vec
    # modes for the TYPE reason; refused in the two Volcano modes earlier and for
    # the CAPABILITY reason, which is why the two halves pin different messages.
    for label, extra in vec_modes:
        rp, rf, re_ = run_rejection_suite(
            B32_JOIN_KEY_TYPE_REJECTED,
            f"B3-2 STRING join key refused — {label}", extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_
    for label, extra in volcano_modes:
        rp, rf, re_ = run_rejection_suite(
            B32_JOIN_KEY_TYPE_VOLCANO_REJECTED,
            f"B3-2 STRING join key refused earlier (capability) — {label}",
            extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_

    # E-10 — an INT that cannot be materialized into a DOUBLE result column.
    # The usual split with the SIDES SWAPPED: Volcano ANSWERS (diffed against
    # SQLite in its two modes), the two vectorized modes REFUSE.
    for label, extra in volcano_modes:
        mp, mf, me = run_query_suite(
            conn, E10_VOLCANO_ONLY,
            f"E-10 wide INT into a DOUBLE column — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me
    for label, extra in vec_modes:
        rp, rf, re_ = run_rejection_suite(
            E10_VECTORIZED_REFUSED,
            f"E-10 wide INT refused rather than changed — {label}",
            extra_args=extra)
        r_passed += rp
        r_failed += rf
        r_errors += re_
    # ...and the boundary guards, in ALL FOUR modes: the refusal must be exactly
    # one integer wide, must not fire below the bound, and must not have broken
    # the COUNT(DISTINCT) leg that was already correct.
    for label, extra in [("row storage, Volcano", None),
                         ("columnar storage, Volcano", ["--storage", "columnar"]),
                         *vec_modes]:
        mp, mf, me = run_query_suite(
            conn, E10_BOUNDARY_GUARDS,
            f"E-10 boundary guards — {label}", extra_args=extra)
        m_passed += mp
        m_failed += mf
        m_errors += me

    # Week 35 — the behavioural sweep and the computed census. Both run AFTER the
    # suites, so a sweep finding is read alongside the run it describes.
    findings = sweep_rejection_suites()
    findings += assert_b32_pins_discriminate()
    mode_census()

    total_passed = p1 + p2 + p3 + p4 + m_passed + r_passed
    total_failed = f1 + f2 + f3 + f4 + m_failed + r_failed
    total_errors = e1 + e2 + e3 + e4 + m_errors + r_errors
    print(f"\nTotal: {total_passed} passed, {total_failed} failed, {total_errors} errors")
    if findings:
        # A sweep whose findings are printed but do not fail the run is a sweep
        # nobody reads.
        print(f"Rejection-suite sweep + B3-2 pin discrimination: "
              f"{len(findings)} finding(s)")
    if total_failed > 0 or total_errors > 0 or findings:
        sys.exit(1)

if __name__ == "__main__":
    main()
