# Seam audit: the join chain across weeks 26–36 — PASS 3

Scope: the joints between the join layers (single hash join -> multi-way DP -> semi/anti
-> derived tables as join inputs -> decorrelated subqueries lowered to joins).
Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `922ca15`.
Predecessors: `seam-join-chain-pass-1.md`, `seam-join-chain-pass-2.md`.

STATUS: in progress — written incrementally, never at the end.

---

## BLOCKER B3-1 — the deterministic tie-break is a function of the row's values **in schema order**, and `JoinEnumeration` PERMUTES that schema; `optimized != --no-optimize` on a `LIMIT` over a reordered join, and the difference reaches a scalar subquery's VALUE

**Severity: BLOCKER.** Reproduced on `data/tpch/sf0.01` in two forms. The second one turns
a plan-shape difference into a different number in the answer column.

### The shape

    SELECT c.c_name, o.o_orderkey
    FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey
                  JOIN nation n  ON c.c_nationkey = n.n_nationkey
    ORDER BY n.n_regionkey
    LIMIT 5

    columnar/vectorized, optimized        columnar/vectorized, --no-optimize
    ------------------------------        ---------------------------------
    Customer#000000002   1116             Customer#000000644   1
    Customer#000000002   2866             Customer#000000401   9
    Customer#000000002   3420             Customer#000000793   12
    Customer#000000002   3908             Customer#000000337   14
    Customer#000000002   4886             Customer#000000314   15

**Five rows, zero overlap.** Volcano cannot run this (`joins.size() > 1` refusal), so the
divergence is entirely inside the one engine that can, between the two legs the project
asserts equal.

### Why — the exact mechanism

`sort_comparator::rowLess` (`src/execution/sort_comparator.h:129-133`), when every declared
key ties, walks the row **in schema order**:

    const int n = std::min(schema.size(), ...);
    for (int i = 0; i < n; ++i) { int c = compareForTieBreak(a[i], b[i]); if (c) return c < 0; }

The header's own claim (`sort_comparator.h:51-54`):

> It is a function of the row's VALUES alone. It cannot consult arrival order, hash bucket
> layout, chunk boundaries, or which side built the hash table — precisely the things that
> differ between the legs.

That is true of *which* values it reads and false of *the order it reads them in*. The
order is `schema`'s, and `schema` here is the JOIN's merged schema, which
`JoinEnumeration::rebuild` builds **in the DP's chosen order**
(`src/planner/join_enumeration.cc:228-273`):

    std::vector<ColumnDef> merged = node->output_schema.columns();   // leftmost = order[0]
    ...
    for (ColumnDef c : rels[r].subtree->output_schema.columns()) { c.relation_slot = r; merged.push_back(c); }

`--explain` on the two legs, same query:

    optimized      LogicalJoin [c_custkey = o_custkey] order=customer@1,nation@2,orders@0
                                                       cost=46250 (written=76601) method=dp
                     merged schema = [c_custkey, c_name, c_nationkey, n_nationkey, n_regionkey,
                                      o_orderkey, o_custkey]
    --no-optimize  LogicalJoin [c_nationkey = n_nationkey]   (written order kept)
                     merged schema = [o_orderkey, o_custkey, c_custkey, c_name, c_nationkey,
                                      n_nationkey, n_regionkey]

and in both plans `VecSort [n.n_regionkey]` is the **immediate parent of the join** —
`LogicalPlanBuilder` puts SORT above the join and below the PROJECT, which is exactly the
placement the comparator's precondition paragraph relies on.

Same seven columns, permuted. A lexicographic order over a permuted tuple is a *different*
total order. Under `--no-optimize` the first tie-break column is `o_orderkey` (unique), so
the survivors are the five smallest order keys in region 0. Under the optimizer the first
column is `c_custkey`, so the survivors are the five smallest order keys **of the smallest
customer** in region 0. Both are legal SQL. The project asserts they are equal.

### The same defect as a wrong VALUE, not just a different row set

A `SCALAR` subquery keeps a user-written `LIMIT 1` (`subquery_materialization.cc:199-202`
caps at `min(user, 2)`, it never widens), and `main.cc:521` threads `args.no_optimize` into
the subquery runner, so the body is planned on the same leg as its parent:

    SELECT count(*) AS n FROM orders o2
    WHERE o2.o_orderkey < (SELECT o.o_orderkey
                           FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey
                                         JOIN nation n  ON c.c_nationkey = n.n_nationkey
                           ORDER BY n.n_regionkey LIMIT 1)

    body alone:   optimized -> 1116        --no-optimize -> 1
    whole query:  optimized -> n = 1115    --no-optimize -> n = 0

**1115 versus 0 in the answer column.** This is precisely the failure the tie-break was
introduced to close — `sort_comparator.h:37-39` records the original as "`COUNT(*)` = 977
versus 1536" — reproduced against the fix, with a cause the fix's own header lists at
`:32-34` and then does not neutralize:

> * `JoinEnumeration` reorders the join spine under the optimizer and not under
>   `--no-optimize`.

The fix removed that bullet's effect on *arrival order*. It left its effect on *schema
order*, and the tie-break is the one consumer that reads schema order as data.

### Why the gate is green anyway

The 119 `optimized == --no-optimize` regression checks pass because the shape needs all
four of: (a) three or more relations, so the DP runs at all (`MAX_DP_RELATIONS` floor); (b)
a DP order that differs from the written one; (c) `ORDER BY` whose declared keys tie across
rows the cut separates; (d) the SORT's input being a **raw join row** — a `GROUP BY` between
join and sort replaces the schema with `buildAggregateSchema`'s, which is mode-independent,
and that is what makes almost every TPC-H query immune. The one entry the tie-break fix
itself nominated as the tripwire (the `DISTINCT` entry in `ENGINE_AGREEMENT_QUERIES`, "still
the only entry whose sort input is a raw join row") is a **two**-relation join, which is
below the DP floor, so it cannot see this.

### Fix shape (minimum)

Do not reorder the merged schema — invariant 1 forbids it, the two contiguous output blocks
are `VecHashJoinNode`'s contract. Make the *comparator* order-independent instead: iterate
the tie-break columns in a canonical order derived from the schema rather than in storage
order, e.g. indices sorted by `(relation_slot, name)`, computed once per sort. That pair is
unique on any schema this can see — two relations cannot share a slot, and a single relation
cannot repeat a column name (catalog duplicate-name refusal `f72c6e1`;
`derivedRelationSchema`'s duplicate refusal) — and it is a binder-assigned value, so it is
identical on both legs no matter which order the DP picks. The paragraph at
`sort_comparator.h:69-73` ("THE PRECONDITION IT RESTS ON, STATED SO IT CAN BE CHECKED")
must be swept with it: the precondition is not "the sort's input row must be the same in
every mode", it is "the sort's input row must be the same **sequence** in every mode", and
the join spine is where that fails.

A regression entry for this needs a **three**-relation join with a tied `ORDER BY` and a
`LIMIT`, sorting over a raw join row — the existing tripwire is one relation short of
being able to fire.

---

## Part A — does pass 2's clean verdict survive fix round 2?

Verdict up front: **pass 2's clean verdict on the join chain survives three of the four
changes intact. The fourth — the tie-break — did not create B3-1, but it is the change that
turned a "both answers are legal" latitude into an invariant the engine now claims to hold
and does not.**

### A.1 — the scan-schema narrowing (`70570dc` + `074aeb7`): no join-side consumer depends on scan width — CLEAN

The change is confined to `Planner::plan` (`src/planner/planner.cc:189-237` for `narrowRows`,
`:288-291`, `:317-320`, `:325-329` for the three call sites), i.e. to the **Volcano** path,
which by its own refusals holds at most one join, no derived table and no subquery. I walked
every join-side consumer of a scan's schema on that path:

| consumer | reads | width-dependent? |
|---|---|---|
| `HashJoinNode` key resolution | bare column NAME against each child schema (`planner.cc:342-350` builds `from_cols`/`join_cols`) | no — and `buildScanSchema` collects `stmt.joins[i].condition` (`logical_plan.cc:332`), so a key can never be narrowed away |
| merged output schema | `node->outputSchema().columns()` ++ right's, stamped slot 1 (`:352-359`) | narrower on both sides now, symmetric with the columnar leg |
| ON-residual type check + evaluation | `merged_schema` (`:382`) | no — an INNER join's residuals are folded into `stmt.where` (`:171-177`) **before** `buildScanSchema` runs at `:186`, and `cloneExpr` leaves `jc->condition` intact, so the residual's columns are collected twice over |
| build/probe swap | `from_row_count` / `join_row_count` — row COUNTS, captured before the moves (`:240`, `:304-308`) | no |
| pruning hint | `preserved_slots` derived from `scan_schema`'s slots, all 0 | no |
| `SeqScanNode` row path | `&rows_[cursor_]` verbatim | **yes, and this is the reason the fix narrows rows and not just the schema** |

Three edges checked rather than assumed:

- **Row width can never disagree with the catalog schema.** `csv_loader.cc:44-50` refuses a
  line whose field count differs, so `r[i]` in `narrowRows` is always in range. This is the
  one place the change could have introduced an out-of-bounds read, and the loader closes it.
- **`keep` is ascending and each index is moved exactly once.** `narrowSchema`
  (`logical_plan.cc:72-79`) iterates `full` in order and pushes matches, so the narrowed
  schema is a *subsequence* of the catalog schema, never a permutation. (Had it been driven
  by the `unordered_set<string> required`'s iteration order it would have been
  non-deterministic across runs — it is not.)
- **Self-join.** `self_join_rows` is copied at `:249-253`, before the FROM scan's move at
  `:290`, and both sides go through `narrowRows` against the same `meta.schema`; for a
  self-join `right_scan_schema` and `scan_schema` are two calls to `buildScanSchema` with
  identical arguments, so the two legs of the join are narrowed identically.
- **Two tables sharing a column name.** `buildScanSchema` narrows by BARE name over one flat
  schema, so `l.team` keeps `drivers.team` in the drivers scan too. Over-inclusion, which is
  the safe direction; verified live (A9 below).

Run: 12 join shapes across all four legs (`row/volcano`, `columnar/volcano`,
`columnar/vectorized`, `columnar/vectorized --no-optimize`), positional TSV, sort-normalised —
`SELECT *` + join, narrow join, self-join, `LEFT JOIN` with and without a residual, an INNER
residual that gets lifted, tied `ORDER BY` + `LIMIT` over a raw join row, `GROUP BY`/`HAVING`
over a join, two relations sharing a column name, an `ORDER BY` column absent from the select
list, and an expression `GROUP BY` key. **12 of 12 agree on all four legs.**

The narrowing does what the commit claims: the `row/volcano` leg now tie-breaks over the same
column set as the columnar legs (shape A7 — a tied `ORDER BY l.season` with `LIMIT 5` over a
raw join row — agrees across all four, which is exactly the case that was luck before). It is
also the reason B3-1 needs **three** relations: at two the DP does not run, so the merged
schema is the written one on every leg.

### A.2 — the deterministic tie-break: see BLOCKER B3-1 above

The fix is correct for what it addresses (arrival order, build side, chunk boundaries) and
`sort_comparator.h`'s properties list is accurate about *values*. It is silent about the
*sequence* those values are read in, and the join spine is the one place that sequence is
plan-dependent. Full writeup at the top of this file.

One consequence worth naming separately, because it is the standing-rule sweep failure that
kept B3-1 invisible: **`python_tools/random_diff.py` — the one randomized
`optimized == --no-optimize` differ in the tree — deliberately refuses to generate the shape.**
Its "TRAP 1" docstring (`random_diff.py:29-38`):

> an `ORDER BY` with TIES makes the comparison order-sensitive on rows whose order SQL does
> not specify, and a reordered join breaks those ties differently — a FALSE FAILURE that
> looks like an optimizer bug ... So the generator emits either no `ORDER BY`, or a TOTAL one
> (a unique tiebreak column appended).

That reasoning was correct before `7b84952`. Since `7b84952` a reordered join breaking a tie
differently is no longer a false failure — it is precisely the failure the tie-break exists to
prevent, and the tool that would have generated B3-1 in its first batch is the one still
configured not to. Sweeping this is part of B3-1's fix, not a separate finding.

### A.3 — `reachesOutsideThisBody` (`8ce4ebf`) upstream of join-key selection — CLEAN

The change makes a conjunct whose only `-1` came from a nested **correlated** `SubqueryExpr`
classify as body-**local** instead of correlated. The join-chain risk is the asymmetric one:
a conjunct that *should* have become a join key being kept in the body, which would be a
wrong ANSWER rather than a refusal. It cannot happen, for a structural reason:
`SuppressNestedCorrelation` (`subquery_decorrelation.cc:87-102`) clears only
`SubqueryExpr::correlated`, and an outer `ColumnRef` produces its `-1` through a completely
different branch of `collectSlots` — so any conjunct carrying a real correlation still
reaches `splitCorrelation`'s key path.

The residual case is a conjunct whose *only* outer reference lives inside the nested body
(`EXISTS (SELECT 1 FROM laps l WHERE EXISTS (SELECT 1 FROM laps l2 WHERE l2.driver_id = d.driver_id))`,
no level-1 key at the middle level). That now yields zero keys, and the pass refuses by name
rather than building a keyless join:

    Error: correlated subquery: no equality links the subquery to the enclosing query,
           so there is no join key to decorrelate on

and the depth guard fires on the level-2 route, as the commit claims. Ran the three-way
differential (optimized / `--no-optimize` / SQLite) on 8 shapes that put the changed
classification directly upstream of a join: nested body-local `EXISTS`, nested correlated
scalar, a nested `EXISTS` whose body itself joins, `NOT EXISTS` with a nested `EXISTS`,
nesting under an uncorrelated `IN`, and semi / anti / correlated-scalar lowerings sitting on
a **three-relation** outer spine (so the DP runs above them). **8 of 8 agree three ways.**

### A.4 — `$scalarN` marked `hidden` (`de45779`) vs join planning — CLEAN

`hidden` has exactly one meaning (`common/schema.h:22-38`): star synthesis skips the column,
resolution never consults it. Three readers, all named in that comment
(`logical_plan.cc:1113`, `planner.cc:431`, `logical_plan.cc:491`), none of them in join
planning. The two places join planning touches column *width* are both correct:

- `JoinEnumeration::rebuild` copies whole `ColumnDef`s **by value**
  (`join_enumeration.cc:269`, and its comment says so explicitly), so `hidden` survives
  reordering and a `$scalarN` column cannot become visible by being moved;
- the vectorized builder's build-side cost reads real materialised widths, and a hidden
  column *is* materialised, so counting it is right, not wrong.

The `$scalarN` join is a LEFT join (`subquery_decorrelation.cc`), so `containsOuterJoin`
declines the whole tree before `rebuild` could reach it anyway — two independent reasons.

Run: 6 shapes putting a `hidden` column inside a join's merged schema — `SELECT *` over a
2-relation and a **3**-relation join each carrying a correlated scalar, `SELECT *` over a join
with `EXISTS` and with `IN`, **two** correlated scalars over one join (so two synthetic
relations widen the same schema), and a correlated scalar over a join whose other input is a
derived table. **6 of 6 agree three ways**, including the column COUNT of `SELECT *`
(14 columns on the 2-relation shape, 19 on the 3-relation one — SQLite's numbers exactly).

---

## Part A′ — pass 2's three open MEDIUMs, re-ranked on the moved tree

(continues below)
