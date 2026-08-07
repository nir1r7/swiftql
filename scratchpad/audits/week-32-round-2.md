# Week 32 round 2 audit — semi-joins and anti-joins

Branch `claude/phase5-week26-qomtkb`, HEAD `6d7c7e9`, diff base `0fcf2b7`.
Round 1's three findings (`2165cfd`, `46a8ebc`, `a72d27a`, `10b9f07`) are
excluded by instruction and were not re-derived.

Method: read-only trace. No build, no test run (a gate owns `build/`).

Tally: **0 blockers, 0 high, 2 medium, 3 low, 1 hunch.**

---

## Target 1 — the new C++ test bodies

Read line by line: `tests/test_vectorized.cc:4168-4317`,
`tests/test_cardinality.cc:596-738`, `tests/test_join_enumeration.cc:792-872`,
`tests/test_vec_plan_builder.cc:1160-1270`. `tests/test_subquery.cc` was NOT
reached — see "Not reached".

### Verified genuine (would fail if the behaviour regressed)

- `VecSemiJoin.ADuplicateBuildKeyEmitsTheProbeRowExactlyOnce` —
  `tests/test_vectorized.cc:4232`. Build `{1,1,2,9}`, probe `{1,2,3,4}`,
  expects `{1,2}`. An inner-join-plus-projection implementation emits `1`
  twice and fails. `EXPECT_EQ` on a `vector<int64_t>` also pins order and
  count, so a duplicate cannot hide. Real.
- The NULL tests genuinely contain NULLs. `nullableSemiJoin`
  (`tests/test_vectorized.cc:4204`) routes both inputs through
  `NullableSourceNode` (`tests/test_vectorized.cc:2310`), which builds the
  chunk with `appendColumnValue` and therefore carries a real validity mask —
  not the `ColumnarTable` path that silently turns `Value::null()` into `0`.
  `VecAntiJoin.ANullOnTheBuildSideEmitsNothingAtAll` (`:4271`) would return
  `{3,4}` without the `build_had_null_key_` flag, so it is load-bearing.
  `VecSemiJoin.ANullProbeKeyIsDroppedUnlessTheBuildSideIsEmpty` (`:4288`)
  asserts `empty_build[0][0].isNull()`, which only holds if the NULL survived
  the operator's row copy — a second, independent check that the NULL is real.
- `Cardinality.SemiJoinIsALeftSideSelectivityFromTheNdvRatio`
  (`tests/test_cardinality.cc:640`) — the "20 rows over 10 distinct keys"
  seeding is doing what its comment claims: semi = `1000*min(1,10/50)` = 200,
  product form = `1000*20/max(50,10)` = 400. The two numbers differ, so the
  test discriminates the rule rather than the arithmetic. `EXPECT_NE(...,400)`
  is redundant but harmless.
- `JoinEnumeration.DeclinesASemiJoinTreeAsSlotOutsideTheRangeTable`
  (`tests/test_join_enumeration.cc:803`) — the "which decline fired" assertion
  (`order_decision.empty()` on every join) is real: the outer-join decline at
  `src/planner/join_enumeration.cc:57` stamps a string, this one does not.
  The control case at the end (two relations, `join_slot == 1`) does separate
  the `<3-relation` guard from this decline.
- `VecPlanBuilder.SemiJoinForcesTheBodyOntoTheBuildSide`
  (`tests/test_vec_plan_builder.cc:1213`) — big body (`laps`), small spine
  (`drivers`), so a cost-driven choice would land the other way. `EXPECT_FALSE(scansTable(kids[0], "laps"))` closes the loophole where `scansTable`
  matches on both sides. Real.

### MEDIUM-1 — `VecSemiJoin.RefusesEveryIllegalCombination` cannot distinguish
### which guard threw, and one of its four cases is already covered by an
### older guard

`tests/test_vectorized.cc:4302-4315`. `EXPECT_THROW(..., std::runtime_error)`
with four different illegal combinations, all against the same exception type.
Concrete consequence: if the `swapped_` guard at
`src/execution/vec_hash_join_node.cc:26` were deleted, case 1
(`make(probe_schema, true, false, nullptr)`) would still throw — the
pre-existing `left_outer_ && swapped_` check does not fire (left_outer is
false), but the `output_schema_.size() != probe_child_->outputSchema().size()`
check at `:40` also does not fire (both are size 1), so case 1 would actually
*stop* throwing and the test would catch it. Case 2
(`swapped=false, left_outer=true`) is the one at risk: delete the semi-specific
`left_outer_` guard at `:31` and the node constructs fine, the test fails —
so that one is genuine too. Verdict: the four cases are individually
load-bearing, but the test cannot say *which* message it got, and the operator
emits four distinct messages by design. `EXPECT_THROW` → matching on the
message (as the round-1 fix did for the harness) is the consistent standard.
Severity MEDIUM because a future guard reordering can silently move a case
onto the wrong message with the test still green.

### LOW-1 — `Cardinality.SemiAndAntiEstimatesPartitionTheLeftSide` pins a
### tautology alongside its real assertion

`tests/test_cardinality.cc:659-671`. `EXPECT_DOUBLE_EQ(semi + anti, left)` is
true by construction of the code under test — `src/planner/cardinality_estimator.cc:459`
computes `anti = l_rows - semi` from the same `semi`, and the clamp at `:460`
is a no-op whenever `frac ∈ [0,1]`, which `std::min(1.0, ...)` at `:452`
guarantees. It cannot fail unless the subtraction itself is rewritten. The
`EXPECT_DOUBLE_EQ(anti, 600.0)` on the line above is the assertion carrying
the test. Not wrong, just not the check its comment claims ("any drift between
them is a rule that stopped being a partition").

### LOW-2 — `VecPlanBuilder` semi-join tests load the body table by hand, which
### is not what the CLI does

`tests/test_vec_plan_builder.cc:1163-1181`. `buildVecWithBodyTable` adds the
body's table to the map because the file-local `loadColumnar` helper
(`:27`) walks only `stmt.joins`. **Checked and NOT a bug in the engine**: the
real CLI collects nested-query tables (`src/cli/main.cc:355-375`, the loop that
explicitly skips a table the catalog does not have and comments that "a nested
query's tables need them too"), so `tables.at(scan->table_name)` at
`src/planner/vectorized_plan_builder.cc:196` and
`tables.at(leafScanTable(join->children[1].get()))` at `:281` both resolve on
the real path. Recorded because the divergence between the test harness and
the CLI is exactly where a future "the body's table is missing" regression
would hide from these tests: no unit test in this file exercises the
production table-collection path for an IN body.

---

## Target 2 — the three round-1 fixes as new code

- **Suite split.** `python_tools/compare_against_sqlite.py` — verified that
  `WEEK32_LOWERING_REFUSED_VOLCANO` is *derived* from
  `WEEK32_LOWERING_REFUSED` (same three query strings) rather than retyped, so
  a new shape added to the vec list cannot lose its Volcano leg. Both lists
  assert a message the corresponding path actually emits: the vec list against
  `subquery_lowering.cc`'s wording, the Volcano list against
  `src/planner/planner.cc:59`'s "IN subqueries are lowered to a semi-join…".
  No query lost coverage. ✅
- **`semantics == STANDARD` guard.** `src/planner/cardinality_estimator.cc:390`.
  Grepped every reader of `join.join_slot` in `src/`:
  `join_enumeration.cc:94,135,136,139` (all behind
  `hasSlotOutsideRangeTable`'s decline), `predicate_pushdown.cc:279` (now
  guarded), `cardinality_estimator.cc:395` (now guarded),
  `vectorized_plan_builder.cc:120` (see MEDIUM-2), `logical_plan.cc:698-740`
  (construction, STANDARD only). No other site merges body stats. STANDARD
  joins are untouched: the guard wraps only the merge loop and the early
  `return out` at `:463` sits *below* the merge and *above* the LEFT-join
  branch, so `join_type == LEFT` behaviour is unchanged. ✅

### MEDIUM-2 — `collectSlotTables` writes the body's table at slot `-1`, and it
### is reachable with two `IN` conjuncts

`src/planner/vectorized_plan_builder.cc:118-131`. `collectSlotTables` has no
`semantics` guard: line 120 does
`out[join->join_slot] = leafScanTable(join->children[1].get())`, so for a
semi/anti node it stamps the **body's** table name at key `-1`, then recurses
into `children[0]`.

Concrete reachable input:
`SELECT l.lap_id FROM laps l WHERE l.driver_id IN (SELECT driver_id FROM drivers) AND l.lap_id IN (SELECT lap_id FROM laps)`
— the query `VecPlanBuilder.TwoInConjunctsLowerToTwoStackedSemiJoins`
(`tests/test_vec_plan_builder.cc:1257`) already builds. Two stacked semi joins;
lowering the outer one runs the cost block at
`src/planner/vectorized_plan_builder.cc:216-225` **before** the semantics
early-return at `:337`, so `rowWidth(join->children[0].get(), catalog)` is
called on the *inner semi join*. `isSingleRelation` (`:56`) returns false at
that JOIN node, so `collectSlotTables` runs on a semi-join node and writes
`slot_tables[-1] = "drivers"`.

**Not currently a wrong answer**: `rowWidth`'s consumer loop at `:169` looks up
`slot_tables[col.relation_slot]` for the columns of the *spine* schema, whose
slots are 0/1, so the `-1` entry is never read; and the width it computes is
discarded, because the semi branch returns before `setCostDecision`. It is
dead work over a poisoned map. Severity MEDIUM rather than LOW because it is a
live violation of the `join_slot == -1` contract stated on
`src/planner/logical_plan.h:96-101` ("every reader of `join_slot` must either
decline on `semantics != STANDARD` or be provably unreachable") — this reader
does neither, and the "provably unreachable" half is what the next change
breaks. Minimal fix: `if (join->semantics != JoinSemantics::STANDARD) { collectSlotTables(join->children[0].get(), out); return; }` at the top of
`collectSlotTables`, and hoist the semantics early-return above the cost block.

---

## Target 3 — `NOT IN` with NULLs

Hand-traced `src/execution/vec_hash_join_node.cc:200-252` against all four
cases. All correct:

| case | trace | SQL | verdict |
|---|---|---|---|
| NULL in subquery result | build `serializeKey` fails (`:105`) → `build_had_null_key_ = true`; probe short-circuits at `:209`, zero rows | `x NOT IN S` is FALSE-or-UNKNOWN for every x; WHERE keeps neither | ✅ |
| NULL on probe side, S non-empty | `serializeKey` fails at `:228`, the inner `build_keys_.empty()` test at `:229` is false → `continue`, row dropped | `NULL NOT IN (1)` is UNKNOWN | ✅ |
| empty subquery result | `build_keys_` empty, `build_had_null_key_` false; non-NULL probe key misses → `hit == false != (sem==SEMI)`… i.e. `false != false` is false → **not** skipped → emitted. NULL probe key takes the `:229` branch and is also emitted | `x NOT IN ()` is TRUE, including for NULL x | ✅ |
| duplicate build keys | `build_keys_` is an `unordered_set<string>`, insert at `:113` collapses them; the probe loop emits the probe row once per probe row, never per match | `R ▷ S` emits each R row at most once | ✅ |

Also traced the buffer/loop interaction at `:250`: `break` when
`output_buffer_` is non-empty vs `continue` when empty are equivalent here
(`output_cursor_ == 0`, so the `while` head re-tests to the same answer), and
neither discards rows. The comment overstates the danger but the code is right.

### LOW-3 — a NaN in the subquery result is treated as a NULL for ANTI

`src/execution/vec_hash_join_node.cc:105` sets `build_had_null_key_` on **any**
`serializeKey` failure, and `isUnmatchableKey`
(`src/execution/key_encoding.h:88-91`) returns true for NaN as well as NULL.
Concrete input: a DOUBLE column whose subquery result is `{1.0, NaN}`, probe
value `3.0` → ANTI emits nothing; the relational answer is that `3.0` survives,
since NaN is a value that simply never compares equal. **Marked LOW, not
MEDIUM, because it does not break the oracle**: `key_encoding.h:62-63` records
that SQLite converts NaN to NULL on storage, so SQLite would also return
nothing here, and `compare_against_sqlite.py` will agree. The flag's *name* and
the comment at `:106-108` ("the NULL that makes `x NOT IN S` never TRUE") are
what is inaccurate — it is an unmatchable-key flag, and the ANTI collapse it
drives is only justified for the NULL half.

---

## Target 4 — `join_slot == -1` and the two range tables

- `PredicatePushdown::distribute` — `src/planner/predicate_pushdown.cc:275-280`.
  The decline is **complete**. Without it, `by_slot.find(-1)` would match the
  bucket `slotOf` uses for "references none, several, or unresolved"
  conjuncts, pushing a cross-relation or constant residual into the subquery
  body, where it is unresolvable. The unconditional recursion into
  `children[0]` is preserved, so a WHERE conjunct still reaches the spine's
  scans with the semi join interposed. ✅
- `JoinEnumeration` — `src/planner/join_enumeration.cc:92-100`.
  `hasSlotOutsideRangeTable` fires on `join_slot < 1`, and `-1 < 1`. The
  decline is checked **before** `decompose()` moves any subtree, so the tree
  returns untouched. ✅ Recursion is `children[0]` only, so the body's own join
  tree is never entered from the outer pass — correct, since it was already
  optimized by its own `LogicalPlanBuilder::build` call.
- Pruning hint — `src/planner/vectorized_plan_builder.cc:295-300`. The body
  gets `lower(join->children[1].get(), nullptr)`, i.e. **no hint**, so an outer
  predicate can never prune the body's chunks. The spine still receives the
  hint via `children[0]`, which is correct (the WHERE filter sits above the
  semi join and its conjuncts must hold on spine rows). ✅

### HUNCH — `development.md`'s *Relation slots and query levels* table
The table was read against the code and its `CardinalityEstimator` row now
matches the guarded merge (that was round 1's fix). I did **not** finish
checking the remaining rows of that table against every listed consumer —
specifically I did not verify the `VectorizedPlanBuilder` row against
`collectSlotTables`, which MEDIUM-2 says is an unguarded reader. If that row
claims the builder never reads `join_slot` for a non-STANDARD join, it is wrong
by omission for a third time. Flagged as a hunch, not a finding: I ran out of
budget before reading the table itself.

---

## Not reached

- `tests/test_subquery.cc` (+323/−… lines) — the largest test delta of the
  week, not read at all. It carries the lowering-shape tests, the
  `output_schema == children[0]->output_schema` / `join_slot == -1`
  invariant test, the retired materialized-IN suite, and
  `SemiJoinLowering.RefusesABodySharedByTwoExpressions`.
- `python_tools/compare_against_sqlite.py` (+214) — only the two refusal
  lists were checked for derivation and message. The 13
  `WEEK32_SEMI_JOIN_VEC_ONLY` queries were not read, so whether they actually
  cover the NULL/duplicate/empty cases the plan claims is unverified.
- `src/planner/subquery_materialization.{h,cc}` (−98/−24) — the removal of
  the `Kind::IN` branch, `MAX_MATERIALIZED_IN_VALUES` and `distinctNonNull`
  was not checked for a surviving caller.
- `development.md` table rows (see HUNCH above).
- `README.md` Week 32 section and starting notes were not diffed.
