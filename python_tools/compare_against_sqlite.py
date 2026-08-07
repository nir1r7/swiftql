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

# Week 30 (subquery parsing + binding). NOTHING here executes: the week ends in a
# refusal, and REACHING that refusal is the assertion — everything before it
# (lex, parse, nested scope resolution, correlation detection, validation of the
# nested query against its OWN schema) had to succeed to get there. SQLite
# answers all of them, which is why this is a rejection suite rather than a diff
# suite: the same stance Week 26 took when multi-way joins bound but did not
# execute ("nothing new this week returns rows to diff").
#
# Run in all four modes. The refusal is engine-independent by construction — one
# check at the end of Validator::validate, which both Planner::plan and
# LogicalPlanBuilder::build call first — and running it everywhere is what proves
# that rather than asserting it.
WEEK30_SUBQUERY_BIND_EXPECT = "not yet executable (Week 31)"
WEEK30_SUBQUERY_BINDS = [
    # scalar, uncorrelated (TPC-H Q22's shape)
    "SELECT team FROM laps WHERE speed > (SELECT AVG(speed) FROM laps)",
    # scalar, correlated (Q17) — and the scalar form must compose with
    # arithmetic, which is why it lives at the primary level
    "SELECT l.lap_id FROM laps l WHERE l.speed > "
    "0.2 * (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team)",
    # scalar in HAVING, uncorrelated (Q11)
    "SELECT team, AVG(speed) FROM laps GROUP BY team "
    "HAVING AVG(speed) > (SELECT AVG(speed) FROM laps)",
    # EXISTS, correlated (Q4/Q21) — `select *` inside must stay legal, so the
    # arity rule cannot apply to EXISTS
    "SELECT d.name FROM drivers d WHERE EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id AND l.speed > 340)",
    # NOT EXISTS (Q21): the leading-NOT production, which parseCompare's NOT
    # lookahead cannot see because it fires only after a complete left operand
    "SELECT d.name FROM drivers d WHERE NOT EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id)",
    # ...and in the middle of an AND chain
    "SELECT d.name FROM drivers d WHERE d.age > 30 AND NOT EXISTS "
    "(SELECT * FROM laps l WHERE l.driver_id = d.driver_id)",
    # IN (subquery), uncorrelated (Q18/Q20) — a DIFFERENT production from the
    # constant list, which still parses as an InExpr
    "SELECT name FROM drivers WHERE driver_id IN (SELECT driver_id FROM laps)",
    # NOT IN (subquery) (Q16)
    "SELECT name FROM drivers WHERE driver_id NOT IN "
    "(SELECT driver_id FROM laps WHERE speed > 340)",
    # nested two deep, the inner one correlated to the MIDDLE block (Q20)
    "SELECT name FROM drivers d WHERE d.driver_id IN "
    "(SELECT l.driver_id FROM laps l WHERE l.speed > "
    " (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team))",
    # correlated across a JOIN in the outer query: the ref names relation 1, so
    # (query_level 1, relation_slot 1). This is the shape that binds to the wrong
    # relation if the two numbering domains are conflated
    "SELECT l.team FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "WHERE EXISTS (SELECT * FROM laps l2 WHERE l2.team = d.team)",
    # an inner alias shadowing an outer one, and an unqualified name present in
    # both blocks: the inner block wins both times, and neither is an ambiguity
    "SELECT x.team FROM laps x WHERE EXISTS (SELECT * FROM drivers x WHERE x.age > 30)",
    "SELECT l.lap_id FROM laps l WHERE EXISTS "
    "(SELECT * FROM drivers d WHERE team = 'Ferrari')",
    # a subquery in a query that also uses a LEFT JOIN, so the Week 29 passes are
    # on the same tree
    "SELECT d.name FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id "
    "WHERE d.age IN (SELECT season FROM laps l2)",

    # --- round 1 ---
    # a correlated ref inside the NESTED query's own ON clause. Its slot indexes
    # the OUTER range table, and validateJoinCondition's `relations` is the inner
    # one, so indexing it reported "column 'lap_id' not found in table 'd'" — an
    # error the query is not entitled to, against a relation it never named
    "SELECT lap_id FROM laps l WHERE EXISTS "
    "(SELECT 1 FROM drivers d JOIN drivers d2 ON d.driver_id = d2.driver_id "
    " AND d.age = l.lap_id)",
    # the control for the cross-product refusal below: a REAL inner key beside a
    # correlated residual is legal, and the residual must not be what saves it
    "SELECT lap_id FROM laps l WHERE EXISTS "
    "(SELECT 1 FROM drivers d JOIN laps p ON d.driver_id = p.driver_id "
    " AND p.speed > l.speed)",
    # a correlated GROUP BY key is legal SQL — it is constant within every group.
    # Both spellings must behave the same: the skip used to key on whether the
    # binder had written a qualifier back, which it only does for a block holding
    # two or more relations, so the SAME subquery was refused under a
    # one-relation outer query and accepted under a two-relation one
    "SELECT lap_id FROM laps l WHERE EXISTS "
    "(SELECT COUNT(*) FROM drivers d GROUP BY season)",
    "SELECT l.lap_id FROM laps l JOIN drivers dd ON l.driver_id = dd.driver_id "
    "WHERE EXISTS (SELECT COUNT(*) FROM drivers d GROUP BY season)",
    # BETWEEN clones its left operand before binding and cloneExpr SHARES the
    # statement, so two SubqueryExpr nodes reach one SelectStatement in a single
    # bind. Both must stay marked correlated, or collectSlots stops contributing
    # -1 for the second and pushdown pushes a correlated conjunct onto one scan
    "SELECT lap_id FROM laps l WHERE "
    "(SELECT MAX(d.age) FROM drivers d WHERE d.driver_id = l.driver_id) "
    "BETWEEN 1 AND 99",
    # the IN operand reaches constant folding (dispatch site 14). NOTE: this
    # suite only asserts the query reaches the refusal, which it would do either
    # way — the folding itself is pinned by
    # SubqueryDispatch.ConstantFoldingReachesTheInOperandAndNotTheBody
    "SELECT team FROM laps WHERE season * (2 + 3) IN (SELECT driver_id FROM drivers)",

    # --- round 2 ---
    # a LEGAL correlated aggregate argument. The type check for it moved to the
    # Binder, which is the only layer holding the scope chain, so this is the
    # control that it did not become a blanket refusal
    "SELECT l.lap_id FROM laps l JOIN drivers d ON l.driver_id = d.driver_id "
    "WHERE EXISTS (SELECT SUM(d.age) FROM drivers x)",
    # a LOCAL expression group key must still satisfy its own select item, or
    # adding the query level to exprKey has simply broken expression grouping
    "SELECT lap_id FROM laps l WHERE EXISTS "
    "(SELECT driver_id + 1 FROM drivers d GROUP BY driver_id + 1)",
]

# Each of these must fail EARLIER than the refusal above, and for its own stated
# reason. Without them the Week 31 refusal becomes a catch-all that hides a real
# defect behind a temporary one — the discipline that placed Week 26's multi-key
# refusal past the plan-time type checks.
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
  + WEEK30_ALIAS_REBIND_QUERIES

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

    # Week 30. Two suites, two jobs. The BINDS suite asserts that each required
    # TPC-H subquery form reaches the Week 31 refusal — nothing this week returns
    # rows, so reaching the refusal is the only end-to-end evidence the
    # checkpoint has. The REJECTED suite asserts that everything a query can get
    # wrong earlier still outranks it, which is what stops the refusal becoming a
    # catch-all. Both run in all four modes: the refusal is one check at the end
    # of Validator::validate, and running it everywhere is what proves the two
    # engines agree rather than asserting it.
    week30_binds = [(q, WEEK30_SUBQUERY_BIND_EXPECT) for q in WEEK30_SUBQUERY_BINDS]
    for label, extra in modes:
        for suite, name in ((week30_binds, "Week 30 subqueries bind"),
                            (WEEK30_REJECTED_QUERIES, "Week 30 rejections")):
            rp, rf, re_ = run_rejection_suite(
                suite, f"{name} — {label}", extra_args=extra)
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
