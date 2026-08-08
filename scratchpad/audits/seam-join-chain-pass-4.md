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

---

## BLOCKER P4-B2 — the totality screen's carve-out is wrong: `inferExprType` does NOT decide a STRING-vs-numeric COMPARISON at plan time, so `mayRaise` calls a raising conjunct total and all three movers move it. `optimized -> Error` where `--no-optimize` answers

**Severity: BLOCKER.** The exact divergence class fix round 3's totality screen was written
to close, still open, in the INTRODUCED direction, on the shipped `catalog.json`, through
three different movers including the one this round added.

### The claim that is false

`predicate_pushdown.cc:380-384`, the screen's own carve-out:

> What it does NOT have to cover, because `inferExprType` (logical_plan.cc) decides it at
> PLAN time — in both legs, before any pass in this file runs — is every TYPE error: STRING
> arithmetic, a non-STRING LIKE or SUBSTRING operand, a mixed IN list, a CASE with mixed
> branches. Those raise identically in both legs no matter how the conjuncts are arranged.

Four constructs are listed. A COMPARISON across the STRING boundary is not among them, and
it is not decided at plan time. `inferExprType` (`logical_plan.cc:159-171`):

    if (op == "+" || op == "-" || op == "*" || op == "/") {
        if (l == TypeId::STRING || r == TypeId::STRING) throw …;
        …
    }
    return TypeId::INT;   // comparison / AND / OR

It types a comparison as INT **without comparing its operand types at all**. The throw
happens per row instead, in `Value`'s `NUMERIC_COERCE` macro (`value.cc:50-57`), which is
shared by `== != < > <= >=` — every one of them raises `Type mismatch in Value comparison`
across the STRING boundary. `mayRaise` (`predicate_pushdown.cc:413-417`) screens BinaryExpr
by operator and returns `mayRaise(left) || mayRaise(right)` for anything that is not
`+ - * /`, so `l.team = l.lap_id` — two plain `ColumnRef`s — is classified **total**.

### The failing shapes, on the shipped catalog

    T1  (mover: orderByWork)
        SELECT COUNT(*) FROM laps l WHERE l.team LIKE 'zzz%' AND l.team = l.lap_id
          optimized       -> Error: Type mismatch in Value comparison
          --no-optimize   -> 0

        --explain, the two legs:
          optimized      VecFilter [((l.team = l.lap_id) AND l.team LIKE 'zzz%')]  est=500
          --no-optimize  VecFilter [(l.team LIKE 'zzz%' AND (l.team = l.lap_id))]
        The equality is scored more selective, so the sort moves it AHEAD of a LIKE that
        matches nothing, and it is then evaluated on all 10000 rows.

    T3  (mover: distribute)
        SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id
        WHERE d.nationality = 'Zzz' AND l.team = l.lap_id
          optimized       -> Error       --no-optimize -> 0

        LogicalJoin [driver_id = driver_id]
          LogicalFilter [(l.team = l.lap_id)]     <- pushed below the join, 10000 rows
            LogicalScan [laps, 3 columns]
          LogicalFilter [(d.nationality = Zzz)]   est=1

    T5  the same as T3 with `l.team < l.speed`      optimized -> Error, --no-optimize -> 0
    T6  the same as T3 with `l.team != l.lap_id`    optimized -> Error, --no-optimize -> 0

    T7  (mover: pushIntoDerived + the FILTER-over-PROJECT descent — fix round 3's own
         new rule, so the screen and the rule that needs it shipped together)
        SELECT COUNT(*) FROM (SELECT l.team AS t, l.lap_id AS n FROM laps l) x
        WHERE x.t LIKE 'zzz%' AND x.t = x.n
          optimized -> Error       --no-optimize -> 0

    T2  control — the same two conjuncts written in the raising order:
        both legs Error, which is correct and is what the invariant asks for.
    T4  control — `l.team + 1 > 0`, which IS in the carve-out's list:
        both legs `Error: '+' requires numeric operands`, at plan time, as claimed.

**This is pass 3's B3-2 with a different operator class and the same three causes.** Fix
round 3 corrected my pass-3 attribution by finding that B3-2 had three movers
(`orderByWork`, `distribute`, `pushIntoJoin`'s leftover loop) rather than one; the fix then
screened by OPERATOR and enumerated `+ - * /`, `SUBSTRING` with a computed position, unary
minus, `IntervalLiteral` and `SubqueryExpr` — and rested the rest on a carve-out that
mis-states what `inferExprType` checks. Every mover the round enumerated re-opens the
divergence on this operator class, and the round's own new mover makes a fourth.

### What the class is, stated so the fix can be bounded

A conjunct raises through `Value`'s comparison operators exactly when the two operand
VALUES have different `variant` indices and at least one is STRING. `inferExprType` proves
neither side is STRING only for arithmetic. So the raising comparisons are those where the
two operands' INFERRED types straddle the STRING boundary — which `inferExprType` already
computes and throws away at `logical_plan.cc:171`.

### Fix shape (minimum), and it is smaller than it looks

Two independent options, and the first is strictly better because it removes the hazard
rather than routing around it:

1. **Make the carve-out true.** Have `inferExprType` refuse a comparison whose operands
   straddle STRING, the way it already refuses arithmetic — one `if` beside the existing
   one at `logical_plan.cc:165-169`. The predicate becomes a plan-time error in BOTH legs,
   `mayRaise`'s classification becomes correct without changing, and this also closes the
   same hole for every other consumer of per-row comparison (the WHERE cascade, the ON
   residual, `HAVING`). It is the same stance Week 29 took for join keys, and
   `validator.cc:125` already asserts the fact it rests on ("`Value::operator==` throws
   `Type mismatch` across the STRING boundary, so the identical predicate in a WHERE clause
   is an error") — a sentence that is true only for the col-vs-literal form today.
2. **Or make `mayRaise` true**: give it the schema and return `true` for a comparison whose
   operands straddle STRING. Strictly more code, keeps the query answerable, and leaves the
   two legs agreeing on an error-free answer only when written order is lucky.

Whichever is chosen, the carve-out paragraph (`predicate_pushdown.cc:377-404`) must be
swept: its list of what `inferExprType` decides is the load-bearing claim, it is enumerated
rather than derived, and this is the second time in two rounds that an enumerated list in
this file has been short.

---

### A.4 — predicate pushdown into derived bodies, and the `firstMayRaise` precondition — **the ENTRY rule holds; the DESCENT rule and the screen's carve-out are P4-B1 and P4-B2**

#### A.4.1 the ENTRY rule (FILTER over DERIVED) — CLEAN, and its stated reason is the right one

`pushIntoDerived` attaches the surviving conjuncts directly above the BODY's ROOT
(`predicate_pushdown.cc:670-673`), and the claim that this is safe for any body shape is
correct for a reason that can be checked: `derivedRelationSchema` renames and re-stamps and
does nothing else (`logical_plan.cc:480-513`), so the body root's rows ARE the relation's
rows, and σ at that point is σ at the relation. In particular the filter lands ABOVE the
body's `LIMIT` / `DISTINCT` / `AGGREGATE`, never below, which is what a body-shape argument
has to deliver. Measured:

| body shape | optimized | --no-optimize |
|---|---|---|
| `ORDER BY … LIMIT 10`, outer `WHERE x.lap_id < 5` | 4 | 4 |
| `SELECT DISTINCT`, outer `WHERE x.t = 'Ferrari'` | 1 | 1 |
| `GROUP BY … HAVING`, outer `WHERE x.t = 'Ferrari'` | 1 | 1 |

Ten further shapes across the derived boundary agree with SQLite on both legs: a body that
is a `LEFT JOIN` with the filter on the null-supplied column and with `IS NULL` on it; the
same body as a JOIN INPUT; a derived body inside a semi-join body and inside an anti-join
body; `NOT IN` and `NOT EXISTS` over a NULL-bearing derived body (0 and 18 respectively —
the three-valued split, right in both directions); a residual spanning two relations over a
3-relation spine carrying a derived input; and a semi join over a spine that also carries
one. `remapOntoDerivedBody`'s all-or-nothing rewrite leaves a declined conjunct
byte-identical, so nothing is dropped or doubled: `entering` and `staying` partition the
conjunct list and `filterOnto` re-attaches `staying` above the relation.

The `pushdown=skipped (…)` stamp is a genuine improvement on the silent decline it replaces,
and its two reason strings are the two the code can actually produce.

#### A.4.2 the DESCENT rule (FILTER over PROJECT) — **BLOCKER P4-B1**, above

Its precondition — "every named column is a plain passthrough" — is a SET-equivalence
condition and the pass needs an ERROR-behaviour condition as well. `remapThroughProject`
inspects only the project expressions the conjunct NAMES; the sibling expressions are the
ones whose row count changes.

#### A.4.3 the `firstMayRaise` precondition itself — sound as stated, and its stated scope is now short

The core argument is correct and I could not break it: AND over TOTAL conjuncts is
commutative under 3VL (a filter keeps only TRUE), so the survivor set entering position k
depends on the SET of the conjuncts before it; and σ_p(R⋈S) ≡ σ_p(R)⋈S for an inner join,
and σ_p(R)⟕S ≡ σ_p(R⟕S) on the preserved side, which is the only side `distribute` pushes
to. The index bookkeeping is right where it is easy to get wrong:

* `pushIntoJoin` re-sorts the leftover buckets back into WRITTEN index order before
  `filterOnto` (`predicate_pushdown.cc:599-605`), so a leftover can never land after a
  frozen conjunct — the case the comment names;
* every downstream `orderByWork` re-derives `firstMayRaise` on the list it is actually
  given, so a frozen conjunct that changed index (because an earlier one was pushed away)
  still freezes the right suffix;
* `mayRaise`'s dispatch defaults to `true` for an unrecognised `Expr` subtype, which is the
  safe direction, and screens every CASE arm rather than the taken one.

Three carve-out members were checked live rather than believed: `SUBSTRING` with literal
start/length is refused at plan time in every out-of-range form I could write
(`SUBSTRING(team, 0, 2)`, `(team, -1, 2)`, `(team, 1, 0-1)`), integer division by zero
yields NULL, and an `InExpr`'s list is `std::vector<Value>` (`ast.h:108-112`) so it holds no
expression that could raise — `mayRaise(in->operand)` really is the whole obligation there.

The fourth member is where it breaks: the carve-out asserts `inferExprType` decides *every*
type error at plan time and it does not decide a COMPARISON. See **P4-B2**.

**The stated scope is also short in a second, non-behavioural way.** The precondition
paragraph enumerates the moves it governs as "freely permuting a prefix of TOTAL conjuncts"
and "pushing any of them below an inner join". `pushIntoDerived` (entry) and the
FILTER-over-PROJECT descent are two more moves, added in the same round, and neither appears
in the paragraph. Sweeping that is part of P4-B1's fix; recorded here so the two are not
treated as separate chores.

#### A.4.4 an honest caveat on P4-B1's proposed fix

"Decline the descent when any project expression `mayRaise`" is sound but **not** free, and
the difference from the conjunct case matters. `mayRaise` screens arithmetic by OPERATOR,
so `l.speed * 2 AS v` — DOUBLE arithmetic, which cannot overflow — answers "may raise". The
screen's own comment accepts that conservatism on the ground that "post-folding an
arithmetic conjunct in a WHERE is rare"; a computed column in a derived body's SELECT LIST
is not rare, and measured live it is exactly the shape B3-3's 9.4x was claimed on:

    SELECT COUNT(*) FROM (SELECT l.driver_id AS k, l.speed * 2 AS v FROM laps l) x
    WHERE x.v > 600 AND x.k < 5
      LogicalDerived [x, 2 columns]
        LogicalFilter [(v > 600)]          <- declines the descent (computed column)
          LogicalProject [k, v]
            LogicalFilter [(l.driver_id < 5)]   <- descended: this is what would be lost
              LogicalScan [laps, 2 columns]

So the fixer has a real choice to make rather than a mechanical edit: accept the loss on
DOUBLE-arithmetic bodies, or make the projection screen type-aware (DOUBLE `+ - * /` cannot
overflow, which the schema at that point does supply). Recorded rather than decided here —
but the unsafe status quo is not one of the three options.

---

### A.5 — the standing sweep, run over the guards fix round 3 changed

The rule: when a refusal, guard or invariant changes, every comment, precondition,
assertion and header citing it must be swept. Checked, guard by guard.

**SWEPT and correct.** `sort_comparator.h`'s precondition paragraph is rewritten around
column identity (one imprecision, P4-L1). `join_enumeration.cc`'s `rebuild` comment now
draws the materialized-order / comparison-order distinction and tells a future consumer
which one it may rely on. `join_enumeration.h:75` no longer asserts the retracted
`max(l, r)` paragraph and dates the `hasSlotOutsideRangeTable` name explicitly — pass 2's
B-5 and the third copy the prompt names are both closed. `validator.h:15-40` states the
four-producer containment accurately, including WHY the check cannot live at the producers
(`semantics` is set after construction) and why the AST loop survives.
`subquery_lowering.h:66` and `subquery_decorrelation.h:57` both point at the walk rather
than claiming local coverage. `development.md:854` and the `PredicatePushdown` row carry
the corrected text.

**P4-L2 (LOW, carry-over — pass 2's B-1, still open after two fix rounds).** Three live
source comments still name `hasSlotOutsideRangeTable`, a function that has not existed since
`18af84f`, and one `development.md` row still asserts the behaviour that commit reversed:

* `src/planner/join_enumeration.cc:585` — "DEAD since Week 30: hasSlotOutsideRangeTable
  declines the whole tree" (the *fact* is right; the name is not);
* `src/planner/cardinality_estimator.cc:464` — "JoinEnumeration also declines these trees —
  hasSlotOutsideRangeTable fires on join_slot == -1";
* `src/planner/cardinality_estimator.cc:513` — "(hasSlotOutsideRangeTable declines the tree
  on join_slot -1)";
* `development.md:808` — "**Level-agnostic, and now LIVE.** … **The decline is silent**, in
  the same shape as the <3-relation one — there was no ordering decision to report".

The last one is the one that matters: it is a live row of the consumer table whose stated
purpose is to be the audit trail, it asserts the opposite of what the code does, and
`development.md:854` — a row in the SAME file — states the correction. A reader who finds
808 first is told the decline is silent and that no ordering decision was available; both
halves are false, and the second is the premise P4-M1 turns on. `join_enumeration.h:75`
shows the right way to do it (name the function, date the name), so the pattern to copy is
already in the tree.

**P4-L3 (LOW, and it is the surface a reader consults first).**
`predicate_pushdown.h:20-36` WAS swept for this round and overshoots. It enumerates the
moves as three —

> MOVING a conjunct — reordering it inside one filter OR pushing it below a join OR pushing
> it into a derived body — decides whether the query ERRORS … **Every move this pass makes
> is now screened**

— and then states the guarantee absolutely. There is a fourth move (descending below the
body's PROJECT), it is not in the list, and it is not screened (P4-B1); and the screen the
sentence points at rests on a carve-out that is false for comparisons (P4-B2). The header
is not stale relative to the `.cc`; it is a stronger claim than the `.cc` delivers, which is
the failure mode this project has logged repeatedly under "a dead assertion reads as a
guarantee and stops anyone looking". It must be corrected with P4-B1 and P4-B2, not after
them.

---

## Part B — hunting what three passes missed

Both blockers came out of this part, found the way the standing rule predicts: **a
precondition was written down for the first time, and the thing it enumerates is shorter
than the thing it governs.** P4-B1's precondition enumerates the columns a conjunct NAMES
and misses the columns a projection COMPUTES; P4-B2's enumerates four type errors and
misses the fifth. Everything else I could construct is below, and it is clean.

Method note. Pass 3 established the joints are clean under 240 randomized shapes and 41
hand-built ones, so this pass aimed its randomizer at the surfaces fix round 3 WIDENED
rather than re-covering that ground: derived bodies of eight shapes (plain, computed,
filtered, grouped, LEFT-joined, 3-relation-joined, DISTINCT, ORDER-BY-LIMIT) in the FROM
position and as join inputs, 2–4 relation spines, `IN` / `NOT IN` / `EXISTS` / `NOT EXISTS`
/ correlated-scalar predicates stacked on them, and outer conjuncts that name derived
columns (so the entry and descent rules fire). Three legs per query: optimized,
`--no-optimize`, SQLite; positional TSV, sort-normalised; a shape both SwiftQL legs refuse,
or SQLite cannot parse, is skipped rather than counted as agreement.

### B4-1 — the derived boundary under the new pushdown (13 hand shapes, all agree three ways)

`LEFT JOIN` body with the outer filter on the null-supplied column (`> 300` -> 2) and with
`IS NULL` on it (-> 18); the same body as a JOIN INPUT with the `IS NULL` across the
boundary (-> 18); a derived body inside a semi-join body and inside an anti-join body;
`NOT IN` over a NULL-bearing derived body (-> 0) and `NOT EXISTS` over the same (-> 18) —
the three-valued split, correct in both directions and matching SQLite; a residual spanning
two relations over a 3-relation spine carrying a derived input; a semi join over a spine
that also carries one; composite join key across the derived boundary; and the three body
shapes the ENTRY rule's "any body shape" claim rests on (`LIMIT`, `DISTINCT`,
`GROUP BY`/`HAVING`). A derived body with a `(a, b)` column alias list is correct on both
legs and cannot be oracled — SQLite does not parse that form.

### B4-2 — multi-way spines, residuals and semi/anti chains (13 shapes, all agree three ways)

f1: an outer WHERE entering and descending into a 3-relation derived body; the same body as
a join input with conjuncts on both sides; semi + anti stacked on a 3-relation spine; an
anti join over a LEFT-joined spine; a residual spanning two relations on a 4-relation
spine; a `LEFT` join with an ON residual and a WHERE on the preserved side; a composite key
across the derived boundary; a 3-relation spine with a TOTAL `ORDER BY … LIMIT 10`.
TPC-H `sf0.01`: a 4-relation spine whose derived input holds its own 3-relation join; a
semi join over a 4-relation spine; an anti join plus a residual; a correlated scalar over a
3-relation spine; a `GROUP BY` derived body joined to two relations. No conjunct dropped,
doubled or applied on the wrong side on any of them.

### B4-3 — the DP really does run in the newly reachable places, and the answers do not move

Three widenings measured on `--explain` and then checked for result preservation:

| newly enumerated region | order line | legs agree |
|---|---|---|
| a derived body under a **2-relation** outer block (below `MIN_ENUMERATED_RELATIONS`, so `reorder` returns early and only `applyToSpineLeaves` reaches it) | `order=customer@1,nation@2,orders@0 cost=32486 (written=54074) method=dp` | 239 = 239 = SQLite |
| the body of a **correlated scalar**, i.e. `children[1]` of a LEFT join the pass DECLINED | `order=customer@1,nation@2,orders@0 cost=35486 (written=60074) method=dp` | 0 = 0 = SQLite |
| a **semi join's** body | `order=drivers@1,drivers@2,laps@0 cost=43104 (written=60637) method=dp` | 20 = 20 = SQLite |

The middle row is the one worth naming: a LEFT join declines the SPINE, and the walk still
enters the declined node's `children[1]`. That is correct — the body is a separate block —
and it is the exact asymmetry P4-M1 says is missing one case, since the semi/anti decline
leaves `children[0]` unvisited while the LEFT decline leaves `children[1]` visited.

### B4-4 — join key types: the parts that are CORRECT

Re-confirmed from pass 3 and extended: `keyFieldText`'s numeric affinity is right everywhere
(an integral DOUBLE takes the integer path), the `int_keys` SIMD gate independently requires
`TypeId::INT` on both resolved key columns, and the new plan walk does not over-refuse — a
composite key mixing an INT component and a STRING component that are each matched to their
own type is accepted and correct (`l2.driver_id = d.driver_id AND l2.team = d.team` -> 20,
SQLite 20), while the same shape with `l2.team = d.age` is refused by name and by producer.

### B4-5 — the shapes I could NOT break, recorded so the negative means something

* `mayRaise`'s dispatch: `InExpr`'s list is `std::vector<Value>` so it holds no raising
  expression; `SUBSTRING` with literal bounds is refused at plan time in all three
  out-of-range forms; integer division by zero yields NULL; an unrecognised `Expr` subtype
  defaults to "may raise". Only the comparison carve-out is wrong (P4-B2).
* `remapOntoDerivedBody` / `remapThroughProject` are both all-or-nothing, so a declined
  conjunct is byte-identical for the caller and `entering`/`staying` partition the list —
  no conjunct is dropped or doubled at either boundary.
* `pushIntoJoin` re-sorts leftovers into written index order before `filterOnto`, so a
  leftover cannot land behind a frozen conjunct.
* `rebuild`'s key re-orientation is symmetric with respect to the type rule, so no
  reordering can produce a key pair the validator did not already see (A.2.2).
