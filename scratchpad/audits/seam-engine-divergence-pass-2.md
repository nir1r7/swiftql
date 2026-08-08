# Seam audit — engine divergence (Volcano vs vectorized), PASS 2

Repo /home/user/swiftql, branch `claude/phase5-week26-qomtkb`, HEAD `ee9c9d7`.
Predecessor: `scratchpad/audits/seam-engine-divergence-pass-1.md` (read in full).

Status: IN PROGRESS — appended as each item is confirmed.

---

## Part A — is pass 1's fix (87c08a2) real and complete?

### A0. What the fix actually does

`git show 87c08a2` touches two files:

- `src/planner/plan_nodes.cc` — `HashAggregateNode::open` gains a
  `std::vector<std::string> group_order`, pushed once per first-seen group key
  (plan_nodes.cc:261 and :324 for the synthesized zero-row scalar group), and the
  materialization loop changes from `for (auto& [key_str, group_accs] : accumulators)`
  to `for (const auto& key_str : group_order) { auto& group_accs = accumulators[key_str]; }`.
- `python_tools/compare_against_sqlite.py` — a new `ENGINE_AGREEMENT_QUERIES` list (2
  queries) and `run_engine_agreement_suite`, comparing four SwiftQL modes against each
  other with `preserve_order=True`, no SQLite leg.

So Volcano now emits groups in *first-encounter order*, matching
`VecHashAggregateNode::group_order_` (vec_hash_aggregate_node.cc:169/243). The claim it
rests on is: **first-encounter order in Volcano == first-encounter order in vectorized**,
i.e. *both engines encounter input rows in the same order*.

### A1. FINDING E-1 (HIGH) — the fix makes the two orders *usually*-identical, not
### identical: the two engines choose the hash join's build side by DIFFERENT RULES,
### and the build side is what decides the join's output row order.

A hash join emits **probe-major**: for each probe row, one output row per matching build
entry. So *which side builds* determines the order of the join's output rows, and
therefore the first-encounter order of any `GROUP BY` above it — the exact thing
87c08a2 made load-bearing.

The two engines decide it from different inputs:

- **Volcano** (`src/planner/planner.cc:289`):
  ```
  bool swap = !outer && from_row_count < join_row_count;
  ```
  `from_row_count` / `join_row_count` are RAW table cardinalities read straight off the
  loaded table (`planner.cc:190`, `:243-248`) — `ColumnarTable::num_rows` or
  `table_rows.size()`. Volcano never runs `LogicalPlanBuilder`, `PredicatePushdown` or
  `CardinalityEstimator`, so this is raw-count-only, in every mode, with or without
  `--no-optimize`.

- **Vectorized** (`src/planner/vectorized_plan_builder.cc:527-529`):
  ```
  bool from_builds = outer ? false
                   : (use_simd ? cost_simd_from < cost_simd_join
                               : cost_hash_from < cost_hash_join);
  ```
  with `from_est = join->children[0]->estimated_rows` (vectorized_plan_builder.cc:326)
  — the **post-pushdown, post-filter cardinality ESTIMATE** — and `from_w/join_w` real
  per-side row widths from `rowWidth()` when `estimate_driven`.

Expanding `hashJoinCost` (cost_model.cc:5-11):
```
cost_hash_from = 2F + J + F*wF*0.001
cost_hash_join = 2J + F + J*wJ*0.001
cost_hash_from < cost_hash_join  <=>  F*(1 + 0.001*wF) < J*(1 + 0.001*wJ)
```
Two consequences:

1. **Under `--no-optimize`** the vec path falls back to raw table row counts and
   `wF == wJ == 8.0` (vectorized_plan_builder.cc:331, :364-372), so the condition
   collapses to `F < J` — *byte-identical to Volcano's rule*. This is why the seam
   has held so far.
2. **Under the optimizer** `F`/`J` are estimates after predicate pushdown, and the
   widths are real. Any query where pushdown flips the ordering of the two sides'
   cardinalities — a large table made small by a selective WHERE — puts the build
   side on a **different** side in Volcano than in the vectorized path, reversing the
   join's output row order, and hence the GROUP BY's first-encounter order.

The `estimate_driven` branch does not even need the estimate to be *accurate*: it needs
only to be on the other side of the comparison from the raw count. The estimator is
explicitly a guess (independence assumption for AND, `FALLBACK_SELECTIVITY` for LIKE and
anything unrecognized — cardinality_estimator.cc:132-140), so this is reachable by
construction, not by pathology.

**Also flips on width alone**, with no filter at all: with `F == J` Volcano's strict `<`
gives no-swap (JOIN builds), while the vec path gives `from_builds = (wF < wJ)`. Two
equal-cardinality tables of different width diverge with *no* WHERE clause.

`VecSimdLoopJoinNode` does NOT add a third order: its `cost_simd_from < cost_simd_join`
reduces to the same `F(1+0.001wF) < J(1+0.001wJ)` (the quadratic `F*J*CPU_SIMD_COMPARE`
term is symmetric and cancels), and its emission is probe-major/build-index order like the
hash join (vec_simd_loop_join_node.cc:150-186). The builder's own comment at :513-518
asserts this coincidence and it checks out.

**Why the new regression suite does not catch it.** `ENGINE_AGREEMENT_QUERIES` has two
entries; the second is the join one:
```
SELECT d.team, MIN(l.season) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id
GROUP BY d.team ORDER BY MIN(l.season) LIMIT 3
```
It has **no WHERE clause**, so `from_est == 10000` (laps) and `join_est == |drivers|`
exactly equal the raw counts — the one configuration in which the two rules provably
agree. The suite pins the aggregate node's fix and nothing about the join-side rule the
fix now depends on.

Severity HIGH rather than BLOCKER pending a *run*: the divergence is proven in the source,
but a demonstrated wrong answer additionally needs a tie spanning the LIMIT cut on the
shipped data. Constructed and run below (see A1-repro).

#### A1-repro — the query, and the predicted divergence

Constructed from the shipped catalog. The only thing needed is a WHERE whose *estimate*
crosses `|drivers| = 20` while its *actual* selectivity does not — the estimator's AND
rule is the textbook independence product (cardinality_estimator.cc:139-140), and the
FILTER floor is `max(est, 1.0)` (cardinality_estimator.cc:365), so repeated/implied
conjuncts drive the estimate to the floor while every row survives:

```sql
SELECT d.team, MIN(l.season)
FROM drivers d JOIN laps l ON d.driver_id = l.driver_id
WHERE l.season = 2022 AND l.season = 2022 AND l.season = 2022
  AND l.season = 2022 AND l.season = 2022 AND l.season = 2022
GROUP BY d.team ORDER BY MIN(l.season) LIMIT 3
```

- Actual surviving laps: 2417 rows, all seven teams, **every group's `MIN(season)` = 2022**
  — a seven-way tie at a cut of 3 (verified directly against `data/laps.csv`).
- Estimated laps after filter: `10000 * (1/4)^6 = 2.44` (season NDV = 4), vs `drivers` = 20.

Side decisions that follow:

| mode | rule | build side | probe order |
|---|---|---|---|
| row-volcano / col-volcano | `from_row_count(20) < join_row_count(10000)` -> `swap=true`, `HashJoinNode(right=laps, node=drivers)` and `right_` is the build input (plan_nodes.cc:614, :672-687) | drivers | **laps** |
| col-vec `--no-optimize` | `estimate_driven=false`, raw counts, `w=8` both -> `from_builds = 20 < 10000 = true` -> `VecHashJoinNode(join_child, from_child, swapped=true)`, first arg is the PROBE | drivers | **laps** |
| col-vec (optimized) | `from_est=20`, `join_est=2.44` -> `from_builds = false` -> `VecHashJoinNode(from_child, join_child, swapped=false)` | laps | **drivers** |

Predicted answers (computed from the CSVs, first-encounter group order under each probe order):

- laps-order first-encounter teams: `AlphaTauri, Alpine, McLaren, Ferrari, Williams, Mercedes, RedBull`
- drivers-order first-encounter teams: `RedBull, AlphaTauri, McLaren, Williams, Ferrari, Mercedes, Alpine`

so `LIMIT 3` gives

- Volcano (both storages) and vec `--no-optimize`: **AlphaTauri, Alpine, McLaren**
- vec optimized: **RedBull, AlphaTauri, McLaren**

Different row SETS (`Alpine` vs `RedBull`), which `normalize`'s sort cannot repair —
exactly the failure mode 87c08a2 was written to close, reached one layer lower down.
Note this is *also* an optimize-vs-`--no-optimize` divergence inside the vectorized path
alone, so it is in scope for the optimizer-preservation seam too, which sorts and so
cannot see it.

**Run pending** — the formal gate re-acquired `build/` (PID 18173,
`compare_against_sqlite.py`) after my first check. Results appended below when it frees.

### A2. Is the fixed ordering now load-bearing, and is the load-bearing part tested?

Yes, and no.

Before 87c08a2, GROUP BY emission order was unspecified on both engines and nothing
depended on it. After it, a *correctness* property — "the four modes return the same rows
under `ORDER BY <agg> LIMIT n`" — is attached to a chain of three separate mechanisms,
only the first of which is tested:

1. `HashAggregateNode` emits in first-encounter order. **Tested** by
   `ENGINE_AGREEMENT_QUERIES`.
2. The two engines' joins emit rows in the same order, which requires the same build-side
   choice. **Not tested, and false** — finding E-1.
3. The two engines' *scans* emit rows in the same order — row storage (`SeqScanNode` over
   `std::vector<Row>` in CSV order) vs columnar (`VecScanNode` over `ColumnarTable` chunks
   with zone-map pruning). Pruning only *skips* whole chunks and cannot reorder, and both
   walk chunks ascending, so this link holds today; but nothing asserts it, and it is now
   a correctness dependency rather than a performance detail.

There is no assertion anywhere — no C++ test, no comment at the deciding sites — telling a
future editor of `planner.cc:289` or `vectorized_plan_builder.cc:527` that the build-side
rule is now observable in query *results*. The comment 87c08a2 added lives in
`HashAggregateNode`, two layers above the code that can break it.

### A3. What the fix costs, unmentioned in the commit

Small but real, and none of it is in the commit message:

- `std::vector<std::string> group_order` holds a **third full copy of every group key**:
  the key already exists as the `accumulators` map key and as nothing in `group_keys`
  (that map is keyed by the same string). For a high-cardinality GROUP BY over a wide
  composite key this is +1 heap string per group. `std::vector<const std::string*>`
  pointing at the map's stable keys would have cost a pointer.
- One extra hash lookup per group at materialization: the loop now does
  `accumulators[key_str]` *and* `group_keys[key_str]`, where the old loop got
  `group_accs` free from the iterator. Two lookups per group instead of one.
- `accumulators[key_str]` uses **`operator[]`, not `.at()`** (plan_nodes.cc:331). If
  `group_order` ever held a key absent from `accumulators`, `operator[]` inserts an
  **empty** vector and the following `group_accs[i]` loop reads out of bounds — silent UB,
  no diagnostic. Unreachable today (nothing erases from `accumulators`), but `.at()` costs
  nothing and turns it into a throw. **LOW, ranked as E-6 below.**

### A4. Was the CLASS fixed, or one instance? — container-iteration sweep

Swept every `unordered_map`/`unordered_set`/`std::set` in the tree for one whose
*iteration order* reaches an output. Result: **no second instance found.** Details:

- `HashAggregateNode::accumulators` — the fixed site.
- `DistinctNode::seen_` (plan_nodes.h:141) and `VecDistinctNode`'s local `seen`
  (vec_distinct_node.cc:19) — membership only; output is streamed in first-seen order.
- `HashAggregateNode` / `VecHashAggregateNode` `distinct_keys` (plan_nodes.cc:28,
  vec_hash_aggregate_node.h:42) — only `.size()` is read for COUNT(DISTINCT).
- `HashJoinNode::hash_table_`, `VecHashJoinNode::hash_table_` / `build_keys_` — probed by
  key, never iterated. No outer-join build-side drain exists on either engine (the
  preserved side is forced to probe), which is what keeps it that way.
- `DictionaryEncoder::str_to_id` (dictionary_encoder.h:17) — lookup only; `dict` itself is
  built in first-encounter order, so dictionary codes are deterministic.
- `narrowSchema` (logical_plan.cc:72-79) — iterates the FULL schema in order and tests
  membership in the `required` set. Column order is the table's, not the set's. Correct.
- `collectSlots` / `collectCols` result sets — membership tests only (`.count`, `.find`).
- `JoinEnumeration::rebuild`'s `placed` (join_enumeration.cc:218) — membership only; the
  key list order comes from the deterministic `edges` vector.
- `PredicatePushdown` already uses `std::map<int, ...>` *deliberately*, with the reason
  written down (predicate_pushdown.cc:337-338). That is the one place the codebase had
  already internalized this rule.
- `ColumnarEvalCache::compiled_` (columnar_eval.h:35) — pointer-keyed compile cache, never
  iterated.
- `std::stable_sort` is used on both sort paths (plan_nodes.cc:514-521,
  vec_sort_node.cc:47-54) with identical comparators, so the sort itself introduces no
  arbitrary tie-break — it *propagates* the input order, which is why E-1 is reachable.

So the class of bug that was fixed is genuinely a single instance at the *container* level.
E-1 is the same class one level up: an output order implied by a **plan decision** rather
than by a container, and that level was not swept at all in pass 1.

### A5. Audit of the new `ENGINE_AGREEMENT_QUERIES` / `run_engine_agreement_suite`

**(a) Is "SQLite cannot adjudicate this" correct, or was the oracle merely inconvenient?**

**Correct.** SQL leaves the relative order of rows tied under `ORDER BY` unspecified, so
`ORDER BY MIN(season) LIMIT 3` over a seven-way tie has C(7,3)=35 legal answers and every
engine's choice is one of them. SQLite's answer is not an oracle for *which* three; it is
a peer. Putting the query in `QUERIES` would assert a guarantee SQL does not make, and
would go red on a correct engine. The reasoning in the commit is sound, and the premise it
rests on is real: I verified against `data/laps.csv` directly that all seven teams have
`MIN(season) = 2022`, so the tie genuinely spans the cut of 3.

The honest alternative — `ORDER BY MIN(season), team LIMIT 3` — *is* SQLite-adjudicable,
but it defeats the test: with a total order the emission order does no work and the
pre-fix binary passes. So the choice to leave `QUERIES` is right, not a dodge.

**(b) Does it discriminate?** Structurally yes; **but it is not self-checking, and that
is a finding.**

- The suite's entire discriminating power comes from the *data* producing a tie at the
  cut. The comment says so — "Each query must have a tie SPANNING the LIMIT cut, or it
  proves nothing" — and then **nothing asserts it**. `data/laps.csv` is generated by
  `python_tools/generate_data.py`; regenerate it with a different season range and both
  queries become vacuous passes with no signal. That is precisely the
  `JoinEnumeration.DeclinesASemiJoinTree...` failure mode the implementer just fixed
  elsewhere: a test that passes for a reason its comment *describes* rather than
  *enforces*.
- The file already has the right pattern and did not use it. `main()` opens with
  `check_rows_equal_non_finite()` and `check_year_coercion_dependency()`
  (compare_against_sqlite.py:2402-2403) — self-checks of the harness's own preconditions.
  A `check_engine_agreement_tie_precondition()` that runs
  `SELECT team, MIN(season) FROM laps GROUP BY team` and asserts that at least `limit+1`
  groups share the boundary aggregate value would cost four lines and make the suite
  self-checking. **Ranked E-5 (MEDIUM).**
- Pre-fix discrimination itself: reasoned, and the commit reports it measured (2 of 2
  failing pre-fix). Independent confirmation by building a pre-fix binary is **pending the
  gate** — see the run block below.

**(c) Does it run in the gate, and does failure turn the gate red?** **Yes, both.**
- Invoked unconditionally in `main()` at compare_against_sqlite.py:2455-2465 — not behind
  a flag, not inside a loop that could skip it.
- `ea_p/ea_f/ea_e` fold into `m_passed/m_failed/m_errors` (:2463-2465), which fold into
  `total_failed`/`total_errors` (:2699-2701), which drive `sys.exit(1)` (:2707-2708).
- `.claude/skills/verify/SKILL.md` gate 3 is `python3 python_tools/compare_against_sqlite.py`,
  and a non-zero exit is a failed gate. So a divergence is fatal, not merely printed.
- One structural limit worth naming: `run_engine_agreement_suite` calls `run_swiftql` for
  all four modes, and `run_swiftql` raises on a non-zero exit. So **no query Volcano
  refuses can ever live in this suite** — it would be counted as an ERROR, which is also
  fatal. The suite is therefore structurally confined to the shapes both engines execute,
  i.e. it can never cover the vec-only families (multi-way, derived, semi/anti,
  correlated). That is the right behaviour but it bounds the suite's reach sharply, and
  the reach is not stated anywhere.

**(d) Is "the four modes agree" worth pinning where SQLite cannot follow — and what does
it establish?**

Worth pinning: **yes**, with a precise reason. Volcano is designated the correctness
baseline for three query families the vectorized path answers alone; a baseline whose row
order is a function of `std::hash` bucket layout is not a baseline. Determinism plus
cross-mode agreement is the only property left to assert once SQL declines to specify the
answer, and it is a real one.

What it establishes:
- The four modes returned the *same* rows in the *same* order, for these two queries, on
  this dataset, in this build.

What it does **not** establish, and none of this is said in the file:
- **Agreement is not correctness.** All four modes emitting a wrong-but-identical group
  set passes. The suite cannot distinguish "both right" from "both wrong" — by
  construction, since it deleted the only external oracle.
- **It does not test determinism.** Each mode is run exactly once. A mode that was
  nondeterministic *across runs* would make the suite flaky rather than red, and nothing
  would name the cause. (Pre-fix Volcano was in fact deterministic run-to-run — libstdc++
  `std::hash<std::string>` is unseeded — so "deterministic" was never the broken property;
  "agrees with the other engine" was. The commit message conflates the two.)
- **It does not cover the mechanism the fix now depends on.** Both queries have no WHERE
  clause, so the join-side rule provably coincides across all four modes (finding E-1).
  The suite pins the aggregate node and nothing below it.
- **It cannot cover any vec-only shape** — see (c).

---

## Part B — the seam taken fresh

### B1. FINDING E-2 (HIGH) — the shapes only one engine executes, and what actually
### vouches for them

**The refusals are one-directional.** Volcano refuses four families; the vectorized path
refuses nothing Volcano accepts. I checked every `throw` in
`vectorized_plan_builder.cc` (lines 100, 134, 145, 306, 463, 683): all six are internal
consistency assertions ("lowered join input does not match its logical schema",
"a semi/anti join's build input must output exactly its key columns", key-not-found,
unknown node type), not capability declines. So the vec path is a strict superset.

Volcano's four refusals, all in `Planner::plan`, all re-fired per nested block because
`runVolcanoToRows` re-enters `Planner::plan` (main.cc:150):

| # | Shape | Site | TPC-H cells (from the comment at planner.cc:98-113) |
|---|---|---|---|
| 1 | `stmt.joins.size() > 1` — three or more relations | planner.cc:23 | 14 (q2 q3 q5 q10 q11 q18 q21) |
| 2 | `IN (subquery)` anywhere in WHERE/HAVING | planner.cc:71 | 4 (q16 q20) |
| 3 | any correlated subquery | planner.cc:76 | 4 (q4 q17) |
| 4 | derived table in FROM or any JOIN | planner.cc:119 | 12 (q7 q8 q9 q13 q15 q22) |

There is a fifth, subtler split that is not a refusal: for shapes both engines *do*
execute, only the vectorized path runs `PredicatePushdown`, `JoinEnumeration` and
`CardinalityEstimator` (main.cc:565-590). Volcano's plan shape is the written order,
always. So "the two engines agree" never means "the two engines ran the same plan".

**What correctness evidence exists for the vec-only families.** Being precise, because
"no cross-engine check" is not the same as "no evidence":

1. **SQLite, in the two vectorized modes.** `compare_against_sqlite.py` runs
   `MULTIWAY_QUERIES`, the Week 31/32/33/34/35 vec-only blocks and TPC-H against SQLite
   with `ordered = "ORDER BY" in query.upper()` (compare_against_sqlite.py:2141). This is
   a genuine external oracle and is stronger evidence than cross-engine agreement. The
   gate-3 log this session records **101 queries diffed in the two vectorized modes**.
2. **optimized vs `--no-optimize`**, the fourth mode. This is the only *internal*
   differential for those families.
3. **`random_diff.py`**, which compares `--format tsv` positionally for generated
   multi-way joins (per the note at compare_against_sqlite.py:2016-2022).
4. **C++ unit tests**, e.g. `JoinEnumeration.ReorderedPlansReturnTheWrittenOrdersRows`.

**Where that evidence has a hole, and the hole is exactly this seam's subject.**

Evidence (2) is much weaker than it reads. optimized and `--no-optimize` share **every
physical operator** — the same `VecHashJoinNode`, `VecHashAggregateNode`, `VecSortNode`,
`VecDistinctNode`. They differ only in the *logical* plan handed to the same lowering. So
a defect *inside a vectorized operator* is invisible to it by construction; only SQLite can
see one. For the four vec-only families, SQLite is therefore a **single** oracle, not a
redundant pair, and every claim about them rests on it alone.

And SQLite has one documented blind spot — the one pass 1's own fix identified: **it cannot
adjudicate a tie spanning a LIMIT cut.** Put those two facts together:

> For a query in a vec-only family with `ORDER BY <non-total key> LIMIT n` and a tie at the
> cut, **there is no oracle at all.** SQLite cannot adjudicate it (by 87c08a2's own
> argument). Volcano refuses the shape, so cross-engine agreement is unavailable. And
> optimized vs `--no-optimize` differ *legitimately* in join order — `JoinEnumeration`
> reorders the spine — which changes probe order, which changes GROUP BY first-encounter
> order, which changes which tied row survives. So even a real disagreement between those
> two modes is not obviously a bug.

**Correction to my own first draft, kept because the distinction is the point.** Sorting in
`normalize` hides an *order* difference; it does NOT hide a *set* difference. A tie-at-cut
divergence changes which rows survive, so a sorted comparison still sees it. Concretely,
`test_new_queries.py`'s `run_optimizer_invariant` (test_new_queries.py:542-568) compares
optimized against `--no-optimize` with `normalize(...)` at its sorted default and **would
fail on a tie-at-cut set difference**. So the vec-only families are better covered on this
axis than "no oracle" suggests:

- A tie-at-cut divergence between the two vectorized modes **is** caught, by a check that
  asserts a property stronger than SQL guarantees (both answers are legal; the harness
  demands they be equal anyway). That is the right doctrine for this project and it is the
  same doctrine `ENGINE_AGREEMENT_QUERIES` adopts.
- A tie-at-cut divergence between **Volcano and the vectorized path** is *not* caught by any
  SQLite-diffed suite, and cannot be: put such a query in `QUERIES` and it fails against
  SQLite for a legal reason, in every mode. The only suite that can hold it is
  `ENGINE_AGREEMENT_QUERIES`, which has two entries and neither varies the build side.
- And `ENGINE_AGREEMENT_QUERIES` structurally cannot be extended to the vec-only families
  at all: `run_engine_agreement_suite` calls `run_swiftql` in all four modes, so Volcano's
  refusal surfaces as an ERROR. Covering them needs a **two-mode** agreement suite, which
  is what `run_optimizer_invariant` already is — but its query list contains no tie-at-cut
  shape.

So the residual hole is narrower than "no oracle", and sharper: **it is the absence of any
query, anywhere in the harness, whose ORDER BY is non-total at a LIMIT cut.** The class is
unaddable to the SQLite-diffed suites by construction, and the two suites that could hold
it (`ENGINE_AGREEMENT_QUERIES`, `run_optimizer_invariant`) contain no instance.

Severity HIGH not BLOCKER: I could not construct a *TPC-H* instance. TPC-H's ORDER BY
clauses are deliberately near-total (q2 `s_acctbal desc, n_name, s_name, p_partkey`;
q21 `numwait desc, s_name`), and q3's `revenue desc, o_orderdate` ties only on a
floating-point sum. The hole is structural and demonstrable in principle; I have no
shipped query that falls into it today.

### B2. NULL semantics — checked end to end, **no divergence found**

NULLs are reachable in this engine despite CSV being unable to express one: outer-join
null-extension, `x/0`, aggregates over an empty group, and a materialized scalar subquery
returning zero rows.

- **Three-valued AND/OR.** `evaluate()` (evaluator.cc:110-122) and `runLogical()`
  (expression_executor.cc:274-296) implement the same tri-state `-1/0/1` lattice, with the
  same "false dominates / true dominates" ordering, and both deliberately run *before* any
  blanket NULL propagation. `runLogical` explicitly does not call `propagateNulls`.
- **Blanket NULL propagation** for every other operator: evaluator.cc:125 vs
  `propagateNulls` (expression_executor.cc:97-106). Same rule.
- **NULL is not true.** Volcano's `FilterNode` uses `evaluate` then `!isNull() && asInt()!=0`;
  the vec path's `evalFallback` uses the identical test twice (columnar_eval.cc:110, :124),
  and the fast-path `scanColumn` skips invalid rows outright (columnar_eval.cc:35, :40).
  `CASE` treats a NULL WHEN as not-taken on both (evaluator.cc:218; the vec path declines to
  compile `CaseExpr` at all and falls back to `evaluate`).
- **NULL in aggregates.** Both skip NULL before accumulating, both use
  `is_star ? count : distinct ? distinct_keys.size() : non_null_count` for COUNT, both gate
  SUM/AVG on `non_null_count > 0`, both keep the argument `Value` for MIN/MAX
  (plan_nodes.cc:290-326 vs vec_hash_aggregate_node.cc:203-226). Confirms pass 1's T2c.
- **NULL as a group key.** Both serialize through the shared `appendGroupKeyField`
  (`key_encoding.h`), so a NULL group buckets identically. Both `Row`↔`ColumnVector`
  conversions are NULL-aware (`appendColumnValue` back-fills the validity prefix,
  `readColumnValue` returns `Value::null()` — vec_types.h:88-136), so an outer join's NULLs
  survive materialization into a chunk.
- **NULL in join keys.** Both drop them via the shared `isUnmatchableKey`
  (plan_nodes.cc:639 `serializeRowKey`, vec_hash_join_node.cc `serializeKey`), and both
  null-extend rather than drop under `left_outer`.
- **Sort.** `compareForSort` is shared, with NULLs-first-ascending, and the two comparator
  lambdas are byte-identical. The header (value.h:46-59) records *why* `operator<` is not
  used — it is not a strict weak ordering over NULL.

### B3. Type and precision — checked, **no divergence found**

- **Integer overflow.** Both paths route `+ - * /` and unary `-` on INT through
  `checked_arith.h` (`checkedAdd/Sub/Mul/Div/Negate`) — evaluator.cc:143-147, :163 vs
  `arithKernelInt` / `runNegate` (expression_executor.cc:211-223, :293-305). Same throw.
- **INT/INT division truncates, `x/0` is NULL** on both (evaluator.cc:147 vs
  expression_executor.cc:218-222), including the DOUBLE case where `/0.0` is NULL rather
  than an infinity (expression_executor.cc:201-205).
- **Mixed INT/DOUBLE comparison.** `Value`'s operators coerce both to double
  (`NUMERIC_COERCE`, value.cc:50-78); the compiled path inserts `CAST_DOUBLE` on the INT
  side (expression_executor.cc:568-575). Same numeric result, same precision loss above
  2^53 on both.
- **SUM/AVG accumulator width.** Both accumulate into a `double` and both report
  `aggregateResultType(SUM) == DOUBLE`. A large-INT SUM loses precision identically on both
  — a shared limitation, not a divergence.
- **String comparison.** `Value::operator<` on two STRINGs is `std::string` byte order;
  `compareKernel<std::string>` and `scanColumn<std::string>` use the same operators. No
  collation anywhere, on either path.
- **`LIKE` and `SUBSTRING` are literally shared functions.** `expression_executor.cc`
  includes `evaluator.h` and calls `likeMatch` and `substringOf` directly
  (expression_executor.cc:326, :406). They cannot drift.
- **`IN`.** The compiled `IN_SET` splits `in_ints`/`in_doubles`/`in_strings` specifically to
  reproduce `Value::operator==`'s INT-exact / INT-vs-DOUBLE-coerced rule
  (expression_executor.cc:38-42, :645-658). Mixed STRING/numeric lists decline to compile
  and fall back to `evaluate`, which raises the same error.
- **Dates** are STRINGs; `IntervalLiteral` throws identically on both if it survives folding
  (evaluator.cc:237; the vec path declines to compile it and falls back to that throw).

The alignment here is architectural, not coincidental: **whenever the vectorized compiler
cannot reproduce a semantic exactly, it returns `nullptr` and the caller falls back to
`evaluate()`** (expression_executor.cc:483-485, columnar_eval.cc:98-125). Every decline I
found is on that pattern — NULL literals, `CaseExpr` (to preserve short-circuiting),
STRING-vs-numeric comparison, non-INT logical operands. That is the right architecture for
this seam and it is why B2/B3 came back clean.

### B4. Empty and degenerate inputs — checked, **no divergence found**

- **Aggregate over an empty relation.** Both synthesize one default group when
  `group_by_cols_.empty() && <map>.empty()` (plan_nodes.cc:320-325 vs
  vec_hash_aggregate_node.cc:232-237), giving `COUNT -> 0` and `SUM/AVG/MIN/MAX -> NULL` on
  both. SQL-correct on both.
- **Zero-row GROUP BY** emits nothing on both.
- **Empty build side of a join.** `build_width_` is taken from the build child's *schema*,
  not from a row (plan_nodes.cc:667 vs vec_hash_join_node.cc:79-80), so a zero-row build
  side still null-extends to the right width under `left_outer`.
- **`LIMIT 0`.** `LimitNode::next` tests `count_ >= limit_` before pulling
  (plan_nodes.cc:576) and `VecLimitNode::nextChunk` tests `rows_emitted_ >= limit_` before
  pulling (vec_limit_node.cc:12). Both return immediately and **never call the child**, so a
  per-row runtime error below the LIMIT fires on neither. Identical.
- **`LIMIT n` truncation.** Volcano counts rows; the vec node truncates the final chunk in
  place, resizing `sel.indices` when a filter is applied and otherwise resizing each column
  *and its validity vector* (vec_limit_node.cc:38-48). The validity resize is the
  non-obvious part and it is present.

### B5. DISTINCT and duplicates — no divergence found beyond E-1's reach

Both `DistinctNode` and `VecDistinctNode` key on every output column via the shared
`appendGroupKeyField` and emit in first-seen order; `VecDistinctNode` consumes the incoming
selection before deduplicating. Confirms pass 1's T2e.

But note the same exposure as E-1: DISTINCT's output order is *input* order, so
`SELECT DISTINCT ... ORDER BY <non-total key> LIMIT n` inherits every join-side and
join-order divergence above it. `DISTINCT` was not part of 87c08a2's reasoning and is not
in `ENGINE_AGREEMENT_QUERIES`.

### B6. FINDING E-3 (MEDIUM) — `development.md` → *Relation slots and query levels* is
### wrong a THIRD time, and the code already says so

The file was checked against the code, as instructed. Its Week 32/33/34 tables are
accurate. Its **containment statement is not**, and the staleness is not marginal — it is
the load-bearing premise the whole "Unreachable" half rests on.

`development.md:770` (section heading and fact 1):

> ### Unreachable with a correlated ref today (behind the refusal)
> 1. `Validator::validate` refuses any statement with `has_correlated_subquery` … A
>    `ColumnRef` with `query_level > 0` exists **only** inside a correlated subquery, so
>    none reaches a plan.

**There is no such refusal.** `validator.cc:150-151`: *"Week 33 REMOVED the refusal that
stood here."* And the same comment block, at validator.cc:142-146, names this document by
hand:

> *"the containment development.md's slot-consumer table rested on — 'a ColumnRef with
> query_level > 0 exists ONLY inside a correlated subquery, and those are refused here' —
> NO LONGER HOLDS AT THIS SITE."*

So the code was corrected, the correction explicitly points at this document, and the
document was not updated. Two rows in the tables below it restate the dead precondition
verbatim:

- **`materializeSubqueries` / `buildReplacement`** — *"safe by PRECONDITION … `Validator::validate`
  refuses a correlated statement first, so this pass only ever meets an uncorrelated
  `SubqueryExpr` … If a later week lets it run before validation, **or on a correlated
  node, this row is void**."* It now runs on correlated nodes routinely —
  `subquery_materialization.cc:351` has an explicit `if (sq->correlated)` arm, whose own
  comment says *"Week 33 deleted that refusal, and this pass … kept trusting a rule that no
  longer existed."* **The row is void by its own terms and was not removed.**
- **`buildAggregateSchema` (guarded)** — *"Week 31 checked and did NOT arm it: a correlated
  group key can only appear inside a correlated subquery, **which is refused**."* The
  stated reason is dead; whatever keeps that guard unarmed today is something else
  (decorrelation's own refusals), and the document does not say what.

No wrong answer follows: the real containments are in the code (`materializeSubqueries`'s
own `sq->correlated` test, `ColumnId`'s private slot, `subquery_decorrelation`'s named
refusals). The defect is that this file's *stated purpose* is to be the list a future week
reads instead of re-verifying — *"A missing row is worse than a wrong one. A future week
reads this as already-checked and skips the verification."* A row that says
"safe by precondition P" where P is false is that failure exactly, and this is the third
time this section has been found wrong.

MEDIUM, not LOW, only because the file explicitly asks to be trusted in place of
re-verification. If it were an ordinary comment it would be LOW.

(Minor, noted for the record: the audit brief cites `docs/development.md`; the file is at
the repo root, `/home/user/swiftql/development.md`. `docs/` contains only the week plans
and the TPC-H baseline.)

### B7. What "0 result divergences" actually measures

Pass 1 reported the harness at 0 divergences and this session's gate-3 log records
`1336 passed, 0 failed, 0 errors`. That number is real, and it is smaller than it sounds:

1. **It is not four modes for most of the interesting SQL.** The same log's own census:
   *168 diffed in all four modes; 101 diffed in the two vectorized modes only*
   (MULTIWAY 16, W31 1, W32 20, W33 18, W34 41, W35 5). For those 101, "the engines agree"
   was never tested — Volcano refused them (finding E-2).
2. **Row order is compared only when the query text contains `ORDER BY`**
   (compare_against_sqlite.py:2141). Every GROUP BY without ORDER BY is compared as a
   sorted multiset, which is correct SQL practice and also means emission order is
   invisible there — the property 87c08a2 made load-bearing.
3. **A tie at a LIMIT cut is unadjudicable by SQLite** — the whole reason
   `ENGINE_AGREEMENT_QUERIES` had to leave `QUERIES`. Note precisely what this does and does
   not mean: a tie-at-cut divergence is a row-SET difference, which a sorted comparison
   *would* see; the problem is that SQLite's own answer is a third legal set, so no SQLite
   suite can hold such a query at all. The class is therefore excluded by construction from
   every one of the 1336 comparisons, and covered only by the two
   `ENGINE_AGREEMENT_QUERIES` entries. Finding E-1 is a third instance neither reaches.
4. **`normalize` keys rows by column NAME into a dict**, so duplicate column names collapse
   — the file documents this at :2005-2022 and points at `random_diff.py` for the coverage.
5. **The two Volcano modes differ only in storage**, and the two vectorized modes only in
   the optimizer. There is no row-storage vectorized mode at all (main.cc:548 rejects
   `--execution vectorized` without `--storage columnar`), so the engine axis and the
   storage axis are not fully crossed: every Volcano-vs-vec comparison is also a
   row-vs-columnar comparison, except on the `col-volcano` leg.

So: 0 divergences is a true statement about *the shapes and properties the harness can
see*, and pass 1 was right to report it. It is not evidence that the two engines agree on
`ORDER BY <agg> LIMIT n` with a tie, on any vec-only family, or on any plan shape where the
two engines choose different build sides — which is finding E-1.

### B8. Measurement — how many harness queries can see this class at all

Not reasoned; measured, by running every string-list suite in
`compare_against_sqlite.py` through the SQLite oracle and asking, for each query with
`ORDER BY … LIMIT n`: is there a tie spanning the cut, **and do the tied rows differ in any
output column** (an immaterial tie — several identical rows — cannot change the answer).

```
queries with ORDER BY + LIMIT in the whole file : 32
   of those, a tie SPANNING the cut             : 10
   of those, a MATERIAL tie (tied rows differ)  :  2
```

Both material ones are `ENGINE_AGREEMENT_QUERIES` — the two 87c08a2 added. **Every other
query in the file is immune by construction**: `SELECT team FROM laps … ORDER BY team
LIMIT 5` ties five ways on `team`, but all five rows are the same row, so any choice is the
same answer.

Two things follow:

1. Pass 1's "0 result divergences" was never at risk from this class, and green was not
   luck in the sense of "a tie was one row away" — it was structural: **before 87c08a2 the
   corpus contained zero queries that could express the divergence.** The commit did not
   just fix a bug, it created the only two queries in the project that can see it.
2. Because the corpus contains exactly two, the coverage of this class is exactly as wide as
   those two queries' plan shapes — one single-scan GROUP BY, one two-relation join with no
   WHERE. Finding E-1 lives one step outside both.

(One query, `SELECT team, round / (round - 1) AS g FROM laps ORDER BY team, g, round
LIMIT 20`, could not have its ORDER BY key resolved by the probe — it orders on a select
alias and a base column together. Inspected by hand: the twenty rows are all
`('AlphaTauri', <same g>)`, an immaterial tie.)
