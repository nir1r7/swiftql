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
