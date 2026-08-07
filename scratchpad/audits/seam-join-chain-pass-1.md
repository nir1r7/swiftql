# Seam audit: weeks 26 -> 27 -> 28 -> 29 (join chain)

Question: does DP join enumeration still hold once outer joins exist?

Scope: the SEAM, not the individual weeks. Read-only (no C++ build); grounded with the
pre-built `build/swiftql` against `data/tpch/sf0.01`.

## Verdict

Correctness holds. Every outer-join tree is declined by enumeration before any subtree
moves, the fixed `[FROM] ++ [JOIN]` schema order survives reordering at depth 3, and
`Schema::indexOf(name, slot)` resolves on the reordered merged schema. Optimized and
`--no-optimize` agree on every shape tested.

One real defect, in the cost model rather than in the results: **`CardinalityEstimator`
has no `DERIVED` case**, so from Week 34 on the DP costs a derived relation at 0 rows and
reorders on that number, and two optimizer decisions silently switch off for the whole
query. Details in H-1.

---

## H-1 (high, cost model / silent omission) — `CardinalityEstimator::estimateNode` has no `DERIVED` case; the DP costs a derived relation at 0 rows and reorders on it

`src/planner/cardinality_estimator.cc:308-532`. The switch covers SCAN, FILTER, JOIN,
AGGREGATE, PROJECT/SORT/DISTINCT, LIMIT — and falls off the end at
`src/planner/cardinality_estimator.cc:532` (`return StatsContext{};`) for
`LogicalNodeType::DERIVED`. That default:

- never recurses into the derived body, so nothing in the body is stamped either;
- never assigns `node.estimated_rows`, so a `LogicalDerived` keeps the
  `-1.0` default from `src/planner/logical_plan.h:63` *with the optimizer on*;
- returns an empty `StatsContext`, so no key of the derived side can ever supply an NDV.

### What it does to the DP (weeks 28 + 34 seam)

`src/planner/join_enumeration.cc:490`:

    rels[r].rows = std::max(leaves[r]->estimated_rows, 0.0);

For a derived leaf that is `max(-1.0, 0.0)` = **0.0**. `rels[r].ctx` (line 486) is the
empty context. So in `stepCost` -> `joinCardinality`
(`src/planner/cardinality_estimator.cc:226`) every subset containing the derived relation
has `left_rows` or `right_rows` = 0 and therefore `rows` = 0, and the transition through
it is scored as near-free by `hashJoinCost`'s `min()` at
`src/planner/join_enumeration.cc:307`.

Concrete input, `data/tpch/sf0.01`:

    SELECT n.n_name, d.s
    FROM nation n
    JOIN customer c ON n.n_nationkey = c.c_nationkey
    JOIN (SELECT o.o_custkey AS k, count(*) AS s FROM orders o GROUP BY o.o_custkey) d
      ON c.c_custkey = d.k
    ORDER BY d.s DESC LIMIT 3

`--explain` (optimizer on) reports:

    LogicalJoin [c_nationkey = n_nationkey] order=customer@1,@2,nation@0 cost=1525 (written=4216) method=dp
      LogicalJoin [c_custkey = k]
        LogicalScan [customer, 2 columns]                       est=1500
        LogicalDerived [d, 2 columns]                           <-- no est=
          ... whole body unstamped ...
      LogicalScan [nation, 2 columns]                           est=25

The search hoists the derived relation (~1000 groups over 15000 `orders` rows) ahead of
`nation` (25 rows) and claims a 2.8x improvement over the written order. The entire margin
is manufactured by the fabricated 0: the derived relation is the only input the model
thinks is empty. The written-floor guard at `src/planner/join_enumeration.cc:541` cannot
contain this, because it scores *both* orders through the same `orderCost` and therefore
through the same 0.

### What it does to the two physical decisions

`src/planner/vectorized_plan_builder.cc:329` computes
`estimate_driven = from_est >= 0 && join_est >= 0`. A join whose child is a derived
relation gets `join_est == -1`, and — because the JOIN case propagates the -1 upward, see
below — every join *above* it gets `from_est == -1` too. So for the whole spine:

- the Week 22 cost-based build-side choice falls back to the pre-Week-22 raw-row-count
  heuristic (`src/planner/vectorized_plan_builder.cc:361-372`);
- SIMD eligibility is gated on `estimate_driven`
  (`src/planner/vectorized_plan_builder.cc:531`), so the SIMD loop join is never costed;
- no `build=... cost=...` string is printed, so `--explain` gives no signal that the
  decisions were skipped.

Observable in the physical plan above: `c_nationkey = n_nationkey` (single INT key) lowers
to a bare `VecHashJoin` with no cost decision. The *identical* join in a query without a
derived relation lowers to
`VecSimdLoopJoin [c_nationkey = n_nationkey] ... algo=simd (hash=1502)`.

The -1 propagation: `src/planner/cardinality_estimator.cc:363-367` reads
`l_rows = -1`, and `flooredJoinCardinality` (`:296`) applies its >=1 floor only when
`left_rows >= 1.0 && right_rows >= 1.0`, so -1 passes through unfloored and every ancestor
node prints blank. In the derived-first variant of the same query, `LogicalDerived`, both
`LogicalJoin`s, `LogicalSort`, `LogicalProject` and `LogicalLimit` all print no `est=`.

### It also falsifies a stated invariant (target 5)

`src/planner/join_enumeration.cc:454-462` states:

> a derived relation has no TableStats, so `joinCardinality`'s no-statistics branch
> `max(l, r)` — which is NOT multiplicative — now runs on a query the CLI can type [...]
> Week 28 also recorded that `method=written-floor` had never executed. It is reachable
> from the CLI now.

Both halves are false as written. `joinCardinality` takes the *multiplicative* branch on
these queries, because `have_ndv` (`:243`) is set from **either** side and the non-derived
side always supplies an NDV — the derived side merely contributes nothing. So what runs is
`0 * r / ndv`, not `max(l, r)`. And `method=written-floor` did not fire in either query
tested; the DP won outright (`cost=1525 (written=4216) method=dp`), because the fabricated
0 makes the reordered plan look strictly better rather than merely path-dependent. The
guard the comment nominates as the containment is the one thing the 0 defeats.

### Not a wrong answer

Verified: optimized and `--no-optimize` return byte-identical results on both derived
shapes above. The reordered plan is legal; only its justification is fabricated.

### Fix shape

Add a `DERIVED` case to `estimateNode` that recurses into `children[0]`, sets
`node.estimated_rows` from the body's root, and returns a context re-stamped to the
derived relation's own normalized slot-0 schema (the same shape
`derivedRelationSchema` already gives the node). That is the minimum that makes
`rels[r].rows` in `join_enumeration.cc:490` a real number.

---

## Targets confirmed clean

### T1 — build/probe swap cannot swap a side Week 29 requires fixed, at any relation count

Three independent guards, and none of them depends on relation count:

- `src/planner/vectorized_plan_builder.cc:539` — `from_builds = outer ? false : ...`.
  The forcing is unconditional and precedes every cost comparison.
- `src/planner/vectorized_plan_builder.cc:531` — `use_simd` carries `!outer`, so the
  SIMD branch (`:598`), which has no unmatched path, is unreachable for an outer join.
- `src/execution/vec_hash_join_node.cc:16` — the operator constructor throws on
  `left_outer_ && swapped_`.

The `swapped=true` branch at `src/planner/vectorized_plan_builder.cc:613-617` silently
drops `join->on_residual`, which would be a real bug — but `on_residual` is non-null only
for a LEFT join (`src/planner/logical_plan.cc:939-946` is the only writer, and it is
inside `if (jc.type == JoinType::LEFT)`), and a LEFT join can never reach that branch.
The same latent drop exists in `JoinEnumeration::rebuild`
(`src/planner/join_enumeration.cc:249-251` constructs fresh `LogicalJoin`s and copies
neither `join_type`, `on_residual` nor `semantics`) and rests on the same invariant —
which holds only because `containsOuterJoin` declines first. Two drops, one guard; worth
knowing they are coupled, but neither is reachable today.

Reordering cannot reach an outer join at all — see T3.

### T2 — `[FROM] ++ [JOIN]` order survives reordering; `indexOf(name, slot)` resolves 3 deep

`JoinEnumeration::rebuild` (`src/planner/join_enumeration.cc:204-251`) stamps the leftmost
leaf's merged columns with `order[0]` and each added relation's with `r`, so every column
of the merged schema carries its true binder slot no matter where the relation sits on the
spine. `Schema::indexOf(name, slot)` (`src/common/schema.cc:32`) is an exact (name, slot)
match, and two relations cannot share both, so duplicate names stay resolvable
(invariant 3 intact).

The one asymmetry is deliberate and correct: `rebuild:235-237` forces `from_slot = 0` on
the *first* join's keys, because that join's left input is a leaf whose own schema stamps
slot 0. `leftKeyIndices` (`src/planner/vectorized_plan_builder.cc:97`) resolves against the
physical child schema, so the first join matches on slot 0 and every later join matches on
real binder slots. `CardinalityEstimator`'s JOIN case reads the keys at
`cardinality_estimator.cc:365-367` **before** the restamp at `:378-382`, which is what
keeps the two in step.

Verified end to end on a real reorder, sf0.01:

    SELECT o.o_orderkey, c.c_name, n.n_name
    FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey
                  JOIN nation n ON c.c_nationkey = n.n_nationkey
    WHERE n.n_name = 'BRAZIL' ORDER BY o.o_orderkey LIMIT 5

`order=customer@1,nation@2,orders@0 cost=17675 (written=52779) method=dp` — the leftmost
relation is slot 1, the top join lowers `swapped` (`build=join-subtree`), and the result is
identical to `--no-optimize`.

### T3 — the decline on outer joins is complete

`containsOuterJoin` (`src/planner/join_enumeration.cc:91`) tests
`join_type != JoinType::INNER` and recurses into **all** children, and runs at
`:433` before `decompose` moves anything. `JoinType` has only INNER and LEFT
(`src/parser/parser.cc:147-149` is the only producer), so `!= INNER` is exhaustive.
Semi/anti joins are a separate axis and are declined by a separate guard:
`hasSlotOutsideRangeTable` fires on `join_slot < 1` (`:136`), and both lowerings construct
their join with `/*join_slot=*/-1` unconditionally
(`src/planner/subquery_lowering.cc:94`, `src/planner/subquery_decorrelation.cc:581`) —
so there is no semi/anti node the test can miss.

`rebuild` is the only code that reorders, and it runs only after both declines. Therefore
no path reorders across an outer join.

A mixed inner+outer tree still gets a valid plan — the decline returns the tree untouched
and reports `join-ordering=skipped (outer join)`. Verified on sf0.01:

    ... FROM nation n JOIN customer c ON ... LEFT JOIN orders o ON ...

    LogicalLeftJoin [c_custkey = o_custkey] join-ordering=skipped (outer join)  est=15000
      LogicalJoin [n_nationkey = c_nationkey]                                   est=1500
    ...
    VecLeftHashJoin [...] build=orders cost=31620 (outer: the preserved side must probe, hash only)

The inner block below is *not* separately enumerated — `JoinEnumeration::apply`
(`:574`) returns immediately once it has handed the topmost JOIN to `reorder`, whether
`reorder` accepted or declined. That is the documented Week 29 cost, not a defect.

### T3b — the NULL rule composes across a stacked outer join

Week 27's shared encoding drops a row whose key tuple is unmatchable
(`src/execution/key_encoding.h:97`, `vec_hash_join_node.cc:53-64`). Week 29's outer join
does **not** inherit that drop: `vec_hash_join_node.cc:302-305` emits null-extended on an
unmatchable probe key, `:309-312` on a hash miss, and `:345` after a residual rejects every
candidate. So NULLs manufactured by a lower outer join, fed as probe keys into an outer
join above, are preserved rather than deleted. `:340` deliberately does not set `matched`
for a residual-rejected candidate, which is the other half of the same rule.

### T4 — the three cardinality rules compose

The three are mutually exclusive by construction and all three sit at the STAMP, never
inside `joinCardinality`:

- semi/anti (`cardinality_estimator.cc:443-473`) **returns early**, overwriting `rows`
  entirely — the product form is computed and discarded;
- the outer stamp (`:475-480`) is guarded on `join_type == LEFT` and is reached only on the
  STANDARD path;
- `joinCardinality` (`:276-277`) itself is the only multiplicative rule, and the DP calls
  it directly (`join_enumeration.cc:305`) with neither the floor nor either stamp.

A node cannot take two branches: `semantics != STANDARD` returns before the LEFT test, and
both lowerings leave `join_type` at INNER for semi/anti
(`logical_plan.h:213-215` states this and no code contradicts it).

The non-multiplicative `max(l, r)` branch is *correct where it runs* — it is the FK
containment, and both consumers (the outer `max(rows, l_rows)` and the DP) tolerate it.
What is wrong is the input, not the rule: see H-1. Two smaller notes, both estimation-only:

- **L-1** (low). `cardinality_estimator.cc:471` assigns `node.estimated_rows = rows` for
  semi/anti **without** the >=1 floor the STANDARD path gets at `:365`. With an
  aggregate-topped body the right context is empty (`:511` returns `StatsContext{}`), so
  `frac` stays 1.0 and an ANTI join estimates exactly **0** rows. That 0 then defeats
  `flooredJoinCardinality`'s `left_rows >= 1.0` guard at `:296` for every join above it,
  same propagation as H-1. Week 32/33 territory rather than this seam; recorded because it
  is the same mechanism.
- **L-2** (low). `joinCardinality`'s `have_ndv` is set if **either** side supplies an NDV
  (`:259-264`), so "no statistics" really means "neither side has any". That is what makes
  H-1's derived relation take the multiplicative branch instead of the `max(l, r)` branch
  its own comment predicts. Not a bug in isolation; it is the reason the Week 34 comment at
  `join_enumeration.cc:454-462` is wrong.

### T5 — later guards on earlier invariants

Checked and holding:

- **`decompose` requires written order** (`join_enumeration.cc:170-185`, `leaves[join_slot]`
  indexed by slot, not spine position). Nothing between `LogicalPlanBuilder::build` and
  `JoinEnumeration::apply` perturbs it: `PredicatePushdown::distribute`
  (`predicate_pushdown.cc:270-326`) only attaches filters *onto* leaves and above the whole
  tree, never between two joins. `hasSlotOutsideRangeTable` stops at the first non-JOIN
  node on the spine (`:135`), which would miss a semi join buried under an interposed
  FILTER — but no pass can produce that shape, so it is not live.
- **`countRelations` counting the spine** (`:63-80`) versus `decompose`'s `leaves(n)`.
  Week 34's correlated-scalar rewrite hands its derived relation slot
  `range_table_size + out.lowered` (`subquery_decorrelation.cc:417`), which is exactly
  `n-1` for one scalar and stays in range for more, so the two agree. Moot in practice —
  that join is LEFT (`:434`) and `containsOuterJoin` declines it first.
- **ChunkPruner's `relation_slot < 1`** (`chunk_pruner.h:20-70`) versus a leftmost relation
  that is no longer slot 0. The vectorized builder withholds the hint on
  `!leftmost_is_slot0` (`vectorized_plan_builder.cc:434-443`) rather than delegating, and
  re-applies `pruningHintForPreservedSide` at every join down the spine, so a slot-1
  conjunct left in the residual by Week 29's pushdown decline
  (`predicate_pushdown.cc:301-303`) is stopped at the outer join it belongs to. Confirmed
  on the reordered BRAZIL query: leftmost is `customer@1`, and only `nation`'s own pushed
  filter shows `pruning=on`.
- **Pushdown across a LEFT join at depth.** `distribute` recurses into `children[0]`
  unconditionally (`:316`) and re-tests at each join, so a conjunct owned by a relation
  some *deeper* outer join null-supplies stops at that join. `σ_p(R) ⟕ S ≡ σ_p(R ⟕ S)`
  makes the preserved-side descent sound at any depth.
