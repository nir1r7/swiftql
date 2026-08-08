# Seam audit: the join chain across weeks 26–36 — PASS 4

Scope: the joints between the join layers (single hash join -> multi-way DP -> semi/anti
-> derived tables as join inputs -> decorrelated subqueries lowered to joins).
Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `b2bc70e`
(code identical to the gated `9da0494`).
Predecessors: `seam-join-chain-pass-1.md`, `-2.md`, `-3.md`.

STATUS: in progress — written incrementally.

**One BLOCKER so far: P4-B1, below — fix round 3's own new rule (the FILTER-over-PROJECT
descent) opens the exact divergence class that round's other new rule was written to close.**

Tooling for this pass, so a negative result means something:
* an out-of-tree Debug build at `$SCRATCH/b4` (own directory, no shared build lock taken,
  no source file touched);
* `$SCRATCH/diff4.py` — a positional-TSV differ across four SwiftQL legs
  (`columnar/vectorized`, the same `--no-optimize`, `row/volcano`, `columnar/volcano`)
  and an in-memory SQLite mirror built from the same `catalog.json` the engine reads;
* `$SCRATCH/probe_pairs.cc` — a **structural** probe linked against `libswiftql_lib.a`.
  It plans a query twice (`build`+`pushdown` alone, and `build`+`pushdown`+`JoinEnumeration`),
  then (a) compares the MULTISET of `(relation_slot, name)` pairs on the topmost join's
  merged schema between the two legs, (b) reports whether the column SEQUENCE was permuted
  (so a "same set" result cannot pass vacuously), and (c) scans EVERY node of the optimized
  tree for a duplicated `(relation_slot, name)` pair. This is the only way to check the
  tie-break's stated precondition directly rather than by sampling answers.

---

## Part A — verifying fix round 3

### A.1 — the canonical `(relation_slot, name)` tie-break — **CLEAN** (one comment nit, LOW, P4-L1)

**Pass 3's two B3-1 shapes no longer diverge.** Verbatim, on `data/tpch/sf0.01`:

    SELECT c.c_name, o.o_orderkey
    FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey
                  JOIN nation n  ON c.c_nationkey = n.n_nationkey
    ORDER BY n.n_regionkey LIMIT 5
      optimized == --no-optimize (5 rows, identical, ordered compare)

    SELECT count(*) AS n FROM orders o2 WHERE o2.o_orderkey < (…same body… LIMIT 1)
      optimized -> 1115   --no-optimize -> 1115      (was 1115 vs 0)

#### A.1.1 `rebuild` preserves the SET of `(relation_slot, name)` pairs — verified structurally

The claim in `sort_comparator.h:90-94` is that `rebuild` re-stamps with the binder's
written-order slot, so only the SEQUENCE differs between legs. Read at
`join_enumeration.cc:256-257` and `:315-318`:

    std::vector<ColumnDef> merged = node->output_schema.columns();
    for (ColumnDef& c : merged) c.relation_slot = order[0];        // leftmost block
    …
    for (ColumnDef c : rels[r].subtree->output_schema.columns()) { // r == order[k]
        c.relation_slot = r; merged.push_back(c); }

so the merged set is `{(r, name) : r a relation, name in relation r's OWN schema}` — and a
relation's own schema is slot-0-stamped in both legs (`buildScanSchema`; and
`derivedRelationSchema` explicitly, `logical_plan.cc:495`). The written-order fold builds
the same set (`logical_plan.cc:1066` stamps `join_slot`, and `blockOutputSchema`'s
`:542` mirrors it). `probe_pairs` confirms it on every shape it was given, and reports the
sequence as PERMUTED on the ones that matter so the result is not vacuous:

| shape (catalog) | pair-set | sequence |
|---|---|---|
| 3-rel TPC-H `orders/customer/nation`, `ORDER BY n_regionkey LIMIT 5` | SAME | PERMUTED |
| 4-rel TPC-H with TWO `customer` relations (slots 1 and 3) | SAME | PERMUTED |
| `SELECT *` over the 3-rel TPC-H spine | SAME | PERMUTED |
| 3-rel f1 **self-join** `laps l0 / laps l1 / drivers d` | SAME | PERMUTED |
| `SELECT *` over the f1 self-join | SAME | PERMUTED |
| f1 spine + a **derived** relation as a join input | SAME | same |
| f1 spine + `IN` semi join / `EXISTS` / correlated scalar | SAME | same |
| f1 spine with a `LEFT JOIN` on it | SAME | same |
| two derived relations joined, both aliasing their column `k` | SAME | same |

#### A.1.2 the self-join case the prompt names — **the pair is unique there, and it is the reason it works**

A self-join's merged schema does carry several columns of the same name; they sit at
DIFFERENT slots, because `rebuild` stamps by relation and two relations cannot share a
range-table slot. Behavioural check, four relations with **two `customer` relations** and a
DP order whose LEADING relation is not the written one (`order=customer@1,nation@2,orders@0,customer@3`,
`cost=101801 (written=132152) method=dp`) — the exact configuration pass 3's C3-5 identified
as the one that fires:

    SELECT c.c_name, c2.c_name, o.o_orderkey
    FROM orders o JOIN customer c ON o.o_custkey = c.c_custkey
                  JOIN nation n  ON c.c_nationkey = n.n_nationkey
                  JOIN customer c2 ON o.o_custkey = c2.c_custkey
    ORDER BY n.n_regionkey LIMIT 5
      optimized == --no-optimize, row for row.

#### A.1.3 where the pair is NOT unique, and why that is still safe

`sort_comparator.h:97-101` concedes one case and argues it away. The concession is real —
I constructed three schemas a sort actually sees carrying a duplicated pair, all of them the
schema of the `deterministicCut` sort inserted beneath a plain `LIMIT`:

    SELECT l.lap_id AS a, l.driver_id AS a   FROM <3-relation f1 spine> LIMIT 5
    SELECT l.lap_id + 1 AS a, l.driver_id + 1 AS a  FROM <same> LIMIT 5
    SELECT COUNT(*) AS a, COUNT(*) AS a      FROM <same> LIMIT 5
      probe_pairs: ** DUPLICATE (slot,name) on LIMIT / SORT / PROJECT: (0,a) x2

All three are `optimized == --no-optimize` row for row, and the reason is the one the header
gives: `tieBreakOrder`'s sort is `std::stable_sort`, so the two `(0,a)` columns keep their
SCHEMA order, and that schema is the PROJECT's, which is fixed inside
`LogicalPlanBuilder::build` — i.e. **before any optimizer pass runs at all**.

**P4-L1 (LOW, comment precision).** The header states that reason as "a PROJECTED schema's
order is a function of the SELECT list rather than of the plan"
(`sort_comparator.h:99-101`). That is false for `SELECT *`: the star expansion at
`logical_plan.cc:1200-1218` copies the CHILD's schema column by column, so a star projection's
order is the merged join schema's order, not the select list's. The CONCLUSION survives —
build() runs before pushdown and enumeration, so the copy is of the WRITTEN order and is
plan-independent either way — but the property the sentence names is not the property that
holds. The accurate statement is "a projected schema's order is fixed by
`LogicalPlanBuilder::build`, which runs before every optimizer pass". Same class as the
retracted paragraphs this file has already had to sweep twice: a precondition whose stated
reason is narrower than the fact it depends on. No behavioural consequence today.

---

## BLOCKER P4-B1 — the new FILTER-over-PROJECT descent screens the conjunct for totality and not the PROJECTION it moves below, so a partial expression in a derived body's SELECT LIST is evaluated on fewer rows under the optimizer: `optimized` answers where `--no-optimize` throws

**Severity: BLOCKER.** `optimized != --no-optimize` on a query one leg accepts and the other
refuses, in the only engine that can run the shape. Reproduced three ways on the SHIPPED
`catalog.json`, no custom data.

### The shapes

    P1  SELECT COUNT(*)
        FROM (SELECT l.lap_id AS lap_id, l.lap_id * 1000000000000000 AS big FROM laps l) x
        WHERE x.lap_id < 100

          columnar/vectorized              -> 99
          columnar/vectorized --no-optimize -> Error: integer overflow in '*': the result
                                               does not fit in a 64-bit integer

    P2  the same derived relation as a JOIN INPUT rather than in FROM
        SELECT COUNT(*) FROM drivers d
          JOIN (SELECT l.lap_id AS lap_id, l.driver_id AS did,
                       l.lap_id * 1000000000000000 AS big FROM laps l) x
          ON d.driver_id = x.did
        WHERE x.lap_id < 100
          optimized -> 99      --no-optimize -> Error (same overflow)

    P3  a different partial operator — SUBSTRING with a COMPUTED start
        SELECT COUNT(*)
        FROM (SELECT l.lap_id AS lap_id,
                     SUBSTRING(l.team, l.lap_id - 3, 2) AS s FROM laps l) x
        WHERE x.lap_id > 100
          optimized -> 9900    --no-optimize -> Error: SUBSTRING: start position must be >= 1

Volcano cannot run any of them (`derived tables (FROM (subquery)) are not supported on the
Volcano path`), and `--execution vectorized` requires `--storage columnar`, so this is the
one leg pair the project asserts equal, disagreeing with itself.

### Why — the exact mechanism

`--explain` on P1, optimized:

    LogicalDerived [x, 2 columns]
      LogicalProject [lap_id, big]
        LogicalFilter [(l.lap_id < 100)]      <-- BELOW the projection
          LogicalScan [laps, 1 columns]

The conjunct entered the body (`pushIntoDerived`, safe — see A.4.1) and then DESCENDED below
the body's projection (`PredicatePushdown::apply`'s FILTER-over-PROJECT arm,
`predicate_pushdown.cc:745-779`, via `remapThroughProject`). `big` is now computed for 99
rows instead of 10000, and the overflow lives at `lap_id >= 9224`.

**The precondition that is missing.** `remapThroughProject`'s stated rule
(`predicate_pushdown.cc:284-289`) is:

> σ_p(π(R)) ≡ π(σ_p'(R)) for a row-wise π, with p' the conjunct rewritten onto π's inputs —
> but only where every column p names is a PLAIN PASSTHROUGH.

That is a **set** equivalence, and it is true. It is not an **error-behaviour** equivalence,
and the totality screen this same round introduced says in its own words why that
distinction is the whole point (`predicate_pushdown.cc:331-335`):

> PER-ROW EVALUATION IS NOT TOTAL: `evaluate()` can THROW on a row. That makes every
> conjunct MOVE this pass performs … a decision about whether a query ERRORS, not only
> about how fast it runs.

`firstMayRaise` screens **the conjuncts**. The FILTER-over-PROJECT arm moves a conjunct
below **a projection**, and nothing screens the projection. `remapThroughProject` inspects
only the project expressions the CONJUNCT NAMES (`project.exprs[i]` for the resolved `i`);
the SIBLING expressions — the ones that are actually evaluated fewer times — are never
looked at. In P1 the conjunct names `lap_id`, a plain passthrough, so the rule's own
precondition is satisfied while `big` is the expression that changes row count.

The screen's precondition paragraph (`:357-369`) is stated entirely in terms of "permuting
conjuncts C1..Cn" and "pushing any of them below an inner join". Moving a filter below a
PROJECT is a third kind of move, it arrived in the same round, and the paragraph does not
cover it. This is the standing-rule failure the prompt predicts, one round old: a new rule
was written without being swept into the invariant that governs the rules next to it.

### The direction, established rather than assumed

The descent can only MASK a raise, never introduce one: it moves the filter EARLIER, so the
projection sees a subset, and the conjunct itself sees the same rows either way (the new
filter is planted at `project->children[0]`, i.e. still above any filter the body already
had). Confirmed by the negative controls:

| # | shape | optimized | --no-optimize |
|---|---|---|---|
| P4 | body computes a TOTAL expression (`0 - l.lap_id`) | 99 | 99 |
| P5 | raising conjunct written SECOND in the outer WHERE | 99 | 99 |
| P6 | raising conjunct written FIRST in the outer WHERE (`firstMayRaise` freezes it) | Error | Error |
| P7 | the outer conjunct NAMES the computed column, so `remapThroughProject` declines | Error | Error |
| P8 | body has `ORDER BY … LIMIT 10`; entry must not cross it | 4 | 4 |
| P9 | body has `DISTINCT` | 1 | 1 |
| P10 | body has `GROUP BY … HAVING` | 1 | 1 |

P6 and P7 are the fix working. P1–P3 are the hole beside it.

### Why the gate is green

Nothing in either harness has a derived body whose select list holds a partial expression:
the raising operators are `checkedAdd/Sub/Mul/Div/Negate` and `SUBSTRING` with a computed
position, and every derived body in `compare_against_sqlite.py`, `test_new_queries.py` and
TPC-H projects plain columns or total arithmetic. The B3-2 pins added this round
(`tests/test_predicate_pushdown.cc`) pin the CONJUNCT side of the screen — which is the side
that works. A pin for this needs a raising expression in the body's SELECT LIST and a total
passthrough conjunct in the enclosing WHERE.

### Fix shape (minimum)

The descent needs the projection to be total on the rows it will stop seeing. The cheapest
sound rule, in the same conservative direction the screen already chose, is: **decline the
FILTER-over-PROJECT descent when ANY of the project's expressions `mayRaise`**, not only the
ones the conjunct names — one `firstMayRaise`-shaped scan over `project.exprs`. That keeps
every query in both harnesses (no derived body has a partial projection) and costs pushdown
only on the queries that would otherwise change their error behaviour.

Two things must be swept with it, per the standing rule:
* `predicate_pushdown.cc:284-289` — `remapThroughProject`'s argument must say that the set
  equivalence is not the whole obligation, and name the projection's totality as the second
  half;
* `predicate_pushdown.cc:357-369` — the precondition paragraph enumerates the moves this
  pass performs ("permuting a prefix", "pushing below an inner join"). It is now three
  moves, and the third one is the one that is unguarded. A precondition that lists the moves
  it covers must list all of them, or the next move added will land in the same gap.

---

### A.2 — `Validator::validateJoinKeyTypes` over the finished plan — **CLEAN**

#### A.2.1 all four producers now refuse; the loss is the one the fix recorded

Pass 3's nine K-shapes re-run verbatim on the same two-CSV catalog (`ids(code STRING)` /
`nums(n INT)`), three legs:

| # | producer | pass 3 (vectorized) | now |
|---|---|---|---|
| K1 | written `JOIN ON` | refused | refused (`JOIN ON: …`) |
| K9 | written JOIN, derived input | refused | refused (`JOIN ON: …`) |
| K2 | `IN` -> SEMI | `{beta}` (wrong) | refused (`IN / EXISTS subquery: …`) |
| K3 | `IN` -> SEMI, other direction | `{seventeen}` (wrong) | refused |
| K4 | `NOT IN` -> ANTI_NOT_IN | complement (wrong) | refused (`NOT IN subquery: …`) |
| K5 | `EXISTS` -> SEMI | `{seventeen}` (wrong) | refused |
| K6 | `NOT EXISTS` -> ANTI | complement (wrong) | refused (`NOT EXISTS subquery: …`) |
| K7b | correlated scalar -> `$scalarN` LEFT join | `{seventeen}` (wrong) | refused (`join key: … the subquery's key column`) |
| K8 | `IN` over a derived body | `{seventeen}` (wrong) | refused |

Five of the six wrong answers were the "half a match" signature and one was its complement;
all six are now refusals, and the refusal names its producer. Refusing rather than
implementing affinity is the recorded, deliberate divergence from SQLite (K1..K9 all still
disagree with the oracle by construction).

#### A.2.2 the resolution really does match the physical builder — checked rule by rule

| | `Validator::validateJoinKeyTypes` (`validator.cc:198-212`) | `VectorizedPlanBuilder` (`vectorized_plan_builder.cc:450-511, 852-857`) |
|---|---|---|
| left, bound key | `left.indexOf(from_col, from_slot)` | `left_schema.indexOf(k.from_col, k.from_slot)` — same call |
| left, `from_slot < 0` | `left.indexOf(from_col)` bare name | same bare-name fallback, same condition |
| right, STANDARD | `right.indexOf(join_col)` | `right_schema.indexOf(k.join_col)` |
| right, SEMI/ANTI/ANTI_NOT_IN | positional, `ri = k` | positional, `idx.push_back(i)` — same predicate `semantics != STANDARD` |
| a MISS | `continue` (not this rule's to report) | THROWS by name — so a miss is an error, never a silent skip |
| arity | `ri >= right.size()` -> skip | throws `a semi/anti join's build input must output exactly its key columns` |

The one asymmetry worth naming is that the walk runs on the **written-order** tree (last
statement of `LogicalPlanBuilder::build`, `logical_plan.cc:1249`) and the physical builder
runs on the **reordered** one. That is sound, and the reason is structural rather than
lucky: `JoinEnumeration` is a fifth site that CONSTRUCTS `JoinKey`s (`:269`, `:271`, `:360`,
`:362`), but each one it constructs is an `Edge` — a direction-free pair of the two columns
the written tree already paired — so reordering can only SWAP a key's two sides and
re-stamp `from_slot`. `requireJoinKeyTypes` is symmetric (`l_str == r_str`), so a swap
cannot change the verdict, and the pair of COLUMNS is invariant. The `k == 1` rewrite
(`join_enumeration.cc:287-289`) forces `from_slot = 0` against a LEAF schema, whose columns
are slot-0-stamped in every leaf kind including DERIVED (`logical_plan.cc:495`), and
`buildScanSchema` provably keeps a key column (`logical_plan.cc:332` collects the join
condition), so that rewrite cannot turn a resolvable key into a miss either.

#### A.2.3 the positional rule is right, and I made it discriminate

Positional right-side resolution is the risky half, so it was checked with a shape where
swapping the two key positions changes the ANSWER rather than only the plan
(`p(a,b) = (1,100),(100,1),(7,7)`, `q(x,y) = (1,100)`):

| query | SwiftQL (both legs) | SQLite |
|---|---|---|
| `EXISTS (SELECT 1 FROM q WHERE q.x = p.a AND q.y = p.b)` | `{1}` | `{1}` |
| written in the other order (`q.y = p.b AND q.x = p.a`) | `{1}` | `{1}` |
| `EXISTS (… q.x = p.b AND q.y = p.a)` | `{100}` | `{100}` |
| `NOT EXISTS` of the first | `{100, 7}` | `{100, 7}` |
| correlated scalar with the same composite key | `{}` | `{}` |

The pairing is correct by construction, not by luck: `splitCorrelation`
(`subquery_decorrelation.cc:208`) pushes `keys[i]` and `body_key_refs[i]` in lockstep, and
the body's select list is `body_key_refs` verbatim (`:807`) — for the correlated scalar,
`buildAggregateSchema` emits the group keys first in key order and the rewrite asserts
`renamed.size() == keys.size() + 1` (`:598`). `lowerInSubqueries` has exactly one key and a
one-column body.

Eight further shapes over the f1 catalog (composite `EXISTS`/`NOT EXISTS`, a mixed-type
composite whose SECOND component is the ill-typed one — correctly refused, a semi join
whose body is a JOIN with duplicate names, `IN` over a derived body, a correlated scalar
with a composite key, and an `EXISTS` whose body itself joins) all agree three ways.

#### A.2.4 the AST loop is still Volcano's only cover — confirmed

`Validator::validate`'s `stmt.joins` loop (`validator.cc:414-425`) is unchanged in effect
and its new comment states the containment correctly. Volcano's refusals were re-measured
rather than taken from the comment:

    multi-way joins are not supported on the Volcano path
    derived tables (FROM (subquery)) are not supported on the Volcano path
    IN subqueries are lowered to a semi-join and are not supported on the Volcano path
    correlated subqueries are decorrelated to a semi-join and are not supported on the Volcano path

so `stmt.joins` really is Volcano's only `JoinKey` producer, and K1's refusal fires on the
`row/volcano` leg with the `JOIN ON` context string. `Planner::plan` builds no logical
plan, so the walk cannot see it — the loop's own stated reason for surviving.

---

### A.3 — the sequencing constraint, verified independently — **HOLDS**, and one part of B-3 is still open (P4-M1)

Pass 3 raised the constraint (B-2/B-3 must not be fixed before B3-1). The order was
respected; I checked the consequence rather than the commit order.

**B-2 is fixed and the misattribution is gone.** A `LEFT JOIN` sealed inside a derived body
no longer switches ordering off for the enclosing block's fully inner spine:

    LogicalJoin [driver_id@0 = k] order=laps@0,drivers@1,@2 … method=dp
      LogicalJoin [driver_id = driver_id]
      LogicalDerived [x, 2 columns]
        LogicalProject [k, t]
          LogicalLeftJoin [driver_id = driver_id]      <- sealed, not consulted

**B-3 is fixed for the two shapes it was measured on.** A 3-relation join inside a derived
body used as a join input now gets `order=customer@1,nation@2,orders@0 cost=39938
(written=67601) method=dp` (pass 3 measured `62729` silent); a semi join's BODY containing a
3-relation join gets `order=drivers@1,drivers@2,laps@0 cost=43104 (written=60637) method=dp`.

**The widening did not reopen B3-1.** Four shapes that put a sort inside or above a newly
enumerated region, ordered compare, `optimized` vs `--no-optimize`:

| shape | result |
|---|---|
| derived body: 3-rel join + tied `ORDER BY` + `LIMIT 5` | identical |
| derived body: 3-rel join + PLAIN `LIMIT 5` (a `deterministicCut` sort INSIDE the body) | identical |
| derived body with a tied `ORDER BY … LIMIT` joined to an outer relation | identical |
| semi-join body with a 3-rel join, outer tied `ORDER BY … LIMIT` | identical |

**P4-M1 (MEDIUM, plan quality — the half of B-3 the fix did not reach).** The block BELOW a
declined semi/anti join is still never enumerated, and `slotDeclineReason`'s own comment
(`join_enumeration.cc:159-168`) presents exactly that loss as its motivation. Re-measured
now, post-fix, on the shipped catalog:

    FROM laps l JOIN drivers d ON … JOIN drivers d2 ON …            <- no IN
      LogicalJoin [driver_id@1 = driver_id] order=drivers@1,drivers@2,laps@0
                                            cost=43104 (written=60637) method=dp

    … the same spine … WHERE l.driver_id IN (SELECT d3.driver_id FROM drivers d3)
      LogicalSemiJoin [driver_id@0 = driver_id] join-ordering=skipped (semi/anti join)
        LogicalJoin [team@1 = team]           <- fully inner, 3 relations, NOT enumerated
          LogicalJoin [driver_id = driver_id]

**43104 against 60637, still lost.** The cause is `applyToSpineLeaves`
(`join_enumeration.cc:665-672`): it steps OVER every JOIN on `children[0]` because
`decompose` only accepts a written-order tree. For a spine `reorder` DECLINED, the tree it
returned is UNTOUCHED and therefore still in written order, so `apply` — not the
step-over — is applicable to `children[0]`. The step-over is required for a declined OUTER
join (reordering across it is a wrong answer, not a slow one) and is required for a spine
that was rebuilt; it is neither required nor free for the semi/anti decline, where the
declined node sits ABOVE a block that is legal to reorder and whose merged schema keeps its
`(slot, name)` pairs (A.1) so the semi join's own `leftKeyIndices` still resolves.
Plan-quality only — declining is always legal — hence MEDIUM, not a blocker. Worth naming
because the comment that motivates the decline reads as though the loss were unavoidable.
