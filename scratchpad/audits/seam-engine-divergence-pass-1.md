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

### T2b — outer joins (Week 29, built twice). AGREE. No divergence found.

Compared line-for-line: `HashJoinNode::next` (src/planner/plan_nodes.cc:697-790) against
`VecHashJoinNode::nextChunk` STANDARD arm (src/execution/vec_hash_join_node.cc:299-345).

- Unmatchable probe key (NULL or NaN member): Volcano null-extends at plan_nodes.cc:781-785
  (`else if (left_outer_)` on the serialize failure); vec at vec_hash_join_node.cc:302-304.
  Same predicate (`isUnmatchableKey`), same action.
- Key absent from the build table: Volcano falls into the bucket-exhausted branch and
  null-extends at plan_nodes.cc:750-757; vec at vec_hash_join_node.cc:308-311.
- ON residual failing every candidate: BOTH deliberately do NOT set the matched flag
  (plan_nodes.cc:738-742 `continue` before `probe_matched_ = true`; vec_hash_join_node.cc:336-339
  `continue` before `matched = true`), so the probe row is null-extended rather than dropped.
  Same three-valued test (`!v.isNull() && v.asInt() != 0`) against the merged row and
  `output_schema_` on both — plan_nodes.cc:700-703 vs vec_hash_join_node.cc:207-211.
- Duplicate build keys: both bucket into a vector and emit one output row per bucket entry.
- Empty build side: both null-extend every probe row (no special case needed on either).
- `left_outer && swapped` is rejected in BOTH constructors (plan_nodes.cc:606-610,
  vec_hash_join_node.cc:15-19), and the builder forces the preserved side to probe
  (vectorized_plan_builder.cc:539, planner.cc via the `!swapped` outer path).
- `build_width_` is taken from the build child's SCHEMA, not from a row, on both
  (plan_nodes.cc:667, vec_hash_join_node.cc:79-80), so a zero-row build side still
  null-extends to the right width.

### T2c — COUNT(DISTINCT) and the aggregates (Week 34). AGREE on values.

`HashAggregateNode` (plan_nodes.cc:281-346) vs `VecHashAggregateNode` (vec_hash_aggregate_node.cc
:203-278). NULL skip before accumulation, `distinct_keys` fed by `appendGroupKeyField` on the
same Value, COUNT three-way rule (`is_star ? count : distinct ? distinct_keys.size() :
non_null_count`) identical at plan_nodes.cc:330-333 and vec_hash_aggregate_node.cc:259-262.
SUM/AVG guard on `non_null_count > 0`, MIN/MAX keep the argument Value — identical.
Zero-row scalar aggregate: both synthesize one default group (plan_nodes.cc:311-314,
vec_hash_aggregate_node.cc:232-236), so COUNT->0 and SUM/AVG/MIN/MAX->NULL on both.
Zero-row GROUP BY: both emit nothing.

### T2d — sort comparator. IDENTICAL.

`std::stable_sort` with `compareForSort` per key, `item.desc ? c > 0 : c < 0` — byte-identical
lambdas at plan_nodes.cc:514-521 and vec_sort_node.cc:47-54. NULL ordering therefore cannot
diverge.

### T2e — DISTINCT. AGREE.

Both first-seen order, both key via `appendGroupKeyField` over every output column
(plan_nodes.cc:459-460, vec_distinct_node.cc:39-42). Vec respects the incoming selection
(vec_distinct_node.cc:25-33) before deduplicating, as required.

---

## ISSUE 1 (MEDIUM) — GROUP BY output order diverges between the engines, which under
## `ORDER BY <agg>` + `LIMIT` makes the two engines return DIFFERENT ROWS.

`HashAggregateNode::open` materializes its result rows by iterating the accumulator map
directly:

    src/planner/plan_nodes.cc:317
        for (auto& [key_str, group_accs] : accumulators) {

`accumulators` is `std::unordered_map<std::string, std::vector<AggAccumulator>>`
(plan_nodes.cc:219). Its iteration order is the hash order of the serialized group key —
arbitrary, and a function of bucket count and insertion history.

The vectorized operator deliberately does the opposite: it records first-encounter order in
`group_order_` (vec_hash_aggregate_node.cc:169) and emits from it:

    src/execution/vec_hash_aggregate_node.cc:243
        for (const auto& key_str : group_order_)   // "emit in insertion order for stable output"

Consequence. For a GROUP BY with no ORDER BY the two engines return the same multiset in
different orders — SQL-legal, and masked by the oracle, which sorts
(`normalize(rows, preserve_order=False)`, python_tools/compare_against_sqlite.py:1964).

But it is NOT masked when a LIMIT truncates a tie. Concrete input, already in the suite:

    python_tools/compare_against_sqlite.py:53
    SELECT team, COUNT(*) FROM laps GROUP BY team ORDER BY COUNT(*) LIMIT 5

`SortNode`/`VecSortNode` are both `stable_sort`, so rows that TIE on COUNT(*) keep their
INPUT order — which is exactly the order that differs. With six or more teams and a tie
across the 5th/6th position, Volcano keeps one team and the vectorized path keeps another,
and the two engines return different row SETS. Sorting in `normalize` cannot repair that,
because the difference is in WHICH rows survive, not in their order.

The shipped 10k-row dataset happens not to produce a tie at the cut for this query, which is
why it has never fired; nothing prevents it. The same exposure exists for every
`GROUP BY ... ORDER BY <aggregate> LIMIT n` shape, and TPC-H Q that shape covers.

This is the only place in the phase where the two engines' output ORDER is decided by
different mechanisms rather than by the same comparator. Fix is one line — give
`HashAggregateNode` the same insertion-order vector the vectorized node already keeps — and it
makes Volcano's row order deterministic, which the correctness baseline should be regardless.

## ISSUE 2 (LOW, latent) — a semi/anti logical join's `on_residual` is silently dropped.

    src/planner/vectorized_plan_builder.cc:486-493
        return std::make_unique<VecHashJoinNode>(..., /*on_residual=*/nullptr, join->semantics);

`join->on_residual` is not read on this branch and not checked for emptiness. Today nothing
sets it on a semi/anti node — only `LogicalPlanBuilder` sets `on_residual`, and only on a
`JoinType::LEFT` node (src/planner/logical_plan.cc:973) — so this is unreachable. It is
recorded because the constructor's own guard (vec_hash_join_node.cc:31-34) throws
"a semi/anti join takes no ON residual" for exactly this state, and the builder defeats that
guard by passing nullptr instead of forwarding. Forwarding `std::move(join->on_residual)` would
turn a future decorrelation that produces an inequality residual into a loud plan-time error
instead of a dropped predicate.

### T3 — Week 32 semi-join on the row path under a cascading selection vector. CORRECT.

`VecHashJoinNode::nextChunk` SEMI/ANTI arm, vec_hash_join_node.cc:236-289:
- Reads the incoming selection when present, else a full `iota` range
  (vec_hash_join_node.cc:190-199) — so a cascaded selection from scan-local predicates is
  honoured, not re-derived.
- Each surviving index `r` is visited exactly once by the `for (int r : *indices_ptr)` loop and
  pushed to `output_buffer_` at most once (each arm ends in `continue` or one `push_back`).
  No duplication is structurally possible: unlike the STANDARD arm there is no inner loop over
  the build bucket — `build_keys_` is a `set` and only membership is tested
  (vec_hash_join_node.cc:281).
- The emitted chunk is rebuilt fresh by `fillOutChunk` with `filter_applied = false` and an
  empty `sel` (vec_hash_join_node.cc:149-152), so the consumed selection is not double-applied
  downstream.
- `if (!output_buffer_.empty()) break;` at vec_hash_join_node.cc:288 is required, not
  incidental: the `while` head clears `output_buffer_`, so a `continue` there would discard the
  rows just written. This is correct as written and is commented as such.
- The build side is closed inside `open()` (vec_hash_join_node.cc:145) and `close()` closes only
  the probe child (vec_hash_join_node.cc:364-368) — no double close.

So the row path is correct, not merely working. What it costs is late materialization: the arm
materializes EVERY probe column into a `Row` (vec_hash_join_node.cc:284-287) for an operator
whose output schema IS its input schema, i.e. one that could have emitted the input chunk with a
narrowed selection vector and touched no data at all. That is a performance divergence from the
filter it is structurally equivalent to, not a correctness one.

### T4 — blocking-operator lifecycle. PARITY, with one asymmetry that is not reachable.

All four vectorized blocking operators reset their full state in `open()`
(vec_hash_aggregate_node.cc, vec_sort_node.cc, vec_distinct_node.cc: `materialized_ = false;
cursor_ = 0;` plus their buffer clear) and close only their child. Their Volcano counterparts
reset in `open()` too (`HashAggregateNode::open` clears `results_` and sets `cursor_ = 0`,
plan_nodes.cc:316/350; `SortNode::open` sets `cursor_ = 0`, plan_nodes.cc:522).

The one asymmetry: `DistinctNode` clears `seen_` in `close()` (plan_nodes.cc:471-474) rather
than in `open()`, where `VecDistinctNode` clears `dedup_buffer_` in `open()`. Not grounded as a
bug — every caller pairs open/close exactly once — so it is recorded as an asymmetry, not an
issue.

`VecSimdLoopJoinNode` is correctly excluded from every shape it cannot represent: the eligibility
test at vectorized_plan_builder.cc:531 is `estimate_driven && int_keys && !outer && ...`, and the
semi/anti branch RETURNS at vectorized_plan_builder.cc:486 before that costing runs. So neither
an outer join nor a semi/anti join can reach an operator whose probe loop has no unmatched path
(vec_simd_loop_join_node.cc has no `left_outer` or `semantics` member at all).

### T5 — same week, unequal amounts. Nothing further found in budget.

Week 29 (outer joins) is the one week that produced a full implementation on both paths, and
T2b finds them equivalent. Weeks 32/33/34 (semi/anti, decorrelation, derived tables) produced
nothing on the Volcano path by design, and T1 finds each refusal total.

## NOT REACHED
- `VecDerivedNode` re-entry: it is a pass-through (`nextChunk` forwards its child's chunk
  pointer, vec_derived_node.cc), so a derived table appearing on BOTH sides of a self-join
  would forward the same `DataChunk*` twice. Not traced to a reachable plan in budget.
- The Volcano path's `--storage columnar` scan vs `VecScanNode` zone-map pruning agreement.
- `checked_arith.h` / `columnar_eval.cc` divergence from the scalar `evaluator.cc`.
