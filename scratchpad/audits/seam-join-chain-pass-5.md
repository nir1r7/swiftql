# Seam audit: the join chain across weeks 26–36 — PASS 5 (final)

Scope: the joints between the join layers (single hash join -> multi-way DP -> semi/anti
-> derived tables as join inputs -> decorrelated subqueries lowered to joins).
Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `b14d086`.
Predecessors: `seam-join-chain-pass-1.md` … `-4.md`.

STATUS: IN PROGRESS (written incrementally; each section is committed as it is finished).

Tooling: an out-of-tree Debug build of `b14d086` in a private worktree
(`$SCRATCH/wt5` + `$SCRATCH/b5`), so no shared build lock is taken and no source
file in the checkout is touched. Four legs per shape — `columnar/vectorized`
(`opt`), the same with `--no-optimize` (`noop`), `row/volcano` (`rvol`),
`columnar/volcano` (`cvol`) — on the shipped `catalog.json` unless stated.

---

## Part A — verifying fix round 4

### A.1 — P4-B1, the FILTER-over-PROJECT descent — **FIXED, both halves**

The fix is at `predicate_pushdown.cc:751-756`: before any conjunct is offered to
`remapThroughProject`, `apply()`'s PROJECT arm screens **every** expression in
`project->exprs` with `exprMayRaise` against `project->children[0]->output_schema`
(the projection's INPUT schema, which is where those expressions are evaluated),
and one raising select-list expression stops the whole descent.

**The partial case now declines.** All three of pass 4's shapes, verbatim, on the
shipped catalog:

| # | shape | opt | noop | rvol | cvol |
|---|---|---|---|---|---|
| P1 | `SELECT COUNT(*) FROM (SELECT l.lap_id AS lap_id, l.lap_id * 1000000000000000 AS big FROM laps l) x WHERE x.lap_id < 100` | Error (overflow) | Error | — | — |
| P2 | the same derived relation as a **join input** | Error | Error | — | — |
| P3 | `SUBSTRING(l.team, l.lap_id - 3, 2)` in the body's select list | Error (start >= 1) | Error | — | — |

(pass 4: 99 / Error, 99 / Error, 9900 / Error). Volcano cannot run a derived
table at all, so the `opt`/`noop` pair is the whole obligation and it now holds.

**The total case has NOT lost its pushdown — and this is what resolves my own
A.4.4 caveat.** The screen is type-aware, so DOUBLE arithmetic in the body still
descends. `--explain`, optimized, on the exact query A.4.4 was measured on:

    SELECT COUNT(*) FROM (SELECT l.driver_id AS k, l.speed * 2 AS v FROM laps l) x
    WHERE x.v > 600 AND x.k < 5

    LogicalDerived [x, 2 columns]              est=702
      LogicalFilter [(v > 600)]                est=702
        LogicalProject [k, v]                  est=2105
          LogicalFilter [(l.driver_id < 5)]    est=2105     <- DESCENDED
            LogicalScan [laps, 2 columns]      est=10000

and the mirror shape (conjunct names the passthrough, sibling is a DOUBLE
computed column) descends too:

    SELECT COUNT(*) FROM (SELECT l.lap_id AS lap_id, l.speed * 2 AS big FROM laps l) x
    WHERE x.lap_id < 100
      LogicalProject [lap_id, big]           est=99
        LogicalFilter [(l.lap_id < 100)]     est=99         <- DESCENDED
          LogicalScan [laps, 2 columns]      est=10000

So B3-3's 9.4x survives on precisely the body shape it was claimed on, and the
descent is refused only where the projection is genuinely partial. **A.4.4 is
closed**: the fixer took the third of the three options I recorded (make the
projection screen type-aware) rather than accepting the loss.

The `remapThroughProject` header (`:282-299`) and the four-move enumeration in
the screen's comment (`:396-410`) were both swept with it, and the enumeration
now names the descent as move 4 and says why it needs a second screen. The
`predicate_pushdown.h:20-36` overshoot (P4-L3) is corrected: it now names four
moves and points at the second screen. **P4-L3 closed.**

### A.2 — P4-B2, the comparison carve-out — **FIXED; 36 shapes, 3 movers, 6 operators, both operand orders**

The carve-out is gone. `exprMayRaise` (now `parser/expr_totality.h:150-…`) takes
the schema, types both operands with `staticTypeOf`, and answers
`(l == STRING) != (r == STRING)` for `= != < > <= >=` — i.e. it screens the
comparison by operand type instead of resting on a claim about `inferExprType`.

Verified across the full cross product on the shipped catalog
(`l.team` STRING vs `l.lap_id` INT), `opt` against `noop`:

| mover | shape | operators | orders | result |
|---|---|---|---|---|
| `orderByWork` | `WHERE l.team LIKE 'zzz%' AND l.team OP l.lap_id` | all 6 | both | 12/12 AGREE (`0` / `0`) |
| `distribute` (via `pushIntoJoin`) | `laps l JOIN drivers d ON … WHERE d.nationality = 'Zzz' AND l.team OP l.lap_id` | all 6 | both | 12/12 AGREE |
| `pushIntoDerived` + the PROJECT descent | `FROM (SELECT l.team AS t, l.lap_id AS n FROM laps l) x WHERE x.t LIKE 'zzz%' AND x.t OP x.n` | all 6 | both | 12/12 AGREE |

**36/36 agree**, where pass 4 measured `Error` vs `0` on T1/T3/T5/T6/T7.

The result is not vacuous — the freeze is visible and the control still moves:

    WHERE l.team LIKE 'zzz%' AND l.team = l.lap_id
      LogicalFilter [(l.team LIKE 'zzz%' AND (l.team = l.lap_id))]   <- written order kept
    WHERE l.team LIKE 'zzz%' AND l.lap_id = 5                        (total)
      LogicalFilter [((l.lap_id = 5) AND l.team LIKE 'zzz%')]        <- reordered

and the same for the other two movers, each against its own control:

    distribute      WHERE d.nationality = 'Zzz' AND l.team = l.lap_id
                      LogicalFilter [(l.team = l.lap_id)]      <- FROZEN above the join
                        LogicalJoin [driver_id = driver_id]
                    control, `l.team = 'Ferrari'` instead:
                      LogicalJoin [driver_id = driver_id]
                        LogicalFilter [(l.team = Ferrari)]     <- pushed to its own scan

    pushIntoDerived FROM (SELECT l.team AS t, l.lap_id AS n FROM laps l) x
                    WHERE x.t LIKE 'zzz%' AND x.t = x.n
                      LogicalFilter [(x.t = x.n)]                                   <- stays
                        LogicalDerived [x, …] pushdown=skipped (predicate can raise)
                          LogicalProject [t, n]
                            LogicalFilter [l.team LIKE 'zzz%']   <- entered AND descended

The third is the whole round in one plan: the total conjunct written first enters
the body and descends below its projection, the raising one written second is
refused entry and the refusal is stamped.

---

## BLOCKER P5-B1 — a LEFT JOIN's `ON` residual is the one per-row expression in the join layer that NOTHING screens, and predicate pushdown on the preserved side changes the row set it is evaluated on: `optimized` answers where every other leg throws

**Severity: BLOCKER.** `optimized != --no-optimize` on the shipped `catalog.json`,
in the MASKING direction, reproduced three ways with three different partial
operators and two negative controls. The optimized `columnar/vectorized` leg is
the *only* one of the four that answers; `row/volcano` and `columnar/volcano`
both throw, so this is not a capability difference.

### The shapes

    R1  SELECT COUNT(*) FROM laps l
        LEFT JOIN drivers d
          ON l.driver_id = d.driver_id AND l.lap_id * 1000000000000000 > 0
        WHERE l.lap_id < 5

          opt  -> 4
          noop -> Error: integer overflow in '*': the result does not fit in a 64-bit integer
          rvol -> Error (same)      cvol -> Error (same)

    R2  the same with SUBSTRING and a computed start, and a WHERE that keeps
        only the rows the residual is TOTAL on
        SELECT COUNT(*) FROM laps l
        LEFT JOIN drivers d
          ON l.driver_id = d.driver_id AND SUBSTRING(l.team, l.lap_id - 3, 2) = 'x'
        WHERE l.lap_id > 9990
          opt -> 10        noop / rvol / cvol -> Error: SUBSTRING: start position must be >= 1

    R3  the residual reads the NULL-SUPPLYING side, and the WHERE is on the
        preserved side (drivers' ages run 21..38, so the residual raises for
        every driver younger than 31)
        SELECT COUNT(*) FROM laps l
        LEFT JOIN drivers d
          ON l.driver_id = d.driver_id AND SUBSTRING(d.name, d.age - 30, 2) = 'x'
        WHERE l.driver_id = 1
          opt -> 538       noop / rvol / cvol -> Error

### Why — the exact mechanism

`--explain` on R1, optimized:

    LogicalLeftJoin [driver_id = driver_id] residual=((l.lap_id * 1000000000000000) > 0)  est=4
      LogicalFilter [(l.lap_id < 5)]        est=4        <- pushed BELOW the join
        LogicalScan [laps, 2 columns]       est=10000
      LogicalScan [drivers, 1 columns]      est=20

Week 29 split the residual routing by join type (`logical_plan.cc:1092-1105`):
an INNER join's ON residuals are folded into the WHERE conjunction, **ahead of**
the written WHERE (`:1144-1147`), so they are ordinary conjuncts and the totality
screen sees them in the right order. A LEFT join's residual is instead attached
to the node (`lj->on_residual`) and evaluated **inside the probe loop**, once per
candidate pair (`plan_nodes.cc:736-742, 778`; the vectorized join does the same).

`distribute` then recurses into `children[0]` **unconditionally** for a LEFT
join, on the identity σ_p(R) ⟕ S ≡ σ_p(R ⟕ S) (`predicate_pushdown.cc:476-483`).
That identity is exactly the same kind of claim P4-B1 turned on: **it is a SET
equivalence and it is only half the obligation.** It preserves the join's
OUTPUT; it does not preserve the set of CANDIDATE PAIRS, and the ON residual is
the expression evaluated on the candidate pairs. Fewer left rows means fewer
pairs means a raise the residual was owed is masked.

Nothing screens `on_residual`. `firstMayRaise` is applied to the WHERE conjunct
list in `pushIntoJoin` (`:541`), to the derived-body list in `pushIntoDerived`
(`:623`) and to the descent list in `apply` (`:727`). `on_residual` is not a
conjunct of any of those lists — it is a field on the join node — and no consumer
of `expr_totality.h` looks at it. `expr_totality.h`'s own statement of the rule
enumerates **three** consumers (predicate_pushdown, chunk_pruner, the LIMIT rule
in logical_plan) and says "every other expression is evaluated on every row that
reaches its node"; the ON residual is an expression whose node's input rows this
pass changes, and it is in neither half of that sentence.

### Negative controls — the mechanism is the one named, not something else

| # | shape | opt | noop | rvol | cvol |
|---|---|---|---|---|---|
| C1 | the **INNER** analogue of R1 (residual folded into the WHERE, ahead of it, so `firstMayRaise` freezes at index 0) | Error | Error | Error | Error |
| C2 | LEFT, residual `d.age * 1000000000000000 > 0` — total on this data (ages 21..38) | 4 | 4 | 4 | 4 |
| C5 | LEFT, residual `l.speed * 2 > 0` — DOUBLE, cannot raise | 4 | 4 | 4 | 4 |
| C6-ctl | R3 with the WHERE removed, so no pushdown is possible | Error | Error | Error | Error |
| C3 | LEFT, residual `l.team = l.lap_id` — raises on the FIRST surviving pair too, so masking cannot show | Error | Error | Error | Error |

C1 is the finding stated the other way round: the INNER path is safe *because*
the residual becomes a conjunct in the list the screen reads. The LEFT path was
split off from it in Week 29 and never rejoined it.

**The attribution is airtight, because the three legs that agree are exactly the
three that do no predicate pushdown.** `Planner::plan` — the Volcano path — never
calls `PredicatePushdown::apply` at all (`main.cc` invokes it only on the
vectorized path, at `:138` and `:579`), and `--no-optimize` gates the same call.
So `noop`, `rvol` and `cvol` all evaluate the residual on the full candidate-pair
set and all raise; the one leg that pushes a conjunct below the join is the one
leg that answers.

### Why the gate is green

No harness query has a partial expression in an ON clause. TPC-H's one residual
ON predicate is Q13's `o_comment not like '%special%requests%'` (LIKE on a STRING
column — total), and `compare_against_sqlite.py` / `test_new_queries.py` put only
comparisons of plain columns and literals in ON. A pin for this needs a LEFT JOIN
whose ON carries a raising expression **and** a WHERE on the preserved side that
removes the rows it raises on — the same two-part construction P4-B1's pin needed.

### Fix shape (minimum)

The residual is one expression, not a list, so there is no ordering question and
the screen collapses to a single guard: in `distribute`, when
`join->on_residual && exprMayRaise(join->on_residual.get(), join->output_schema)`,
**decline the recursion into `children[0]`** for that join — leaving the bucket in
`by_slot` so `pushIntoJoin`'s leftover loop lifts it above the tree, which is the
same "degrade instead of drop" path the LEFT/semi decline already uses. That
costs pushdown only on a query that has a raising ON residual at all.

Two sweeps go with it, per the standing rule:
* `predicate_pushdown.cc:467-486` — the paragraph that justifies the
  unconditional `children[0]` recursion cites σ_p(R) ⟕ S ≡ σ_p(R ⟕ S) as if it
  settled the question. It must say that the identity is about the join's OUTPUT
  and that the ON residual is evaluated on the join's CANDIDATE PAIRS, which the
  identity says nothing about.
* `parser/expr_totality.h:22-46` — the "THREE CONSUMERS" enumeration and the
  sentence "every other expression is evaluated on every row that reaches its
  node". A LEFT join's ON residual is a fourth site whose row set a plan rewrite
  moves. **This is the fifth retracted/short enumeration in five passes, and it
  is in the file that was written this round to be the single statement of the
  rule.**

---

### A.3 — the wider change: SwiftQL now DEFINES evaluation order. What that means for JOIN shapes

`parser/expr_totality.h` states the rule: a conjunct is evaluated on the rows for
which every conjunct **written before it** evaluated TRUE; every other expression
is evaluated on every row that reaches its node; nothing may change that set for
an expression that can raise. `AND` is therefore not commutative for error
behaviour, by design. Worked through for the join layer, shape by shape, each one
measured on the shipped catalog with all four legs.

| # | join shape | what the rule requires | verdict |
|---|---|---|---|
| 1 | **`ON` residual of an INNER join, partial** | the residual is folded into the WHERE conjunction AHEAD of the written WHERE (`logical_plan.cc:1144-1147`), so it is conjunct 0 and `firstMayRaise` freezes the whole list | **HOLDS** (E10, C1: both legs Error) |
| 2 | **`ON` residual of a LEFT join, partial** | it is NOT a conjunct of any list the screen reads; it is evaluated per candidate pair inside the probe loop, and pushdown on the preserved side shrinks that set | **BROKEN — P5-B1** |
| 3 | **semi/anti join probe predicate** | the probe expression must be total | **HOLDS, structurally**: `lowerInSubqueries` refuses a computed left operand outright (`IN subquery: the left operand must be a column reference`), so the probe expression is always a bound `ColumnRef` and `exprMayRaise` on a resolvable ColumnRef is FALSE. A semi/anti join also carries no `on_residual` (asserted at `cardinality_estimator.cc:475`), and `grep` confirms `on_residual` is assigned in exactly one place, `logical_plan.cc:1132`, under `jc.type == JoinType::LEFT`. That single assignment site is what bounds P5-B1 |
| 4 | **a conjunct that straddles two relations** | `soleSlot` returns -1 so it cannot be pushed; it stays in `residual`, which is re-sorted into WRITTEN index order before `filterOnto` | **HOLDS** (E1–E4, E6: raising straddle written first and written second, 2- and 3-relation spines, all legs agree) |
| 5 | **a predicate pushed to one side of a join** | only conjuncts before `firstMayRaise` may be pushed; a pushed conjunct sees one relation's rows instead of the join's survivors | **HOLDS** (E7–E9, and the 24 `distribute` shapes of A.2) |
| 6 | **the DP reordering relations under a frozen conjunct** | a frozen conjunct sits in the residual FILTER above the join and is evaluated on the join's OUTPUT, which reordering preserves as a SET; whether an expression raises is a property of the set, not the sequence | **HOLDS** (E5, E6, E9; and `orderByWork` re-derives `firstMayRaise` against the schema of the child it is actually handed, `predicate_pushdown.cc:436-440`, so a conjunct whose index moved still freezes the right suffix) |
| 7 | **`GROUP BY` key / `HAVING` / `ORDER BY` expression that can raise, over a join** | evaluated on every row reaching their node; pushdown does not change the aggregate's or sort's input set | **HOLDS** (E11–E13) |
| 8 | **decorrelated `EXISTS` / correlated scalar whose BODY holds a partial expression** | the body is planned as its own block and its row set does not depend on the probe input | **HOLDS** (E17, E18) |

Two of these deserve the argument spelled out, because they are the ones that
look unsafe and are not:

**Row 6 — reordering cannot change a raise.** `rebuild` preserves the merged
schema's `(relation_slot, name)` pair SET (pass 4 A.1, re-verified in A.5 below),
so a frozen conjunct above the join still resolves to the same columns with the
same types; and the join's output as a SET is order-independent, so "some row
raises" is invariant. The one way order could matter is a LIMIT beneath the
filter, and there is none — `applyLimit` places a LIMIT at or below the
projection, never below a WHERE filter.

**Row 5 — the restamp does not change the verdict.** `distribute` calls
`restampSlots(c, 0)` before attaching a bucket to `children[1]`, and `filterOnto`
then re-runs `firstMayRaise` against that relation's own slot-0 schema. The
conjunct's operand types are the same columns either way (the merged schema
resolves them by slot, the leaf schema by slot 0 after the restamp), so the two
screens agree by construction rather than by luck.

### A.4 — P4-M1, re-confirmed at HEAD and RE-SIZED (still MEDIUM)

Unchanged at `b14d086`. The same two `--explain` lines:

    FROM laps l JOIN drivers d ON … JOIN drivers d2 ON d.team = d2.team
      LogicalJoin [driver_id@1 = driver_id] order=drivers@1,drivers@2,laps@0
                                            cost=43104 (written=60637) method=dp

    … the same spine … WHERE l.driver_id IN (SELECT d3.driver_id FROM drivers d3)
      LogicalSemiJoin [driver_id@0 = driver_id] join-ordering=skipped (semi/anti join)
        LogicalJoin [team@1 = team]           <- fully inner, 3 relations, NOT enumerated
          LogicalJoin [driver_id = driver_id]

**Re-sized with the clock rather than only with the model.** `--explain-analyze`,
`columnar/vectorized`, same query, written spine vs the same spine hand-written in
the order the DP picks for the un-semi'd version:

| | spine join time | total execution |
|---|---|---|
| `laps ⋈ drivers ⋈ drivers` (what the planner leaves) | 419.7 ms + 84.9 ms = **504.7 ms** | 1144.7 ms |
| `drivers ⋈ drivers ⋈ laps` (what the DP would pick) | 319.2 ms + 0.5 ms = **319.7 ms** | 929.8 ms |

**1.58x on the spine, 1.23x on the whole query**, answers identical (32193 both
ways). The cost model's own 43104 / 60637 is 1.41x, so the measurement and the
model agree on direction and roughly on size.

**The fix is still not one line, and both halves of pass 4's reason survive at
HEAD.** `subquery_lowering.cc:92-94` still does `Schema left_schema =
spine->output_schema;` and hands the copy to the join as its output schema
(`subquery_decorrelation.cc:813` likewise), and `subquery_lowering.cc:100-112`
still records that the loop comparing the two was DELETED because "it compared a
copy of one object with the object". Reordering the spine after that copy is
taken is precisely what makes those two objects genuinely different — the stored
schema would carry the WRITTEN column order while the child emits the DP's — and
the check that survives downstream (`vectorized_plan_builder.cc:846-851`, and
`VecHashJoinNode`'s constructor) compares only SIZE, which a permutation does not
change. So the fix is: enumerate `children[0]`, then RE-DERIVE the semi/anti
node's output schema from it, and restore the deleted equality as a real
assertion on two now-different objects.

Severity stays **MEDIUM**: declining is always legal, the loss is plan quality
only, and it is measured rather than asserted. Recorded a second time because the
comment that motivates the decline (`join_enumeration.cc:159-168`) still reads as
though the loss were unavoidable.

### A.5 — the structural probe re-run against the new plan shapes — **CLEAN**

Fix round 4 changed `applyLimit` (`logical_plan.cc:1042-1054`) to place a `LIMIT`
BELOW a projection that can raise, which moves the shapes pass 4's probe covered.
`probe_pairs` was rebuilt against `b5`'s `libswiftql_lib.a` and re-run, with the
new shapes added:

| shape | pair-set | sequence |
|---|---|---|
| 3-rel f1 spine, `ORDER BY d.age LIMIT 5` | SAME | PERMUTED |
| `SELECT *` over the 3-rel spine, `LIMIT 5` | SAME | PERMUTED |
| duplicate output name, passthrough / computed / aggregate (3 shapes) | SAME | PERMUTED (+ the known `(0,a) x2` duplicates) |
| **`SELECT l.speed * 1000000000000000 AS a … LIMIT 5`** (total DOUBLE projection: `applyLimit` does NOT descend) | SAME | PERMUTED |
| **`SELECT l.lap_id * 1000000000000000 AS a … LIMIT 5`** (partial INT projection: `applyLimit` DOES descend) | SAME | PERMUTED |
| derived relation as a join input, `ORDER BY … LIMIT 5` | SAME | PERMUTED |

The three `DUPLICATE (slot,name)` reports are pass 4's A.1.3 cases and are still
safe for the reason recorded there (stable sort over a schema order fixed inside
`LogicalPlanBuilder::build`, before any optimizer pass runs).

**And the behaviour under the new LIMIT placement agrees.** Seven shapes putting a
RAISING projection under a `LIMIT` over a join, four legs, ordered compare:

| # | shape | `vec` | `vecno` |
|---|---|---|---|
| L1 | raising `SUBSTRING` projection, `ORDER BY` with heavy ties, `LIMIT 3`, 3-rel spine | 3 rows | identical |
| L2 | raising INT-overflow projection, same | 3 rows | identical |
| L3 | raising projection, **plain** `LIMIT 3` (so `deterministicCut` inserts its sort ABOVE the project and `applyLimit` does not descend) | Error | Error |
| L4 | raising projection, TOTAL `ORDER BY`, `LIMIT 3` | Error | Error |
| L5 | raising projection over a DERIVED join input | 3 rows | identical |
| L6 | raising projection over a SEMI join | 3 rows | identical |
| L7 | raising projection over a LEFT join (all four legs) | 3 rows | identical on all four |

L1 vs L4 is worth stating so it is not mistaken for a defect: both descend the
LIMIT, and they differ only because the three rows the two orderings cut to are
different rows — one trio raises and the other does not. That is the rule working
as designed (the PLAN decides which rows the projection sees), and it is the same
in both legs.

---

## Part B — hunting what four passes missed

Everything in Part B was aimed at the join layer specifically: multi-way spines
under reordering, semi/anti in a chain, derived tables as join inputs,
decorrelated subqueries lowered to joins, join key types / composite keys / NULL
keys, and the predicate/residual split. **P5-B1 came out of the last of those** —
the predicate/residual split is where the join chain keeps a per-row expression
that is not a conjunct of any list, and that is exactly the object the round-4
rule does not reach.

### B5-1 — P5-B1's other movers, and the one negative that isolates the mechanism

The blocker is not confined to `distribute` on a two-relation join. Three more
instances, each with a different mover:

    R4  the masker is the pushed WHERE conjunct ALONE — chunk pruning contributes
        nothing (`chunks_skipped=0/2`, because `driver_id`'s zone map spans the
        full range in both chunks)
        SELECT COUNT(*) FROM laps l LEFT JOIN drivers d
          ON l.driver_id = d.driver_id AND SUBSTRING(l.team, l.driver_id - 5, 2) = 'x'
        WHERE l.driver_id > 10
          opt -> 4915      noop / rvol / cvol -> Error

    R5  the masker is `pushIntoDerived` + the FILTER-over-PROJECT descent: the
        LEFT join's PRESERVED side is a derived relation and the conjunct enters
        its body
        SELECT COUNT(*) FROM (SELECT l.lap_id AS lid, l.driver_id AS k, l.team AS t
                              FROM laps l) x
        LEFT JOIN drivers d ON x.k = d.driver_id AND SUBSTRING(x.t, x.k - 12, 2) = 'x'
        WHERE x.k > 15
          opt -> 2422      noop -> Error
          (optimized plan: LogicalFilter [(l.driver_id > 15)] lands BELOW the
           body's LogicalProject, three nodes under the LogicalLeftJoin)

    R6  the masker is `distribute` reaching relation 1 of a THREE-relation spine
        whose top join is the LEFT one
        SELECT COUNT(*) FROM laps l JOIN drivers d ON l.driver_id = d.driver_id
        LEFT JOIN drivers d2 ON d.team = d2.team AND SUBSTRING(d.name, d.age - 30, 2) = 'x'
        WHERE d.age > 33
          opt -> 4087      noop -> Error

R4 matters most: it rules out the storage layer. In R1 the zone map skips a chunk
in BOTH legs (`chunks_skipped=1/2` on the optimized and the `--no-optimize` leg
alike — and it is not the skip that saves the optimized leg, since `laps` chunk 1
carries `lap_id` up to 9998, i.e. the overflow), and R4 has no skip at all. So
the mover is the pushed filter, in all four instances, and the fix belongs in
`distribute`.

### B5-2 (MEDIUM, P5-M1) — inside a LEFT JOIN's `ON` clause there is NO conjunct cascade, so the SAME predicate text has two different error behaviours depending on the join type, and the rule that now DEFINES evaluation order does not say so

All four legs agree, so this is not a gate divergence — it is the project's own
newly-written rule being false for a construct the CLI accepts.

`expr_totality.h` states the rule as: *a conjunct is evaluated on the rows for
which every conjunct WRITTEN BEFORE IT evaluated TRUE*, and cites
`evaluatePredicate()` / `evalPredicate()` as implementing "exactly this cascade".
A LEFT join's ON residual is not run through either: it is one `Expr` handed to
`evaluate()` per candidate pair (`plan_nodes.cc:736-742`, and the vectorized
join's residual path), and `evaluate()` computes BOTH operands of an `AND` before
looking at the operator. Measured, same predicate text in three positions:

    ON  … AND l.lap_id > 9990 AND SUBSTRING(l.team, l.lap_id - 9990, 2) = 'x'   (LEFT JOIN)
          opt / noop / rvol / cvol -> Error: SUBSTRING: start position must be >= 1
    ON  … the identical text on an INNER JOIN (residual folded into the WHERE)
          all four legs -> 0
    WHERE l.lap_id > 9990 AND SUBSTRING(l.team, l.lap_id - 9990, 2) = 'x'
          all four legs -> 0

So `A AND B` inside a LEFT join's ON evaluates B on rows where A is FALSE, and
the same text one keyword away does not. That is a real semantic difference the
user can neither see nor predict from the rule as written, and it is the reason
P5-B1 is possible at all: an expression with no cascade has no "written before"
row set to protect, so the screen has nothing to hook onto and the row set gets
decided by whatever the optimizer leaves above the join.

**Ranked MEDIUM, not LOW**: it is not cosmetic (it decides whether a query
errors), it is not a leg divergence (all four agree), and the correct fix is a
decision the fixer has to make rather than a mechanical edit — either give the
residual the cascade (split it into conjuncts and evaluate them in order, which
makes the LEFT and INNER forms agree) or write the exception into
`expr_totality.h` and `join_condition.h`. Doing neither leaves the rule's central
sentence false.

### B5-3 — NULL semantics through the whole chain — **CLEAN** (16 shapes, three ways)

Every shape below uses a derived body that manufactures NULL keys with a LEFT
JOIN (`SELECT d.driver_id AS k, l.lap_id AS nk, d.team AS t FROM drivers d LEFT
JOIN laps l ON d.driver_id = l.driver_id AND l.lap_id < 3`), then puts those NULL
keys where the chain can drop or keep them. `vec`, `vecno` and SQLite agree on
all 16:

NULL key as an inner join key (2) and as a LEFT join key (20); `NOT IN` over a
NULL-bearing body (0 — the three-valued answer) against `NOT EXISTS` over the
same body (9998 — the two-valued one), which is the `ANTI_NOT_IN` / `ANTI` split
live and correct in both directions; `IN` over it (2); a NULL OUTER key against
`NOT IN` (0), `NOT EXISTS` (18) and `IN` (2); a COMPOSITE key with one NULL
component through a semi join (2) and an anti join (18); semi + anti stacked on a
spine carrying the NULL-key derived input (12); an anti join over a LEFT-joined
spine (4409); a correlated scalar over a spine with a NULL key (2); two derived
relations joined on a composite key, both aliasing the same names (10000); a
DOUBLE key (`AVG(...)`) joined to an INT key with integral values (20); and a
four-layer chain derived -> LEFT join -> semi -> anti (18).

### B5-4 — join key types, re-confirmed at HEAD — **CLEAN**

Pass 3's nine K-shapes re-run on the two-CSV catalog (`ids(code STRING)`,
`nums(n INT)`), `columnar/vectorized`: all eight producible shapes are refused by
name, and each names its producer (`JOIN ON:`, `IN / EXISTS subquery:`,
`NOT IN subquery:`, `NOT EXISTS subquery:`, `join key: … the subquery's key
column`), including K9 (derived relation as a join input). The two same-type
controls answer. So `Validator::validateJoinKeyTypes` still covers all four
`JoinKey` producers at `b14d086`.

### B5-5 — the standing sweep

**P4-L3 is CLOSED.** `predicate_pushdown.h:27-37` now names four moves, says the
count is part of the claim, and names the second screen the fourth move carries.

**P4-L1 is still OPEN, and it has a SECOND live copy** (LOW, carried).
`sort_comparator.h:100-101` still says "a PROJECTED schema's order is a function
of the SELECT list rather than of the plan", which is false for `SELECT *`
(`logical_plan.cc:1247-1266` copies the CHILD's schema column by column). The
copy pass 4 did not find is `logical_plan.cc:987`, in `deterministicCut`'s own
header: "the projected schema's own order is a function of the SELECT list". Same
sentence, same falsity, same surviving conclusion (both orders are fixed inside
`LogicalPlanBuilder::build`, before any optimizer pass runs). Two files now, one
of them the file the fix round touched.

**P4-L2 is still OPEN after three fix rounds** (LOW, carried). Verbatim at HEAD:
`join_enumeration.cc:585`, `cardinality_estimator.cc:464` and `:513` still name
`hasSlotOutsideRangeTable`, gone since `18af84f`; and `development.md:808` still
asserts "**The decline is silent** … there was no ordering decision to report",
which `development.md:854` corrects, which
`join-ordering=skipped (semi/anti join)` contradicts in the plan output, and
which is the premise P4-M1 turns on.

**The fifth retracted paragraph the prompt predicts is `expr_totality.h`'s own
enumeration, and it is P5-B1's.** The file written this round to be the single
statement of the rule opens with "THE THREE CONSUMERS, and why each needs the
same answer" and the sentence "every other expression is evaluated on every row
that reaches its node". A LEFT join's ON residual is a fourth site whose row set
a plan rewrite moves, and the sentence quietly assumes no rewrite changes what
reaches a node — which is precisely what `distribute` does. `predicate_pushdown.h`
inherits it ("All four are screened now"), and `development.md`'s Week 32
consumer table states the mechanism as a virtue: "The recursion into `children[0]`
stays unconditional, which is what keeps a WHERE conjunct reaching the spine's
scans". All three must be swept with the fix. **That is five consecutive passes in
which the defect was an enumerated precondition one item shorter than the thing
it governs.**

### B5-6 — P5-B1 is not an artifact of the f1 catalog

Same shape on `data/tpch/sf0.01`:

    SELECT COUNT(*) FROM orders o
    LEFT JOIN customer c
      ON o.o_custkey = c.c_custkey
     AND SUBSTRING(o.o_orderstatus, o.o_custkey - 140, 2) = 'x'
    WHERE o.o_custkey > 145
      opt -> 13532      noop / rvol / cvol -> Error: SUBSTRING: start position must be >= 1
    the same without the WHERE -> Error on all four legs.

And it is not a `COUNT(*)` artifact: the projected form

    SELECT l.lap_id FROM laps l LEFT JOIN drivers d
      ON l.driver_id = d.driver_id AND SUBSTRING(l.team, l.driver_id - 12, 2) = 'x'
    WHERE l.driver_id > 15 LIMIT 3
      opt -> 1, 8, 11      noop / rvol / cvol -> Error

**The single guard proposed in P5-B1 covers all six instances**, which is worth
stating because R5 and R6 route through different `apply` arms. In R5 the
conjunct reaches the derived body only because `distribute` walked past the LEFT
join into `children[0]` and `filterOnto` planted it there; `apply`'s child loop
(and `applyToSpineLeaves`) then found a FILTER-over-DERIVED that would not exist
if the recursion had declined. Same for R6's deeper relation. Declining the
`children[0]` recursion at a LEFT join with a raising residual leaves the bucket
in `by_slot`, and the existing leftover loop lifts it above the whole tree.

### B5-7 — the shapes I could NOT break, recorded so the negatives mean something

* **A correlated INEQUALITY cannot become an ON residual.** `EXISTS (SELECT 1
  FROM drivers d WHERE d.age > l.driver_id)` is refused by name, and the refusal
  text names the very object P5-B1 is about: "a correlated inequality has no
  equi-join to lower to; it would have to ride as an ON residual on the semi/anti
  join". So the decorrelation path cannot manufacture a second residual site.
* **A semi/anti join carries no ON residual** — asserted at
  `cardinality_estimator.cc:473-475` and pinned at
  `tests/test_join_enumeration.cc:893`. `grep` confirms `on_residual` is assigned
  in exactly one place in the whole tree (`logical_plan.cc:1132`), under
  `jc.type == JoinType::LEFT`. That single site is what bounds P5-B1.
* **An outer join never swaps build and probe** (`vectorized_plan_builder.cc:1093-1102`,
  `from_builds` forced false, and the `(outer: the preserved side must probe)`
  suffix says so in the plan), so the physical layer cannot change the candidate-
  pair set on its own.
* **Engine laziness cannot mask the residual under a `LIMIT`.** `deterministicCut`
  puts a sort beneath every `LIMIT` over a join, which is a pipeline breaker, so
  Volcano's pull-based `LimitNode` cannot stop the probe loop early. Both LIMIT
  forms (plain and `ORDER BY`) raise on all four legs.
* **`applyLimit` cannot descend a `LIMIT` below a join.** Its only descent is
  past a `PROJECT` (`logical_plan.cc:1043`), and `orderIsPlanStable` returns
  false for a `JOIN`, so a `LIMIT` over an un-ordered join always gets the
  `deterministicCut` sort first.
* The 36-shape comparison matrix (A.2), the 18-shape evaluation-order battery
  (A.3), the 16-shape NULL battery (B5-3) and the K1–K9 key-type battery (B5-4)
  produced no other disagreement.

### B5-8 — the guard has to be PER-JOIN, and no existing pin can catch P5-B1

**Per-join.** The LEFT join carrying the raising residual does not have to be the
top of the spine:

    SELECT COUNT(*) FROM laps l
    LEFT JOIN drivers d ON l.driver_id = d.driver_id
                       AND SUBSTRING(l.team, l.driver_id - 12, 2) = 'x'
    JOIN drivers d2 ON l.team = d2.team
    WHERE l.driver_id > 15
      opt -> 7786      noop -> Error

    LogicalJoin [team@0 = team] join-ordering=skipped (outer join)
      LogicalLeftJoin [driver_id = driver_id] residual=(SUBSTRING(l.team, (l.driver_id - 12), 2) = x)
        LogicalFilter [(l.driver_id > 15)]      <- descended PAST the LEFT join
          LogicalScan [laps, 2 columns]

`distribute` already recurses join by join and already re-applies its INNER/LEFT
test at each one (`predicate_pushdown.cc:476-483` says so explicitly), so the new
condition goes in the same place and inherits that property. Nothing else changes.

**No existing pin can catch it, and the control pin stays green.**
`tests/test_predicate_pushdown.cc:871`
(`PreservedSidePredicateStillPushesThroughALeftJoin`) pins exactly the move
P5-B1 shows is unsafe — but its query has **no ON residual at all**
(`drivers d LEFT JOIN laps l ON d.driver_id = l.driver_id WHERE d.age > 30`), so
a residual-aware guard leaves it passing. That is the shape of the missing pin:
the same query plus a raising ON residual, asserting the filter now stays ABOVE
the join. The three totality pins at `:1048`, `:1073` and `:1121` all put the
partial expression in a WHERE conjunct; `:1486`/`:1509` put it in a projection.
None puts it in an `ON` clause, and `grep` finds no test in the tree that does.

### B5-9 — the randomized joint sweep

`$SCRATCH/gen5.py`, aimed at the join layer rather than at ground passes 3 and 4
already covered: left-deep spines of 2–4 relations mixing base tables and five
derived-body shapes, `JOIN` / `LEFT JOIN` at each level, single and COMPOSITE
`ON` keys, total and PARTIAL `ON` residuals, `IN` / `NOT EXISTS` predicates
stacked on the spine, cross-relation WHERE conjuncts, and an `ORDER BY 1 LIMIT n`
tail on a fifth of the shapes. Two independent modes:

* **mode A (semantic)** — every expression total; three legs, `vec` / `vecno` /
  SQLite, sort-normalised positional TSV.
* **mode B (error behaviour)** — a PARTIAL expression planted in a WHERE conjunct,
  an `ON` residual of a LEFT join, or the select list; `vec` vs `vecno` only,
  because SQLite promotes on overflow and cannot adjudicate. Both legs refusing
  with the SAME message counts as agreement; different messages would be a diff.

A `--no-optimize` TIMEOUT at the harness's 90-second cap is counted as a SKIP,
not as agreement: a leg that did not finish has not agreed with anything. (These
are real — the unoptimized leg materialises the whole join product on a 4-relation
spine, in a Debug build.)

**Result: pending at the time this section was written; the tally is appended
below rather than estimated.** Everything reported above is hand-built and
reproducible from the CLI, so no finding in this file depends on the sweep.

---

## Summary

| Severity | Count | IDs |
|---|---|---|
| BLOCKER | 1 | P5-B1 |
| HIGH | 0 | — |
| MEDIUM | 2 | P5-M1 (new), P4-M1 (carried, re-confirmed and re-sized) |
| LOW | 2 | P4-L1 (carried, still open — and a SECOND copy found), P4-L2 (carried from pass 2's B-1, still open after three fix rounds) |

**Fix round 4 closed both of pass 4's blockers, and closed them well.** P4-B1's
FILTER-over-PROJECT descent now screens the whole select list against the
projection's input schema, all-or-nothing — and because the screen is type-aware,
the ordinary DOUBLE-arithmetic body still descends, which is the third of the
three options I recorded in A.4.4 and the only one that costs nothing. All three
of pass 4's failing shapes now refuse on both legs, and B3-3's pushdown is intact
on the exact body its 9.4x was claimed on. P4-B2's carve-out is gone: `mayRaise`
moved into `parser/expr_totality.h`, took a schema, and now screens a comparison
by operand type — verified across the full cross product of six operators, both
operand orders and all three movers, 36/36 agreeing where pass 4 measured `Error`
vs `0`, with the freeze and its control both visible in `--explain`. P4-L3 is
closed. The structural probe still reports SAME pair-sets on every shape,
including the two the new `applyLimit` moves.

**What this pass returns is the same defect one construct further out.** Round 4
did something larger than fix two bugs: it made SwiftQL **define** evaluation
order, in one file, for one screen, with three named consumers. I worked that
definition through the join layer shape by shape — the INNER ON residual, the
semi/anti probe predicate, the straddling conjunct, the predicate pushed to one
side, the DP reordering under a frozen conjunct, GROUP BY / HAVING / ORDER BY over
a join, a decorrelated body — and seven of the eight hold, several of them for
reasons that are structural rather than lucky.

The eighth is **P5-B1**. A LEFT JOIN's `ON` residual is the one per-row expression
in the join layer that is not a conjunct of any list the screen reads: Week 29
split it off from the WHERE fold precisely because ON and WHERE are not
interchangeable for an outer join, and it has been evaluated inside the probe loop
ever since. `distribute` recurses into the preserved side unconditionally on
σ_p(R) ⟕ S ≡ σ_p(R ⟕ S) — which is a SET equivalence about the join's OUTPUT and
says nothing about its CANDIDATE PAIRS, the thing the residual is actually
evaluated on. That is the identical shape of argument P4-B1 turned on, one round
later, in the neighbouring function.

    SELECT COUNT(*) FROM laps l LEFT JOIN drivers d
      ON l.driver_id = d.driver_id AND l.lap_id * 1000000000000000 > 0
    WHERE l.lap_id < 5
      optimized -> 4      --no-optimize -> Error      row/volcano -> Error      columnar/volcano -> Error

Six instances, four movers (`distribute` at the top join and at a deeper one,
`pushIntoDerived` + the PROJECT descent, and a LEFT join at the bottom of a
three-relation spine), three partial operators, on both the shipped `catalog.json`
and TPC-H `sf0.01`; five negative controls including the INNER analogue, which is
safe *because* its residual becomes a conjunct in the list the screen reads. The
attribution is airtight: the three legs that agree are exactly the three that
never call `PredicatePushdown`. The fix is one condition in `distribute`, in the
same per-join test that already distinguishes INNER from LEFT.

**P5-M1** is what makes P5-B1 possible and outlives it: inside a LEFT JOIN's ON
clause there is no conjunct cascade at all, so `ON k = k AND A AND B` evaluates B
on rows where A is FALSE while the same text in a WHERE, or on an INNER join, does
not. All four legs agree, so it is not a gate divergence — it is the central
sentence of `expr_totality.h` being false for a construct the parser accepts.

**P4-M1 is unchanged and now measured with a clock**: the fully inner spine below
a declined semi/anti join is still never enumerated (43104 against 60637 by the
model; **1.58x on the spine's join time and 1.23x on total execution** by
`--explain-analyze`), and both halves of pass 4's "not one line" still hold at
HEAD — the semi join still stores a copy of the spine's schema and the equality
check that would catch the drift is still deliberately absent, with only a SIZE
check downstream.

**And the standing rule held for the fifth pass running.** The retracted paragraph
is in `parser/expr_totality.h` — the file written this round to be the single
statement of the rule. It opens with "THE THREE CONSUMERS" and asserts that every
expression outside them "is evaluated on every row that reaches its node", which
quietly assumes no rewrite changes what reaches a node. `distribute` changes it.
`predicate_pushdown.h` inherits the claim ("All four are screened now") and
`development.md`'s Week 32 table states the mechanism as a virtue. Separately,
pass 4's own P4-L1 is still open and has a **second** live copy of the false
sentence, at `logical_plan.cc:987`; and P4-L2 — three source comments naming a
function deleted in `18af84f`, plus `development.md:808` asserting the opposite of
what the code does — is now open after three fix rounds.

**Verdict: the join chain's seam is NOT clean. One blocker — `optimized` answers
where `--no-optimize` and both Volcano legs throw, on a LEFT JOIN whose `ON`
residual can raise — and it is the same class as the two this round fixed, in the
one per-row expression the new rule's own enumeration does not name.**

---

## Provenance

Every measurement in this file was taken against `b14d086`, from an out-of-tree
Debug build in a private worktree (`$SCRATCH/wt5` + `$SCRATCH/b5`); no shared
build lock was taken and no source file in `/home/user/swiftql` was modified by
this pass. The only file its commits touch is this one. Concurrent auditors
pushed to the same branch throughout; none of their commits touched `src/`.

Every finding here is reproduced from the CLI on catalogs already in the repo
(`catalog.json`, `data/tpch/sf0.01/catalog.json`) plus the two-CSV key-type
catalog passes 3 and 4 used, so re-pinning any of them against a newer HEAD is one
command each.
