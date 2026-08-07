# Week 34 Round 1 Audit — IN PROGRESS

Range: fe17f40..HEAD, branch claude/phase5-week26-qomtkb. Started ~14:41 UTC.
Targets: Q17 decorrelation, countRelations fix, slot-0 stamping, COUNT(DISTINCT),
citation sweep, derived-table alias-list oracle blind spot.

## Findings

(appending as confirmed)

---

## F1 — BLOCKER — Q17's LEFT join returns NULL where SQL requires 0 for a COUNT body

`src/planner/subquery_decorrelation.cc:270-284` (the `join->join_type = JoinType::LEFT;`
block) and its rationale at `src/planner/subquery_decorrelation.h:87-92`.

The comment states the rule as "SQL says a scalar subquery over zero rows is NULL".
That is true for AVG/SUM/MIN/MAX and **false for COUNT**: an aggregate query with no
GROUP BY over an empty input returns exactly one row with `COUNT = 0`. The correlated
scalar subquery therefore evaluates to `0`, not NULL, for an outer row with no matching
body rows.

`requireDecorrelatableScalarBody` (`src/planner/subquery_decorrelation.cc:170-188`)
accepts *any* single aggregate, COUNT included. The rewrite groups the body by the
correlation key, so a key with zero rows produces **no group row at all** — the LEFT
join then NULL-extends it. NULL, not 0.

Concrete input (data/ shipped in-repo, every body row filtered out by `speed > 999`):

```
SELECT d.name FROM drivers d
WHERE d.age > (SELECT COUNT(*) FROM laps l
               WHERE l.driver_id = d.driver_id AND l.speed > 999)
```

- SQLite (python3 sqlite3, equivalent 2-row fixture and the shipped shape): every outer
  row qualifies, because the subquery is `0` and `age > 0`.
- SwiftQL `--storage columnar --execution vectorized --no-cache`: **0 rows**.
  Verified against `build/swiftql` (binary is current: built 14:38:25, last src commit 14:38:43).
- The AVG control of the same shape correctly returns 0 rows in both.

Silent wrong answer, in the exact deliverable Week 33 missed. It is invisible to the
oracle harness only because no Week 34 harness query uses a COUNT body in a correlated
scalar position.

Minimal fix, either:
 (a) refuse a COUNT body in `requireDecorrelatableScalarBody` by name (consistent with
     the week's "refuse rather than mis-rewrite" discipline), or
 (b) wrap the replacement `ColumnRef` in `COALESCE(x, 0)` for COUNT bodies only —
     which needs a COALESCE the dialect may not have, so (a) is the minimum.

Note the same class extends to any body expression that is non-NULL over an empty input;
the guard only admits a single bare aggregate, so COUNT is the whole reachable set today.

---

## Verified clean (with the evidence)

**Q17 itself.** `SELECT l.team, COUNT(*) FROM laps l WHERE l.speed > 1.02 *
(SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team) GROUP BY l.team ORDER BY
l.team` — SwiftQL optimized and `--no-optimize` both match python3 sqlite3 on the same
10 000-row laps data exactly (7 groups, 394/619/1012/596/398/628/390). The LEFT join is
right for the AVG/SUM/MIN/MAX families (SQLite: zero-row group -> NULL, confirmed);
F1 is the COUNT exception.

**The non-aggregate refusal fires, and fires first.**
`requireDecorrelatableScalarBody` (`src/planner/subquery_decorrelation.cc:167-188`) is
called at `:214`, immediately after the body is moved out and *before* `splitConjuncts`,
`splitCorrelation`, the body plan build and the join graft. `refuse` throws
(`:11-13`) and nothing in `src/planner/` catches `std::runtime_error` around planning
(only `constant_folding.cc:92,111`, which are unrelated), so the throw reaches the CLI.
Confirmed on three shapes: a bare column body, a body that is provably one row per key,
and `AVG(x)*2` (not a *bare* aggregate) — all three refused by the named message.
Note the body has already been `std::move`d out of the shared `SelectStatement` when the
refusal fires; that is only safe because the throw is terminal. If a future caller ever
catches and retries, it re-plans an emptied body. Not a defect today.

**Target 2 — countRelations counting the spine.** `join_enumeration.cc:57-77`. It now
returns 1 for `DERIVED` and, for a non-STANDARD join, only the left spine. That is
strictly *smaller* than the old scan count, so `slot >= n` became **stricter**, not
looser — there is no shape that now passes a slot check it should fail. Verified
positively that the search still reorders a derived relation:
`SELECT d.name, x.c FROM drivers d JOIN (SELECT driver_id, COUNT(*) AS c FROM laps
GROUP BY driver_id) x ON d.driver_id = x.driver_id JOIN drivers e ON d.driver_id =
e.driver_id` reports `order=drivers@0,@1,drivers@2 cost=40 (written=40) method=dp` with
`LogicalDerived` taken as a leaf, and optimized == `--no-optimize` on results. The absent
decline string is the right call — `hasSlotOutsideRangeTable` (`:133-141`) tests
`join_slot` and `from_slot`, and a derived relation carries an in-range `join_slot`.

**Target 3 — slot-0 stamping.** `logical_plan.cc:484-492` stamps every derived column
`relation_slot = 0`; the outer slot is applied only by the merged-schema loops
(`logical_plan.cc:529-534` for FROM/JOIN, `subquery_decorrelation.cc:257-262` for the
scalar rewrite). Both dependent claims hold at the code and in behaviour:
`restampSlots(c, 0)` (`predicate_pushdown.cc:175`) stamps a pushed conjunct to 0, which
is exactly the slot a derived leaf's own schema carries; and `ChunkPruner`'s `< 1` test
(`chunk_pruner.h:21-31`) stays sound because the pruning hint is consumed only by
`VecScanNode` (`vec_scan_node.cc:22`) against its own table's zone maps, and a
derived-relation predicate is not routed into the body's scan. Adversarial check for the
name-collision hazard: `SELECT COUNT(*) FROM (SELECT speed AS driver_id, team FROM laps)
x WHERE x.driver_id > 300` returns 6968 in both optimizer modes, which equals the true
`speed > 300` count — i.e. it did NOT prune `laps` on `driver_id`'s zone maps (1..20),
which is the silent-wrong-answer this test was built to trip.

**Target 4 — COUNT(DISTINCT).** Both engines share the rule and the encoding.
Vectorized `vec_hash_aggregate_node.cc:205-212, 258-263`; Volcano `plan_nodes.cc:286-289,
329-333`. NULL exclusion is structural in both — the distinct insert sits *after*
`if (val.isNull()) continue;`, so `COUNT(DISTINCT x)` counts distinct non-NULL values,
which is SQL. Keys go through `appendGroupKeyField`, not `Value::toString`, so the Week 27
`%.15g` collapse cannot recur. Behaviour, byte-identical across `--storage row
--execution volcano` and `--storage columnar --execution vectorized`:
duplicates within a group (7 teams, distinct driver counts 2/3/5/3/2/3/2 against
977..2556 rows), an empty group (`WHERE speed > 999 GROUP BY team` -> zero rows in both,
no phantom group), a scalar aggregate over empty input (`COUNT(DISTINCT team)` -> `0` in
both, one row — correct). NULLs are not reachable from CSV (invariant 14) and are pinned
only structurally, which is the pre-existing limitation, not a Week 34 regression.

**Target 5 — citation sweep, spot-checked on four sites, all updated:**
`src/parser/ast.h` (the "FROM is Week 34" promise at old :155 is gone; the surviving
comment is about ownership); `src/planner/subquery_materialization.cc:172` (now records
Week 34 was named as the candidate and *is not one*, checked); `src/planner/
vectorized_plan_builder.cc:295-311` (the old "no STANDARD join is ever built above a semi
join" build-order claim is replaced by the DERIVED lowering case, which is the fact that
dissolved it); `src/planner/subquery_decorrelation.h:36-43` (condition 3 now scopes
itself to `requireDecorrelatableBody` and names the Week 34 sibling that inverts it).
`development.md:780-795` records how the Week 32 containment was replaced. No stale
citation found in the sample.

## F2 — LOW — the alias-list tests pin the schema, not the behaviour they justify

`tests/test_logical_plan.cc:1702-1717`. The two tests are the whole coverage for a
feature SQLite cannot parse, and they assert only `derived->output_schema.column(i).name`
and an arity `EXPECT_THROW`. The comment directly above them
(`tests/test_logical_plan.cc:1699-1701`, echoing `vectorized_plan_builder.cc:291-294`)
justifies the feature by saying the rename "has to survive into the plan schema, because
resolveColumnIndex and every indexOf above the graft look the new names up" — and
neither test reaches `resolveColumnIndex`, a physical plan, or a row. Nothing pins that
the pre-rename name stops resolving, which is the half that can silently regress into a
wrong-column read.

I verified the behaviour by hand against `build/swiftql` and it is correct today:
`SELECT d.a, d.b FROM (SELECT team, speed FROM laps) AS d (a, b)` returns the team and
speed values under the new names; `SELECT d.team FROM (...) AS d (a, b)` -> `column
'team' not found in 'd'`; the bare `SELECT team` form -> `SELECT: column not found`;
`SELECT *` expands to `a, b`. So this is a coverage gap, not a live defect. Minimal fix:
one `EXPECT_THROW` on the pre-rename qualified name and one execution-level assertion.

## Not reached
Nothing outstanding on targets 1-6 beyond the above. Not attempted (out of budget):
the harness suite definitions in `python_tools/compare_against_sqlite.py`, the Binder
`Scope::owned_schemas` lifetime, and `VecDerivedNode`'s own operator lifecycle.
