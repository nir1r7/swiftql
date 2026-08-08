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
