# Seam audit: the join chain across weeks 26–36 — PASS 3

Scope: the joints between the join layers (single hash join -> multi-way DP -> semi/anti
-> derived tables as join inputs -> decorrelated subqueries lowered to joins).
Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `922ca15`.
Predecessors: `seam-join-chain-pass-1.md`, `seam-join-chain-pass-2.md`.

STATUS: COMPLETE.

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

## BLOCKER B3-2 — Week 29's STRING-vs-numeric join-key refusal was written into the `stmt.joins` loop and never extended to the three join producers that arrived after it; a semi/anti/decorrelated join key silently half-matches, and the answers disagree with SQLite in both directions

**Severity: BLOCKER.** A silent wrong answer on a query the engine accepts, in both
vectorized modes, against the SQLite oracle. Reproduced column-to-column on an ordinary
schema.

### The rule, and where it lives

`src/planner/validator.cc:284-318` (Week 29, deferred from the Week 27 audit) states the
problem exactly:

> A join key is compared as TEXT, which carries no type tag: a STRING "7" matches an INT 7
> while "007" does not, while the identical predicate in a WHERE clause throws Type
> mismatch — half a match, with no error either way, and both halves reachable on the
> shipped catalog (drivers.team vs laps.lap_id). Under an outer join the unmatched half
> comes back as null-extended rows rather than as missing ones, which is why it is closed
> here.

and closes it with

    for (const JoinKey& k : on.keys) { ... if (l_str != r_str) throw ...; }

That loop is inside `for (size_t i = 0; i < stmt.joins.size(); ++i)`. **`stmt.joins` is one
of four producers of a `JoinKey` in this engine.** The other three are
`subquery_lowering.cc` (`IN` / `NOT IN` -> `LogicalSemiJoin` / `LogicalAntiJoin`),
`subquery_decorrelation.cc::splitCorrelation` (correlated `EXISTS` / `NOT EXISTS`), and
`subquery_decorrelation.cc`'s correlated-scalar rewrite (the `$scalarN` LEFT join). None of
the three is covered, and all three shipped *after* Week 29.

### The failing shapes

Data (`scratchpad`-local, two CSVs — a zero-padded STRING id column against an INT one,
which is an entirely ordinary schema and is why the shipped catalogs cannot see this):

    ids(code STRING, label STRING) = ('016','alpha'), ('17','beta'), ('0018','gamma'), ('19','delta')
    nums(n INT, tag STRING)        = (16,'sixteen'), (17,'seventeen'), (18,'eighteen'), (20,'twenty')

| # | query | SwiftQL (both vec modes) | SQLite |
|---|---|---|---|
| K1 | `FROM ids i JOIN nums m ON i.code = m.n` | **refused** — "cannot join a STRING column with a numeric one" | 3 rows |
| K9 | `FROM nums m JOIN (SELECT i.code AS c FROM ids i) x ON m.n = x.c` | **refused**, same message | 3 rows |
| K2 | `FROM ids i WHERE i.code IN (SELECT m.n FROM nums m)` | `{beta}` | `{alpha, beta, gamma}` |
| K3 | `FROM nums m WHERE m.n IN (SELECT i.code FROM ids i)` | `{seventeen}` | `{eighteen, seventeen, sixteen}` |
| K4 | `... m.n NOT IN (SELECT i.code FROM ids i)` | `{eighteen, sixteen, twenty}` | `{twenty}` |
| K5 | `... EXISTS (SELECT 1 FROM ids i WHERE i.code = m.n)` | `{seventeen}` | `{eighteen, seventeen, sixteen}` |
| K6 | `... NOT EXISTS (SELECT 1 FROM ids i WHERE i.code = m.n)` | `{eighteen, sixteen, twenty}` | `{twenty}` |
| K7b | `... WHERE (SELECT count(*) FROM ids i WHERE i.code = m.n) > 0` | `{seventeen}` | `{eighteen, seventeen, sixteen}` |
| K8 | `... m.n IN (SELECT x.c FROM (SELECT i.code AS c FROM ids i) x)` | `{seventeen}` | `{eighteen, seventeen, sixteen}` |

**K1 and K9 are the containment working.** K2–K8 are the same key comparison with the guard
missing: exactly one row matches — the one whose text is already canonical (`'17'` vs `17`)
— and `'016'` / `'0018'` silently do not, where SQLite's numeric affinity converts them and
does. The `NOT` forms return the complement, so the error appears as extra rows as well as
missing ones. Optimized and `--no-optimize` agree with each other throughout; the divergence
is against the oracle, not between the legs, which is why the 119 invariant checks are blind
to it by construction.

`--explain` shows there is nothing exotic in the plan — an ordinary semi join on a
mismatched key:

    LogicalSemiJoin [n = code]
      LogicalScan [nums, 2 columns]
      LogicalProject [code]
        LogicalScan [ids, 1 columns]

### Reachable without a custom catalog

On the **shipped** `catalog.json`, with a string literal as the body's output column:

    SELECT count(*) FROM laps l WHERE l.driver_id IN (SELECT '016' AS s FROM drivers d)
      SwiftQL 0        SQLite 495
    SELECT count(*) FROM laps l WHERE l.driver_id NOT IN (SELECT '016' AS s FROM drivers d)
      SwiftQL 10000    SQLite 9505
    ... IN (SELECT '16.0' ...) / (SELECT ' 16' ...) / (SELECT '+16' ...)
      SwiftQL 0        SQLite 495   (three more non-canonical renderings)
    ... IN (SELECT '16'  ...)
      SwiftQL 495      SQLite 495   ("half a match", the half that agrees)

Five divergent forms, one agreeing form, from the same pair of types — which is the precise
signature Week 29 named and refused.

### Why the encoding cannot be blamed instead

`key_encoding.h` is careful and correct for the contract it states: `keyFieldText` routes an
integral DOUBLE through the integer path so `7.0` and `7` join (matching SQLite's affinity),
and it never intends to normalise a STRING. The bug is not in the encoding; it is that the
one guard which keeps ill-typed pairs away from the encoding is attached to a single
producer. The `int_keys` SIMD gate (`vectorized_plan_builder.cc:555-558`) is independently
safe — it requires `TypeId::INT` on **both** resolved key columns — so nothing reaches the
flat int64 buffer with a string; the exposure is entirely in the text-hash path.

### Fix shape

Move the type test out of `Validator::validate`'s `stmt.joins` loop and apply it where a
`JoinKey` becomes a `LogicalJoin`'s key — i.e. once, at or just below `LogicalJoin`
construction, where all four producers converge and both sides' schemas are known. Doing it
at the producers instead means writing it three more times, which is how it came to be
missing once already. Whichever site is chosen, refusing (Week 29's decision) rather than
implementing affinity keeps the two behaviours consistent with each other and with the WHERE
clause, which still throws `Type mismatch` on the identical predicate.

The standing sweep this belongs to: `validator.cc:284-296`'s comment says the defect "is
closed here". It is closed for one of four producers, and the comment must say which.

---

## Part A′ — pass 2's three open MEDIUMs, re-ranked on the moved tree

All three reproduce verbatim on `922ca15`. I **agree with pass 2's MEDIUM on all three**, and
add one coupling that did not exist when pass 2 ranked them.

### B-2 (`containsOuterJoin` recurses into a DERIVED body) — still MEDIUM, unchanged

`join_enumeration.cc:91-100` is byte-identical. Re-ran pass 2's shape on `catalog.json`:

    LogicalJoin [driver_id@0 = k] join-ordering=skipped (outer join)
      LogicalJoin [driver_id = driver_id]              <- three inner relations
      LogicalDerived [x, 2 columns]
        LogicalProject [k, t]
          LogicalLeftJoin [driver_id = driver_id]      <- the only outer join, sealed

Still a plan-quality and reason-misattribution defect only; declining is always legal.

### B-3 (`JoinEnumeration::apply` never descends past the topmost JOIN) — still MEDIUM, and the measurement is unchanged

`join_enumeration.cc:612-618` is byte-identical, comment premise included. Re-measured on
`data/tpch/sf0.01`:

    body alone         order=customer@1,nation@2,orders@0  cost=38417 (written=62729) method=dp
    body as join input no order= line anywhere; written order kept throughout

**62729 against 38417, still silent** — no `join-ordering=skipped` line, because the body's
join never meets the pass at all.

### B-5 (`join_enumeration.h:84-91` carries verbatim the paragraph the `.cc` deleted as false) — still MEDIUM, unchanged

Still word for word at `join_enumeration.h:84-91`, and `join_enumeration.h:65` still names
`hasSlotOutsideRangeTable` and still calls the decline "silent" (pass 2's B-1, also still
open). Fix round 2 swept `sort_comparator.h` and `compare_against_sqlite.py` for the
scan-schema asymmetry and did not touch this header.

### !! The new coupling: B-2 and B-3 must not be fixed before B3-1

Both B-2 and B-3 are proposals to make the DP **run in more places**. Under B3-1, every place
the DP newly runs is a place where the optimizer can permute a sort's input schema and change
which rows survive a `LIMIT`. Today a derived body's join and the block under a declined
outer/semi join are immune to B3-1 *because* they are never enumerated — B-3's defect is
accidentally containing B3-1's. Fixing either of them first would widen a live divergence
while closing a plan-quality one. Stated here because neither finding's own writeup can see
the other.

---

## Part B — hunting what passes 1 and 2 missed

Two blockers came out of this part (B3-1 above, found from the tie-break's precondition;
B3-2 above, found from the Week 29 guard's scope). Everything else I could construct is
below, and it is clean.

Method, stated so the negative result means something. Passes 1 and 2 ran hand-built shapes;
this pass adds a **randomized SQLite-oracle sweep aimed specifically at the joints**, because
the only randomized differ in the tree (`python_tools/random_diff.py`) neither generates
derived relations nor semi/anti joins nor correlated subqueries — its generator emits inner
and left joins over base tables only. Mine emits 2–4 relation spines mixing base tables,
self-joins and derived relations, `LEFT` joins interleaved with inner ones, composite ON
keys, `IN` / `NOT IN` / `EXISTS` / `NOT EXISTS` / correlated-scalar predicates stacked on
those spines, and optional `GROUP BY` / `LIMIT`. `ORDER BY` is always **total** (a unique
tail key appended) so that this leg answers *correctness* and not *tie-break determinism* —
B3-1 owns the latter, and left un-neutralised it would have swamped the run.

Three legs per query: optimized / `--no-optimize` / SQLite, positional TSV, sort-normalised.

**240 randomized shapes over two seeds (20260808, 777): 240 ok, 0 diff, 0 skipped.**

Plus five hand batteries where randomness is the wrong tool because the interesting cases are
sparse:

#### C3-1 — NULL join keys through the whole chain (15 shapes, all agree three ways)

The CSV loader cannot express NULL, so every NULL here is manufactured by a `LEFT JOIN`
inside a derived body whose key column is null-extended for 10 of the 20 drivers. Tested with
that NULL-bearing column as: an INNER join's build key and its probe key; a `LEFT` join's
key; the body of `IN` and of `NOT IN`; the **probe** side of `IN`, `NOT IN` and `NOT EXISTS`;
one component of a **composite** key under both INNER and LEFT; a correlated `EXISTS` /
`NOT EXISTS` body key; a correlated scalar's key; a three-relation spine so the DP reorders
around the NULL-bearing relation; and a join with the NULL-bearing derived relation on
**both** sides. All 15 agree with SQLite in both optimizer modes, including the three-valued
splits (`NOT IN` over a NULL-bearing body -> 0; `NOT EXISTS` over the same -> 10).

This also re-runs, on a live plan, the shape queued as
`VecDerivedNode::nextChunk` forwarding its child's chunk pointer: two derived relations on
the two sides of one join (`(NB) x JOIN (NB) y ON x.k = y.k`) returns the right answer,
because the two are separate `VecDerivedNode`s over separate subplans — a plan tree cannot
route one node's output to two consumers. **Not worse than recorded**; if anything this is
evidence the reachable-plan hunt should stop.

#### C3-2 — the predicate/residual split at depth (12 shapes, all agree three ways)

Aimed at "dropped, applied twice, or applied on the wrong side": a `LEFT` join's ON residual
in the middle of a spine; the same with a WHERE conjunct on the null-supplying side (which
must NOT be pushed below the join, and must turn the LEFT into an INNER at the WHERE, not at
the ON); an INNER residual on a three-relation spine (lifted into WHERE, then pushed back
down by `distribute`); a residual **spanning two relations** on three- and four-relation
spines (so it must stay above the tree while the DP reorders underneath it); a residual
referencing both sides of a `LEFT` join; a semi join over a three-relation spine that also
carries residuals; an anti join over a LEFT-joined spine; a `LEFT` join sealed inside a semi
join's body; a WHERE conjunct on the null-supplied side of a `LEFT` join at the end of a
reordered spine; and two residuals plus a `GROUP BY`. No conjunct is dropped, duplicated or
misplaced on any of them.

#### C3-3 — semi/anti in a chain, and derived tables as join inputs (11 shapes + 3, all agree three ways)

Two semi joins stacked; a semi and an anti stacked; semi + anti + correlated `EXISTS` on one
three-relation spine; a semi join whose **body is itself a two- and a three-relation join**;
composite keys across the derived boundary; derived relations on **both** sides with a
composite key; an anti join whose body is a derived relation containing a `LEFT` join; and a
four-relation spine carrying a derived input, a semi join, and a total `ORDER BY ... LIMIT`.

#### C3-4 — join key types (the part of B3-2 that is CORRECT)

`keyFieldText`'s numeric affinity is right and is right everywhere, not only where the
Validator guards. `INT` key against an **integral DOUBLE** key matches, and matches SQLite,
across a plain join (`ON l0.driver_id = x.ad` where `ad = AVG(driver_id)` -> 500 = SQLite),
inside a **composite** key alongside an INT component, and through the `IN` lowering
(`l0.driver_id IN (SELECT AVG(ld.driver_id) ... GROUP BY ld.driver_id)` -> 500 = SQLite).
The `int_keys` SIMD gate (`vectorized_plan_builder.cc:555-558`) independently requires
`TypeId::INT` on both resolved key columns, so nothing ill-typed can reach the flat int64
buffer; B3-2's exposure is confined to the text-hash path.

#### C3-5 — the exact boundary of B3-1, established by negative shapes

Not every reordering diverges, and knowing which do is what a regression entry needs.
On `catalog.json` the DP reorders a three-relation spine to `order=laps@0,drivers@2,laps@1`
(`cost=41854 (written=59788)`) and the two legs still agree — because `laps@0` stays leftmost,
so the leading (and here unique) tie-break column `lap_id` is unchanged. **B3-1 fires exactly
when the DP changes which columns lead the merged schema**, i.e. when the leftmost relation
moves, which is what `order=customer@1,...` does on the TPC-H shape and what
`order=laps@0,...` does not do on the f1 ones. A regression entry for B3-1 therefore cannot be
written against either f1 catalog with the shapes those catalogs support; it needs
`data/tpch/sf0.01` (or a catalog where the cheapest leading relation is not the written one).

#### C3-6 — B3-1 is not confined to the `LIMIT` cut

Dropping the `LIMIT` from the TPC-H shape leaves the *ordering of the whole result* different
between the two legs — the first five rows are the same five that differed under `LIMIT 5`.
For a query that wrote an explicit `ORDER BY`, that is the ordering the tie-break exists to
make reproducible, so the defect is one step wider than "which rows survive the cut".

---

## Summary

| Severity | Count | IDs |
|---|---|---|
| BLOCKER | 2 | B3-1, B3-2 |
| HIGH | 0 | — |
| MEDIUM | 3 | B-2, B-3, B-5 (all carried from pass 2, all re-verified, severity agreed) |
| LOW | 1 | B-1 (carried from pass 2, still open) |

**Part A.** Three of fix round 2's four changes leave pass 2's clean verdict intact, and I
checked each against the join chain rather than in isolation: the scan-schema narrowing
touches no join-side consumer (12 shapes x 4 legs, and the loader's field-count refusal is
what keeps `narrowRows` in bounds); `reachesOutsideThisBody` cannot hide a real correlation
from join-key selection, because an outer `ColumnRef` produces its `-1` through a branch the
suppression does not touch (8 shapes, three ways); and `hidden` is read in exactly three
places, none of them in join planning, while `rebuild` copies whole `ColumnDef`s so a
`$scalarN` column cannot become visible by being reordered (6 shapes, three ways, column
counts matched to SQLite).

The fourth — the deterministic tie-break — is where the pass stops being clean.

**Part B.** 240 randomized oracle shapes plus 41 hand-built ones across NULL keys, the
predicate/residual split, semi/anti chains, derived boundaries and key types found no third
defect. The two that are here were both found the same way, and it is the way the prompt's
standing rule predicts: **a guard or a precondition was written for the world as it stood, the
join chain then grew a new producer, and the guard's text was not swept.**

- **B3-1**: `sort_comparator.h` states its precondition as "the sort's INPUT row must be the
  same in every mode". The join spine makes it the same *set* and a different *sequence*, and
  the tie-break reads sequence as data. `optimized != --no-optimize` on `LIMIT` over a
  reordered join, and 1115 vs 0 in the answer column once a scalar subquery carries it.
- **B3-2**: Week 29's STRING-vs-numeric join-key refusal was written into
  `Validator::validate`'s `stmt.joins` loop. Three more `JoinKey` producers shipped in Weeks
  32–34 and none is covered, so `IN`, `NOT IN`, correlated `EXISTS`/`NOT EXISTS` and the
  correlated-scalar rewrite all half-match, silently, against SQLite — in both directions,
  since the `NOT` forms return the complement.

**Verdict: the join chain's seam is NOT clean. Two blockers, both silent, both from the same
failure mode — a correctness rule stated for one producer while the chain quietly grew three
more — and the audit does not end here.**

