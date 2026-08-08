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
