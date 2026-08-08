# Seam audit — optimizer result preservation (pass 3, final)

HEAD `922ca15`, branch `claude/phase5-week26-qomtkb`.
Predecessors: `seam-optimizer-preservation-pass-1.md`, `-pass-2.md`.

Status: IN PROGRESS (written incrementally; summary block at end).

## BLOCKER B3-1 — `optimized` and `--no-optimize` return DIFFERENT ROWS. The fix-round-2 sort tie-break rests on a precondition `JoinEnumeration::rebuild` breaks by design.

This is the thing the seam exists to forbid, and it is reproducible from the CLI
on a shipped catalog. Run on `data/tpch/sf0.01/catalog.json`,
`--execution vectorized --storage columnar --no-cache`:

```sql
SELECT n.n_name, s.s_suppkey
FROM supplier s
JOIN nation n ON s.s_nationkey = n.n_nationkey
JOIN region r ON n.n_regionkey = r.r_regionkey
ORDER BY n.n_regionkey
LIMIT 5
```

```
optimized                     --no-optimize
n_name   s_suppkey            n_name      s_suppkey
ALGERIA  5                    ALGERIA     5
ALGERIA  25                   MOZAMBIQUE  17
ALGERIA  40                   MOROCCO     20
ALGERIA  46                   MOROCCO     23
ALGERIA  61                   ALGERIA     25
```

Four of five rows differ. Not a formatting difference, not an ordering
difference the harness's `sorted()` would absorb: the ROW SETS differ.

### The mechanism, and it is exactly the premise the fix wrote down

Drop the `LIMIT` and the two legs return **the same 100 rows in a different
order** (`diff` of both outputs sorted: identical; `head -8` of both, unsorted:
different from row 2 on). So the row set is preserved by join enumeration; what
is not preserved is the ORDER the sort produces, and `LIMIT` turns an order
difference into a set difference.

The order comes from the tie-break commit `7b84952` added
(`src/execution/sort_comparator.h:119-137`, `rowLess`):

```cpp
// every declared key ties — see the header comment
const int n = std::min(schema.size(), ...);
for (int i = 0; i < n; ++i) {
    int c = compareForTieBreak(a[i], b[i]);
    if (c != 0) return c < 0;
}
```

The tie-break is **positional**: it walks the sort input's schema in COLUMN
ORDER. Its header states its own precondition, in these words:

> THE PRECONDITION IT RESTS ON, STATED SO IT CAN BE CHECKED: the sort's INPUT
> row must be the same in every mode.

`JoinEnumeration::rebuild` (`src/planner/join_enumeration.cc:214-278`) breaks it,
and says so in its own comment:

```cpp
// merged schema: [left block] ++ [this relation's columns, stamped r].
// Order matches VecHashJoinNode's two contiguous output blocks — a
// slot-sorted "canonical" order is not available (invariant 1).
```

The merged schema is built **in the chosen order**, not the written one. For the
query above `--explain` reports
`order=nation@1,region@2,supplier@0 cost=303 (written=436) method=dp`, so:

| leg | sort input schema, in order |
|---|---|
| optimized | `n_nationkey, n_name, n_regionkey, r_regionkey, s_suppkey, s_nationkey` |
| `--no-optimize` | `s_suppkey, s_nationkey, n_nationkey, n_name, n_regionkey, r_regionkey` |

`ORDER BY n.n_regionkey` ties across the 20 suppliers of region 0. The tie-break
then compares `n_nationkey` first in one leg and `s_suppkey` first in the other.
Two different total orders, two different first five rows.

The row values are the same in both legs. The COLUMN ORDER of the row is not,
and the tie-break is a function of column order. The precondition says "the
same row"; what it needed to say is "the same row **in the same column order**",
and that is false for any query whose sort sits directly above a reordered join.

### Three controls, run, that isolate it to exactly this cause

| control | change | result |
|---|---|---|
| write the FROM list in the order the DP would choose (`FROM nation n JOIN region r JOIN supplier s`), so `order=nation@0,region@1,supplier@2` == written | no reorder, so no schema permutation | **SAME** |
| make the declared key a total order: `ORDER BY n.n_regionkey, s.s_suppkey LIMIT 5` | tie-break never reached | **SAME** |
| put a `GROUP BY` between the join and the sort (`SELECT n.n_name, COUNT(*) ... GROUP BY n.n_name ORDER BY COUNT(*) LIMIT 5`) | sort input is `buildAggregateSchema`'s output, which is plan-independent | **SAME** |

So the failing shape is precisely: **≥ 3 relations (so `JoinEnumeration` runs at
all, `MIN_ENUMERATED_RELATIONS = 3`) + the DP picks a non-written order + the
sort sits directly above that join (no aggregate between) + `ORDER BY` is not a
total order on the surviving columns + the tie is not immaterial.** `LIMIT` is
not required for divergence (the row ORDER already differs); it is required for
the divergence to survive a set comparison.

### Mode census

| mode | verdict |
|---|---|
| `columnar` + `vectorized` | **DIVERGE** |
| `columnar` + `volcano` | SAME — `Error: multi-way joins are not supported on the Volcano path` in both legs |
| `row` + `volcano` | SAME — same refusal |
| `row` + `vectorized` | SAME — `--execution vectorized requires --storage columnar`, refused in both legs |

col-vec is the only mode where a 3-relation join and `JoinEnumeration` coexist,
and it is the mode the invariant harness uses. There is no cross-ENGINE
divergence here, because Volcano cannot run the shape at all.

### Why 119 invariant checks and 0 divergences did not see it

Two independent reasons, both structural:

1. **`run_optimizer_invariant` compares SORTED rows.**
   `python_tools/test_new_queries.py:496-514` — `normalize()` ends
   `return sorted(tuple(...) for row in rows)`. A pure ORDER divergence is
   invisible to it by construction. Only the `LIMIT` variant, where the SET
   changes, would be caught.
2. **The invariant suite runs against `catalog.json`, which has TWO tables.**
   Every query in it that joins is a 2-relation join, and `reorder` returns
   unchanged at `join_enumeration.cc:444` (`n < MIN_ENUMERATED_RELATIONS`, = 3).
   A 3-relation shape needs a self-join or a derived relation; the ones present
   (`self_join_where_both`) are still 2 relations.

So the invariant's "0 divergences" is not evidence about this shape — the suite
contains no query that can reach the code path.

### Ranking

**BLOCKER.** `optimized != --no-optimize` on a query the CLI accepts, on a
catalog that ships in the repo, in the only mode where both the optimizer and
a multi-way join run. Every answer is legal SQL in isolation (SQL does not fix
the order when `ORDER BY` is not total) — and that is exactly the argument
`sort_comparator.h` already rejects, in its own words: *"The project asserts
`optimized == --no-optimize`, so that is a defect even though every one of those
answers is legal SQL."* The tie-break commit accepted that obligation and then
did not discharge it for the one rewrite that permutes the schema.

### What a fix has to decide (stated, not prescribed)

The tie-break needs an ordering of columns that is a function of the QUERY, not
of the plan. `rebuild` already stamps every merged column with its binder slot
(`c.relation_slot = order[k]`), and the written order is by definition slot
order — so `(relation_slot, position-within-relation)` is available at the sort
and is plan-independent. `rebuild`'s comment says a slot-sorted schema cannot be
MATERIALIZED (invariant 1: the join's two output blocks must stay contiguous),
but the tie-break does not need the schema reordered, only a comparison ORDER —
which is a permutation computed once in `rowLess`'s caller. That is a claim
about feasibility, not a patch; I touched no source.

### B3-1b — the amplification: `materializeSubqueries` turns the order divergence into a VALUE divergence, in a query with no `ORDER BY` and no `LIMIT` of its own

The "every answer is legal SQL" defence dies here. `data/tpch/sf0.01/catalog.json`,
col-vec:

```sql
SELECT n_name, n_nationkey FROM nation
WHERE n_nationkey = (
  SELECT n.n_nationkey
  FROM supplier s
  JOIN nation n ON s.s_nationkey = n.n_nationkey
  JOIN region r ON n.n_regionkey = r.r_regionkey
  WHERE s.s_suppkey > 0
  ORDER BY n.n_regionkey DESC
  LIMIT 1)
```

```
optimized                      --no-optimize
n_name  n_nationkey            n_name        n_nationkey
EGYPT   4                      SAUDI ARABIA  20
```

One row either way; a **different** row. The outer block has no `ORDER BY`, no
`LIMIT`, and nothing unspecified about it. `--explain` shows the mechanism
directly — the scalar subquery is materialized to a constant before planning
(`main.cc:500`, `materializeSubqueries`), and the constant differs:

```
optimized      LogicalFilter [(n_nationkey = 4)]
--no-optimize  LogicalFilter [(n_nationkey = 20)]
```

The nested runner threads the flag (`main.cc:519`,
`runVectorizedToRows(..., args.no_optimize)`) precisely so the sub-plan is
tested too — so the body is optimized in one leg and not the other, its
`LIMIT 1` cut lands on a different row for exactly the reason in B3-1, and the
materialized `Literal` carries the difference into a query that has no
order-dependence at all.

Body in isolation, `... ORDER BY n.n_regionkey DESC LIMIT 1`: optimized returns
`n_nationkey = 4`, `--no-optimize` returns `20`.

`sort_comparator.h`'s header already names this amplification as the reason the
tie-break was written — *"a scalar subquery of the same shape — materialized to
a `Literal` — turned that into `COUNT(*)` = 977 versus 1536"*. It closed the
cross-ENGINE instance and left the optimizer instance open.

Same BLOCKER, same root cause; recorded separately because it is the shape that
makes the severity unarguable.

## HIGH B3-2 — `orderByWork` reorders conjuncts on an estimate, and predicate evaluation is NOT total. The optimizer both MASKS and INTRODUCES a per-row error.

`src/planner/predicate_pushdown.cc:239-255`:

```cpp
// Order conjuncts most-selective-first (smallest keep-fraction). Stable so ties
// and stat-less predicates (fallback selectivity) keep their original order.
void orderByWork(std::vector<std::unique_ptr<Expr>>& conjuncts, ...) {
    std::stable_sort(conjuncts.begin(), conjuncts.end(), [&](a, b) {
        return CardinalityEstimator::selectivity(a.get(), ctx)
             < CardinalityEstimator::selectivity(b.get(), ctx); });
}
```

`orderByWork` is called only from `filterOnto` and `PredicatePushdown::apply`,
both inside the `--no-optimize` gate. So conjunct ORDER differs between the two
legs. The columnar predicate cascade short-circuits — `columnar_eval.cc:143-146`,
AND evaluates the right conjunct only over the left's survivors — and per-row
evaluation **can throw**. Reordering therefore decides whether a throwing
conjunct is ever reached.

Both directions are reachable from the CLI on the shipped `catalog.json`,
`--execution vectorized --storage columnar`.

**Direction 1 — the optimizer MASKS an error.**

```sql
SELECT team FROM laps
WHERE SUBSTRING(team, lap_id - lap_id, 2) = 'x' AND speed = 333.3333
```
```
optimized       (0 rows)
--no-optimize   Error: SUBSTRING: start position must be >= 1
```
`--explain`: written `[(SUBSTRING(...) = x) AND (speed = 333.3333)]`, optimized
`[(speed = 333.3333) AND (SUBSTRING(...) = x)]`. `speed = 333.3333` has a real
NDV so its selectivity is ~1e-4; the SUBSTRING conjunct is a `=` with no
`col op lit` shape, so it gets `FALLBACK_EQ_SELECTIVITY = 0.1`
(`cardinality_estimator.h:14`, used at `:159`). The speed conjunct sorts first,
keeps zero rows, and the SUBSTRING is never evaluated.

**Direction 2 — the optimizer INTRODUCES the error.**

```sql
SELECT team FROM laps
WHERE team LIKE 'zzz%' AND SUBSTRING(team, lap_id - lap_id, 2) = 'x'
```
```
optimized       Error: SUBSTRING: start position must be >= 1
--no-optimize   (0 rows)
```
`--explain`: written `[team LIKE 'zzz%' AND (SUBSTRING(...) = x)]`, optimized
`[(SUBSTRING(...) = x) AND team LIKE 'zzz%']`. `LIKE` deliberately has no
selectivity rule (`cardinality_estimator.cc:120`) and takes
`FALLBACK_SELECTIVITY = 0.5`; the SUBSTRING conjunct's 0.1 sorts ahead of it, so
a predicate that matched nothing is demoted below one that throws.

This is the second direction that matters: the first can be argued as the
optimizer being kinder, the second is the optimizer manufacturing a failure on
a query that succeeds without it.

### Mode census

| mode | optimized | `--no-optimize` |
|---|---|---|
| `columnar` + `vectorized` | **Error** | 0 rows |
| `columnar` + `volcano` | Error | Error |
| `row` + `volcano` | Error | Error |
| `row` + `vectorized` | refused (needs columnar), both legs | — |

Volcano evaluates the whole WHERE per row with no cascade, so it throws in both
legs; col-vec is where the divergence lives.

### The precondition, and that it is BELIEVED rather than checked

`orderByWork`'s comment argues only about COST — *"'Expected work' is modeled as
selectivity only — optimal when per-predicate eval cost is uniform"* — and
defers a cost-weighted ranking to Week 28. It never states the correctness
precondition a reordering needs, which is that **conjunct evaluation is total**:
that no conjunct can fail on a row another conjunct would have removed. That
precondition is false today and has been since Week 25 added `SUBSTRING`, whose
`substringOf` raises per row for a computed start < 1 (`evaluator.cc`; the
plan-time twin at `logical_plan.cc:234-243` catches only the *constant* case, and
says so: *"A computed position still raises at execution"*).

Every other per-row raise is in the same class. `checkedArith` overflow throws;
integer division by zero does **not** (it yields NULL — verified:
`SELECT 100 / (lap_id - lap_id) FROM laps LIMIT 1` prints `NULL`), so the
division case is safe by accident rather than by rule.

### Why the harnesses do not see it

`run_optimizer_invariant` catches an exception from either leg and records an
ERROR — so this shape WOULD be caught if it were in the suite. It is not: no
query in `test_new_queries.py` puts a computed-argument `SUBSTRING` in a `WHERE`
alongside a second conjunct. `grep -rn "SUBSTRING" python_tools/` finds the
function only in select lists and in fully-constant argument positions.

### Ranking

**HIGH, not BLOCKER.** `optimized != --no-optimize` on a CLI-typable query on
the shipped catalog, in both directions, and the failing side is loud rather
than silently wrong — an error is not a wrong answer. It is HIGH rather than
MEDIUM because direction 2 means the optimizer can turn a working query into a
failing one, which is the strongest form of "not result-preserving" short of a
wrong row.
