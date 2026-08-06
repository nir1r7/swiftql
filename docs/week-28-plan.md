# Week 28 — Join Enumeration

Teaching plan for the Phase 5 / Week 28 checkpoint. Working branch:
`claude/phase5-week26-qomtkb` (everywhere the workflow says `main`, read that).

**Checkpoint (README):** *`EXPLAIN` shows cost-based multiway join order
decisions.*

Read that literally. The deliverable is a **decision**, made from costs, on the
**order relations are joined in**, **visible in `EXPLAIN`**. It is not "make
TPC-H fast", it is not a new operator, and it is not a new join shape. Outer
joins are Week 29; subqueries begin at Week 30. TPC-H 22/22 is the phase goal
and it informs *which* generalizations are worth making — it is not this week's
bar.

Weeks 26 and 27 built everything this week consumes and left it deliberately
unused. Week 26 widened the range table to N relations and made
`predicate_pushdown` route by slot. Week 27 made N-way trees execute, and — this
is the sentence that defines Week 28 — kept **the written order exactly as the
parser gave it**. Every join in every plan today is `stmt.joins[i]` folded
left-deep in source order. This week that fold stops being the plan and becomes
the *baseline* one candidate among many is compared against.

So Week 28 is not "add a feature". It is **"break one identity and repair
everything that silently depended on it"**. The identity is:

```
the relation at the bottom of the left spine is binder slot 0,
and the k-th join adds binder slot k
```

Nothing in `src/` asserts that. Four things assume it. Finding them is most of
the week's work; the dynamic program itself is about 90 lines.

---

## What Week 28 must deliver

| README bullet | Tasks |
|---|---|
| Add left-deep dynamic-programming join ordering with a configurable limit | 3, 4, 5 |
| Use a greedy fallback for larger join graphs | 4 |
| **Checkpoint:** `EXPLAIN` shows cost-based multiway join order decisions | 6 |
| (Week 22's deferred data-volume cost term, due here — see below) | 2 |

The Week 22 scope note is a fourth, non-optional bullet even though it is not
written in the Week 28 section:

> *"the data-volume (bytes-materialized) term is deferred to Week 28 … it first
> affects a plan choice in Week 28, where differing intermediate-result widths
> across join orderings make it discriminate."*

A DP that ranks orderings on CPU terms alone is a DP that cannot see the one
thing join ordering exists to control: the size of the intermediate results.
Task 2 lands it.

## The Starting notes, and where each is answered

Every bullet of the README's *"Starting notes, from Week 27's foundations"*
block that sits above the Week 28 section:

| Starting note | Answered in |
|---|---|
| **`LogicalJoin::keys` is already the join graph's edge list, `join_slot` is the vertex id.** Enumeration needs no new structure to *represent* the graph — it needs to fold in a different order and re-derive each join's keys and merged schema. The identity `joins[i] → slot i+1` is what breaks first; anything inferring a slot from a tree position must read `join_slot` | **Task 3**. No `JoinGraph` class: `decompose()` reads `LogicalJoin::keys` into a flat edge list and hands the slots back. The audit of "anything that still infers one from the other" is Task 3 §4 — `distribute()` already reads `join_slot` (Week 26 fixed it), `LogicalPlanBuilder`'s fold keeps `i + 1` because it *is* the written-order construction and enumeration runs after it. The two that genuinely break are `leftKeyIndices()`'s slot resolution and `ChunkPruner`'s `relation_slot < 1` |
| **The data-volume cost term now has a real consumer and a placeholder in its seat.** `rowWidth` returns `columns * 8.0` for any multi-relation input, which is exactly the case enumeration compares. Land the real per-relation width sum with the enumeration, not before | **Task 2** — both halves: `joinOutputCost()` (the bytes-materialized term) and the per-relation `rowWidth` sum, keyed on `ColumnDef::relation_slot` and a slot→table map read off the spine |
| **`--no-optimize` must keep the written join order.** It is the benchmark baseline and the differential oracle — `compare_against_sqlite.py` runs the vectorized suite twice | **Task 5 §2**. The pass is called inside `main.cc`'s existing `if (!args.no_optimize)` block, alongside pushdown and estimation. Task 7's harness work is what makes the second run able to catch a reordering that changes an answer |
| **Residual `ON` conjuncts are already in the `WHERE` conjunction by the time pushdown runs**, so enumeration inherits them for free — but the fold is inner-join-only, and Week 29's outer join must split them back apart | **Task 3 §5** — verified, not coded. Enumeration must not re-split, re-fold, or re-place them, and the `join_condition.h` marker stays exactly where it is |
| **A residual referencing *two* relations stays above the whole join tree today** (`soleSlot()` returns -1). Once enumeration can choose which pair joins first, such a conjunct becomes attachable to the lowest join whose output contains both relations — a real optimization, out of scope until the ordering exists | **Deliberately still deferred.** See "What not to build" below. The ordering now exists, so the note's precondition is met — but the checkpoint does not require it, and Week 29 is about to redefine what "attach a predicate to a join" means for an outer join. Landing it now means landing it twice |
| **Key serialization is one shared contract** (`src/execution/key_encoding.h`) — Week 29 and Week 32 inherit injectivity, NULL policy and exact DOUBLE comparison by using it and lose all three by hand-rolling | **Task 3 §3** — enumeration produces `JoinKey`s and never goes near a serializer. But re-orienting an edge swaps which column is `from_col` and which is `join_col`, and *that* changes field order inside the composite tuple. Why it is still safe is a one-paragraph argument you must actually make, not assume |

## Prerequisite knowledge

Read all of these before writing a line. Week 28 introduces no new `Expr`
subtype, so `development.md`'s 18-site dispatch checklist is **not** a per-task
chore — but the four files below encode the positional assumption this week
deletes.

- **`src/planner/cardinality_estimator.cc`** — specifically the `JOIN` case's
  NDV product and the `have_ndv` / `divisor > 1.0` distinction. Its NDV handling
  was corrected **twice** during Week 26 and the comments record both
  corrections. Join enumeration consumes this directly, so if you read one file,
  read this one. **Do not copy the formula into the DP.** Task 1 exists to stop
  you.
- **`src/planner/vectorized_plan_builder.cc`** — `leftKeyIndices()` /
  `rightKeyIndices()` (why one throws on a slot miss and the other resolves by
  bare name), `rowWidth()` / `isSingleRelation()` / `leafScanTable()`, and the
  `pruning_where` routing in the `JOIN` case.
- **`src/planner/logical_plan.cc`, `LogicalPlanBuilder::build`** — the left-deep
  fold, lines 595–626. The merged-schema construction there is the thing Task 3
  re-implements for an arbitrary order, and the difference between the two is
  the entire correctness argument.
- **`src/storage/chunk_pruner.h`** — the `col->relation_slot < 1` test and the
  comment explaining why it is safe. That comment is true only while the
  leftmost scan is relation 0.
- **`src/planner/predicate_pushdown.cc`** — `soleSlot()` / `distribute()`.
  Enumeration runs *after* this pass and must not disturb what it placed.
- `src/planner/cost_model.{h,cc}` — all four constants and why
  `CPU_SIMD_COMPARE` is 0.02 (`docs/hash-vs-simd-crossover.md`). Task 2 adds a
  fifth constant and must not perturb the calibrated four.
- The `invariants` skill — **1** (fixed join schema order), **2**
  (`relation_slot` is the relation identity), **11** (result-preservation:
  optimized ≡ `--no-optimize` ≡ SQLite), **12** (chunk pruner only prunes
  FROM-side predicates), **13** (cost-based decisions are result-invariant
  internals — tests assert output equality, never internal choices, *except via
  `--explain` shape checks*). Invariant 13 is what makes Task 6 a real
  deliverable rather than cosmetics: `--explain` is the only sanctioned surface
  for asserting a cost decision.
- The `optimizer-diff` skill — the optimized-vs-`--no-optimize` workflow is this
  week's primary correctness gate.

## The identities: which hold, which break

Week 27's sticky note, updated. Copy this out before starting.

```
HOLDS  (1)  LogicalJoin::children[1] is ALWAYS exactly one relation.
            children[0] may be a whole join subtree.          [left-deep]
            Week 28 must NOT produce a bushy tree — rightKeyIndices(),
            rowWidth()'s isSingleRelation() split and the whole lowering
            path are written against this.

HOLDS  (2)  Output row order is [children[0] columns] ++ [children[1] columns],
            regardless of which side physically builds. VecHashJoinNode emits
            two contiguous blocks; the merged schema must match that, so a
            slot-sorted "canonical" column order is NOT available.

BREAKS (3)  the relation at the bottom of the left spine is binder slot 0
BREAKS (4)  the k-th join on the spine adds binder slot k
```

(3) and (4) are the same assumption in two costumes, and the four places that
hold it are:

| Site | What it assumes | What happens when it stops being true |
|---|---|---|
| `leftKeyIndices()` (vec builder) | `JoinKey::from_slot` resolves against `children[0]`'s **physical** schema | The bottom join's left child is a *leaf*, whose own schema stamps every column slot 0. A `from_slot` of 3 misses and the function **throws** — Week 27 made a miss loud on purpose. Task 3 §2 |
| `LogicalPlanBuilder::build`'s merged schema | the left block needs no `relation_slot` stamp | It only "needs none" because relation 0's columns already carry 0. Put relation 3 at the bottom and every reference from above (`SELECT`, `WHERE` residual, `GROUP BY`) that carries binder slot 3 fails to resolve slot-first. Task 3 §2 |
| `ChunkPruner`'s `relation_slot < 1` | a `slot < 1` ref reaching a scan hint belongs to the scanned table | The `pruning_where` hint travels down `children[0]` to the leftmost scan. If that scan is relation 2's table and the hint holds a slot-0 ref, a chunk gets pruned on **another table's** value. Task 5 §3 |
| `CardinalityEstimator`'s JOIN merge | the left context's entries are already in merged-schema numbering | A leaf context stamps slot 0. Merged above a non-zero leftmost relation, every later key lookup on that relation misses, `have_ndv` goes false and the estimate quietly degrades. Task 1 §3 |

Only one of the four is currently *reachable* as a wrong answer, and none is
reachable today at all — which is exactly why they are dangerous. They are
load-bearing assumptions with no test holding them up.

## Where the pass goes in the pipeline

`src/cli/main.cc`, inside the existing optimizer block:

```
LogicalPlanBuilder::build          (written-order left-deep fold, unchanged)
  ↓
PredicatePushdown::apply           (Week 21/26)
  ↓
JoinEnumeration::apply             ← NEW
  ↓
CardinalityEstimator::estimate     (Week 20)
  ↓
VectorizedPlanBuilder::build       (Week 18/22/23.5/27)
```

Both boundaries are forced, and you should be able to defend each:

**After pushdown.** Enumeration ranks orderings by the size of each relation's
contribution. Before pushdown every relation's filters sit in one `LogicalFilter`
*above* the whole join tree, so every leaf costs as its raw table size and the
search would order on base cardinalities — choosing wrong on exactly the queries
the optimizer exists for. The README's own Week 23 measurement is the evidence:
the 2.23× came from "pushdown + build-side + algorithm selection pay together",
and the filtered join is the row where they do. Post-pushdown, each leaf is a
self-contained `Scan` or `Filter(Scan)` subtree whose `estimated_rows` is the
count *after* its local `WHERE` — which is the number ordering must use.

**Before estimation.** `CardinalityEstimator::estimate` stamps `estimated_rows`
on the tree, and three consumers read those stamps: `--explain`'s *Optimized
Logical Plan* section, `VectorizedPlanBuilder`'s build-side decision
(`estimate_driven`), and `EXPLAIN ANALYZE`'s `est=` column. If enumeration ran
after, every stamp would describe a tree that no longer exists. Running the
estimator on the *final* shape also means the DP's internal numbers and the
printed ones come from one function — Task 1's whole point.

The estimator is pure (it reads catalog stats and stamps), so running it on the
leaf subtrees inside enumeration and again on the whole tree afterwards is
idempotent and costs microseconds.

## What not to build

Rule 2 of `CLAUDE.md`: *minimum code that solves the problem, nothing
speculative.* Four things are tempting this week and all four are out of scope.
Say so in the commit message so the next week does not think they were
forgotten.

1. **Bushy trees.** Left-deep only. `LogicalJoin::children[1]` being one
   relation is an invariant three files depend on; a bushy DP is a different
   search *and* a different lowering path.
2. **Attaching two-relation residuals to the lowest legal join.** The Starting
   note's precondition ("until the ordering exists") is now met, so this is a
   judgement call, not an impossibility. Defer it, and here is the tradeoff to
   state out loud: it would need the residual re-type-checked against an
   intermediate schema it was never checked against, it changes *where a
   predicate is evaluated* — which is precisely the semantics Week 29's outer
   join redefines (`join_condition.h`'s marker) — and it moves the checkpoint's
   needle not at all. Landing it now means landing it twice. Leave a one-line
   comment at `soleSlot()`'s definition recording that the blocker is gone.
3. **Reordering two-relation joins.** Make the pass a no-op below three
   relations. With one join there is no ordering decision: the hash join is
   symmetric and which side builds is Week 22's decision, made at lowering from
   the same estimates. Reordering two relations would change merged-schema
   column order and the `build=` explain text for exactly zero modelled gain,
   while putting every Week 22 / 23.5 steering assertion and every
   `test_vec_plan_builder.cc` schema-order assertion at risk. This single line
   removes most of the week's blast radius.
4. **A CLI flag for the DP limit.** "Configurable" is satisfied by a named
   `constexpr` in one header, which is how every other tuning constant in this
   project is expressed (`CPU_SIMD_COMPARE`, `MEM_PER_BYTE`, `BATCH_SIZE`). A
   flag is a knob with no consumer; if Week 37's benchmarking wants one, it can
   add it against a real measurement.

---

## Task 1 — One cardinality rule, shared by the search and the stamp

### Why it matters

The DP evaluates a candidate ordering by estimating how many rows each
intermediate join produces. `CardinalityEstimator::estimateNode`'s `JOIN` case
already computes exactly that number, and its comments record two separate
Week 26 corrections to how it does it:

- the left-side lookup is **slot-exact** (`findForRef`), because a merged
  context can hold one column name at several slots and a bare-name fallback
  would hand back a different relation's NDV with no signal;
- `have_ndv` is tracked separately from the product, because an NDV of **1** is
  a perfectly usable statistic (every left row matches every right row) while
  leaving `divisor == 1.0`. Testing `divisor > 1.0` instead sent that case to
  the no-statistics fallback and underestimated a constant-key join by the table
  size.

If the DP re-implements this arithmetic, you get a third copy of a rule that has
already been wrong twice, and the failure mode is the worst kind: the search
picks an order using one cardinality model while `--explain` prints another, so
the plan looks unjustifiable and the disagreement is invisible until someone
hand-computes both. This is the same reasoning that produced
`src/execution/key_encoding.h` in Week 27 ("every one of those properties was
violated by an operator that restated the rules locally") and the same reasoning
that promoted `collectSlots` to a shared walker rather than accept an eleventh
dispatch site.

Downstream: Task 4's DP calls this; Task 6's `written=` cost comparison calls
this; the final `CardinalityEstimator::estimate` pass calls this. Three callers,
one rule.

### Conceptual explanation

Two things need extracting from `estimateNode`, and one thing needs adding.

**(a) The join-cardinality formula, as a free function.** Its inputs are: the
two inputs' row counts, the equi-join keys *oriented* so that `from_col` /
`from_slot` address the left input and `join_col` addresses the right, and the
two `StatsContext`s. Its output is a row estimate. That is a pure function —
lift it out unchanged and have `estimateNode`'s `JOIN` case call it.

**(b) Per-subtree estimation, callable from outside.** Enumeration needs each
leaf's `estimated_rows` *and* its `StatsContext`. `estimateNode` already returns
both (it stamps rows and returns the context) but it is private. Make it
reachable. The smallest change is a public wrapper with a name that says what it
is for.

**(c) The leftmost-leaf restamp.** Under enumeration the leftmost relation is
not necessarily slot 0, and a leaf's `StatsContext` stamps its entries with
`col.relation_slot`, which for any standalone scan is 0. When the `JOIN` case
merges contexts it restamps only the *right* side (to `join.join_slot`). That is
correct for a written-order tree, where the left block genuinely is slot 0. Under
enumeration, the left block's true identity is on the merged schema's first
column. One added restamp fixes it, and it is a no-op in written order.

Note the ordering discipline inside the `JOIN` case: **key lookups happen before
the merge restamp.** At the bottom join, `from_slot` is 0 and the leaf context is
stamped 0, so the lookup hits — that is the pairing Task 3 §2 makes deliberate.
The restamp is a merge-time concern only. Do not hoist it.

### Code

```cpp
// src/planner/cardinality_estimator.h  — add near the class

// Estimated output rows of an inner equi-join.
//
// `keys` are ORIENTED: from_col/from_slot address `left`, join_col addresses
// `right`. `left` may be a merged context holding one column name at several
// slots, which is why the left lookup is slot-EXACT (findForRef) — honouring the
// slot only when it happens to hit would make the disambiguation advisory.
// `right` is always exactly one relation (left-deep), so a bare-name match there
// is unambiguous and -1 asks for it deliberately.
//
// Shared by CardinalityEstimator's JOIN case and Week 28's join enumeration.
// The two MUST NOT hold separate copies: the search would then rank orderings
// under one model while --explain prints another, and the NDV rule this encodes
// was already corrected twice in Week 26 (the slot-exact left lookup, and
// have_ndv tracked separately from the product so an NDV of 1 stays a usable
// statistic instead of falling through to the no-stats branch).
double joinCardinality(double left_rows, double right_rows,
                       const std::vector<JoinKey>& keys,
                       const StatsContext& left, const StatsContext& right);

class CardinalityEstimator {
    public:
        static void estimate(LogicalPlanNode& root, const Catalog& catalog);
        static double selectivity(const Expr* pred, const StatsContext& ctx);

        // Estimate ONE subtree in isolation: stamps estimated_rows bottom-up and
        // returns the column statistics visible above it. Week 28's enumeration
        // needs both for every leaf of the join graph before it can cost an
        // ordering. Same function `estimate` drives, so a leaf costed here and
        // the same leaf stamped by the final whole-tree pass cannot disagree.
        static StatsContext estimateSubtree(LogicalPlanNode& node, const Catalog& catalog);

    private:
        static StatsContext estimateNode(LogicalPlanNode& node, const Catalog& catalog);
};
```

```cpp
// src/planner/cardinality_estimator.cc

double joinCardinality(double left_rows, double right_rows,
                       const std::vector<JoinKey>& keys,
                       const StatsContext& left, const StatsContext& right) {
    // body moved VERBATIM out of estimateNode's JOIN case — divisor/have_ndv
    // loop, the FK-like max() fallback, and the >=1-row floor. Moving it
    // unchanged is the point: no behaviour delta, so every existing
    // test_cardinality.cc expectation must still pass byte-for-byte.
    double divisor = 1.0;
    bool have_ndv = false;
    for (const JoinKey& k : keys) {
        const ColumnStatsEntry* lk = left.findForRef(k.from_col, k.from_slot);
        const ColumnStatsEntry* rk = right.find(k.join_col, -1);
        int64_t ndv = std::max(lk ? lk->stats->distinct_count : int64_t(0),
                               rk ? rk->stats->distinct_count : int64_t(0));
        if (ndv > 0) { divisor *= static_cast<double>(ndv); have_ndv = true; }
    }
    double rows = have_ndv ? (left_rows * right_rows) / divisor
                           : std::max(left_rows, right_rows);
    if (left_rows >= 1.0 && right_rows >= 1.0) rows = std::max(rows, 1.0);
    return rows;
}

StatsContext CardinalityEstimator::estimateSubtree(LogicalPlanNode& node,
                                                   const Catalog& catalog) {
    return estimateNode(node, catalog);
}
```

and the `JOIN` case shrinks to:

```cpp
        case LogicalNodeType::JOIN: {
            auto& join = static_cast<LogicalJoin&>(node);
            StatsContext left  = estimateNode(*node.children[0], catalog);
            StatsContext right = estimateNode(*node.children[1], catalog);
            node.estimated_rows = joinCardinality(node.children[0]->estimated_rows,
                                                  node.children[1]->estimated_rows,
                                                  join.keys, left, right);

            // merge contexts [left ++ added relation], restamping each block to
            // the slot the MERGED SCHEMA gives it — must stay in lockstep with
            // the merged-schema construction (LogicalPlanBuilder::build, and
            // JoinEnumeration's rebuild).
            StatsContext out = std::move(left);
            // Week 28: a leaf's own context stamps slot 0, because a standalone
            // scan has one relation and nothing to disambiguate. That is the
            // leftmost relation's real identity only while the leftmost relation
            // IS slot 0 — which join enumeration no longer guarantees. Read the
            // truth off the merged schema's first column. No-op in written order.
            if (node.children[0]->type != LogicalNodeType::JOIN &&
                node.output_schema.size() > 0) {
                const int left_slot = node.output_schema.column(0).relation_slot;
                for (ColumnStatsEntry& e : out.entries) e.relation_slot = left_slot;
            }
            for (ColumnStatsEntry e : right.entries) {
                e.relation_slot = join.join_slot;
                out.entries.push_back(std::move(e));
            }
            return out;
        }
```

### Implementation guidance

1. Move the formula **verbatim**. Resist "tidying" the `have_ndv` logic on the
   way past; both of its subtleties are load-bearing and both are documented in
   place. Carry the comments across with the code — they are the record of the
   two corrections.
2. `joinCardinality` needs `JoinKey`, which arrives via
   `planner/logical_plan.h` → `planner/join_condition.h`. No new include.
3. Keep the `>= 1.0` floor **inside** `joinCardinality`. The DP must see the
   same floored number the stamp will show, or a zero-row candidate ranks
   infinitely better than it will actually be.
4. `estimateSubtree` is a one-line wrapper, not a copy. Do not make
   `estimateNode` public — the wrapper's name documents the contract ("one
   subtree, in isolation") and gives you a place to hang that comment.
5. **Gotcha:** the left-side restamp must run *after* the key lookups, which it
   does if you leave it where the merge already was. If you hoist it above the
   `joinCardinality` call to "keep the stamping together", the bottom join's
   `from_slot == 0` lookup starts missing and every multi-way estimate silently
   degrades. Nothing fails; the numbers just get worse.

### Verification

```bash
cmake --build build -j$(nproc)
cd build && ./tests/swiftql_tests --gtest_filter='Cardinality*'
```

Every existing `test_cardinality.cc` expectation must pass **unchanged**. This
task is a pure refactor; a single changed number means the move was not verbatim.

Add two tests to `tests/test_cardinality.cc`:

- `JoinCardinalityMatchesTheJoinNodeEstimate` — build a two-relation logical
  plan, run `CardinalityEstimator::estimate`, then call `joinCardinality`
  directly with the same inputs and assert the two agree exactly. This is the
  anti-drift test; it is the reason the function was extracted.
- `LeftmostLeafContextTakesTheMergedSchemaSlot` — hand-build a `LogicalJoin`
  whose merged schema stamps the left block with slot 2, estimate it, and assert
  a key lookup against the returned context at slot 2 resolves. Fails before the
  restamp, passes after. Note it must be hand-built: no *query* can produce this
  shape until Task 3 lands, which is exactly why it needs a unit test rather
  than an end-to-end one.

---

## Task 2 — The data-volume cost term, and a real per-relation row width

### Why it matters

Week 22 wrote down a promissory note and Week 27 restated it:

> *"`rowWidth` returns `columns * 8.0` for any multi-relation input, which is
> exactly the case enumeration compares — differing intermediate widths across
> orderings are the whole reason the term discriminates. Land the real
> per-relation width sum with the enumeration, not before."*

Two distinct gaps hide in that sentence.

**Gap A — there is no cost for materializing output.** `hashJoinCost` charges
for building the hash table (`CPU_HASH_BUILD` per build row), for probing
(`CPU_HASH_PROBE` per probe row), and for the hash table's memory
(`MEM_PER_BYTE` per build-side byte). Nothing charges for the rows the join
*emits*. Look at `VecHashJoinNode::nextChunk`: every matched pair builds a whole
`Row` by value and pushes it into `output_buffer_`, then `fillOutChunk` copies it
again into a `DataChunk`. That is the dominant cost of a join that fans out, and
it is precisely the cost join ordering exists to minimise — the entire point of
joining the selective pair first is to never materialise the big intermediate.
A DP blind to it will happily pick an order that produces a five-million-row
intermediate because the CPU terms only count *inputs*.

**Gap B — a join subtree's row width is a guess.** `rowWidth` falls back to
`columns * 8.0` whenever `isSingleRelation()` is false. Week 27 chose that
deliberately and said why: `leafScanTable()` returns *relation 0's* table for a
whole subtree, so looking the columns up in its stats attributes one table's
widths to another table's columns wherever a name is shared — "wrong in a
plausible direction, the hardest kind to notice". The fallback was the honest
choice while nothing consumed it. Now the DP compares orderings whose whole
difference is intermediate width, so the placeholder *is* the measurement.

Downstream: Task 4's DP consumes both. `VectorizedPlanBuilder`'s build-side
decision consumes Gap B's fix for the top join of every multi-way plan — today
that decision is made on a proxy width.

### Conceptual explanation

**The output term.** Model it as bytes materialised:

```
outputCost = output_rows × output_width × CPU_MATERIALIZE_BYTE
```

where `output_width` is the sum of both inputs' widths (the joined row carries
every column of both). It belongs to *whatever* algorithm runs, because both
`VecHashJoinNode` and `VecSimdLoopJoinNode` materialise output ("output is
materialized, as with the hash join" — README, Week 23.5).

**Where it must NOT go: inside `hashJoinCost`.** Two reasons, and both matter.

1. It is **symmetric under a build-side swap** — output rows and output width do
   not depend on which input builds. Adding it to `hashJoinCost` would add the
   same constant to both sides of every Week 22 comparison, changing no
   decision, while inflating the `cost=` number `--explain` prints for every
   single-join query in the project. That is Week 22's own argument for why the
   term "cannot change the decision and is not testable here".
2. `CPU_SIMD_COMPARE = 0.02` is **calibrated against measured hardware** so the
   modelled hash/SIMD crossover matches the real one at ≈50 build rows
   (`docs/hash-vs-simd-crossover.md`). Perturbing either cost function
   invalidates that calibration. A separate additive term, applied by the caller
   that needs it, leaves the calibration intact.

So: a new free function in `cost_model.h`, called by the enumerator only.

**Choosing the constant.** Do not pick a number and move on — derive it and
record the derivation in the header, the way `CPU_SIMD_COMPARE` records its
measurement. A defensible anchor: materialising one joined row is roughly the
same order of work as the probe that produced it, and rows in this schema are on
the order of 40 bytes. `CPU_HASH_PROBE` is 1.0, so

```
CPU_MATERIALIZE_BYTE = 1.0 / 40 = 0.025
```

States plainly: *a 40-byte output row costs the same as one probe*. If Week 37's
profiling says otherwise, the constant moves and the derivation is there to
argue with. What you must not do is leave it unjustified — costs are unitless
and comparable only to each other, so an arbitrary magnitude here silently
re-weights the whole search.

**The width sum.** Every column of a merged join schema carries the binder slot
of the relation it came from (invariant 2, and Task 3 makes it true for the left
block too). So given a slot→table map, each column's `avg_width` can be read from
*its own* table's stats. The map comes off the left spine: at each `LogicalJoin`,
`children[1]` is exactly relation `join_slot`, and the leftmost block's slot is
stamped on the merged schema's first column.

Inside the DP you do not need the walker at all — the DP knows its subset, so
`width(S) = Σ_{r ∈ S} width(leaf_r)`, each term from the *existing*
single-relation `rowWidth`. The walker is for `VectorizedPlanBuilder`, where the
tree is all that is left.

### Code

```cpp
// src/planner/cost_model.h  — append

// Week 28 — data-volume (bytes-materialized) term. Deferred here from Week 22,
// which noted it "cannot change the decision" at single-join scope: output rows
// and output width are invariant under a build-side swap, so the term cancels in
// every Week 22 comparison. It first discriminates in Week 28, where two join
// ORDERINGS produce different intermediate results.
//
// Deliberately NOT folded into hashJoinCost/simdLoopJoinCost. Those two are
// compared against each other, and CPU_SIMD_COMPARE is calibrated against
// measured on-device crossover (docs/hash-vs-simd-crossover.md); adding a term
// to both changes no decision but invalidates the calibration's provenance and
// inflates every cost= string --explain has printed since Week 23.
//
// Derivation: both join operators materialize every output row (VecHashJoinNode
// builds a Row per match, then copies it into a DataChunk). Anchor the weight so
// that materializing one ~40-byte row costs the same as probing one row
// (CPU_HASH_PROBE = 1.0): 1.0 / 40 = 0.025. Re-derive from profiling in Week 37
// if TPC-H orderings disagree with measurement.
constexpr double CPU_MATERIALIZE_BYTE = 0.025;

// cost of emitting `output_rows` rows of `output_width` bytes each. Applied by
// join enumeration once per join in a candidate ordering; symmetric under a
// build-side swap, which is why it lives outside the two algorithm costs.
double joinOutputCost(double output_rows, double output_width);
```

```cpp
// src/planner/cost_model.cc
double joinOutputCost(double output_rows, double output_width) {
    output_rows  = std::max(output_rows, 0.0);    // same clamps as the two above
    output_width = std::max(output_width, 0.0);
    return output_rows * output_width * CPU_MATERIALIZE_BYTE;
}
```

```cpp
// src/planner/vectorized_plan_builder.cc — replace rowWidth's multi-relation arm

// slot -> table for every relation in a join subtree. children[1] of each join
// IS relation join_slot; the leftmost block's slot is stamped on the merged
// schema's first column, which is the only place it is recorded once join
// enumeration may put a relation other than 0 at the bottom of the spine.
void collectSlotTables(const LogicalPlanNode* node,
                       std::unordered_map<int, std::string>& out) {
    if (node->type != LogicalNodeType::JOIN) return;
    const auto* join = static_cast<const LogicalJoin*>(node);
    out[join->join_slot] = leafScanTable(join->children[1].get());
    const LogicalPlanNode* left = join->children[0].get();
    if (isSingleRelation(left)) {
        // A join always carries at least its own key columns per side, so the
        // merged schema cannot be empty — but read defensively: an empty schema
        // here would index out of bounds rather than lose a width.
        if (join->output_schema.size() > 0)
            out[join->output_schema.column(0).relation_slot] = leafScanTable(left);
        return;
    }
    collectSlotTables(left, out);
}

double rowWidth(const LogicalPlanNode* child, const Catalog& catalog) {
    if (isSingleRelation(child)) {
        const std::string& table = leafScanTable(child);
        if (!catalog.hasStats(table)) return child->output_schema.size() * 8.0;
        const TableStats& ts = catalog.getStats(table);
        double width = 0.0;
        for (const auto& col : child->output_schema.columns()) {
            auto it = ts.columns.find(col.name);
            width += (it != ts.columns.end()) ? it->second.avg_width : 8.0;
        }
        return width;
    }

    // Week 28: multi-relation input. Every column of a merged join schema
    // carries the binder slot of the relation it came from, so each avg_width
    // comes from ITS OWN table instead of relation 0's — the attribution error
    // Week 27 refused to make, and the reason it fell back to columns * 8.0.
    // That fallback is now the thing being measured (differing intermediate
    // widths across orderings are what the data-volume term discriminates on),
    // so the real sum lands with the enumeration that consumes it.
    std::unordered_map<int, std::string> slot_tables;
    collectSlotTables(child, slot_tables);
    double width = 0.0;
    for (const ColumnDef& col : child->output_schema.columns()) {
        auto t = slot_tables.find(col.relation_slot);
        if (t == slot_tables.end() || !catalog.hasStats(t->second)) {
            width += 8.0;                 // documented uniform proxy, per column
            continue;
        }
        const TableStats& ts = catalog.getStats(t->second);
        auto it = ts.columns.find(col.name);
        width += (it != ts.columns.end()) ? it->second.avg_width : 8.0;
    }
    return width;
}
```

### Implementation guidance

1. Do the `cost_model` half first and unit-test it before touching `rowWidth` —
   the two are independent and the cost function is trivially testable.
2. **Do not** change `hashJoinCost` or `simdLoopJoinCost`, their constants, or
   their signatures. If a Week 22 or Week 23.5 test changes behaviour, you have
   perturbed the calibration; back out and re-read the "where it must NOT go"
   argument.
3. `rowWidth`'s single-relation arm is untouched — extract it or leave it
   inline, but do not rewrite it. Every existing width-driven decision must keep
   its current number.
4. `collectSlotTables` is a *file-local* helper in the anonymous namespace next
   to `leafScanTable` and `isSingleRelation`. Do not export it; there is one
   consumer.
5. **Gotcha:** the fallback is per *column* (`+= 8.0`), not per subtree. A
   subtree with two relations where only one has stats must charge real widths
   for the columns it knows and 8.0 for the rest — falling back to
   `size() * 8.0` for the whole subtree throws away information you have.
6. **Gotcha:** `slot_tables` is keyed on `relation_slot`, which for a *self*-join
   distinguishes the two occurrences (slots 1 and 2, same table name). Both map
   to the same table string. That is correct and intentional — do not key on the
   table name.

### Verification

Unit tests in `tests/test_cost_model.cc`:

- `OutputCostIsSymmetricUnderBuildSideSwap` — `joinOutputCost` takes no side
  argument; assert that the *total* cost of a join
  (`hashJoinCost(a,wa,b) + joinOutputCost(n,w)` vs
  `hashJoinCost(b,wb,a) + joinOutputCost(n,w)`) differs by exactly the same
  delta as the two `hashJoinCost` calls alone. This is the test that pins the
  argument for why the term lives outside.
- `OutputCostGrowsWithWidth` — same rows, wider output, strictly greater cost.
- `OutputCostZeroRowsIsFiniteAndNonNegative` — matches the shape of the three
  existing zero-row tests.

Width, in `tests/test_vec_plan_builder.cc` (needs a real catalog with stats):

- `MultiRelationRowWidthSumsPerRelationStats` — build a three-relation plan,
  call the builder, and assert the top join's `build=`/`cost=` string differs
  from what the `columns * 8.0` proxy would produce. `laps` and `drivers` both
  have a `team` column of different average width, which is exactly the
  attribution error the old code made — pick a query where both appear.

End-to-end sanity (no assertion, read it):

```bash
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized \
  --no-cache --explain --query \
  "SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id \
   JOIN drivers d2 ON d.team = d2.team"
```

The Physical Plan's top join should now print a `cost=` derived from real
widths. Single-join queries must print exactly what they printed before —
diff a couple against `git stash` output if unsure.

---

## Task 3 — Decompose the join tree, and rebuild it in a chosen order

### Why it matters

This is the week's structural change and the one that can return wrong rows.
Everything else either fails loudly or only mis-ranks a plan.

Two operations, inverse to each other:

- **decompose** a written-order left-deep tree into *N leaf subtrees* + *an
  undirected edge list*, and
- **rebuild** a left-deep tree from a permutation of those relations.

Rebuild is where `LogicalPlanBuilder::build`'s fold gets re-derived for an
arbitrary order, and it is where the two broken identities are repaired: the
merged schema's slot stamping and `JoinKey::from_slot`'s meaning at the bottom
join. Get either wrong and you get plausible rows — Week 27's own worst defect
was of exactly this kind (31440 rows by name versus 32193 by slot, no error
either way).

Downstream: Task 4 searches over what this produces; Task 5 wires it; Task 6
prints it. The correctness of the whole week reduces to one theorem, stated
below.

### Conceptual explanation

**The graph is already in the tree.** `LogicalJoin::keys` is the edge list and
`join_slot` is the vertex id (the Starting note says so). Each `JoinKey`
`{from_col, join_col, from_slot}` on a join with `join_slot == b` is an edge
between relation `from_slot` and relation `b`, carrying one column name at each
end. Direction is an artifact of the written order, so record edges
direction-free and orient them at rebuild time.

**Decompose only ever sees a written-order tree.** This removes an ambiguity you
would otherwise have to solve. `JoinEnumeration::apply` runs exactly once, on
the tree `LogicalPlanBuilder::build` produced, so at decompose time
`joins[i] → slot i+1` still holds, the leftmost relation is slot 0, and every
`from_slot` is a true binder slot. Assert it rather than infer it.

**The correctness theorem.** An inner equi-join is associative and commutative,
so any order computes the same relation *provided*:

1. **every edge is used exactly once.** At the step relation `r` is added, take
   exactly the edges incident to `r` whose other endpoint is already placed.
   Each edge is consumed at the step its later-in-order endpoint arrives.
   Summing over steps: every edge, once.
2. **no step is a cross product.** Only add `r` if it has at least one edge to
   the placed set. SwiftQL has no cross-product operator (README, *Syntax
   Deliberately Not Supported*), so a disconnected step is not merely expensive,
   it is unbuildable. The original graph is connected by construction — every
   `ON` clause must yield at least one key linking its relation to a preceding
   one — so a connected order always exists.
3. **residuals are untouched.** They already sit in the `WHERE` conjunction and
   were placed by pushdown. Reordering neither creates nor destroys a column
   they reference: the top join's output holds every relation's columns under
   any order.

**Slot stamping — the crux.** Two numbering domains, and the boundary is the
first join:

| Schema | Stamps | Why |
|---|---|---|
| A leaf's own `output_schema` (scan, or filter over scan) | every column slot **0** | A standalone scan has one relation and nothing to disambiguate. Pushed conjuncts were restamped to 0 by `distribute()` and `ChunkPruner` reads `slot < 1` as scan-local. Do not touch this |
| A `LogicalJoin`'s merged schema | each column its relation's **binder slot** | References from above (`SELECT`, residual `WHERE`, `GROUP BY`, later joins' keys) carry binder slots and resolve slot-first |

In written order the left block of the *first* join is relation 0, whose columns
already carry 0, so `LogicalPlanBuilder` never had to stamp it. Under
enumeration it must. Every later join's left input is a merged schema and is
already correct.

**`JoinKey::from_slot` — the consequence.** Three consumers resolve `from_col`
against the *left child's own schema*: `leftKeyIndices()` (physical, throws on a
miss), `LogicalJoin::explain()` (logical) and `joinCardinality()` (the left
`StatsContext`). At the first join that schema is a **leaf**, stamping 0. So:

```
JoinKey::from_slot is the relation slot AS PRESENTED BY THE LEFT CHILD'S
OWN SCHEMA:  0 when the left child is a single relation,
             the binder slot when it is a join subtree.
```

Today those coincide, which is why the rule has never been written down. State
it in `join_condition.h` and set `from_slot = 0` on the first join's keys. This
is unambiguous — there is exactly one relation on the left there, so slot 0
cannot select the wrong column.

The tempting alternative — restamping the leftmost leaf's *own* schema to its
binder slot — is worse: the pushed filter below it carries slot-0 refs, which
would then resolve only by bare-name fallback, and bare-name fallback is the
mechanism Week 27 spent a week removing.

**Key field order in the composite tuple.** Re-orienting an edge swaps which
column is `from_col` and which is `join_col`, and a different order also changes
*how many* keys a given join carries (a triangle gives the last-added relation
two edges, whichever relation that is). `serializeKey` walks `key_idx` in vector
order and `leftKeyIndices` / `rightKeyIndices` are both built from the same
`keys` vector, so the two sides always agree *within one plan* — which is all
injectivity requires. The encoding contract in `key_encoding.h` is untouched and
must stay untouched. Note the one real consequence: an ordering can turn a
one-key join into a two-key join, and `int_keys` in the vec builder gates SIMD
on `keys.size() == 1`. Task 4 addresses that as a costing approximation.

### Code

```cpp
// src/planner/join_enumeration.cc — anonymous namespace

// One relation of the join graph: its post-pushdown subtree plus the four
// numbers the search runs on.
struct Relation {
    int slot = -1;                              // binder range-table position
    std::unique_ptr<LogicalPlanNode> subtree;   // Scan, or Filter(s) over Scan
    StatsContext ctx;                           // entries stamped `slot`
    double rows = 0.0;
    double width = 0.0;                         // bytes/row
};

// One equi-join edge, direction-free. Rebuild orients it.
struct Edge {
    int slot_a; std::string col_a;
    int slot_b; std::string col_b;
};

// Walk the left spine of a WRITTEN-ORDER tree, moving each relation's subtree
// out and recording every key as an edge. Only ever called on the tree
// LogicalPlanBuilder::build produced, so joins[i] -> slot i+1 still holds: the
// leftmost relation is slot 0 and every from_slot is a true binder slot.
// `leaves` is indexed BY SLOT, not by spine position — that is the whole point.
void decompose(std::unique_ptr<LogicalPlanNode> node,
               std::vector<std::unique_ptr<LogicalPlanNode>>& leaves,
               std::vector<Edge>& edges) {
    if (node->type != LogicalNodeType::JOIN) {
        leaves[0] = std::move(node);   // bottom of the spine == relation 0
        return;
    }
    auto* join = static_cast<LogicalJoin*>(node.get());
    for (const JoinKey& k : join->keys)
        edges.push_back(Edge{k.from_slot, k.from_col, join->join_slot, k.join_col});
    leaves[join->join_slot] = std::move(join->children[1]);
    decompose(std::move(join->children[0]), leaves, edges);
}

// Fold `order` into a left-deep tree, re-deriving each join's keys and merged
// schema for the order actually chosen.
std::unique_ptr<LogicalPlanNode> rebuild(const std::vector<int>& order,
                                         std::vector<Relation>& rels,
                                         const std::vector<Edge>& edges) {
    std::unordered_set<int> placed{order[0]};
    std::unique_ptr<LogicalPlanNode> node = std::move(rels[order[0]].subtree);

    // The leftmost leaf's OWN schema stamps every column slot 0 (a standalone
    // scan has one relation and nothing to disambiguate; its pushed filter's
    // refs were restamped to 0, and ChunkPruner reads slot < 1 as scan-local).
    // The MERGED schema is where a relation acquires its binder slot. In written
    // order the leftmost relation IS slot 0, which is the only reason this loop
    // has never existed in LogicalPlanBuilder::build.
    std::vector<ColumnDef> merged = node->output_schema.columns();
    for (ColumnDef& c : merged) c.relation_slot = order[0];

    for (size_t k = 1; k < order.size(); ++k) {
        const int r = order[k];

        // Every edge incident to r whose other end is already placed — and
        // exactly those. Each edge is therefore consumed once, at the step its
        // later-in-order endpoint arrives, which is what makes the reordered
        // tree compute the same relation as the written one.
        std::vector<JoinKey> keys;
        for (const Edge& e : edges) {
            if (e.slot_b == r && placed.count(e.slot_a))
                keys.push_back(JoinKey{e.col_a, e.col_b, e.slot_a});
            else if (e.slot_a == r && placed.count(e.slot_b))
                keys.push_back(JoinKey{e.col_b, e.col_a, e.slot_b});
        }
        // Connectivity is the search's job; a miss here is a planner bug, and a
        // keyless LogicalJoin is a cross product SwiftQL has no operator for.
        if (keys.empty())
            throw std::runtime_error(
                "internal: join enumeration produced a cross product at relation slot "
                + std::to_string(r));

        // FIRST join only: the left input is a LEAF, whose own schema stamps
        // slot 0. leftKeyIndices() resolves from_col against that schema and
        // THROWS on a miss (Week 27, deliberately — the bare-name fallback IS
        // the bug). Slot 0 there is unambiguous: exactly one relation is
        // present. Every later join's left input is the merged schema below,
        // where binder slots are real. Same rule LogicalJoin::explain() and
        // joinCardinality() read.
        if (k == 1) for (JoinKey& key : keys) key.from_slot = 0;

        // merged schema: [left block] ++ [this relation's columns, stamped r].
        // Order matches VecHashJoinNode's two contiguous output blocks — a
        // slot-sorted "canonical" order is not available (invariant 1).
        for (ColumnDef c : rels[r].subtree->output_schema.columns()) {
            c.relation_slot = r;
            merged.push_back(c);
        }
        node = std::make_unique<LogicalJoin>(std::move(node),
                                             std::move(rels[r].subtree),
                                             std::move(keys), r, Schema(merged));
        placed.insert(r);
    }
    return node;
}
```

### Implementation guidance

1. **Sequence the week here.** Land Task 3 with the *identity permutation* wired
   in — `order = {0, 1, ..., N-1}` — before Task 4 exists. Rebuilding the
   written order must produce a byte-identical plan: same `--explain` output,
   same results, same tests. If it does not, the decompose/rebuild pair is
   wrong, and you will find that out with one moving part instead of two.
2. `leaves` is sized `N` and indexed by slot. Assert every entry is non-null
   after `decompose` — a null means a `join_slot` was outside `[1, N)`, which is
   a Week 26 invariant violation and should be loud.
3. `merged` is threaded through the loop as the running left block. Do not
   rebuild it from `node->output_schema` each iteration: for `k == 1` that
   schema is the *leaf's*, still stamped 0.
4. **Gotcha:** the `for (ColumnDef c : ...)` loop variable is **by value**. A
   reference mutates the join scan's own schema and destroys the leaf's slot-0
   stamping. `LogicalPlanBuilder::build` has the same by-value loop with the
   same comment; copy the comment.
5. **Gotcha:** `ColumnDef` carries `hidden` as well as `relation_slot`. Copying
   whole `ColumnDef`s preserves it; constructing `{name, type, slot}` does not.
6. **Gotcha:** do not touch `stmt`/`joins` or re-run `classifyJoinCondition`.
   The statement is gone by now — `LogicalPlanBuilder::build` consumed it. The
   tree is the only source of truth.
7. §5, verified not coded: after rebuild, the residual `WHERE` filter above the
   join tree is *untouched* and still references relations by binder slot. Read
   the `LogicalFilter` above the join and confirm you have not moved,
   re-conjoined or re-typed its predicate. The `join_condition.h` Week 29 marker
   stays exactly where it is.
8. Add the `from_slot` rule to `join_condition.h`'s `JoinKey` comment. It is now
   a contract with three consumers and it has never been written down.

### Verification

With the identity permutation wired in:

```bash
cd build && ./tests/swiftql_tests            # all 614 must pass, unchanged
cd .. && python3 python_tools/compare_against_sqlite.py   # 568 passed, 0 failed
```

Then a targeted structural test in a new `tests/test_join_enumeration.cc`:

- `IdentityOrderRebuildsAnIdenticalPlan` — build a three-relation logical plan,
  capture every node's `explain()` into a vector, run decompose+rebuild with the
  identity order, capture again, assert equal.
- `RebuildStampsTheLeftmostRelationsBinderSlot` — rebuild with order
  `{2, 0, 1}` and assert the top join's merged schema stamps its first column
  with slot 2 (and that the *leaf* subtree's own schema still stamps 0).
- `RebuildOrientsEdgesAndUsesEachExactlyOnce` — sum `keys.size()` over the
  rebuilt tree's joins and assert it equals the original edge count, for several
  permutations of a triangle graph.
- `RebuildSetsFirstJoinKeysToSlotZero` — order `{2, 0, 1}`, assert the bottom
  join's `keys[0].from_slot == 0` while the next join's is a real binder slot.

---

## Task 4 — The search: left-deep DP with a limit, and a greedy fallback

### Why it matters

This is the README bullet, and it is the smallest task of the week. The
structure it searches (Task 3) and the numbers it searches on (Tasks 1 and 2)
are the hard parts; the search itself is textbook System-R.

It matters *because* of what it makes measurable: with only `laps` (10k) and
`drivers` (20) in the shipped catalog, a three-relation query already has orders
whose modelled costs differ by two orders of magnitude. `FROM laps l JOIN laps
l2 ON l.driver_id = l2.driver_id JOIN drivers d ON l.driver_id = d.driver_id` is
a star centred on `l`. Written order joins the two 10k tables first, producing a
5,000,000-row intermediate, then joins 20 rows to it. Order `[l, d, l2]`
produces a 10,000-row intermediate first. Same final cardinality, ~125× the
modelled cost. That is the query Task 6 and Task 7 assert on.

### Conceptual explanation

**State.** A subset of relations, as a bitmask. `best[S]` = the cheapest
left-deep plan joining exactly the relations in `S`, recording: cumulative cost,
output rows, output width, the relation added last, and the predecessor subset
(for reconstructing the order).

**Transition.** `best[S ∪ {r}]` from `best[S]` and a single relation `r ∉ S`,
legal only if `r` is adjacent to `S`. Left-deep means the right input is always
a single relation, which is invariant 1 and also why the state space is
`O(2^N · N)` rather than the `O(3^N)` of a bushy DP.

**Cost of one transition:**

```
join_cost   = min( hashJoinCost(rows(S), width(S), rows(r)),
                   hashJoinCost(rows(r), width(r), rows(S)) )
out_rows    = joinCardinality(rows(S), rows(r), oriented_keys, ctx(S), ctx(r))
out_cost    = joinOutputCost(out_rows, width(S) + width(r))
cost(S∪{r}) = cost(S) + join_cost + out_cost
```

Taking the `min` over build sides mirrors what `VectorizedPlanBuilder` will
actually do at lowering, so the search does not penalise an order for a
build-side choice the lowering would not make.

**Two documented approximations.** State both in the header; do not pretend they
are not there.

1. **The DP costs the hash join only, never the SIMD loop join.** SIMD
   eligibility depends on key count and key type, and an ordering can change a
   join's key count (Task 3). Costing four algorithm/side combinations per
   transition to model a per-join decision that lowering will re-make anyway
   buys ordering accuracy only in the narrow band where SIMD wins — a build side
   under ~50 rows, where the *absolute* cost is negligible and cannot flip an
   ordering. Hash-only, documented.
2. **Independence.** `joinCardinality` divides by the NDV product, and
   `CardinalityEstimator`'s `FILTER` case already notes that column statistics
   are not narrowed by a filter. Errors compound multiplicatively across a
   spine. This is the standard System-R assumption and the honest position is to
   name it; histograms are a README "Possible Extension", not Week 28.

**Base case.** `best[{r}]` = cost 0, `rows = leaf_r->estimated_rows`,
`width = rowWidth(leaf_r)`. Cost 0 is right: the scan happens under every
ordering, so charging for it adds the same constant to every candidate.

**The limit and the fallback.** `O(2^N · N)`: N=10 is ~10k transitions
(microseconds), N=20 is ~20M (visible in the plan timer, for a query shape no
TPC-H query has — Q9 and Q21 top out at 6 relations). Cap at 10 and fall back to
greedy: start from the cheapest single relation, then repeatedly add the
connected relation minimising the transition cost. `O(N²)`, always connected,
never optimal, always legal.

**Determinism.** Ties must break deterministically or `--explain` output becomes
unstable across runs and the harness's steering assertion flaps. Iterate
relations in ascending slot order and use strict `<` when comparing costs, so
the first-found (lowest-slot, and for equal costs the written-order) candidate
wins.

### Code

```cpp
// src/planner/join_enumeration.h

#pragma once
#include "catalog/catalog.h"
#include "planner/logical_plan.h"
#include <memory>

// Week 28 — left-deep join ordering.
//
// Pipeline position is forced at both ends:
//   AFTER  PredicatePushdown — each relation's local filters must already sit on
//          its own scan, or every leaf costs as its raw table size and the search
//          orders on base cardinalities;
//   BEFORE CardinalityEstimator::estimate — which stamps the tree --explain
//          prints and VectorizedPlanBuilder costs, so it must see the final shape.
//
// Left-deep only. LogicalJoin::children[1] is always exactly one relation, an
// invariant rightKeyIndices(), rowWidth()'s isSingleRelation() split and the
// whole lowering path are written against.

// No-op below three relations: with one join there is no ordering decision.
// Which side builds is Week 22's decision, made at lowering from the same
// estimates, and reordering two relations would change merged-schema column
// order and every Week 22 / 23.5 steering assertion for zero modelled gain.
constexpr int MIN_ENUMERATED_RELATIONS = 3;

// Search-space cap. Left-deep DP is O(2^N * N): N=10 is ~10k transitions
// (microseconds), N=20 is ~20M (visible in the plan timer). Above the cap,
// greedy. A named constant rather than a CLI flag — nothing needs to vary it per
// query, and a flag would be a knob with no consumer (CLAUDE.md rule 2).
constexpr int MAX_DP_RELATIONS = 10;

class JoinEnumeration {
    public:
        // Reorders the single join tree in `node`, if any, and returns it.
        // Never called under --no-optimize (see main.cc).
        static std::unique_ptr<LogicalPlanNode> apply(std::unique_ptr<LogicalPlanNode> node,
                                                      const Catalog& catalog);
};
```

```cpp
// src/planner/join_enumeration.cc — the search

// best[S] for one subset
struct Sub {
    double cost = std::numeric_limits<double>::infinity();
    double rows = 0.0;
    double width = 0.0;
    int last = -1;          // relation added last
    uint32_t prev = 0;      // predecessor subset; walk back for the order
};

// Statistics visible above a subset: the concatenation of its relations' leaf
// contexts, each already stamped with its own binder slot. Cheap enough to
// rebuild per transition at N <= 10; caching it is a micro-optimization with no
// measurement behind it.
StatsContext contextFor(uint32_t s, const std::vector<Relation>& rels) {
    StatsContext ctx;
    for (const Relation& rel : rels) {
        if (!(s & (1u << rel.slot))) continue;
        ctx.entries.insert(ctx.entries.end(), rel.ctx.entries.begin(), rel.ctx.entries.end());
    }
    return ctx;
}

// Edges between relation r and the placed set, oriented left = placed.
// BINDER slots throughout: the search is pure arithmetic and never touches a
// physical schema, so the first-join from_slot = 0 rewrite is rebuild()'s
// concern alone.
std::vector<JoinKey> keysBetween(uint32_t placed, int r, const std::vector<Edge>& edges) {
    std::vector<JoinKey> keys;
    for (const Edge& e : edges) {
        if (e.slot_b == r && (placed & (1u << e.slot_a)))
            keys.push_back(JoinKey{e.col_a, e.col_b, e.slot_a});
        else if (e.slot_a == r && (placed & (1u << e.slot_b)))
            keys.push_back(JoinKey{e.col_b, e.col_a, e.slot_b});
    }
    return keys;
}

// One transition's cost and output shape. Build side is chosen by min(), which
// is what VectorizedPlanBuilder will do at lowering — so an order is not
// penalised for a build-side choice lowering would never make. The SIMD loop
// join is deliberately NOT costed here; see the header note on approximations.
struct Step { double cost; double rows; double width; };

Step stepCost(const Sub& s, uint32_t placed, const Relation& rel,
              const std::vector<Edge>& edges, const std::vector<Relation>& rels) {
    std::vector<JoinKey> keys = keysBetween(placed, rel.slot, edges);
    StatsContext left = contextFor(placed, rels);
    double rows  = joinCardinality(s.rows, rel.rows, keys, left, rel.ctx);
    double width = s.width + rel.width;
    double join_cost = std::min(hashJoinCost(s.rows, s.width, rel.rows),
                                hashJoinCost(rel.rows, rel.width, s.rows));
    return Step{s.cost + join_cost + joinOutputCost(rows, width), rows, width};
}

// Left-deep DP over subsets. Only CONNECTED extensions are legal: SwiftQL has no
// cross-product operator, so a disconnected step is unbuildable, not merely
// expensive. The original graph is connected by construction (every ON clause
// yields at least one key to a preceding relation), so an order always exists.
std::vector<int> enumerateDP(const std::vector<Relation>& rels,
                             const std::vector<Edge>& edges,
                             const std::vector<uint32_t>& adj) {
    const int n = static_cast<int>(rels.size());
    std::vector<Sub> best(1u << n);
    for (int r = 0; r < n; ++r) {
        Sub& b = best[1u << r];
        b.cost = 0.0; b.rows = rels[r].rows; b.width = rels[r].width; b.last = r;
    }

    for (uint32_t s = 1; s < (1u << n); ++s) {
        if (best[s].last < 0) continue;                  // unreachable subset
        for (int r = 0; r < n; ++r) {                    // ascending: deterministic ties
            if (s & (1u << r)) continue;
            if (!(adj[r] & s)) continue;                 // would be a cross product
            Step step = stepCost(best[s], s, rels[r], edges, rels);
            Sub& target = best[s | (1u << r)];
            if (step.cost < target.cost) {               // strict: first found wins ties
                target = Sub{step.cost, step.rows, step.width, r, s};
            }
        }
    }

    std::vector<int> order;
    for (uint32_t s = (1u << n) - 1; ;) {
        order.push_back(best[s].last);
        uint32_t p = best[s].prev;
        if (p == 0) break;
        s = p;
    }
    std::reverse(order.begin(), order.end());
    return order;
}
```

The greedy fallback is the same transition function without the subset table:

```cpp
// O(N^2) fallback above MAX_DP_RELATIONS. Never optimal, always connected,
// always legal — which is the only guarantee the tree builder needs.
std::vector<int> enumerateGreedy(const std::vector<Relation>& rels,
                                 const std::vector<Edge>& edges,
                                 const std::vector<uint32_t>& adj) {
    const int n = static_cast<int>(rels.size());
    int start = 0;
    for (int r = 1; r < n; ++r) if (rels[r].rows < rels[start].rows) start = r;

    std::vector<int> order{start};
    uint32_t placed = 1u << start;
    Sub cur{0.0, rels[start].rows, rels[start].width, start, 0};

    while (static_cast<int>(order.size()) < n) {
        int pick = -1; Step best_step{};
        for (int r = 0; r < n; ++r) {
            if (placed & (1u << r)) continue;
            if (!(adj[r] & placed)) continue;
            Step step = stepCost(cur, placed, rels[r], edges, rels);
            if (pick < 0 || step.cost < best_step.cost) { pick = r; best_step = step; }
        }
        if (pick < 0) break;   // disconnected graph: impossible by construction
        order.push_back(pick);
        placed |= 1u << pick;
        cur = Sub{best_step.cost, best_step.rows, best_step.width, pick, placed};
    }
    return order;
}
```

### Implementation guidance

1. `uint32_t` masks cap you at 32 relations; `MAX_DP_RELATIONS = 10` is well
   inside. Do not reach for `std::bitset` or a dynamic mask — nothing needs it.
2. `best[s].last < 0` is the "unreachable subset" test. A disconnected subset is
   never populated, and skipping it is what keeps the DP from costing cross
   products it would then have to reject at rebuild time.
3. `adj[r]` is a bitmask of `r`'s neighbours, built once from `edges` before the
   loop. Both endpoints of each edge get the other set.
4. **Gotcha — greedy's start.** Starting from the smallest relation is a
   heuristic, not a proof. It can strand you: a small relation on the periphery
   of a chain forces a bad second step. Acceptable for a fallback nothing under
   TPC-H reaches; do not silently "improve" it without a measurement.
5. **Gotcha — a subset with `prev == 0`.** The reconstruction loop must stop at
   a singleton, not at mask `0`. A singleton's `prev` is 0, which is why the
   break is after `push_back`.
6. **Gotcha — determinism.** Use strict `<` in both searches, and iterate
   relations ascending. With `<=` the *last* equal-cost candidate wins, ties
   break on iteration order, and the `--explain` line Task 7 asserts on becomes
   flaky.
7. Guard both entry points: fewer than `MIN_ENUMERATED_RELATIONS` returns the
   tree unchanged before any of this runs.

### Verification

`tests/test_join_enumeration.cc`:

- `DpPicksTheCheapOrderOnAStarGraph` — three hand-built relations (10k, 10k, 20)
  with a star topology and a shared key NDV of 20; assert the order puts the
  20-row relation second, and that its modelled cost is strictly below the
  written order's.
- `DpRefusesDisconnectedExtensions` — a chain A–B–C; assert no returned order
  ever places C before B.
- `GreedyAndDpAgreeOnThreeRelations` — the two searches must coincide on a graph
  small enough for greedy to be optimal. Cheap regression on the shared
  transition function.
- `AboveTheLimitFallsBackToGreedy` — construct `MAX_DP_RELATIONS + 1` relations
  and assert the call returns a legal connected order promptly. Assert
  legality, not optimality.
- `FewerThanThreeRelationsIsANoOp` — the single-join tree comes back pointer-
  identical (or at least explain-identical). This is the guard that protects
  every Week 22/23.5 assertion.

---

## Task 5 — Wiring, `--no-optimize`, and the pruning-hint positional assumption

### Why it matters

Three small edits, one of which prevents a wrong answer.

`--no-optimize` is not a debug flag in this project. It is the benchmark
baseline (README's *Optimizer Impact* table), the differential oracle
(`compare_against_sqlite.py` runs the vectorized suite twice), and the
result-preservation invariant (invariant 11). A reordering that only happens
with the optimizer on is exactly what makes the second run able to catch it. If
enumeration also ran under `--no-optimize`, the two runs would agree by
construction and the oracle would be worthless.

The pruning hint is the wrong-answer one. `ChunkPruner` treats a `relation_slot
< 1` reference in a scan's hint as scan-local, and `chunk_pruner.h` says why
that is safe: pushed conjuncts arrive restamped to 0, and "a slot >= 1 ref
reaching a scan hint can only be a residual … routed to the FROM-side scan". The
unstated half of that argument is *the FROM-side scan is relation 0*. Enumeration
deletes it. Put relation 2's table at the bottom of the spine, hand it a hint
containing a slot-0 reference, and a chunk is pruned on another table's value —
rows vanish, no error.

### Conceptual explanation

**Wiring.** One line in `main.cc`, between the two passes it is sandwiched
between, inside the block that already exists.

**Reachability of the pruning hazard.** Be precise here, because it determines
how much machinery is justified. Post-pushdown, the filter above the join holds
only conjuncts whose `soleSlot()` is `-1`: multi-relation, constant, or
unresolved. `collectSimplePredicates` accepts only `ColumnRef op Literal` at an
AND leaf, so:

- a multi-relation conjunct is `ColumnRef op ColumnRef` → rejected;
- an `OR` is indivisible → rejected;
- a constant conjunct has no `ColumnRef` → rejected;
- a slot `-1` `ColumnRef` with a literal *would* pass — but the Binder leaves a
  ref unresolved only when it matches no relation, and the Validator then rejects
  the query with "column not found".

So the hazard is **currently unreachable**. That is not a reason to leave it: the
invariant that makes it unreachable is the one being deleted, and the next person
to add a conjunct shape that survives pushdown inherits a silent wrong answer.
The containment must therefore be cheap and local. It is: withhold the hint when
the leftmost relation is not slot 0, which costs nothing today (the hint carries
nothing prunable) and removes the positional dependency entirely.

Note which hint this is. The per-scan hints created by pushdown — a
`LogicalFilter` whose direct child is a `SCAN` — are unaffected: their refs were
restamped to 0 by `distribute()` and they sit directly above the correct scan,
under any ordering. Only the hint originating *above the join tree* travels down
the spine, and only that one is withheld.

### Code

```cpp
// src/cli/main.cc — inside the existing if (!args.no_optimize) block

                if (!args.no_optimize) {
                    logical = PredicatePushdown::apply(std::move(logical), catalog);

                    // Week 28: choose the join order. AFTER pushdown, so each
                    // relation's leaf carries its own filters and costs at its
                    // filtered cardinality; BEFORE estimation, so the stamps
                    // --explain prints and VectorizedPlanBuilder costs describe
                    // the tree that will actually run. Deliberately inside this
                    // block: --no-optimize keeps the written order, which is the
                    // benchmark baseline AND the differential oracle
                    // (compare_against_sqlite.py runs the vectorized suite twice,
                    // and a reordering that only happens with the optimizer on is
                    // what makes the second run able to catch it).
                    logical = JoinEnumeration::apply(std::move(logical), catalog);

                    CardinalityEstimator::estimate(*logical, catalog);
                }
```

```cpp
// src/planner/vectorized_plan_builder.cc — Lowering::lowerNode, JOIN case

            // ChunkPruner treats a relation_slot < 1 ref in a scan hint as
            // scan-local (chunk_pruner.h). That is true of the leftmost scan
            // only while the leftmost relation IS slot 0 — which join
            // enumeration (Week 28) no longer guarantees. With relation 2 at the
            // bottom of the spine, a slot-0 ref in the hint would prune relation
            // 2's chunks on relation 0's value: rows vanish, no error.
            //
            // Unreachable today (post-pushdown the residual above a join holds
            // only multi-relation, OR or constant conjuncts, none of which
            // collectSimplePredicates accepts) — but the reason it is
            // unreachable is exactly the invariant being deleted. Withhold the
            // hint instead of depending on it. Costs nothing: the hint carries
            // nothing prunable in the case it is withheld.
            const bool leftmost_is_slot0 =
                join->output_schema.size() > 0 &&
                join->output_schema.column(0).relation_slot == 0;
            auto from_child = lower(join->children[0].get(),
                                    leftmost_is_slot0 ? pruning_where : nullptr);
            auto join_child = lower(join->children[1].get(), nullptr);
```

### Implementation guidance

1. Add `src/planner/join_enumeration.cc` to `swiftql_lib` in the root
   `CMakeLists.txt`, next to `predicate_pushdown.cc`. Add
   `tests/test_join_enumeration.cc` to `tests/CMakeLists.txt`.
2. `main.cc` needs `#include "planner/join_enumeration.h"`.
3. **Do not** touch `src/planner/planner.cc`. Volcano refuses multi-way joins
   and gains nothing from ordering a single join. Cost-based optimization
   applies only to the columnar/vectorized path (README *Limitations*), and
   Week 27 made the capability split deliberate.
4. **Do not** reorder the `--explain` capture blocks in `main.cc`. The
   pre-optimization *Logical Plan* section is captured before pushdown and shows
   the written order; the *Optimized Logical Plan* section shows the chosen one.
   The diff between those two sections is the checkpoint's primary evidence, and
   it works only because the capture points are where they are.
5. **Gotcha:** `countScans` / `scan_uses` in the vec builder counts every scan
   before lowering begins and decrements as it goes, so a self-join's last scan
   moves its table and earlier ones copy. Reordering changes the *sequence* of
   lowering but not the counts — the mechanism is order-independent. Confirm it
   with the self-join queries in the harness rather than reasoning about it
   twice.

### Verification

```bash
# 1. optimized and --no-optimize must return identical rows on every multi-way shape
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized \
  --no-cache --query "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id \
  JOIN drivers d ON l.driver_id = d.driver_id WHERE l.lap_id < 200"
# ... and again with --no-optimize. Same number, or stop.

# 2. --no-optimize must keep the WRITTEN order — check the Physical Plan's scan order
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized \
  --no-cache --no-optimize --explain --query "<same query>"

# 3. the full gates
cd build && ./tests/swiftql_tests
cd .. && python3 python_tools/compare_against_sqlite.py
python3 python_tools/test_new_queries.py
```

Use the `optimizer-diff` skill's workflow for (1) and (2) — result diff,
plan-shape diff, then estimated-vs-actual — rather than eyeballing counts.

---

## Task 6 — `EXPLAIN` shows the decision

### Why it matters

This *is* the checkpoint. And invariant 13 says why it has to be `--explain` and
nothing else: cost-based decisions are result-invariant internals, so tests
assert output equality and never internal choices — "except via `--explain` shape
checks". `--explain` is the only sanctioned surface on which a join-order
decision can be asserted at all. Without it the week ships a search whose output
nobody can observe, and Task 7 has nothing to test.

There is a precedent to follow exactly: Week 23 put the build-side decision on
the physical join as `build=… cost=… (alt=…)`, later extended with
`algo=… (simd=…)`. The rules that precedent established are all load-bearing:

- **print it only when estimates drove it** — "printing `cost=` under
  `--no-optimize` would claim an optimizer decision that never happened";
- **costs are unitless — never append a time unit** (`cost_model.h`);
- **an empty decision string leaves every pre-existing plan string
  byte-identical**, which is what keeps the whole existing `--explain` test
  surface passing.

### Conceptual explanation

The decision belongs on the **logical** plan, because ordering is a logical-tree
shape, and it is the *Optimized Logical Plan* section that shows the reordered
tree. Put it on the **top** `LogicalJoin` only — one line per query, describing
one decision. The per-join `build=`/`cost=` strings in the Physical Plan section
already describe the per-join decisions, and they now describe them for the
chosen order.

What the line must contain for a reader to see that the decision was
cost-based:

- **the chosen order**, unambiguously. Relations can share a table name (a
  self-join), so render each as `table@slot` — the same disambiguation idiom
  `qualifyIfAmbiguous` already established for column names (`team@1`);
- **the chosen order's cost**;
- **the written order's cost**, as the alternative. This is the whole point:
  without a comparand the number is decoration. Costing the written order is one
  extra call to the same transition function;
- **which search ran** — `dp` or `greedy`.

```
LogicalJoin [driver_id = driver_id]   order=laps@0,drivers@2,laps@1 cost=40040 (written=5030060) method=dp
```

Reading the two `--explain` sections together gives the full story: the *Logical
Plan* section shows the written tree, the *Optimized Logical Plan* section shows
the reordered one with this annotation, and the *Physical Plan* section shows the
per-join build side and algorithm chosen for it.

### Code

```cpp
// src/planner/logical_plan.h — inside LogicalJoin

struct LogicalJoin : LogicalPlanNode {
    std::vector<JoinKey> keys;
    int join_slot;
    // Week 28: set by JoinEnumeration on the TOP join of a reordered tree, and
    // nowhere else. Empty for every single-join plan, every --no-optimize plan
    // and every hand-built test tree, so all pre-existing explain strings are
    // byte-identical. Same discipline as VecHashJoinNode::cost_decision_ —
    // and the same rule: never print it when estimates did not drive the
    // decision, or --explain claims an optimizer choice that never happened.
    std::string order_decision;
    ...
};
```

```cpp
// src/planner/logical_plan.cc — LogicalJoin::explain(), at the end

    s += "]";
    if (!order_decision.empty()) s += " " + order_decision;
    return s;
```

```cpp
// src/planner/join_enumeration.cc — building the string

// Cumulative cost of one complete left-deep order, using the same transition
// function the search does. Costing the WRITTEN order too is what makes the
// printed decision evidence rather than decoration.
double orderCost(const std::vector<int>& order, const std::vector<Relation>& rels,
                 const std::vector<Edge>& edges) {
    uint32_t placed = 1u << order[0];
    Sub cur{0.0, rels[order[0]].rows, rels[order[0]].width, order[0], 0};
    for (size_t k = 1; k < order.size(); ++k) {
        Step step = stepCost(cur, placed, rels[order[k]], edges, rels);
        placed |= 1u << order[k];
        cur = Sub{step.cost, step.rows, step.width, order[k], placed};
    }
    return cur.cost;
}

// `table@slot`, the same disambiguation idiom qualifyIfAmbiguous uses for
// columns — two relations can share a table name (a self-join), and an order
// that cannot name its relations apart is not an auditable decision.
std::string renderOrder(const std::vector<int>& order, const std::vector<Relation>& rels) {
    std::string s;
    for (size_t k = 0; k < order.size(); ++k) {
        if (k) s += ",";
        s += leafScanTableOf(rels[order[k]].subtree.get()) + "@" + std::to_string(order[k]);
    }
    return s;
}

// ... at the end of reorder(), after rebuild():
    std::ostringstream d;
    d << std::fixed << std::setprecision(0)     // costs are unitless: no unit suffix
      << "order=" << renderOrder(order, rels)
      << " cost=" << chosen_cost
      << " (written=" << written_cost << ")"
      << " method=" << (use_dp ? "dp" : "greedy");
    static_cast<LogicalJoin*>(root.get())->order_decision = d.str();
```

### Implementation guidance

1. `renderOrder` must run **before** `rebuild` moves the subtrees out of `rels`,
   or capture the table names into the `Relation` struct during decompose. The
   second is cleaner; a `std::string table` field on `Relation` costs nothing
   and removes a use-after-move.
2. `leafScanTableOf` is a local walk down `children[0]` to the `LogicalScan` —
   the same shape as the vec builder's `leafScanTable`. Do not export the
   builder's copy across a header for one call; two four-line walkers in two
   translation units is the smaller mess than a new shared header, and unlike
   `collectSlots` this one has no silent-failure mode (a missing case is a
   crash, not a lost answer).
3. Set `order_decision` **only** on the returned root join, and only when the
   pass actually ran (three or more relations). The `MIN_ENUMERATED_RELATIONS`
   early return leaves it empty.
4. **Gotcha:** `collectLogicalNodes` in `main.cc` puts `explain()` into
   `line.label` and pads to the widest label. A long decision string widens the
   whole section. That is cosmetic and matches how the Physical Plan section
   already behaves — do not "fix" it by moving the string into a new column.
5. **Gotcha:** `benchmark.py`'s q-error regex anchors on `rows_out=` being
   immediately followed by `est=`. This change touches neither; confirm rather
   than assume by running one `--explain-analyze`.

### Verification

```bash
# the star query: the decision must be visible, and must name a different order
# than the written one
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized \
  --no-cache --explain --query \
  "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id \
   JOIN drivers d ON l.driver_id = d.driver_id"
# expect, in === Optimized Logical Plan ===, on the top LogicalJoin:
#   order=laps@0,drivers@2,laps@1 cost=... (written=...) method=dp
# and the written cost strictly greater than the chosen cost.

# --no-optimize prints no Optimized section at all, and no order= anywhere
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized \
  --no-cache --no-optimize --explain --query "<same>"

# a single join must print exactly what it printed last week
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized \
  --no-cache --explain --query \
  "SELECT laps.lap_id, drivers.name FROM laps JOIN drivers ON laps.driver_id = drivers.driver_id"
```

The third command is the regression that matters most: no `order=`, and a
`LogicalJoin [...]` line identical to the current output.

---

## Task 7 — Tests, correctness harness, and documentation

### Why it matters

Invariant 13 draws the line this task has to respect: a join order is a
**result-invariant internal**. Asserting "the plan joined `drivers` second" in a
result test would pin an optimizer decision that is free to change with a
constant; asserting only that results match would let the search return the
written order forever and pass. Both halves are needed, and they go in different
places:

- **results** — optimized ≡ `--no-optimize` ≡ SQLite, in
  `compare_against_sqlite.py` and `test_new_queries.py`'s invariant suite;
- **the decision** — from `--explain`, in a steering runner, exactly as
  Week 23.5 did for join *algorithm* selection (`run_join_steering`).

The steering runner is the checkpoint's executable form. Copy its shape rather
than inventing one.

### Conceptual explanation

**Result queries.** Three-relation joins already run in the two vectorized modes
via `WEEK27_MULTIWAY_QUERIES`. Week 28 changes the order those execute in, so
the existing six queries are already a regression suite for this week — running
them unchanged is a real test, not a formality. Add shapes that specifically
*give the search a choice*:

- a **star** (one relation adjacent to both others): several legal orders, wildly
  different costs — the discriminating case;
- a **chain** A–B–C: only orders keeping B adjacent are legal, so it tests the
  connectivity guard;
- a **triangle**: every order legal, and the last-added relation carries two
  keys — so an ordering changes a join from one key to two, exercising the
  composite-key path from Task 3's field-order argument;
- one shape where a **selective filter on a middle relation** should pull it
  earlier, which is the interaction between pushdown and enumeration.

Keep the result queries **bounded**. The star query's natural cardinality is five
million rows; that is fine for `--explain` (no execution) but is not something to
put in a harness that runs it in four modes. Add a `WHERE l.lap_id < 200` or
equivalent to the *result* variants and leave the unbounded form to the steering
runner.

**Steering.** Assert the *leftmost* relation (or the full `order=` string) read
from the Optimized Logical Plan section, for a handful of queries whose optimal
order is derivable by hand from the shipped stats (`laps` 10k, `drivers` 20,
NDV(driver_id) = 20). Hand-derive each expectation and put the derivation in the
comment, the way `WEEK22_QUERIES` and `WEEK23_5_QUERIES` do — an expectation
nobody can re-derive is an expectation nobody can debug.

### Code

```python
# python_tools/test_new_queries.py

# ─── Week 28: cost-based join ordering ───────────────────────────────────────
# Three-relation shapes where more than one order is legal, so the search has a
# real choice. Vectorized-only (multi-way execution is, since Week 27), and
# bounded so the harness does not materialize a five-million-row intermediate in
# four modes. Results are order-invariant, so these assert output equality across
# optimized / --no-optimize / SQLite; the ORDER itself is asserted separately
# from --explain, per invariant 13.
#   star_pivot_small      → drivers (20 rows) is adjacent to both laps scans:
#                           joining it second avoids a 5M-row intermediate
#   chain_middle_required → d only touches l2, so any legal order keeps l2 first
#   triangle_all_legal    → every order legal; the last relation added carries
#                           TWO keys, so ordering changes a 1-key join into a
#                           2-key one (composite key path)
#   filtered_middle       → a selective filter on the middle relation should pull
#                           it earlier: pushdown and enumeration interacting
WEEK28_QUERIES = [
    ("w28_star_pivot_small",
     "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id "
     "JOIN drivers d ON l.driver_id = d.driver_id WHERE l.lap_id < 200"),
    ...
]

# Expected leftmost-two relations, hand-derived from the shipped stats
# (laps 10k rows, drivers 20 rows, NDV(driver_id) = 20):
#   star_pivot_small: l ⋈ l2 estimates 10000*10000/20 = 5,000,000 rows, then ⋈ d
#     leaves it at 5,000,000. l ⋈ d estimates 10000*20/20 = 10,000, then ⋈ l2
#     reaches the same 5,000,000 — same answer, one intermediate two orders of
#     magnitude smaller. So drivers must come SECOND.
WEEK28_EXPECTED_ORDER = {
    "w28_star_pivot_small": ["laps@0", "drivers@2", "laps@1"],
    ...
}


def run_join_order_steering(queries):
    """Week 28: assert each query's chosen join order matches the hand-derived
    expectation, read from --explain's Optimized Logical Plan section. Mirrors
    run_join_steering (Week 23.5) — an internal decision is assertable only
    through --explain (invariant 13)."""
    VEC = ["--execution", "vectorized", "--storage", "columnar"]
    ...
    section = result.stdout.split("=== Optimized Logical Plan ===")[-1] \
                           .split("=== Physical Plan ===")[0]
    # `order=laps@0,drivers@2,laps@1 cost=... (written=...) method=dp`
    match = re.search(r"order=(\S+)", section)
    ...
```

```python
# python_tools/compare_against_sqlite.py

# Week 28 gives multi-way joins a cost-chosen order. Results are order-invariant,
# so these run in the two VECTORIZED modes (multi-way is vectorized-only since
# Week 27) and are diffed against SQLite. The second vectorized run is
# --no-optimize, which keeps the WRITTEN order — that pairing is what makes this
# suite able to catch a reordering that changes an answer.
WEEK28_JOIN_ORDER_QUERIES = [ ... ]
```

### Implementation guidance

1. Add `tests/test_join_enumeration.cc` to `tests/CMakeLists.txt` in the same
   commit as the file, or the tests silently do not run — the same false-green
   the root `CMakeLists.txt` comment records for `ctest`.
2. Add the new multi-way queries to the **vectorized** and **optimizer-invariant**
   runs only, never to the default row/Volcano run, where they are a refusal
   rather than a result. `WEEK27_QUERIES` is the pattern; follow it exactly.
3. Update the expected totals in `development.md`'s *Unit Tests* and
   *`compare_against_sqlite.py`* sections. Those printed numbers are a gate; a
   stale one turns "everything passed" into a statement nobody checks.
4. README updates, all in the Week 28 section:
   - mark the checkpoint ✅;
   - a *Shipped / Why it was required* table, matching Weeks 24–27's format, for
     the things the two bullets did not anticipate. At minimum: the two slot
     identities that broke and how each is repaired; the data-volume term and
     the per-relation width sum finally landing from Week 22; the pruning-hint
     positional dependency; and the "no-op below three relations" scope decision;
   - a *Starting notes, from Week 28's foundations* block for Week 29 — the
     outer join is about to change what an `ON` predicate means, and it now
     inherits a tree whose leftmost relation may not be slot 0. Say so
     explicitly, and repeat the `join_condition.h` ON/WHERE fold marker.
   - the *Limitations* list gains the two documented approximations: enumeration
     costs the hash join only, and cardinalities compound under independence.
5. **Do not** add multi-way join-order queries to the benchmark tables. Week 37
   publishes performance; a benchmark number recorded now is a number that has
   to be re-recorded then.

### Verification

The full gate, in order (the `verify` skill runs this sequence):

```bash
cmake --build build -j$(nproc)
cd build && ./tests/swiftql_tests          # all pass, count updated in development.md
cd .. && python3 python_tools/compare_against_sqlite.py   # 0 failed, 0 errors
python3 python_tools/test_new_queries.py                  # incl. the new steering runner
```

Then the checkpoint itself, read by eye once:

```bash
./build/swiftql --catalog catalog.json --storage columnar --execution vectorized \
  --no-cache --explain --query \
  "SELECT COUNT(*) FROM laps l JOIN laps l2 ON l.driver_id = l2.driver_id \
   JOIN drivers d ON l.driver_id = d.driver_id"
```

and confirm, in one screen: the *Logical Plan* section shows the written order,
the *Optimized Logical Plan* section shows a different order with
`order=… cost=… (written=…) method=dp`, the chosen cost is strictly lower, and
the *Physical Plan* section's per-join `build=`/`cost=` lines describe the chosen
tree.

---

## Definition of done

- [ ] `EXPLAIN` on a three-or-more-relation query prints the chosen order, its
      cost, the written order's cost, and the search method — on the top
      `LogicalJoin` of the *Optimized Logical Plan* section only.
- [ ] The chosen order is genuinely cost-based: on the star query the search
      returns an order the written fold does not, and `written=` exceeds `cost=`.
- [ ] Left-deep DP under `MAX_DP_RELATIONS`, greedy above it; both only ever
      extend by a **connected** relation.
- [ ] Enumeration is a no-op below three relations. Every single-join
      `--explain` string, every Week 22 build-side assertion and every Week 23.5
      algorithm assertion is unchanged.
- [ ] `--no-optimize` keeps the written order, and results are identical to the
      optimized run on every shape.
- [ ] `joinCardinality` has exactly one implementation, called by both the
      search and the estimator.
- [ ] `joinOutputCost` exists, is applied per join by the enumerator, and is
      **not** inside `hashJoinCost` or `simdLoopJoinCost` — neither of those two
      functions or their four constants changed.
- [ ] `rowWidth` sums real per-relation `avg_width` for a multi-relation input;
      the `columns * 8.0` placeholder survives only as a per-column fallback
      where stats are absent.
- [ ] A leaf subtree's own schema still stamps slot 0; the merged schema stamps
      every relation's binder slot, including the leftmost block; the first
      join's `JoinKey::from_slot` is 0 and the rule is written down in
      `join_condition.h`.
- [ ] The pruning hint is withheld when the leftmost relation is not slot 0.
- [ ] Every edge is used exactly once in the rebuilt tree (unit-tested across
      several permutations).
- [ ] Full gate green: unit tests, `compare_against_sqlite.py`,
      `test_new_queries.py` including the new order-steering runner.
- [ ] README Week 28 checkpoint marked ✅, with a *Shipped / Why it was required*
      table, a *Starting notes* block for Week 29, and the two costing
      approximations recorded under *Limitations*.
- [ ] Nothing built that the checkpoint did not require: no bushy trees, no
      residual re-placement, no CLI flag, no outer-join or subquery groundwork.
