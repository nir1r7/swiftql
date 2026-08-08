# Seam audit: subquery chain (weeks 31 -> 35), pass 2

Scope: the joints between W31 (materialize-then-substitute) / W32 (IN cap removed,
semi joins) / W33 (EXISTS decorrelation) / W34 (correlated scalar + derived RTEs) /
W35 (subqueries inside derived bodies).

Part A verifies commit `8a23b9d` (the F1/F2 coupled-pair fix). Part B hunts fresh.

Status: IN PROGRESS.

---

## Part A — is `8a23b9d` real and complete?

### A1. Is the collision impossible, or merely unlikely? — IMPOSSIBLE, and for a
reason stronger than the commit message states.

`subquery_decorrelation.cc:444-459` renames positions `0..keys.size()-1` of the
body plan's output schema to `$k0..$k{n-1}` and repoints `keys[i].join_col` at
them. Three things have to hold for the bare-name lookup in
`rightKeyIndices(..., positional=false)` (`vectorized_plan_builder.cc:125-149`)
to be unique, and all three do:

1. **Pairwise distinct among themselves.** `$k` + `to_string(i)` over distinct
   `i`. Trivially true.
2. **No user identifier can be `$kN`.** Confirmed at the lexer, not assumed —
   `$` is not in the identifier alphabet and there is no quoted-identifier
   production, so no catalog column, alias or `AS d (a,b)` rename can spell one.
3. **No OTHER column of the same schema can be `$kN`.** This is the one the
   commit message hand-waves ("$ is not lexable"), and it holds for a different
   reason: the schema `rightKeyIndices` searches is
   `join_child->outputSchema()` — the `VecDerivedNode`'s schema, i.e. exactly
   `normalized`, which the width check at `:447-452` proves is
   `keys.size() + 1` columns wide. Positions `0..n-1` are the renamed keys; the
   ONE remaining column is the aggregate, named `aggregateOutputName(agg)`,
   which renders `FUNC(arg)` and cannot be `$kN`. So the search space contains
   exactly n+1 names, n of them generated-distinct and one provably outside the
   generated namespace.

The collision F1 named is therefore genuinely gone, not made unlikely.

**Nested `$scalarN` at a different level does not reopen it.** Two correlated
scalars in one block produce `$scalar0` and `$scalar1`, and BOTH publish a
column literally named `$k0` into the same merged schema. That merged schema is
never the one `rightKeyIndices` searches (it searches each build child's own
schema), and the merged schema is only read through slot-qualified lookups:
`leftKeyIndices` uses `indexOf(from_col, from_slot)` and throws on a miss
rather than falling back (`vectorized_plan_builder.cc:97-105`), and the
substituted outer `ColumnRef` is stamped `ColumnId::local(derived_slot)`
(`subquery_decorrelation.cc:494`) so `resolveColumnIndex`
(`evaluator.cc:72-79`) hits the slot-qualified overload first. Verified the
same shape for the AGGREGATE name too: two `COUNT(*)` bodies publish two
columns both named `COUNT(*)` at different slots, and the same slot-qualified
path separates them.

### A2. Is the guard still reachable? — Reachable globally, DEAD on this call
site, and that is the correct outcome (not a moved problem).

`derivedRelationSchema`'s duplicate-name check (`logical_plan.cc:497-505`) has
two callers: `buildRelation` (`logical_plan.cc:855`, `:867`) for USER-written
`FROM (...) AS d`, and the `$scalarN` site. It is still live and still
user-facing on the first — `SELECT * FROM (SELECT d.driver_id, l.driver_id
FROM drivers d JOIN laps l ON ...) x` is exactly what it is for, and there the
message's advice ("give one of them an alias") is actionable.

On the `$scalarN` site it can no longer fire: every name handed to it is either
`$kN` (generated distinct) or the single aggregate column. That is the right
shape — the guard was never `$scalarN`'s guard, it was a user-facing check that
an internal path was accidentally routed through. The fix removed the routing,
not the check. The check did not lose a caller it needed.

### A3. Order is now load-bearing. What establishes it?

`renamed[i] <-> keys[i]` is asserted purely by index. What makes index i of the
body plan's output schema the i-th correlation key:

- `body.select_list` is rebuilt as `[body_key_refs[0..n-1]..., agg_expr]`
  (`subquery_decorrelation.cc:389-396`), in `keys` order — the same loop pushes
  `group_by` and `select_list`, so they cannot disagree.
- `body_plan->output_schema` is the terminal `LogicalProject`'s schema
  (`logical_plan.cc:1105-1106`), which `buildProjectSchema` builds one column
  per select-list item, in order. **Note the commit comment cites
  `buildAggregateSchema` here; that is the wrong function** — the aggregate
  schema is one node lower. The property still holds (both are select-list
  ordered), but the cited justification is not the code that runs.
- The width check `renamed.size() == keys.size() + 1` (`:447`) then pins it.

Can the order differ between two places that must agree?
- **planner vs executor:** no. The rename is applied once, before the
  `LogicalDerived` is constructed; both `--explain` and the vectorized lowering
  read the same `LogicalDerived::output_schema`.
- **optimized vs `--no-optimize`:** no. The rename happens in
  `LogicalPlanBuilder::build`, which runs before any optimizer pass, and
  `JoinEnumeration` declines the whole tree on `containsOuterJoin`
  (`join_enumeration.cc:92-99`) — this lowering always produces a
  `JoinType::LEFT`. So enumeration cannot reorder the `$scalarN` join.
- **Volcano vs vectorized:** not applicable — `Planner::plan` refuses every
  correlated subquery outright (`planner.cc:71-79`), so Volcano never sees a
  `$kN`.

### A4. Does the synthetic name leak to a user?

Yes, in one place, and it is new with this commit. `LogicalJoin::explain()`
(`logical_plan.cc:742-749`) prints `... = keys[i].join_col`, so `--explain` on
a correlated-scalar query now renders

    LogicalLeftJoin [driver_id = $k0]

where before `8a23b9d` it rendered `[driver_id = driver_id]`. `$scalarN`
already leaked via `LogicalDerived::explain()` (`:710-711`) before this commit
and is shown in `docs/week-36-plan.md:127`, so the family of leak is
pre-existing; the `$kN` half is not. No test, oracle entry or rejection suite
pins either string (`run_tpch.py` and `test_new_queries.py` only grep for the
node NAME substring), so nothing breaks — it is a cosmetic regression only.
See L-1 below.

**Verdict on Part A: the fix is real and complete.** It closes F1 and F2
without relaxing the guard, the collision is impossible rather than unlikely,
and the ordering it now depends on is established in one place and consumed in
one place.

---

## Part B — findings

### B-1 — BLOCKER (pending execution). `SELECT *` plus a correlated scalar
subquery emits the synthetic relation's columns in the result.

`lowerCorrelatedScalars` grafts the `$scalarN` `LogicalDerived` as
`children[1]` of a **`JoinType::LEFT`, `JoinSemantics::STANDARD`** join whose
`output_schema` is a **MERGED** schema — spine columns plus
`[$k0..$k{n-1}, <agg>]` stamped at `derived_slot`
(`subquery_decorrelation.cc:467-476`). That is deliberate and correct: the
outer predicate has to be able to read the aggregate column.

But `LogicalPlanBuilder::build`'s `SELECT *` expansion runs LAST, over
`node->output_schema` (`logical_plan.cc:1086-1101`), and that node is the
filter above this merged join. It skips only `col.hidden`, and
`derivedRelationSchema` explicitly sets `hidden = false` on every column it
normalizes (`logical_plan.cc:492-494`). So the star expands over the
synthetic relation's columns too.

Failing shape:

    SELECT * FROM drivers d
    WHERE d.age > (SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id)

SQLite returns 5 columns (`drivers.*`). SwiftQL should return
**7** — `driver_id, name, nationality, team, age, $k0, COUNT(*)`.

This is a **wrong answer, not an error**: the extra columns resolve cleanly
(`resolveColumnIndex` hits `$k0` at `derived_slot`), so nothing throws.

Contrast with the other three lowerings, which are immune: `SEMI`/`ANTI` /
`ANTI_NOT_IN` joins set `output_schema` to `children[0]`'s
(`subquery_lowering.cc:92-95`, `subquery_decorrelation.cc:629-632`), so a
star over them sees only spine columns. The correlated-scalar lowering is the
one that broke that containment on purpose — and `development.md`'s "Relation
slots and query levels" says so ("A derived table's columns *are* in scope
above it") — but nothing re-narrowed the star.

**Second symptom of the same defect, inside a derived body.** `buildRelation`
computes `expected` from `blockOutputSchema` (`logical_plan.cc:855`) and
compares it against the built plan's schema (`:869-887`).
`blockOutputSchema` models the spine, the aggregate and the project
(`logical_plan.cc:505-560`) and **does not model any subquery lowering at
all**. For a non-star body the project schema is written from the select
list, so the two agree; for a `SELECT *` body they cannot. So

    SELECT COUNT(*) FROM (
      SELECT * FROM drivers d
      WHERE d.age > (SELECT COUNT(*) FROM laps l WHERE l.driver_id = d.driver_id)
    ) x

should hit the drift check and report
`internal: derived table 'x' was bound against a 5-column schema but planned
to 7 columns` — an internal-defect message for legal SQL.

**Not introduced by `8a23b9d`**; it is a Week 34 defect. Before the rename the
extra columns were named `driver_id` and `COUNT(*)`, so the top-level symptom
was `driver_id` appearing twice in the output. The fix changed what the wrong
output looks like, not whether it is wrong.

**Coverage hole that hides it.** Every `WEEK34_CORRELATED_SCALAR_VEC_ONLY`
entry names its select list explicitly (`SELECT COUNT(*) AS n ...`,
`SELECT d.driver_id AS did ...`) — checked all of them. Not one is
`SELECT *`. TPC-H Q17 also names its columns. So the oracle cannot see this.
`SELECT *` bodies ARE used elsewhere in the suite, but only inside `EXISTS`
(where they are harmless — EXISTS reads no values) and inside derived tables
with no correlated scalar.

STATUS: to be executed once the gate releases `build/`.

### B-2 — MEDIUM (pending execution). A column alias on an UNWRAPPED correlated
scalar body breaks the substitution; the WRAPPED form of the same query works.

`lowerCorrelatedScalars` computes the substituted reference's name from the
aggregate *node*:

    const std::string agg_name = aggregateOutputName(agg);      // :374
    ...
    ref->column_name = agg_name;  ref->id = ColumnId::local(derived_slot);  // :492-494

but the derived relation's column name comes from `buildProjectSchema`, which
lets a SELECT alias override the canonical name
(`logical_plan.cc:375`: `if (!expr->alias.empty()) cols.back().name = ...`).

The two disagree exactly when the alias sits on the node that gets moved into
the rewritten select list — i.e. when the wrapper IS the aggregate:

    -- UNWRAPPED, aliased: agg_expr carries alias "a", so the relation publishes
    -- [$k0, "a"] while the outer ref looks for "AVG(l2.speed)"
    SELECT COUNT(*) FROM laps l WHERE l.speed >
      (SELECT AVG(l2.speed) AS a FROM laps l2 WHERE l2.team = l.team)

    -- WRAPPED, aliased: the alias stays on the BinaryExpr, which is lifted out;
    -- the bare AggregateExpr moved into the select list has no alias, so the
    -- relation publishes [$k0, "AVG(l2.speed)"] and it works
    SELECT COUNT(*) FROM laps l WHERE l.speed >
      (SELECT 0.5 * AVG(l2.speed) AS a FROM laps l2 WHERE l2.team = l.team)

Expected symptom for the first: `column not found: 'AVG(l2.speed)'` from
`inferExprType` (`logical_plan.cc:132`) — a message naming a column the user
never wrote, for legal SQL SQLite answers.

Not a wrong answer: the bare-name fallback searches the whole merged schema,
and `aggregateOutputName` renders `FUNC(arg)`, which no identifier can spell,
so it must miss rather than hit the wrong column. That is why this is MEDIUM
and not a BLOCKER — but it is the same "resolve by a name derived in one place
and looked up in another" shape that H-1/H-2 and F2 were, on the one path that
still does it.

It also contradicts a claim the suite is built on: `compare_against_sqlite.py`
says the wrapped and constant-outside forms "produce the SAME plan", and
Week 36's header says the same. With an alias present they do not — one plans
and one does not.

STATUS: to be executed once the gate releases `build/`.
