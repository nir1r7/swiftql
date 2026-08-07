# Seam audit: subquery chain (weeks 30 -> 34), pass 1

Scope: the SEAM between week 30 (nested scopes / SubqueryExpr / ColumnRef::query_level),
week 31 (materialize-then-substitute), week 32 (IN/NOT IN -> semi/anti), week 33
(EXISTS decorrelation + ColumnId), week 34 (derived tables + correlated scalar).
Not a re-audit of any single week. Read-only; no build run. Probes executed with the
pre-existing `build/swiftql` binary (`--storage columnar --execution vectorized`).

Status: COMPLETE for targets 1-5 as stated below.

---

## Target 1 — routing totality and disjointness: VERIFIED

Four productions, all reachable only from `LogicalPlanBuilder::build`'s single
site at `src/planner/logical_plan.cc:997-1040`, in fixed order:

| shape | consumer | where it is decided |
|---|---|---|
| uncorrelated SCALAR / uncorrelated EXISTS | AST substitution | `subquery_materialization.cc:396-402` (runs before planning) |
| uncorrelated IN / NOT IN | SEMI / ANTI_NOT_IN join | `subquery_lowering.cc:60`, `:100` |
| correlated EXISTS / NOT EXISTS | SEMI / ANTI join | `subquery_decorrelation.cc:511`, `:589` |
| correlated SCALAR | LEFT join over LogicalDerived | `subquery_decorrelation.cc:322` |
| correlated IN / NOT IN | refused by name | `subquery_decorrelation.cc:637-646` |
| anything else surviving | refused by name | `logical_plan.cc:1038-1039`, `:1074-1075` |

Disjointness holds because each pass tests a *pair* (`kind`, `correlated`) that
partitions the space, and because the two conjunct-consuming passes run before
the one that descends into conjuncts:

- `materializeSubqueries` skips exactly `correlated` (`subquery_materialization.cc:351`)
  and `Kind::IN` (`:361`), and both arms still recurse into the BODY. So the body of
  a surviving node is materialized, and only the node itself is left for the planner.
- `lowerInSubqueries` / `lowerExistsSubqueries` consume WHOLE conjuncts; a declined
  conjunct is put back in `kept`.
- `lowerCorrelatedScalars` runs last (`logical_plan.cc:1021`) and walks INTO conjuncts
  via `forEachSubquery`. `forEachSubquery` visits `sq->operand` and then `return`s at
  `subquery_materialization.cc:43-49` — it never descends into `sq->subquery`. So a
  surviving correlated IN's body is not re-entered by the scalar pass.

Nested-block shapes route by recursion, one lowering each, confirmed by execution:

- EXISTS inside a derived table — `buildRelation` (`logical_plan.cc:866`) calls
  `LogicalPlanBuilder::build` on the body, so the body's WHERE goes through the same
  three passes. Probe: `SELECT COUNT(*) FROM (SELECT d.driver_id FROM drivers d WHERE
  EXISTS (SELECT 1 FROM laps l WHERE l.driver_id = d.driver_id)) x` -> 20 (correct).
- IN whose body contains a correlated scalar — `materializeSubqueries` IN arm recurses
  (`:392`), the correlated scalar survives, `planBody` (`subquery_lowering.cc:25`)
  builds the body, and the body's own `lowerCorrelatedScalars` takes it. Probe:
  `... WHERE d.driver_id IN (SELECT l.driver_id FROM laps l WHERE l.speed >
  (SELECT AVG(l2.speed) FROM laps l2 WHERE l2.team = l.team))` -> 20, plans and runs.

Positions lowering does not read are refused, not silently dropped: HAVING
(`logical_plan.cc:1074-1075`), non-top-level WHERE (`:1038-1039`), SELECT/GROUP BY/
ORDER BY (`validator.cc:456` `allow_subqueries=false`), and JOIN ... ON
(`validator.cc:689`). The ON case matters at this seam: an inner join's ON residuals
are folded into `stmt.where` at `logical_plan.cc:985-988`, which is BEFORE the
lowering block, while a LEFT join's residuals stay on the join and would reach
`inferExprType` (dispatch site 12) as an internal-defect message. `validator.cc:689`
closes it. Clean.

## Target 2 — scalar cardinality across the rewrite: VERIFIED

Week 31's rule is a runtime check on a materialized result
(`subquery_materialization.cc:229-231`), backed by the LIMIT-2 cap at `:201` so two
rows are provable. Week 34's rewrite plans the body once, so that check has no site.
The guarantee is instead made STRUCTURAL and the structure is enforced:

- `requireDecorrelatableScalarBody` (`subquery_decorrelation.cc:294-305`) refuses
  LIMIT / DISTINCT / HAVING / an own GROUP BY / a select list that is not exactly one
  expression, and requires an aggregate (optionally in a constant wrapper) via
  `constantWrapperAggregateSlot`.
- The rewrite then supplies `GROUP BY <correlation keys>` and selects
  `[keys..., agg]` (`:389-396`). One aggregate over a grouping of exactly the join
  keys yields exactly one row per key tuple, by construction.
- The join keys ARE the group keys (`splitCorrelation` produces both from the same
  equalities, `:108-119`), so an outer row matches at most one body row.

"A body returning two rows for one outer key" is therefore not reachable through this
path — it is refused at `subquery_decorrelation.cc:268-273` with the message that
names exactly this reason. The refusal is load-bearing, and it is the right one.

Corollary checked: because at most one body row matches, lowering a correlated scalar
that sits UNDER AN OR is also cardinality-safe (the LEFT join cannot duplicate an
outer row). Probe: `... WHERE d.age > 100 OR d.age > (SELECT COUNT(*) FROM laps l
WHERE l.driver_id = d.driver_id AND l.speed > 999)` -> 20, matching the non-OR form.

## Target 3 — NULL semantics: VERIFIED, no leakage

Each rule has exactly one producer and they do not cross:

- `ANTI_NOT_IN` (three-valued) is produced at `subquery_lowering.cc:100` and NOWHERE
  else (grep over `src/`). `ANTI` (two-valued) is produced at
  `subquery_decorrelation.cc:589` and nowhere else. `logical_plan.h:121-136` documents
  the split, and `vectorized_plan_builder.cc:488-494` passes `join->semantics` straight
  through to `VecHashJoinNode` rather than re-deriving it.
- Empty-group rules: `COUNT` is the exception and is keyed on
  `agg->function_name == "COUNT"` (`subquery_decorrelation.cc:388`) — so
  `COUNT(DISTINCT x)` is covered, which testing the `distinct` flag would have missed.
  The CASE wrapper is planted at the AGGREGATE's own slot inside the constant wrapper
  (`:454-487`), so `1 + COUNT(*)` over an empty group is 1 and not 0. SUM/AVG/MIN/MAX
  take the LEFT join's null-extension unchanged (`:434`).
- The materialized (uncorrelated) SCALAR path keeps its own zero-row rule with a TYPED
  null (`subquery_materialization.cc:236-241`) so `inferExprType` still answers — a
  different mechanism for a different path, correctly not shared.
- `NOT IN` over an empty set and `NOT EXISTS` over the same predicate agree on the
  shipped data (both 20) — a consistency check only; the shipped catalog has no NULL
  in an INT key, so the three-valued divergence itself is not exercisable from this
  binary and is left to the SQLite oracle.

## Target 4 — ColumnId containment: VERIFIED

No path reintroduces a bare slot where a qualified one is required. The two
containment mechanisms (`logical_plan.h:183-215`) are both live and neither is
substituted for the other:

- (a) semi/anti joins keep `output_schema` == children[0]'s and `join_slot == -1`
  (`subquery_lowering.cc:92-94`, `subquery_decorrelation.cc:579-582`). Every reader of
  `join_slot` declines on `semantics != STANDARD`: `predicate_pushdown.cc:301-303`,
  `join_enumeration.cc:72`, `cardinality_estimator.cc:400/443`,
  `vectorized_plan_builder.cc:201`.
- (b) derived relations normalize to slot 0 (`logical_plan.cc:484-485`) and the outer
  slot is applied by the merged join schema — the FROM/JOIN spine at
  `logical_plan.cc:954-958`, the synthetic scalar relation at
  `subquery_decorrelation.cc:417-422`.
- Week 34's derived range-table entries carry a qualified id end to end:
  `Binder::relationSchema` (`binder.cc:185-212`) pushes a real `RangeEntry` with an
  owned schema, so `resolveColumnRef` stamps `ColumnId::outer(level, slot)` exactly as
  for a base table. The synthetic `$scalarN` relation gets
  `ColumnId::local(derived_slot)` at `subquery_decorrelation.cc:444`, never a bare
  name — the comment at `:438-441` names the failure it avoids and the code matches it.
- `derived_slot = range_table_size + out.lowered` (`:417`) with
  `range_table_size = stmt.joins.size() + 1` (`logical_plan.cc:1023`) stays inside
  `countRelations`, which counts the SPINE and returns 1 for a DERIVED node
  (`join_enumeration.cc:65-79`) — so `1 + joins.size() + n_scalars` bounds the largest
  slot `joins.size() + n_scalars`. Consistent; `hasSlotOutsideRangeTable` cannot fire
  spuriously on this shape.

## Target 5 — pass-pair asymmetry: one real asymmetry found (F2), no second Week-35

Walked every pass that traverses either (i) a statement's relations or (ii) a logical
tree, checking that each handles BOTH `LogicalDerived`/`TableRef::isDerived()` AND
`semantics != STANDARD`:

| pass | derived | semi/anti | verdict |
|---|---|---|---|
| `materializeSubqueries` (`subquery_materialization.cc:305-312`) | recurses | node survives | OK (Week 35 fix) |
| `needsSubqueryMaterialization` (`:273-282`) | recurses | via `has_subquery` | OK |
| `collectQueryTables` (`:120-129`) | recurses | recurses | OK |
| `foldConstants` | per scope via `binder.cc:181` (body bound by `relationSchema`) | declines SubqueryExpr | OK |
| `Binder::bindQuery` (`binder.cc:40,53`) | binds body with `parent`, not `&scope` | new scope | OK |
| `Validator` (`validator.cc:172-178`) | derived entry schema via shared helpers | n/a | OK |
| `PredicatePushdown::distribute` (`predicate_pushdown.cc:301-317`) | derived is an ordinary slot; bottom-of-spine filter lands ABOVE the derived node | declines | OK |
| `JoinEnumeration::countRelations` (`join_enumeration.cc:65-79`) | returns 1 | left spine only | OK |
| `leafScanTableOfOrNull` (`:41-44`) / `leafScanTableOrNull` (`vectorized_plan_builder.cc:55`) | returns nullptr | n/a | OK |
| `collectSlotTables` (`vectorized_plan_builder.cc:200-205`) | skips entry | declines | OK |
| `CardinalityEstimator` (`cardinality_estimator.cc:400,443,469`) | via DERIVED case | branches on semantics | OK |

No second instance of the Week 35 shape (one walker updated, its neighbour not) was
found. The asymmetry that IS present is a different one — F2 below.

---

## Findings

### F1 — MEDIUM. Two correlated equalities on the same body column make the synthetic scalar relation refuse a legal query, with advice the user cannot act on

`src/planner/subquery_decorrelation.cc:390-396` pushes ONE group key and ONE select
item per correlated equality, without deduplicating on the body-side column. It then
runs the result through `derivedRelationSchema`
(`subquery_decorrelation.cc:408-409` -> `src/planner/logical_plan.cc:494-501`), which
was written for USER-WRITTEN derived tables and refuses a duplicate output name.

Concrete input (run against the shipped catalog, `--storage columnar --execution
vectorized`):

```
SELECT COUNT(*) FROM drivers d
WHERE d.age > (SELECT COUNT(*) FROM laps l
               WHERE l.driver_id = d.driver_id AND l.driver_id = d.age)
```

Observed:

```
Error: derived table '$scalar0': column 'driver_id' is produced twice; give one of them an alias
```

The query is legal SQL and SQLite answers it. The user never wrote a derived table,
cannot alias a column of `$scalar0`, and the message names an internal relation — the
same class of defect Week 35 fixed for Q22 (an internal guard surfaced as a user
error), reached from a different direction.

It is a REFUSAL, not a wrong answer, so severity is medium rather than high. But note
what the refusal is doing: it is the only thing preventing a bare-name collision on
the build side (see F2), so it must not simply be relaxed.

Minimal fix: dedupe `keys` / `body_key_refs` in
`lowerCorrelatedScalars` on the body-side `(column_name, id)` pair before building
`group_by` — two equalities `l.k = d.a AND l.k = d.b` mean one group key `l.k` and
two join keys against it, which `LogicalJoin` already supports (`keys` is a vector and
`leftKeyIndices`/`rightKeyIndices` are per-key). Alternative: name the group columns
`$k0..$kN` and resolve the build side POSITIONALLY as the semi/anti path already does,
which also closes F2.

### F2 — LOW. The correlated-scalar lowering is the only one of the four whose build-side keys are resolved BY NAME

Three of the four subquery lowerings arrange the build input to BE the key tuple and
resolve its key indices positionally:

- IN: body column 0 (`subquery_lowering.cc:83-85`);
- EXISTS: body select list REPLACED by the key refs
  (`subquery_decorrelation.cc:571-572`), with the reason (round-1 H-1/H-2/M-3) written
  out at `:545-570`;
- both consumed by `rightKeyIndices(..., positional=true)` at
  `vectorized_plan_builder.cc:471`, because `semantics != STANDARD`.

The correlated-scalar lowering builds a STANDARD join, so the same call passes
`positional=false` and the build-side key is resolved by BARE NAME
(`vectorized_plan_builder.cc:283-299`) against the `$scalarN` relation's schema —
`jn_schema.indexOf(k.join_col)`, first match wins. This is exactly the resolution
mechanism H-1 was, on the one path that did not adopt the fix.

It is SAFE TODAY, and the argument is worth recording rather than re-deriving:
`buildAggregateSchema` emits group keys first, in key order
(`logical_plan.cc:404-455`), so the positional property already holds and the name
lookup happens to agree; and the only way two build-side columns could share a name is
the duplicate-key shape, which F1 shows is refused. So the guarantee currently rests on
F1's refusal plus an emission order stated in a third file. Fixing F1 by dedup alone
would preserve it; fixing F1 by relaxing `derivedRelationSchema` for `$scalarN` would
NOT, and would reopen the wrong-column class. Flagged so the two are not fixed
independently.

---

## Not reached

- The three-valued `NOT IN` divergence itself was not exercised end-to-end: the
  shipped catalog has no NULL in an INT key column, and building a NULL-bearing
  fixture was out of budget. The producer-side separation (ANTI vs ANTI_NOT_IN) is
  verified statically; the PROBE-LOOP application of the flag inside
  `VecHashJoinNode` was not read.
- `HashJoinNode` (Volcano) semi/anti was not read — the Volcano path refuses every
  shape in this chain (`planner.cc:47-123`), so it carries none of these guarantees.
- Interaction with `--no-optimize` vs optimized was not diffed for the new LEFT-join
  shape; `containsOuterJoin` (`join_enumeration.cc:92-99`) declines the whole tree, so
  enumeration is a no-op there, but that was reasoned about, not measured.
