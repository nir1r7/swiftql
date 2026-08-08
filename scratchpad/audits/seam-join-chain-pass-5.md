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

The fix is at `predicate_pushdown.cc:730-756`: before any conjunct is offered to
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
join, on the identity σ_p(R) ⟕ S ≡ σ_p(R ⟕ S) (`predicate_pushdown.cc:474-480`).
That identity is exactly the same kind of claim P4-B1 turned on: **it is a SET
equivalence and it is only half the obligation.** It preserves the join's
OUTPUT; it does not preserve the set of CANDIDATE PAIRS, and the ON residual is
the expression evaluated on the candidate pairs. Fewer left rows means fewer
pairs means a raise the residual was owed is masked.

Nothing screens `on_residual`. `firstMayRaise` is applied to the WHERE conjunct
list in `pushIntoJoin` (`:541`), to the derived-body list in `pushIntoDerived`
(`:625`) and to the descent list in `apply` (`:735`). `on_residual` is not a
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
* `predicate_pushdown.cc:474-486` — the paragraph that justifies the
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
HEAD.** `subquery_lowering.cc:92-95` still does `Schema left_schema =
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
