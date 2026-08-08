# Seam audit — optimizer result preservation (pass 2)

HEAD `ee9c9d7`, branch `claude/phase5-week26-qomtkb`.
Predecessor: `scratchpad/audits/seam-optimizer-preservation-pass-1.md`.

Status: IN PROGRESS (written incrementally; see summary block at end).

## Part A — is pass 1's fix real and complete?

### A.1 The DERIVED estimate is derived from the subplan, not a constant — CORRECT

`src/planner/cardinality_estimator.cc:330-357` (commit `17bfcea`):

```cpp
case LogicalNodeType::DERIVED: {
    estimateNode(*node.children[0], catalog);
    node.estimated_rows = node.children[0]->estimated_rows;
    return StatsContext{};
}
```

It **recurses into the body** and adopts the body's count. Not a constant, not a
FALLBACK_ROW_COUNT. The recursion also stamps every node inside the body, which is
the part that matters for the second-order damage pass 1 described (`--explain`
showing a whole unstamped subtree).

Cost when wrong: the body's own estimate is only as good as the estimator's rules for
whatever tops it (AGGREGATE's NDV product, FILTER's selectivity). A wrong body
estimate misprices the derived leaf in `JoinEnumeration` (`join_enumeration.cc:533`)
and in `VectorizedPlanBuilder`'s build-side/algorithm choice — a **plan-quality**
cost, bounded by the `written_cost` floor at `join_enumeration.cc` (see A.4). It is
strictly better than the pre-fix `-1 → 0`, which was not an estimate at all.

Returning an **empty StatsContext** rather than the body's is the conservative choice
and the commit message argues it correctly: the body's context is keyed by the names
of whatever node tops the body, while the derived relation's columns are the select
list after `AS d (a, b)` renaming, and no mapping is computable at that point. The
consequence is that a join **above** a derived relation gets no NDV for the derived
side and `joinCardinality` falls to its `max(l, r)` containment — an over-estimate,
which is the safe direction. Verified: `joinCardinality`'s lookups are
`out.findForRef(...)` / `right.find(...)`, both of which simply miss on an empty
context.

### A.2 Every node kind is now handled — VERIFIED EXHAUSTIVELY

`src/planner/logical_plan.h:12-14` defines exactly nine kinds:

```
SCAN, DERIVED, JOIN, FILTER, AGGREGATE, PROJECT, SORT, DISTINCT, LIMIT
```

`estimateNode`'s switch (`cardinality_estimator.cc:309-576`) has a case for each:
SCAN:310, DERIVED:330, FILTER:359, JOIN:373, AGGREGATE:528, PROJECT/SORT/DISTINCT:557-559,
LIMIT:567. **Nine of nine.** The trailing `return StatsContext{}` after the switch —
the line the pre-fix DERIVED fell through to — is now unreachable for any value of
the enum, and remains only as the compiler's fall-off-the-end appeasement.

Semi/anti joins are **not** separate node types: they are `LogicalJoin` with
`semantics != STANDARD` (`logical_plan.h:144`, `JoinSemantics` enum), so the JOIN
case covers them (and does, explicitly, at :505-527). `ANTI_NOT_IN` is likewise a
`JoinSemantics` value, handled by the same `!= STANDARD` branch. There is no tenth
node kind hiding anywhere.

**Note the residual smell, LOW:** the switch has no `default:` — which is *correct*
practice (a new enumerator then produces a `-Wswitch` warning rather than silence),
but only if `-Wswitch` is on and warnings are not being drowned. The trailing
`return StatsContext{}` after the switch means a newly added enumerator would still
compile and still silently re-create the exact `-1.0` bug. There is no
`static_assert`, no `default: throw`, and no test that iterates the enum. The
structural defence against a *third* occurrence of this bug class does not exist.

### A.3 Does any pass's CORRECTNESS depend on an estimate?

Consumers of `estimated_rows` (exhaustive grep, `src/`):

| site | what the estimate decides | result-preserving? |
|---|---|---|
| `join_enumeration.cc:533` | join ORDER | yes — order is a plan shape; the pass declines outer/semi/anti trees outright |
| `vectorized_plan_builder.cc:329` `estimate_driven` | whether Week-22 costing runs at all | see below |
| `vectorized_plan_builder.cc:541` `use_simd` | VecSimdLoopJoin vs VecHashJoin | yes — both skip NULL keys identically (`vec_simd_loop_join_node.cc:49,158` vs `vec_hash_join_node.cc:49-58`), and SIMD is INT-keys-only so `key_encoding.h`'s NaN rule cannot differ |
| `vectorized_plan_builder.cc:544` `from_builds` | which side builds | **see LATENT-1 below** |
| `cli/main.cc:302,328` | `est=` printing | cosmetic |

Everything on that list is a plan-shape choice **except one latent hazard**:

#### LATENT-1 (LOW — no constructible query today) — the build-side swap silently DROPS `on_residual`, and the swap is estimate-driven

`src/planner/vectorized_plan_builder.cc:623-631`:

```cpp
std::unique_ptr<VecHashJoinNode> join_node = from_builds
    ? std::make_unique<VecHashJoinNode>(          // FROM builds — swapped
          std::move(join_child), std::move(from_child),
          right_idx, left_idx, join->output_schema, /*swapped=*/true)
                                                  //  <-- no outer, NO on_residual
    : std::make_unique<VecHashJoinNode>(
          std::move(from_child), std::move(join_child),
          left_idx, right_idx, join->output_schema, /*swapped=*/false,
          outer, std::move(join->on_residual));
```

The swapped branch passes neither `left_outer` nor `on_residual`; the residual
`unique_ptr` simply stays behind on the logical node and is **never evaluated**. Same
for the `use_simd` branch at :607-615, which takes no residual parameter at all.

This is safe **only** because `on_residual` is set on `JoinType::LEFT` nodes and
nowhere else (`logical_plan.cc:938-945` routes an INNER join's ON residuals into the
WHERE conjunction instead; the only assignment is `logical_plan.cc:973`, inside
`if (jc.type == JoinType::LEFT)`), and `outer` forces `from_builds = false`
(:544) and `use_simd = false` (:541). So the drop is unreachable.

But note the asymmetry with the SEMI/ANTI branch immediately above (:489-503), which
deliberately **forwards** the residual precisely so `VecHashJoinNode`'s constructor
guard fires — with the file's own comment: *"A constructor check the caller routes
around is not a check."* The STANDARD swapped branch and the SIMD branch route
around exactly that check, with no comment saying why they may. The moment anything
attaches a residual to an INNER `LogicalJoin` — a future decorrelation producing an
inequality correlation is the obvious candidate — this becomes a wrong answer whose
occurrence is decided by a **cardinality estimate**, i.e. a correctness bug that
appears and disappears as the data changes. I could not construct a query that
reaches it today, so: LOW.

### A.4 The four LOWs of `18af84f` — all four fixed as called for; one caller needed a sweep and got it

| # | finding | fix | is it the fix the finding called for? | introduced anything? |
|---|---|---|---|---|
| 1 | semi/anti branch passed `/*on_residual=*/nullptr`, defeating `VecHashJoinNode`'s guard | `vectorized_plan_builder.cc:503` now forwards `std::move(join->on_residual)` | yes | no — but see LATENT-1 above: the STANDARD *swapped* branch and the SIMD branch still route around the same guard, and were not swept |
| 2 | semi/anti stamp had no ≥1 floor | `cardinality_estimator.cc:511` routes through `flooredJoinCardinality(l_rows, r_rows, rows)` | yes | no. The floor cannot exceed `l_rows`: `flooredJoinCardinality` only lifts when `left_rows >= 1.0`, and the lift is to 1.0 ≤ l_rows. The clamp `min(rows, l_rows)` above it is therefore not violated |
| 3 | a false invariant in `join_enumeration.cc`'s Week-34 paragraph | replaced with what is true | yes — see A.5 |
| 4 | the slot decline was silent | `hasSlotOutsideRangeTable` → `slotDeclineReason`, returns *which* cause; `reorder` stamps `join-ordering=skipped (semi/anti join)` | yes | no; the truth value is identical, so no tree's *shape* changed. `--explain` gains a line, which is the point |

Fix 4's test rewrite is itself a finding pass 1 earned: the old
`DeclinesASemiJoinTreeAsSlotOutsideTheRangeTable` used a **two-relation spine**
and so returned at the `n < MIN_ENUMERATED_RELATIONS` guard without ever reaching
the decline it was named for. It asserted a property of a code path it never
executed. The replacement uses a three-relation spine and a control query. Good.

### A.5 The false invariant — who was relying on the wrong version?

The deleted paragraph claimed two things: (a) `joinCardinality`'s no-statistics
`max(l, r)` branch runs for a derived relation, and (b) `method=written-floor` was
therefore CLI-reachable. Both false: `joinCardinality`
(`cardinality_estimator.cc:243-262`) sets `have_ndv` if **either** side yields an
NDV — `ndv = max(lk?ndv:0, rk?ndv:0)` — and the non-derived side of the join always
has one, so the multiplicative branch runs.

**I traced every consumer of the wrong version.** The one thing that could have
rested on it is the written-order floor at `join_enumeration.cc:570-580`, which is
justified by "soundness rests on `rows(S)` being path-independent, which
`joinCardinality`'s no-statistics `max()` branch breaks". Checking that:

- The floor is a **bound, not a fix** — it clamps the outcome regardless of whether
  the estimates are path-dependent. Its correctness does not depend on
  `max(l, r)` ever running. So the false invariant did not make the floor
  *necessary*, and its falsehood does not make the floor *unnecessary*.
- `max(l, r)` still genuinely runs when **neither** side has an NDV — two derived
  relations joined to each other, or a stats-less table. `SELECT * FROM (…) a JOIN
  (…) b ON a.k = b.k JOIN t ON …` is the shape. So the residual claim the new
  comment keeps is true, and the floor keeps its justification.
- `method=` is derived at `join_enumeration.cc:596-599` from `searched` and
  `kept_written` — the two facts that actually determined the printed order — so it
  cannot report `dp` for an order the DP did not produce, whatever the paragraph
  above said. No consumer read the false claim.

**Verdict on Part A: pass 1's fix is real and complete for the defect it names.**
The comment sweep after the invariant was corrected is present and correct; I found
no stale citation of the deleted paragraph anywhere else (`grep` for
`written-floor`, `max(l, r)`, `no-statistics` across `src/` and `docs/` returns only
the corrected sites and `cardinality_estimator.cc`'s own rule comment).

## Part B — the seam taken fresh

### B.0 What the optimizer actually IS, from the code

`--no-optimize` gates exactly one block, and it appears twice —
`src/cli/main.cc:566-588` (top level) and `src/cli/main.cc:134-138`
(`runVectorizedToRows`, the nested-query runner):

```cpp
if (!no_optimize) {
    logical = PredicatePushdown::apply(std::move(logical), catalog);
    logical = JoinEnumeration::apply(std::move(logical), catalog);
    CardinalityEstimator::estimate(*logical, catalog);
}
```

**Three passes. That is the whole gate.** Transitively it also gates
`VectorizedPlanBuilder`'s `estimate_driven` (build-side choice + SIMD-vs-hash),
because that is keyed on `estimated_rows >= 0`.

Everything else in `src/planner/` that rewrites a plan runs in **BOTH** legs:

| pass | where it runs | gated by `--no-optimize`? |
|---|---|---|
| `PredicatePushdown` | main.cc:571 / :135 | **yes** |
| `JoinEnumeration` | main.cc:583 / :136 | **yes** |
| `CardinalityEstimator` | main.cc:587 / :137 | **yes** |
| `foldConstants` (constant_folding.cc) | `Binder::bind`, `binder.cc:181` | **NO** |
| `lowerInSubqueries` (IN → semi/anti join) | `LogicalPlanBuilder::build`, `logical_plan.cc:1000` | **NO** |
| `lowerExistsSubqueries` (EXISTS decorrelation) | `logical_plan.cc:1005` | **NO** |
| `lowerCorrelatedScalars` | `logical_plan.cc:1022` | **NO** |
| `materializeSubqueries` | `main.cc:500`, under `needsSubqueryMaterialization` | **NO** (the *body's own* optimizer is threaded, the rewrite is not) |
| derived-relation normalization (`buildRelation`) | `logical_plan.cc:908` | **NO** |

### FINDING B-1 (MEDIUM, observability of the harness — not a wrong answer) — the `optimized == --no-optimize` oracle is blind by construction to six of the nine plan-rewriting passes

Pass 1's §1 reports "1326 passed, 0 failed" and reads it as evidence that Phase 5's
constructs are result-preserving. That reading is **only valid for the three gated
passes.** For `foldConstants`, the three subquery lowerings, `materializeSubqueries`
and derived normalization, **both legs of the differential run identical code**, so
the oracle cannot detect a bug in any of them — it would produce the same wrong
answer twice and report a pass.

This is not hypothetical for this codebase: Week 32's `ANTI_NOT_IN` three-valued
NULL rule, Week 33's EXISTS/NOT EXISTS split, and Week 34's correlated-scalar
decorrelation are all *semantic* rewrites, all unconditional, and all invisible to
the invariant. The only oracle that covers them is the **SQLite** leg of
`compare_against_sqlite.py` — which is real coverage, but it is a different
oracle with different blind spots (queries SQLite refuses, queries with no fixture,
and the 34 Volcano refusals pass 1 counted).

**What makes this a finding rather than a design note:** the seam's charter, and
`main.cc:126-129`'s own comment, both describe `--no-optimize` as "the differential
oracle". `runVectorizedToRows`'s comment says it threads the flag so that "a runner
that always optimized would give both legs the same subquery result and quietly
stop testing the sub-plan" — the file already understands the failure mode, and
then six passes sit outside the gate with no equivalent note anywhere. Nothing in
`docs/` or in the harness states which passes the invariant does and does not
cover, so "0 divergences" is routinely read as stronger than it is.

Not a correctness bug in itself. Ranked MEDIUM because it changes what every prior
"0 divergences" result means.

### FINDING B-2 (MEDIUM, silent decline — a real plan-quality loss, invisible to a result-comparing harness) — neither optimizer pass ever descends into a derived-table body when the outer block has a join

Both `apply` entry points stop at the FIRST node they can act on and **return
without recursing into their own result**:

`src/planner/join_enumeration.cc:611-618`:
```cpp
std::unique_ptr<LogicalPlanNode> JoinEnumeration::apply(...) {
    if (node->type == LogicalNodeType::JOIN) return reorder(std::move(node), catalog);
    for (auto& child : node->children) child = apply(std::move(child), catalog);
    return node;
}
```

`src/planner/predicate_pushdown.cc:361-368`: same shape — `pushIntoJoin` is called
and its result returned; `apply` never walks back into it.

Consequences, all silent:

1. **A derived body's join order is never enumerated when the outer block has a
   join.** `decompose` takes a `LogicalDerived` as an opaque leaf, so its internal
   3-way join tree is carried through `rebuild` untouched and no `order=` line is
   ever printed for it. Failing shape:
   ```sql
   SELECT ... FROM t JOIN (SELECT ... FROM a JOIN b JOIN c WHERE ...) d ON t.k = d.k
   ```
   The inner 3-relation block gets NO join ordering. Write the same body with no
   join in the outer block —
   `SELECT * FROM (SELECT ... FROM a JOIN b JOIN c) d` — and `apply` descends
   (PROJECT → DERIVED → JOIN) and the DP **does** run on it. So whether a derived
   body is optimized depends on whether the *enclosing* block happens to have a
   join. That is not a stated limitation anywhere.
2. **The same for predicate pushdown**: a derived body's own
   `FILTER`-over-`JOIN` is never rewritten when the outer block is itself a
   `FILTER`-over-`JOIN`.
3. **A declining outer block takes the body down with it.** An outer join or a
   semi/anti join anywhere on the outer spine makes `reorder` return early, so a
   fully-inner multi-relation derived body below it also loses ordering — the
   generalisation of exactly the loss pass 1 found and `18af84f` fixed for the
   outer spine.

The stale comment is the tell. `JoinEnumeration::apply`'s own comment reads:
*"The topmost JOIN is the root of the whole join tree — there is exactly one per
statement until subqueries arrive in Week 30 — so this replaces at most once and
never descends into a tree it has already reordered."*
Subqueries arrived in Week 30, semi joins in Week 32, derived tables in Week 34.
There are now **several** join trees per statement and this function reorders only
the outermost. The precondition the comment names as expiring has expired, and the
code was never swept. This is the standing rule this codebase learned twice.

Invisible to the invariant harness by construction: the result is identical, only
the plan is worse. Ranked MEDIUM (not HIGH) because it costs performance, not
correctness, and I have not measured the magnitude — the gate owned `build/` for
the whole audit window.

### FINDING B-3 (LOW, observability) — the derived body's top physical node is the ONE node in the tree that never inherits its estimate

`src/planner/vectorized_plan_builder.cc:304` is the only recursive call in the
whole lowering that uses `lowerNode(` instead of `lower(`:

```cpp
case LogicalNodeType::DERIVED: {
    auto child = lowerNode(derived->children[0].get(), nullptr);   // <-- not lower()
```

`lower()` is the wrapper that does `phys->estimated_rows = node->estimated_rows`
(:277-281). Every other case (`:444, :445, :646, :653, :661, :668, :673, :679`)
goes through it. So the body's **top** physical node prints no `est=` under
`--explain-analyze` even with the optimizer on and even now that the DERIVED
estimator case exists — while the `VecDerived` above it and everything below it do.
Deeper body nodes are fine because their own cases call `lower()`.

That is the exact symptom pass 1 identified as what hid the original bug for
fourteen weeks: *"the column just goes blank and reads as 'the estimator didn't
run'"* (`main.cc:302` prints `est=` only when `estimated_rows >= 0.0`). One-line
fix; no correctness impact.

