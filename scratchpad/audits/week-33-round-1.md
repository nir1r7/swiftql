# Week 33 Round 1 Audit

Range: `889b6cb..ecee221`, branch `claude/phase5-week26-qomtkb`.
Targets: ColumnId migration, decorrelation correctness, NOT EXISTS semantics,
Week 30 tripwires, stale rationales.

Status: IN PROGRESS (appending as confirmed).

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
