# Week 32 round 3 audit — semi-joins and anti-joins

Branch `claude/phase5-week26-qomtkb`, HEAD `8297bc6`, diff base `0fcf2b7`.
Scope: only the four surfaces round 2 (`scratchpad/audits/week-32-round-2.md`)
did not reach. Nothing round 1 or round 2 covered is re-derived.

Method: read-only. No build, no test run (a gate owns `build/`).

Tally: **0 blockers, 1 high, 2 medium, 3 low.**

---

## Target 3 — the `subquery_materialization` removals

### Clean

- `MAX_MATERIALIZED_IN_VALUES` and `distinctNonNull` have **no surviving
  reader**. `grep -rn` over `src/`, `tests/`, `python_tools/` and `*.md` returns
  only prose: `src/planner/subquery_materialization.h:54` (the tombstone
  comment), `src/planner/subquery_materialization.cc:232`,
  `tests/test_subquery.cc:211`, `README.md:1567`, and `docs/week-3{1,2}-plan.md`.
  No test asserts the cap, and the rejection-suite entry that pinned it was
  removed with a comment recording where the query went
  (`python_tools/compare_against_sqlite.py:627-635`). ✅
- The `Kind::IN` throw at `src/planner/subquery_materialization.cc:228-244` is
  genuinely unreachable on today's code. `buildReplacement` has exactly one
  caller — the `visit` lambda at `:261` — and `:264` returns before it for
  `Kind::IN`. Both walkers that feed `visit` (`forEachStatementExpr` and the
  GROUP BY loop at `:289`) go through the same lambda. ✅
- `stmt.has_subquery = in_survives` (`:307`) is correct for the
  `buildScanSchema` widening it feeds (`src/planner/logical_plan.cc:276`). ✅

### HIGH-1 — a subquery nested inside an `IN` body is a Week 31 regression, and
### it surfaces as an "internal invariant" throw

`src/planner/subquery_materialization.cc:264` + `:171`.

Concrete input, legal SQL that Week 31 answered:

```sql
SELECT name FROM drivers WHERE driver_id IN
  (SELECT driver_id FROM laps WHERE speed > (SELECT AVG(speed) FROM laps))
```

Trace. `forEachSubquery` never descends into a body — it visits only
`sq->operand` (`src/planner/subquery_materialization.cc:43-48`). Under Week 31
the body was materialized **from inside `runOnce`**, which recurses at `:171`
("Innermost first") before moving the statement. Week 32's `:264` returns for
`Kind::IN` *before* `runOnce`, so for an IN node that recursion never happens
and **nothing materializes the body's own subqueries**. The body then reaches
`planBody` (`src/planner/subquery_lowering.cc:25`) →
`LogicalPlanBuilder::build` → the body's `SCALAR` conjunct is not an IN, so
`lowerInSubqueries` keeps it and `refuseUnloweredIn` (`:99-105`, which only
looks for `Kind::IN`) passes it → `inferExprType` at
`src/planner/logical_plan.cc:789` hits dispatch site 12
(`src/planner/logical_plan.cc:244-252`) and throws.

Two problems, not one:
1. **Capability regression, unrecorded.** The shape worked in Week 31 and now
   errors. It is not in `README` → Syntax Deliberately Not Supported, not in
   `WEEK32_LOWERING_REFUSED`, and not in any rejection list.
2. **The diagnostic is wrong about its own cause.** Site 12's message and
   comment say reaching it "means the materialization walker (dispatch site 19)
   missed an `Expr` subtype, or the pass was not run at all" — an internal
   defect report for a user query that is simply unsupported. Every other
   unsupported shape this week gets a named refusal.

**The test that covered this was rewritten to avoid the shape.**
`tests/test_subquery.cc:244-247` changed
`MaterializesTheInnerBodyBeforeRunningTheOuterOne` from
`driver_id IN (SELECT ... WHERE speed > (SELECT AVG(speed) FROM laps))` to a
scalar outer form, with the note "an IN node is no longer materialized, so it
would contribute no run". That is exactly right about the run count and exactly
the edit that removes the only coverage of nested-inside-an-IN-body. Nothing
replaced it.

Minimal fix: either recurse into the body before skipping (call
`materializeSubqueries(*sq->subquery, run)` in the `Kind::IN` arm of `visit`,
which is legal — the body is not moved there), or add a named refusal plus a
README row and a rejection-suite entry.

---

## Target 1 — `tests/test_subquery.cc`

### Verified genuine

- `SemiJoinLowering.RefusesABodySharedByTwoExpressions`
  (`tests/test_subquery.cc:530`) is real, and the round-1 concern is answered:
  `ASSERT_EQ(...use_count(), 2u)` proves the sharing was actually constructed
  before `lowerInSubqueries` is called, so the refusal at
  `src/planner/subquery_lowering.cc:20` is what throws, not the ColumnRef check
  at `:46` (the operand is a plain `driver_id`).
- `SemiJoinLowering.RefusesTheShapesItCannotExpress` — all three needles match
  the message the code really emits: `src/planner/subquery_lowering.cc:48-50`
  for the computed operand, and `:106-108` for both the OR case
  (`src/planner/logical_plan.cc:784`, clause `"a non-top-level position"`) and
  the HAVING case (`src/planner/logical_plan.cc:818`, clause `"HAVING"`). The
  HAVING query has no `WHERE`, so it skips the lowering block entirely and
  reaches the HAVING refusal — the routing the comment claims. ✅
- `SubqueryMaterialization.LeavesAnInNodeForSemiJoinLowering`
  (`tests/test_subquery.cc:190`) — `EXPECT_EQ(runs, 0)` and
  `EXPECT_TRUE(stmt.has_subquery)` both discriminate: the first fails if the
  `Kind::IN` early-return at `src/planner/subquery_materialization.cc:264` is
  removed, the second if `:307` reverts to `= false`. ✅

### MEDIUM-1 — `NoLongerRefusesALargeInSet` tests no large set and cannot fail

`tests/test_subquery.cc:214-220`. The test name, and the comment above it,
say the property is "a large result is no longer a refusal". The body feeds
`canned({oneCol("driver_id", TypeId::INT), {}})` — an **empty** result — and
asserts `EXPECT_NO_THROW`. Two independent reasons it cannot fail:

1. The runner is never invoked at all: `visit`
   (`src/planner/subquery_materialization.cc:264`) returns before `runOnce` for
   `Kind::IN`, so the canned rows are never read. The test would pass with any
   contents, including a million rows.
2. Even under Week 31's code the assertion would have passed, because the cap
   compared `values.size() > 1024` and the set is empty.

Concrete consequence: restore Week 31's `MAX_MATERIALIZED_IN_VALUES` branch
verbatim and this test still passes. It is the only test that names the removed
cap, so the cap's removal has **no** executable coverage in this file. The
honest version is either `runs == 0` with >1024 canned rows, or deletion —
`WEEK32_SEMI_JOIN_VEC_ONLY`'s first query (`lap_id IN (SELECT lap_id FROM
laps)`, 10 000 distinct values) is what actually covers the removal.

### LOW-1 — `SemiJoinLowering.NotInBecomesAnAntiJoin` ends in a no-op assertion

`tests/test_subquery.cc:480`: `EXPECT_NE(plan->explain().find(""),
std::string::npos)`. `std::string::find("")` returns `0` for every string, so
the comparison is `0 != npos` — true unconditionally, for any plan, including an
empty explain. It appears to be a truncated edit (the needle was dropped). The
line above it, `EXPECT_EQ(j->semantics, JoinSemantics::ANTI)`, is what carries
the test; the explain text for ANTI is covered separately by
`SemiJoinLowering.ExplainNamesTheKind` (`:485`), so nothing is uncovered — but a
`find("")` reads as an intentional check and will be trusted by the next reader.

### LOW-2 — `OutputSchemaIsTheLeftChildsAndTheSlotIsMinusOne`'s schema loop is
### unreachable

`tests/test_subquery.cc:459-470`. The per-column `name`/`relation_slot`
comparison is a strict subset of the assertion `lowerInSubqueries` already makes
at construction (`src/planner/subquery_lowering.cc:78-88`, which also compares
`type`). Any divergence throws `"internal: semi/anti join output schema must be
its left child's"` inside `planLowered`, so the test would report an uncaught
`std::runtime_error` rather than a schema diff, and the loop itself can never
report a failure. The `EXPECT_EQ(j->join_slot, -1)` and
`EXPECT_EQ(j->join_type, JoinType::INNER)` assertions above it are the ones that
can fail. Not wrong — just not the check the test's name promises.

---

## Target 2 — the 13 `WEEK32_SEMI_JOIN_VEC_ONLY` oracle queries

`python_tools/compare_against_sqlite.py:475-551`. Read query by query and
hand-evaluated against the correct answer, asking of each whether a plausible
wrong implementation would produce a different number.

### Verified discriminating

- **Duplicate build keys** (`:492-496`). `team IN (SELECT team FROM drivers)` —
  `drivers` has 20 rows over a handful of teams, so an inner-join-plus-projection
  emits each `laps` row once per matching driver. `COUNT(*)` differs by roughly
  5x. The `GROUP BY team` variant localises which team. Genuinely the strongest
  query in the set. ✅
- **Empty build, both polarities** (`:500-504`). `speed > 99999` is empty over
  the shipped data. `IN` → 0, `NOT IN` → every driver. The two answers are on
  opposite ends, so an operator that confused the empty-build branch's polarity
  fails one of them. ✅
- **NULL on the build side, ANTI** (`:515-517`). The `LEFT JOIN … AND l.speed >
  99999` body yields `l.driver_id` NULL for every row, so the set is all-NULL.
  Correct answer 0. An implementation that *drops* NULLs from the build set
  (Week 31's `distinctNonNull` shape) sees an empty build and emits every driver
  — a different number, so the query does catch the classic defect. ✅
- **NULL on the build side, SEMI** (`:518-520`). Body is `{1..4} ∪ {NULL…}`;
  correct answer is the count of drivers with `driver_id < 5`, which is neither
  0 nor all — so it separates "NULL poisons SEMI too" (would give 0) from
  correct. ✅
- **NULL on the probe side, ANTI** (`:527-528`). Every probe key is NULL and the
  build is non-empty; correct answer 0. A naive "no match → emit" ANTI gives the
  full row count. ✅
- The `IN` operand's slot being a real slot above a join spine (`:533-537`) and
  the two-stacked-semi-joins query (`:544-545`) both exercise shapes no unit
  test covers end to end. ✅

### LOW-3 — the NULL-probe `IN` query and the "NULL x included" claim are the
### two weak entries

- `python_tools/compare_against_sqlite.py:525-526`
  (`… WHERE l.driver_id IN (SELECT driver_id FROM laps)`) has correct answer 0,
  and the natural wrong implementation ("NULL never matches → drop the row")
  *also* gives 0. It cannot fail unless SEMI starts emitting NULL-keyed probe
  rows, which no plausible implementation does. Its ANTI sibling on the next
  line carries the pair.
- The comment at `:498-499` claims the empty-build entries cover
  `x NOT IN ()` being TRUE "for every row, **NULL x included**". They do not:
  both probe `drivers.driver_id`, which is never NULL over the shipped data, and
  no query in the list combines a NULL probe key with an empty build. That
  combination is covered only at unit level
  (`VecSemiJoin.ANullProbeKeyIsDroppedUnlessTheBuildSideIsEmpty`,
  `tests/test_vectorized.cc:4288`). The property is not uncovered; the oracle
  comment overstates which layer covers it. The missing query is
  `SELECT COUNT(*) FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id
  AND l.speed > 99999 WHERE l.driver_id NOT IN (SELECT driver_id FROM laps
  WHERE speed > 99999)`.

### Checked and clean

- **Same question to both engines.** `WEEK32_SEMI_JOIN_VEC_ONLY` holds plain SQL
  strings passed unchanged to both SQLite and the CLI by `run_query_suite`;
  there is no per-engine rewriting anywhere in the week-32 block
  (`:1355-1370`). ✅
- **The Volcano half is derived, not retyped** (`:552-555`), so a query added to
  the diffed list cannot lose its refusal leg. ✅
- **`vec_modes` is `(vectorized, columnar)` and `(vectorized, columnar,
  --no-optimize)`** (`:1268-1273`) — i.e. "two vectorized modes" means optimizer
  on and off over columnar storage, not two storage layouts. That matches the
  engine: `src/cli/main.cc:428` only installs a subquery runner for
  `vectorized && columnar`. The comments' "two modes rather than four" is
  accurate. ✅
- **The moved rejection entry is recorded, not silently dropped**
  (`:627-635`). ✅

---

## Target 4 — README and `development.md`

### MEDIUM-2 — `development.md`'s Week 32 consumer table states a row that the
### code contradicts (round 2's HUNCH, now grounded)

`development.md`, *Week 32 consumers (one plan, two range tables)*, the
`VectorizedPlanBuilder, SEMI/ANTI lowering` row:

> **Safe by domain.** `leftKeyIndices` against the probe (outer spine) schema,
> `rightKeyIndices` against the body's own…

and the bullet three lines above it:

> `join_slot` is **-1** … Every reader of `join_slot` must decline on
> `semantics != STANDARD` or be provably unreachable for such a node.

`collectSlotTables` (`src/planner/vectorized_plan_builder.cc:115-130`) is a
reader of `join_slot` inside `VectorizedPlanBuilder` and does **neither**. Line
119 is `out[join->join_slot] = leafScanTable(join->children[1].get())` with no
`semantics` test, so on a semi/anti node it stamps the **body's** table name at
key `-1`. Round 2 established it is reachable — `SELECT l.lap_id FROM laps l
WHERE l.driver_id IN (…) AND l.lap_id IN (…)`, the query
`VecPlanBuilder.TwoInConjunctsLowerToTwoStackedSemiJoins`
(`tests/test_vec_plan_builder.cc:1257`) already builds, reaches it via the cost
block at `src/planner/vectorized_plan_builder.cc:216-225`.

The row is not merely incomplete: it names the two `*KeyIndices` calls and the
output schema as *the* reads the builder performs, which a later week reads as
"this consumer has been checked". This is the third time this table has been
wrong by omission. The row should say the builder has one unguarded reader and
name it, whether or not MEDIUM-2 of round 2 is fixed first.

### Verified correct

- README dialect-table rows (`README.md`, the three added `IN` rows and the
  Volcano row) each quote a message string that exists verbatim in the code:
  `src/planner/subquery_lowering.cc:49-50`, `:107-108`, and
  `src/planner/planner.cc:60-62`. Each names the suite that pins it, and both
  named suites exist. ✅
- The removed cap row and its replacement Limitations bullet
  (`README.md:1567`) are consistent with the code: `MAX_MATERIALIZED_IN_VALUES`
  is gone, not raised. ✅
- The README's "The routing is total: there is no shape where an `IN` subquery
  materializes" is true of `src/planner/subquery_materialization.cc:264`. ✅
- `development.md`'s `PredicatePushdown`, `JoinEnumeration`,
  `CardinalityEstimator` and `VecHashJoinNode` rows all match the code as round
  2 traced it. The `buildAggregateSchema` row's claim that its coverage is "a
  query in `WEEK32_SEMI_JOIN_VEC_ONLY` rather than by reading" is true — the
  `GROUP BY driver_id` body at
  `python_tools/compare_against_sqlite.py:550-551`. ✅

### Not covered by any doc

HIGH-1's shape (a subquery nested inside an `IN` body) is absent from the README
dialect table, from Limitations, and from every rejection suite. The README's
new subquery bullets describe the two productions without saying that the
`IN` production stopped materializing its body's own nested subqueries.

---

## Summary

**1 high, 2 medium, 3 low, 0 hunches.**

| id | where | what |
|---|---|---|
| HIGH-1 | `src/planner/subquery_materialization.cc:264` | a subquery nested inside an `IN` body no longer materializes; legal SQL Week 31 answered now throws site 12's internal-invariant message; the covering test was rewritten away |
| MEDIUM-1 | `tests/test_subquery.cc:214` | `NoLongerRefusesALargeInSet` uses an empty set and never runs the body; passes even with Week 31's cap restored |
| MEDIUM-2 | `development.md` Week 32 consumer table | the `VectorizedPlanBuilder` row claims "safe by domain" while `collectSlotTables` (`src/planner/vectorized_plan_builder.cc:119`) reads `join_slot` unguarded |
| LOW-1 | `tests/test_subquery.cc:480` | `find("")` assertion cannot fail |
| LOW-2 | `tests/test_subquery.cc:459` | schema loop subsumed by the production assert that throws first |
| LOW-3 | `python_tools/compare_against_sqlite.py:498,525` | one non-discriminating oracle query; the empty-build comment claims NULL-probe coverage it does not have |

All four targets were reached.
