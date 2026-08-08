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

(continues below)
