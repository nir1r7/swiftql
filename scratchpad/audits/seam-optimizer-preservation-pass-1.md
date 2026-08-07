# Seam audit — optimizer result preservation across Phase 5 (pass 1)

Scope: is `optimized ≡ --no-optimize ≡ SQLite` across everything weeks 26–36 added,
and are the estimates good enough on multi-join plans to be choosing sensibly?

Binary used: the pre-existing `build/swiftql` (no rebuild, four auditors concurrent).

## 1. Result preservation — CLEAN

Existing harnesses, run as-is:

- `python_tools/compare_against_sqlite.py` — **1326 passed, 0 failed, 0 errors**.
  Its mode census confirms the four-mode oracle (`col-vec` vs `col-vec --no-optimize`
  vs SQLite) covers 168 queries in all four modes and 99 in the two vectorized modes
  (MULTIWAY 16, W31 1, W32 semi 20, W33 decorrelated 18, W34 correlated-scalar 29,
  W34 derived 10, W35 subquery-in-derived-body 5).
- `python_tools/run_tpch.py` — see §6.

**Cross-week shapes the harnesses do not hold.** I built 15 queries that place a
LATER week's construct inside a plan the optimizer (written earlier) reorders, and
diffed optimized vs `--no-optimize` vs SQLite. Script:
`/tmp/.../scratchpad/cross.py` (reproduced below in §5). All 15 preserve results:

| id | shape | rows | verdict |
|---|---|---|---|
| d3a/d3b/d3c | derived relation inside a 3-way join (W34 × W28) | 24 / 2261 / 20 | identical |
| s3a/s3b | semi / anti join above a 3-way inner block (W32 × W28) | 13 / 16 | identical |
| e3a/e3b | decorrelated EXISTS / NOT EXISTS above a 3-way block (W33 × W28) | 13 / 16 | identical |
| o3a/o3b/o3c | LEFT JOIN inside a 3-way block, incl. a null-supplying-side predicate | 24 / 24 / 21 | identical |
| od1 | outer join INSIDE a derived body, inner 3-way block outside (W34 × W29) | 29 | identical |
| sd1/sd2 | semi join inside a derived body / above a derived join (W32 × W34 × W28) | 18 / 20 | identical |
| q4a | 4-way join with a derived relation (DP boundary) | 19 | identical |
| c3a | correlated scalar in SELECT + 3-way join | — | refused symmetrically in both modes (`SELECT: subqueries are supported in WHERE and HAVING only`) — not a divergence |

**No result divergence found anywhere.** Target 1 is clean.

## 2. The declines — COMPLETE, one of them silent where it costs plan quality

Verified by reading `src/planner/join_enumeration.cc` and confirming against `--explain`:

- `containsOuterJoin` (join_enumeration.cc:91) recurses into **every** child, so it
  also sees an outer join buried in a derived body or a semi-join body. That is
  over-declining, i.e. safe. Confirmed on `o3a`/`o3b`: `join-ordering=skipped (outer join)`.
- `hasSlotOutsideRangeTable` (join_enumeration.cc:133) catches `join_slot == -1`
  (semi/anti) and unbound `from_slot`. Confirmed on `s3a`: the whole 3-way inner
  block below the `LogicalSemiJoin` is returned untouched, no `order=` line.
- `countRelations` (join_enumeration.cc:63) counts the SPINE, treating DERIVED as
  one relation and skipping a semi/anti body — verified on `d3a` (n=3, DP ran) and
  `q4a` (n=4).
- Pushdown's null-supplying-side decline holds: `o3c` (`dr.age > 30 OR dr.age IS NULL`
  above a LEFT JOIN) matches SQLite in both modes.

**LOW — observability, not correctness.** The outer-join decline is *reported*
(`join-ordering=skipped (outer join)`) precisely because "a decision was available and
was refused"; the `join_slot == -1` decline is *silent* by the same code's own
argument that no decision was available. But it is not analogous any more. On `s3a`
the block below the semi join is a **fully inner 3-relation tree that the search
could have reordered** — the whole query loses join ordering because an unrelated
`IN` subquery sits above it, and `--explain` shows nothing at all. That is exactly
the loss Week 29 added its decline string for. Input: any query of the form
`FROM a JOIN b JOIN c WHERE x IN (SELECT …)`.
Site: `src/planner/join_enumeration.cc:463` (`if (hasSlotOutsideRangeTable(...)) return node;`).

## 3. Estimate quality — one real defect

### HIGH — `CardinalityEstimator::estimateNode` has no `DERIVED` case, so every estimate at or above a derived relation is negative, and join enumeration reads it as ZERO rows

`src/planner/cardinality_estimator.cc:308-533`: the `switch (node.type)` has cases for
SCAN, FILTER, JOIN, AGGREGATE, PROJECT/SORT/DISTINCT and LIMIT. There is **no
`LogicalNodeType::DERIVED` case**, so a `LogicalDerived` falls through to the
`return StatsContext{}` at line 532 and its `estimated_rows` keeps the
`logical_plan.h:63` default of **-1.0**.

That -1 then propagates:

1. `estimateNode`'s JOIN case reads `r_rows = node.children[1]->estimated_rows` = -1
   (cardinality_estimator.cc:363) and feeds it to `joinCardinality`, giving a negative
   product. `flooredJoinCardinality` (cardinality_estimator.cc:296) only applies the ≥1
   floor `if (left_rows >= 1.0 && right_rows >= 1.0)`, so the negative passes through
   unclamped. PROJECT/SORT/LIMIT copy the child, so the negative reaches the root.
2. `src/cli/main.cc:303`/`:329` print `est=` only when `estimated_rows >= 0.0`, so the
   defect is **invisible** — the column just goes blank and reads as "the estimator
   didn't run".
3. `src/planner/join_enumeration.cc` `rels[r].rows = std::max(leaves[r]->estimated_rows, 0.0)`
   turns the -1 into **0 rows** for the derived leaf. The DP therefore believes a
   derived relation is free to join first, whatever its true size.

**Grounding query** (all shapes CLI-typable, `--storage columnar --execution vectorized`):

```sql
SELECT dr.name AS n, x.lap_id AS lid
FROM drivers dr
JOIN laps l ON l.driver_id = dr.driver_id
JOIN (SELECT lap_id, driver_id FROM laps WHERE speed > 200) AS x
  ON x.driver_id = dr.driver_id
WHERE l.lap_id < 5 ORDER BY n, lid LIMIT 5
```

`--explain` reports `order=drivers@0,@2,laps@1 cost=24 (written=35) method=dp`, and
every node from `LogicalDerived` upward (both `LogicalJoin`s, the `LogicalSort`, the
`LogicalProject`) carries **no `est=` at all**, while the untouched leaves carry
`est=20`, `est=4`, `est=10000`.

`--explain-analyze` on the chosen order:

| node | est | actual rows_out | error |
|---|---|---|---|
| `VecDerived [x]` | none (-1; used as **0** by the search) | **10000** | ∞ / 10000× |
| `VecHashJoin [driver_id = driver_id]` (drivers ⋈ x) | none | **10000** | unestimated |
| `VecHashJoin [driver_id@0 = driver_id]` (top) | none | 2004 | unestimated |

The plan the search **rejected** — the written order `drivers, laps, x`, which it
scored at 35 against its own 24 — builds a **4-row** intermediate at the middle join
(`VecFilter [(l.lap_id < 5)] rows_out=4`) instead of 10000. The chosen order's
middle intermediate is **2500× larger** than the alternative's, and the cost model
preferred it only because the derived relation was priced at zero rows. The two joins
it chose account for 60% of execution time (41.7 ms + 38.4 ms of a ~133 ms run).

This also degrades the *physical* join choice: on any tree containing a derived
relation the `VecHashJoin` lines carry no `build=`/`cost=`/`algo=` annotation at all
(compare `s3a`, where every join prints `build=drivers cost=32 (alt=43) algo=simd (hash=70)`),
because `from_est` is negative. Same root cause.

Not a correctness bug — §1 shows results are preserved — but it is exactly the
"cost model that picks a bad plan and fails no test" the audit asks for. The fix is
one `case LogicalNodeType::DERIVED:` that estimates the body and stamps
`node.estimated_rows = body_rows`; the machinery already exists
(`estimateSubtree` is public and the body is an ordinary logical tree).

### Lesser estimate errors seen on multi-join plans (recorded, not blockers)

- Semi join (`s3a`): `VecSemiHashJoin` est=29 vs `rows_out=13` — 2.2× over. Week 32's
  `min(1, ndv_r/ndv_l)` rule with `ndv_r == ndv_l == 20` gives `frac = 1.0`, so the
  semi join is estimated as a no-op filter. Systematic on every equi-semi-join whose
  two key NDVs match; direction is conservative (over).
- `VecScan [laps] chunks_skipped=1/2` prints `est=10000` while `rows_out=8192`: the
  scan estimate is the full table stat and does not account for zone-map pruning.
  1.22× here, cosmetic, but it means the leaf `rows` the search costs with is the
  unpruned count.

## 4. Written-order fallback and `method=` honesty — CORRECT

`method=` is derived at `join_enumeration.cc:554` from the two facts that actually
determine the printed order (`searched`, `kept_written`), not from which branch was
attempted, so it cannot report `dp` for an order the DP did not produce. Observed
`method=dp` on `d3a`, the §3 query and `q4a`; the `written_cost < chosen_cost` floor
at `join_enumeration.cc:539` is what keeps the derived-relation misestimate in §3
from being a *correctness* problem — the search may only install an order its own
model scores no worse than the written one. That guard is doing real work now
(the comment at :456 predicted this) and it held on every query I ran.
I did not observe `method=written-floor` or `method=written-fallback` fire on any
query I could construct; that is consistent (with `rows=0` for derived leaves the
search's pick is always scored cheaper), not evidence of a bug.

## 5. Fields a later week repurposed — no leak found

- `join_slot == -1` (W32) is read only on the STANDARD path in the estimator
  (cardinality_estimator.cc:405) and is the decline trigger in enumeration
  (join_enumeration.cc:136). Both correct.
- `countRelations` counting scans vs. spine (the W34 correction) is the one place a
  repurposed field did leak, and it is already fixed; I re-verified it on `d3a`
  (derived = 1 relation) and `s3a` (semi body not counted).
- `estimated_rows`'s `-1` sentinel is the one that IS being read as data — see §3.

## 6. TPC-H

`python_tools/run_tpch.py` — exit 0, **no failures, no divergences**. 88 query x mode
cells; 20/22 meaningful vs SQLite (5 in all four modes, 15 vectorized-only), 34 Volcano
refusals pinned by message. Gate reports `NO-BASELINE` (it needs
`--baseline docs/tpch-baseline.json` to gate), not a failure. Q22's fingerprint
confirms the Phase 5 stack is live in it: `{'LogicalDerived': 2, 'LogicalAntiJoin': 2}`
— a W34 derived half and a W33 NOT-EXISTS anti-join half in one query, results
preserved.

## 7. Not reached

- No estimate-vs-actual sweep over the full TPC-H multi-join set (correctness was
  run and is clean; only the two-table catalog shapes above were profiled for
  estimate quality). The §3 defect is expected to be worse there — Q13/Q15/Q22 are
  derived-relation joins, and Q22 confirms two `LogicalDerived` nodes in one plan.
- Did not construct an input that makes `method=written-floor` or
  `method=written-fallback` fire; §4 argues both are consistent with correct code
  rather than dead, but that is an argument, not a measurement.
- Did not audit `constant_folding.cc` or `subquery_materialization.cc` for
  result preservation beyond what the two harnesses and the 15 cross-week queries
  exercise.
