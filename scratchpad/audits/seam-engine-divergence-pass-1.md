# Seam audit — engine divergence (Volcano vs vectorized), weeks 26–36, pass 1

Auditor: parallel seam auditor 5/5. Scope: every operator added or changed weeks 26–36 on BOTH
execution paths. Question: Volcano is the correctness baseline — does the vectorized path agree
with it everywhere, and where it deliberately does not, is that refusal TOTAL?

Status: IN PROGRESS (appended as confirmed).

## Targets
1. Refusal totality for vectorized-only capabilities (IN(subquery), correlated subqueries,
   semi/anti joins, derived tables).
2. Agreement where both engines implement the same operator: outer joins (wk29), the six key
   serializers via `key_encoding.h`, COUNT(DISTINCT) (wk34).
3. Late materialization / selection vectors — wk32 semi-join on the row path.
4. Blocking operators added this phase: close/reset parity.
5. Same-week changes of unequal amount on the two paths.

## Findings

(none yet)

### T1 — refusal totality (Volcano side). VERIFIED so far, no hole found.

Survivor analysis. `materializeSubqueries` (src/planner/subquery_materialization.cc:315-438)
substitutes a constant for every `SubqueryExpr` EXCEPT two shapes, and sets
`stmt.has_subquery = node_survives` (line 438) with `node_survives` true only for
(a) `sq->correlated` (line 355) and (b) `sq->kind == Kind::IN` (line 366). An uncorrelated
`EXISTS` is materialized to a constant on BOTH paths, so it is not a divergence at all.

`Planner::plan` (src/planner/planner.cc:47-81) scans exactly `stmt.where` and `stmt.having`
for those two survivor shapes. That is the complete search space: `Validator::validateExpr` is
called with `allow_subqueries=true` at only two sites — WHERE (validator.cc:324) and HAVING
(validator.cc:413) — and with the `false` default everywhere else (SELECT list 206, GROUP BY
365, ORDER BY 456, and JOIN ON via the default). So no surviving SubqueryExpr can sit in a
position the refusal scan does not walk.

Ordering is correct: all three refusals (planner.cc:23, 71, 76, 119) fire before
`stmt.from.tableName(...)` at planner.cc:124 and before `catalog.getTable` — i.e. before
anything reads the un-lowered node.

Nested bodies are covered by recursion rather than by the scan: a body is executed through
`runVolcanoToRows` (src/cli/main.cc:150), which calls `Planner::plan` again, so the same three
refusals re-fire per block. `runVolcanoToRows` is the only other Volcano entry point in the
tree (only two `Planner::plan` call sites exist).

Derived tables: refused on the block's own `from`/`joins` (planner.cc:114-122); a derived table
nested inside a subquery body is refused when that body reaches `Planner::plan`.

### T2a — the six key serializers. VERIFIED SHARED, no local restatement.

All eight call sites route through `key_encoding.h`; no operator re-derives the encoding:
- Volcano hash aggregate group key — plan_nodes.cc:35
- Volcano COUNT(DISTINCT) key      — plan_nodes.cc:288
- Volcano DistinctNode key         — plan_nodes.cc:460
- Volcano HashJoinNode join key    — plan_nodes.cc:631 (`serializeRowKey`)
- Vec hash aggregate group key     — vec_hash_aggregate_node.cc:17
- Vec COUNT(DISTINCT) key          — vec_hash_aggregate_node.cc:211
- Vec DistinctNode key             — vec_distinct_node.cc:41
- Vec hash join join key           — vec_hash_join_node.cc:61 (`serializeKey`)

`prefixed` is computed identically on both join sides (`key_idx.size() > 1`,
plan_nodes.cc:626 and vec_hash_join_node.cc:56), so a 1-key and an n-key tuple encode the
same bytes on both engines. NULL/NaN drop is the same predicate (`isUnmatchableKey`) on both.
A grep for `\x01` and `toString()` inside the operator files finds only comments — no
open-coded encoder survives.
