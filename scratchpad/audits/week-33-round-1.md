# Week 33 Round 1 Audit

Range: `889b6cb..ecee221`, branch `claude/phase5-week26-qomtkb`.
Targets: ColumnId migration, decorrelation correctness, NOT EXISTS semantics,
Week 30 tripwires, stale rationales.

Status: IN PROGRESS (appending as confirmed).

---

## C-1 (CRITICAL, silent wrong answer — whole result set lost) — `NOT EXISTS` DOES get Week 32's `NOT IN` unmatchable-key rule; the header's central claim is enforced by nothing

`src/planner/subquery_decorrelation.h:49-53` states:

> NOT EXISTS AND NULL. Unlike NOT IN, an anti-join IS exactly NOT EXISTS: EXISTS
> is a pure existence test that is never UNKNOWN, so a NULL join key simply fails
> to match and the outer row survives — which is what SQL says. Week 32's
> unmatchable-key machinery exists for NOT IN's three-valued rule and must NOT be
> applied here.

The reasoning is right. **Nothing implements it.** `subquery_decorrelation.cc:145`
sets `join->semantics = sq->negated ? JoinSemantics::ANTI : JoinSemantics::SEMI`
— the *same* `JoinSemantics::ANTI` Week 32 used for `NOT IN`. `VecHashJoinNode`
has no way to tell the two apart, and its ANTI path applies the NOT IN rule
unconditionally.

### C-1a — a single NULL in the body's key column empties the entire result

`src/execution/vec_hash_join_node.cc:223-227`

```cpp
if (semantics_ == JoinSemantics::ANTI && build_had_unmatchable_key_) {
    ...
    continue;   // pull the next probe chunk; this one contributes nothing
}
```

`build_had_unmatchable_key_` is set at line 123 whenever ANY build row has a NULL
(or NaN) key member. For `NOT IN` that is correct. For `NOT EXISTS` it is not.

```sql
SELECT d.name FROM drivers d
WHERE NOT EXISTS (SELECT * FROM laps l WHERE l.driver_id = d.driver_id);
```

If one row of `laps` has `driver_id` NULL, `build_had_unmatchable_key_` is true
and **every probe chunk is skipped: the query returns ZERO rows.**

SQLite: `l.driver_id = d.driver_id` is UNKNOWN for that lap, so it selects
nothing and is simply invisible; every driver with no matching lap is still
returned. Result: SwiftQL returns {} where SQLite returns a non-empty set.

### C-1b — a NULL correlated (probe) key drops the outer row instead of keeping it

`src/execution/vec_hash_join_node.cc:242-250`

```cpp
if (!serializeKey(*probe_chunk, probe_key_idx_, r, key_buf_)) {
    if (semantics_ == JoinSemantics::ANTI && build_keys_.empty()
        && !build_had_unmatchable_key_) { ...emit... }
    continue;
}
```

A NULL probe key is emitted **only when the build side is empty**. That is `NOT
IN`'s rule (`NULL NOT IN S` is UNKNOWN unless S is empty). For `NOT EXISTS` the
correct answer is: the correlated equality is UNKNOWN for every body row, so the
body yields no rows, `EXISTS` is FALSE, `NOT EXISTS` is **TRUE**, and the outer
row must be emitted regardless of whether the body is empty.

```sql
SELECT d.name FROM drivers d
WHERE NOT EXISTS (SELECT * FROM laps l WHERE l.driver_id = d.driver_id);
```
with one `drivers` row whose `driver_id` is NULL and a non-empty `laps`:
SQLite emits that driver; SwiftQL drops it.

### Reachability under invariant 14 (CSV cannot express NULLs)

C-1b is reachable with ordinary CSV data via Week 29's LEFT OUTER JOIN, which
null-extends the probe side:

```sql
SELECT * FROM drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id
WHERE NOT EXISTS (SELECT * FROM laps l2 WHERE l2.lap_id = l.lap_id);
```

A driver with no laps gets `l.lap_id` = NULL, which becomes the NULL probe key.
`laps` is non-empty, so the row is dropped; SQLite keeps it. No NULL needs to be
stored anywhere. C-1a needs a NULL in the body's key column (a LEFT JOIN inside
the body, or a nullable column once storage supports one).

### Why no gate caught it

The Week 33 harness work (`396afdd`, "diff decorrelated EXISTS") diffs against
SQLite, but the outer spines it exercises are plain scans over NULL-free CSV, so
neither NULL path is entered. The header's paragraph is doing the work a test
should be doing.

### Fix

`JoinSemantics::ANTI` must be split — the operator needs to know whether it is
serving `NOT IN` (three-valued) or `NOT EXISTS` (two-valued). Either a fourth
enumerator (`ANTI_EXISTS`) or a `bool three_valued_` on `VecHashJoinNode`. Both
guards above must then be conditioned on it. Until then, decorrelating a
`NOT EXISTS` should be refused rather than lowered to the `NOT IN` operator.

---

## C-2 (CRITICAL, silent wrong answer) — a correlated ref in the body's `JOIN ... ON` is never extracted and never refused; it resolves by bare name against the BODY's schema

`src/planner/subquery_decorrelation.cc:120-135` splits **only `body.where`**.
The body's ON clauses are untouched, and the body's ON residuals are folded into
the body's WHERE *later*, inside `LogicalPlanBuilder::build`
(`logical_plan.cc:796-799`) — after `splitCorrelation` has already run and
cleared `body.where`.

Trace for:

```sql
SELECT * FROM drivers d
WHERE EXISTS (SELECT * FROM laps l JOIN drivers d2 ON l.driver_id = d2.driver_id
                                                  AND d2.team = d.team
              WHERE l.speed = d.speed);
```

1. `splitCorrelation` sees only `l.speed = d.speed` -> `JoinKey{speed, speed, 0}`;
   `body.where` becomes null (`subquery_decorrelation.cc:86`).
2. `LogicalPlanBuilder::build(body)`: `classifyJoinCondition`
   (`join_condition.cc:88-91`) sees `d2.team = d.team`, finds `d.team` is not
   local, and **routes it to `out.residuals`** — Week 30's guard, working as
   designed.
3. `logical_plan.cc:756` folds inner-join residuals into `on_residuals`;
   `logical_plan.cc:796-799` makes them the body's `stmt.where`.
4. `logical_plan.cc:824` `refuseUnloweredCorrelated` only looks for a surviving
   `SubqueryExpr` (`subquery_decorrelation.cc:172-174`). A correlated
   **ColumnRef** is not a subquery, so it passes.
5. `logical_plan.cc:831` `inferExprType`. The Week 33 branch
   (`logical_plan.cc:126-131`) deliberately routes a correlated ref straight to
   the bare-name fallback: `schema.indexOf("team")` over the body's merged
   `[laps..., drivers...]` schema hits **`laps.team`**. No throw.
6. `PredicatePushdown`: `collectSlots` gives `{1, -1}`, `soleSlot` gives -1, so
   the conjunct is not pushed — it stays as a `LogicalFilter` over the body join.
7. Execution: `evaluator.cc:72-80` `resolveColumnIndex` — `col.id.isLocal()` is
   false, so the slot branch is skipped entirely and it returns
   `schema.indexOf("team")` = `laps.team`.

The body therefore filters `d2.team = laps.team` instead of
`d2.team = <outer drivers>.team`. Wrong rows, no error. `laps` and `drivers`
both have a `team` column in `catalog.json`, so this is buildable today.

This is precisely the collapse `ColumnId` was built to make impossible, and it
survives because the two *newly added* fallbacks — `inferExprType` and
`resolveColumnIndex` — both branch on `isLocal()` and then silently resolve the
correlated ref by bare name rather than throwing. `localSlot()` is never reached,
so the type's guarantee never engages.

Note the interaction with the Task 8 claim at `predicate_pushdown.cc:39-49`,
which asserts "no correlated ref survives into any predicate this walker runs
over". Step 6 above is a correlated ref in a predicate `collectSlots` runs over.
That rationale is false as written (see M-7).

### Fix

Either extract correlated conjuncts from the body's ON clauses as well (they are
already isolated as residuals by `classifyJoinCondition`), or — minimum code —
make `refuseUnloweredCorrelated` also refuse a surviving correlated `ColumnRef`,
and have `inferExprType`/`resolveColumnIndex` throw on a non-local ref instead of
falling back to bare name.

---

## H-1 (HIGH, silent wrong answer) — `rightKeyIndices` resolves the body key by BARE NAME, and its stated "children[1] is exactly one relation" justification is now false

`src/planner/vectorized_plan_builder.cc:92-108`

```
// Resolve the RIGHT input's key columns. children[1] is always exactly one
// relation (left-deep; Week 28's DP keeps that shape), and a standalone scan's
// schema stamps every column slot 0 ... so the bare-name overload is both
// unambiguous and the only one that resolves here.
int i = right_schema.indexOf(k.join_col);
```

That rationale held for Week 28 (left-deep enumeration) and for Week 32's IN
lowering, which took the body key POSITIONALLY
(`subquery_lowering.cc:62`: `body_plan->output_schema.columns()[0].name`) and so
never depended on name lookup.

Week 33 breaks it. `subquery_decorrelation.cc:141-144` builds
`LogicalJoin(spine, body_plan, keys, -1, left_schema)` where `body_plan` is a
whole `LogicalPlanBuilder::build()` result — **which may be a join subtree**.
`join_condition.cc` explicitly documents `EXISTS (SELECT 1 FROM drivers d JOIN
laps p ON ...)` as a supported shape, so a two-relation body is reachable.

`splitCorrelation` (`subquery_decorrelation.cc:81-83`) stores only
`body_side->column_name` — the *name*, discarding `body_side->id`, which is
exactly the relation identity the whole ColumnId migration exists to preserve.
`rightKeyIndices` then does a bare-name lookup over a MERGED body schema, and
invariant 3 says duplicate names in a merged schema are legal. First match wins.

Concrete input (needs a column name shared by the body's two relations, which
invariant 3 declares legal):

```sql
SELECT * FROM drivers d
WHERE EXISTS (SELECT * FROM laps l JOIN teams t ON l.team_id = t.team_id
              WHERE t.name = d.name);
```

`splitCorrelation` yields `JoinKey{from_col="name", join_col="name", from_slot=0}`.
The body's merged schema is `[laps... , teams...]`; if `laps` also carries a
`name` column, `indexOf("name")` returns the **laps** column and the semi-join
probes `d.name` against `laps.name` instead of `teams.name`. Wrong rows, no
error, and `--explain` prints an identical plan.

Fix: carry `body_side->id.localSlot(...)` into the `JoinKey` (a `join_slot`
field, mirroring `from_slot`) and resolve the right key slot-first, throwing on a
slot-qualified miss the way `leftKeyIndices` already does. Minimum stopgap:
refuse a correlated body whose plan is not a single scan.

---

## H-2 (HIGH, silent wrong answer) — an alias in the body's SELECT list can shadow the join key column

`src/planner/vectorized_plan_builder.cc:101` + `src/planner/logical_plan.cc:323`

`rightKeyIndices` resolves `join_col` against the body plan's **output** schema,
i.e. after `buildProjectSchema`, which names columns by their SELECT alias. So an
alias that collides with the key name silently rebinds the key to a different
column.

```sql
SELECT * FROM drivers d
WHERE EXISTS (SELECT l.speed AS driver_id FROM laps l
              WHERE l.driver_id = d.driver_id);
```

- `splitCorrelation` -> `JoinKey{from_col="driver_id", join_col="driver_id", from_slot=0}`
- `buildScanSchema` (`logical_plan.cc:279-320`): body is not `select_star`, body
  `has_subquery` is false, and `body.where` is EMPTY by this point (the
  correlated conjunct was already extracted at line 134), so `required` =
  `{speed}` and the scan is narrowed to `[speed]`.
- `buildProjectSchema` emits one column named `driver_id` holding `speed`.
- `rightKeyIndices` finds `driver_id` at index 0.

The semi-join then probes `d.driver_id` against `laps.speed`. Silent wrong answer.

Same root cause as H-1: the key is matched by name against a schema that is not
the one the name was resolved in.

---

## M-3 (MEDIUM, loud but misleading) — any EXISTS body that is not `SELECT *` fails with a key-resolution error

Same mechanism as H-2 without the name collision. `EXISTS (SELECT 1 FROM ...)` and
`EXISTS (SELECT l.lap_id FROM laps l WHERE l.driver_id = d.driver_id)` — the two
most idiomatic EXISTS bodies in SQL — both narrow `driver_id` out of the scan at
`logical_plan.cc:304-319` (because the correlated conjunct has already been
removed from `body.where`, so `collectCols` never sees the key), then die at
`vectorized_plan_builder.cc:103` with "join key 'driver_id' not found on the
joined relation".

This is a real restriction — decorrelation effectively only supports
`EXISTS (SELECT * ...)` — and it is stated NOWHERE: not in
`subquery_decorrelation.h`'s four numbered validity conditions, not in the README
dialect table. Condition 2 says the key must be "two plain ColumnRefs"; nothing
says the body's key column must also survive its own projection.

Note this is the *inverse* of the failure `buildScanSchema`'s own Week 33 comment
(`logical_plan.cc:288-303`) says it is avoiding. That comment argues the OUTER
scan still widens for a correlated query so nothing gets narrowed away — true,
but it never considers the BODY's scan, which narrows normally and drops exactly
the column decorrelation is about to need.

Fix: add the key columns to `required` before planning the body, or (better,
with H-1) keep the key resolvable against the body's scan schema.

---

(more to come)
