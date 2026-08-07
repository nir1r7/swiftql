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

WEEK32_SEMI_JOIN_VOLCANO_REJECTED = [
    (query, "not supported on the Volcano path")
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
     "whole top-level WHERE conjunct"),
    # AN IN IN HAVING. The join would have to sit above LogicalAggregate. Legal,
    # but no TPC-H query needs it (Q11's HAVING subquery is scalar), so lowering
    # only WHERE is the minimum code that solves the problem.
    ("SELECT team, COUNT(*) FROM laps GROUP BY team "
     "HAVING COUNT(*) IN (SELECT driver_id FROM drivers)",
     "whole top-level WHERE conjunct"),
]

# The Volcano half of the same three queries. Derived from the list above rather
# than retyped, so a shape added there cannot quietly lose its Volcano coverage.
# Note the ordering this pins: Planner::plan's IN guard fires BEFORE any
# lowering-shape check, so an unlowerable shape is still refused as "no semi-join
# on Volcano" — the capability boundary outranks the expressibility one. If a
# later week gives Volcano a semi-join, these entries start failing, which is the
# signal to move each query to whichever refusal then applies.
WEEK32_LOWERING_REFUSED_VOLCANO = [
    (query, "not supported on the Volcano path")
    for query, _ in WEEK32_LOWERING_REFUSED
]

WEEK31_SUBQUERY_VEC_ONLY = [
    "SELECT COUNT(*) FROM laps WHERE speed > "
    "(SELECT AVG(l.speed) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    " JOIN drivers d2 ON d.driver_id = d2.driver_id)",
]
WEEK31_SUBQUERY_VOLCANO_REJECTED = [
    (query, "not supported on the Volcano path") for query in WEEK31_SUBQUERY_VEC_ONLY
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
    (query, "not supported on the Volcano path")
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
    # nested two deep, the inner one correlated to the MIDDLE block (Q20)
    "SELECT name FROM drivers d WHERE d.driver_id IN "
    "(SELECT l.driver_id FROM laps l WHERE l.speed > "
    " (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team))",
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
WEEK33_CORRELATED_NESTED_VOLCANO = [
    (q, "not supported on the Volcano path") for q in WEEK33_CORRELATED_IN_SHAPES]

WEEK33_CORRELATED_BINDS = [
    # scalar, correlated (Q17), composed with arithmetic
    "SELECT l.lap_id FROM laps l WHERE l.speed > "
    "0.2 * (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)",
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

WEEK34_DERIVED_TABLE_VOLCANO_REJECTED = [
    (query, "not supported on the Volcano path")
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
    # TWO correlation keys, and MAX rather than AVG
    "SELECT COUNT(*) AS n FROM laps l WHERE l.speed > (SELECT MAX(l2.speed) FROM laps l2 "
    "WHERE l2.driver_id = l.driver_id AND l2.team = l.team)",
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
    # exists.
    "SELECT l.lap_id AS id FROM laps l WHERE l.speed > 0.2 * "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) ORDER BY id LIMIT 10",
    # ARRIVED FROM WEEK33_CORRELATED_IN_SHAPES. The correlation is INSIDE the IN
    # body and relative to that block, so the scalar rewrite runs during the
    # body's own build() - two lowerings stacked on two different spines in one
    # query, which nothing else here exercises.
    "SELECT d.name AS nm FROM drivers d WHERE d.driver_id IN "
    "(SELECT l.driver_id FROM laps l WHERE l.speed > "
    "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)) ORDER BY nm",
]

WEEK34_CORRELATED_SCALAR_VOLCANO_REJECTED = [
    (query, "not supported on the Volcano path")
    for query in WEEK34_CORRELATED_SCALAR_VEC_ONLY
]

# The scalar shapes decorrelation declines, in all four modes. A NON-AGGREGATE
# body is the important one: after the rewrite the GROUP BY makes one row per key
# by construction, so Week 31's runtime `scalar subquery returned more than one
# row` check has nowhere to live — a query SQL calls an error would return an
# arbitrary row. SQLite answers all of these, so each is a divergence.
WEEK34_CORRELATED_SCALAR_REFUSED = [
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT l2.speed FROM laps l2 WHERE l2.team = l.team)",
     "single aggregate"),
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team LIMIT 1)",
     "LIMIT cannot be decorrelated"),
    ("SELECT COUNT(*) FROM laps l WHERE l.speed > "
     "(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.speed > l.speed)",
     "only an equality between two columns can become a join key"),
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
    (query, "not supported on the Volcano path") for query in MULTIWAY_QUERIES
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

    total_passed = p1 + p2 + p3 + p4 + m_passed + r_passed
    total_failed = f1 + f2 + f3 + f4 + m_failed + r_failed
    total_errors = e1 + e2 + e3 + e4 + m_errors + r_errors
    print(f"\nTotal: {total_passed} passed, {total_failed} failed, {total_errors} errors")
    if total_failed > 0 or total_errors > 0:
        sys.exit(1)

if __name__ == "__main__":
    main()
