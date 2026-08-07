# Week 34 Round 2 Audit

Range: fe17f40..b1cf07e (ignoring `chore:` scratchpad-only commits), branch
claude/phase5-week26-qomtkb. Started ~15:41 UTC.

Targets, in priority order:
1. harness suite definitions in `python_tools/compare_against_sqlite.py`
2. the three Week 34 fixes as new code (COUNT CASE wrapper, VecDerivedNode row count,
   derived-table alias execution tests)
3. Q17 end to end
4. anything else risky

## Findings

(appending as confirmed)

---

## F1 — MEDIUM — the only TWO-CORRELATION-KEY oracle entry cannot fail if a key is dropped

`python_tools/compare_against_sqlite.py:1042-1044` (entry index 2 of
`WEEK34_CORRELATED_SCALAR_VEC_ONLY`), whose comment claims "TWO correlation keys,
and MAX rather than AVG":

```sql
SELECT COUNT(*) AS n FROM laps l WHERE l.speed > (SELECT MAX(l2.speed) FROM laps l2
WHERE l2.driver_id = l.driver_id AND l2.team = l.team)
```

The correlated body's group always CONTAINS the outer row itself, so
`MAX(l2.speed) >= l.speed` for every row by construction and the answer is `n = 0`
no matter what the join does. Measured on the shipped data, `./build/swiftql
--execution vectorized --storage columnar --no-cache`:

| query | n |
|---|---|
| both keys (the suite entry) | 0 |
| `l2.driver_id = l.driver_id` only | 0 |
| `l2.team = l.team` only | 0 |

Dropping either correlation key changes nothing, so the entry does not test the
property its comment names. Worse, `0` is also what a total failure produces — a
rewrite that emitted no rows, or that left the scalar NULL for every key, passes
this entry identically. It is the "returns the trivially-safe answer" class the
week already fixed once (the 0.2-coefficient IN entry at `:1112-1117`, which the
author caught and paired with a discriminating 1.10 twin at `:1146-1149`); this one
was not caught, and it is the **only** two-key entry in the week, so
`splitCorrelation` emitting one join key instead of two is currently invisible to
the oracle.

Minimal fix: make the body's group exclude the outer row, e.g.
`... AND l2.lap_id <> l.lap_id`, or compare against a key set that discriminates —
`l.speed > (SELECT MAX(l2.speed) FROM laps l2 WHERE l2.driver_id = l.driver_id
AND l2.season = 2024)` differs from its one-key form. Any variant whose one-key and
two-key answers differ on this data restores the property.

---

## F2 — LOW — a comment that counts two tests where three now stand

`tests/test_vec_plan_builder.cc:1281-1283`: "These **two** tests are the feature's
ENTIRE possible coverage". Three tests follow it
(`RenamedColumnsCarryTheOriginalColumnsValues`, `ThePreRenameNameNoLongerResolves`,
`SelectStarExpandsToTheRenamedColumns`). The count was right for the two
schema-only predecessors the same commit replaced; the sentence was not updated
when the third landed. No behavioural effect — recorded because the comment is the
only place the coverage boundary is stated, and a reader counting tests against it
will conclude one is stray.

## F3 — LOW / partly a hunch — `ThePreRenameNameNoLongerResolves` matches any `std::runtime_error`

`tests/test_vec_plan_builder.cc:1316-1325`. Both `EXPECT_THROW(..., std::runtime_error)`
assertions accept *any* runtime error, not the resolution failure they name. That is
the leniency the harness's own `run_rejection_suite`
(`python_tools/compare_against_sqlite.py:1696-1704`) explicitly refuses — it matches
the message so "an unrelated failure" cannot pass. Concretely: a mutant that made
`derivedRelationSchema` throw on every alias list (say an off-by-one in the arity
check) leaves this test GREEN while the feature is entirely dead. It is not vacuous
— the two sibling tests fail under that mutant — so this is a weakening, not a hole.
Minimal fix: `EXPECT_THROW`+`catch` on the substring, as the rejection suites do.

---

## Verified clean, with the evidence

**Target 3 — Q17 end to end. All 22 entries of `WEEK34_CORRELATED_SCALAR_VEC_ONLY`
match python3 sqlite3 exactly** on the shipped 10 000-row data, run through
`compare_against_sqlite.run_swiftql` with `--execution vectorized --storage columnar
--no-cache`. Row counts, SwiftQL == SQLite throughout: 1/7/1/1/1/1/10/2084/2084/2198/
4986/1/1/1/1/1/1/1/20/1/20/19/1. The zero-row-group block behaves as SQL requires,
per aggregate kind: `COUNT(*)`, `COUNT(l.lap_id)` and **`COUNT(DISTINCT l.season)`**
over an all-empty body each give `n = 20` (every driver qualifies, i.e. the scalar
read 0), while `AVG` / `SUM` / `MIN` / `MAX` over the same empty body each give
`n = 0` (the scalar read NULL, comparison UNKNOWN). Group of one and duplicate keys:
the mixed entry (`l.lap_id < 60`, 19 of 20 keys non-empty) returns 20 rows == SQLite,
and the all-groups-exist COUNT entry returns 0 == SQLite, so the CASE wrapper is a
no-op where a group exists. Round 1's F1 is closed in behaviour, not just in code.

**Target 2a — the COUNT wrapper keys on the FUNCTION.**
`src/planner/subquery_decorrelation.cc:261` tests `agg->function_name == "COUNT"`,
and `src/parser/parser.cc:577` upper-cases `function_name` at parse, so `count(*)`
and `COUNT(DISTINCT x)` both hit it and the `distinct` flag is not consulted — which
is what the `COUNT(DISTINCT l.season)` oracle entry confirms at runtime. It cannot
reach SUM/AVG/MIN/MAX: the branch at `:317` is the sole writer of the CASE and it is
guarded by that one equality; the four NULL-family oracle entries returning `n = 0`
are the behavioural proof that it is not over-applied. Both CASE branches type as
INT — `aggregateResultType` returns `TypeId::INT` for COUNT
(`src/planner/logical_plan.cc:83`) and the literal is `Value(static_cast<int64_t>(0))`
(`subquery_decorrelation.cc:341`) — so `inferExprType`'s CASE unification
(`logical_plan.cc:43`) needs no promotion.

**Target 2b — `VecDerivedNode` row counting.** `src/execution/vec_derived_node.cc:38-42`
now uses `chunk->filter_applied ? chunk->sel.indices.size() : chunk->num_rows`, which
is character-for-character the convention at `src/execution/vec_limit_node.cc:19-22`.
Confirmed live on `SELECT d.a FROM (SELECT team AS a, speed AS b FROM laps WHERE
speed > 340) AS d WHERE d.a = 'Ferrari'` with `--explain-analyze`: `VecDerived`
reports `rows_in=770 rows_out=770` (the surviving count) over a `VecFilter` that
reports `rows_out=770` out of 10 000 — pre-fix this read the buffer width.
**No other vec node has the defect.** The complete set of row-count writers is
`vec_derived_node.cc:40-41`, `vec_filter_node.cc:38-39`, `vec_hash_join_node.cc:229,360`,
`vec_limit_node.cc:30-31,49-50`, `vec_project_node.cc:121-122`, `vec_scan_node.cc:89`,
`vec_simd_loop_join_node.cc:205`. The only one that reads `num_rows` unguarded is
`vec_filter_node.cc:38` (`rows_in += raw->num_rows`), and that is sound because a
`VecFilter` never stacks directly on another: the plan above puts a materializing
`VecProject` between them (verified in the `--explain-analyze` above — the outer
filter's `rows_in=770` is exact). Distinct / sort / hash-aggregate record no row
counts at all, so there is nothing there to be wrong.

**Target 1 — suite wiring.** No suite is defined and left unused: every
`^[A-Z_]+ = [` list in the file is referenced at least twice (definition plus a
`main()` call or a derived `*_VOLCANO_REJECTED` comprehension). Both Week 34
cross-suite MOVES landed on both ends — the arrival in
`WEEK34_CORRELATED_SCALAR_VEC_ONLY` AND the removal from the source list, with only
the explanatory comment left behind: `WEEK33_CORRELATED_BINDS` (:857-866, Q17's
arithmetic shape) and `WEEK33_CORRELATED_IN_SHAPES` (:830-840, the nested IN whose
scalar is correlated to the IN body). Neither string survives in its old suite, so
the "arrival landed, removal no-oped" defect did not recur. `WEEK34_DISTINCT_AGG_QUERIES`
really is diffed in all four modes — it is concatenated into `QUERIES` at `:1568`,
which `main()` runs in the four mode configurations at `:1758-1775` — matching its
comment. The two Week 34 Volcano-refusal families produce the message their suites
pin, checked directly: a derived table (both plainly and nested inside a subquery
body) reports "derived tables (FROM (subquery)) are not supported on the Volcano
path", and a correlated scalar reports "correlated subqueries are decorrelated to a
semi-join and are not supported on the Volcano path".

**`WEEK34_DERIVED_TABLE_VEC_ONLY` and `WEEK34_DISTINCT_AGG_QUERIES` are not trivial
passes.** All 10 derived and all 7 distinct entries match SQLite exactly, with
answers that discriminate: 5/7/10/10/10/19/10/5/1/770 rows for derived (the aggregate
bodies carry real values, e.g. `s=313.25153531218` matched to 15 digits), and for
distinct `d=3245` on the DOUBLE key-encoding entry — the Week 27 number that a
`Value::toString()`-keyed set gets wrong by 719 — plus `b=0` on the NULL-exclusion
entry (SwiftQL's `x/0` really is NULL, otherwise `COUNT(DISTINCT 0)` would be 1) and
`a=10000, b=20` on the dedupe entry, which a spec collapse would report as 20/20.
The two weakest derived entries (:1039 and :1058, both `ORDER BY t LIMIT 5` over a
single team's rows) still fail on a wrong-column read, since the alternative columns
are numeric.

---

## Not reached, stated plainly

- The Week 26–32 rejection suites were checked for WIRING (all nine are called from
  `main()`, in the modes their comments name) but their individual entries were not
  re-derived for discriminating power — only the Week 33 and Week 34 ones were.
- Target 4 (anything else risky in the week) was not worked as its own pass; the
  parser/binder/validator diff was read only where the three fixes touch it.
