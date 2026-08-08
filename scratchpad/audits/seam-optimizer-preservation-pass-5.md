# Seam audit — optimizer result preservation (pass 5, FINAL)

HEAD `b14d086`, branch `claude/phase5-week26-qomtkb`. Binary `build/swiftql`;
`find src -type f -newer build/swiftql` is EMPTY, so it is this HEAD's build. No
source file touched. Unless stated otherwise every measurement is a single CLI
invocation with `--no-cache --execution vectorized --storage columnar`, run on
both legs by `scratchpad`'s two-line dual-leg wrapper.

Predecessors: `seam-optimizer-preservation-pass-1.md` … `-4.md`.

Status: IN PROGRESS (written incrementally; summary at the end).

---

## 0. Round-4's own repros, re-run first

Pass 4's four P4-1 divergences, verbatim, on this HEAD:

| # | query | OPT | `--no-optimize` |
|---|---|---|---|
| 1 | `WHERE team = 5 AND speed > 999999` | Error: Type mismatch | Error: Type mismatch |
| 2 | `WHERE team > 'zzzzz' AND team = 5` | 0 rows | 0 rows |
| 3 | `WHERE 5 = team AND speed = 333.3333` | *(see §1)* | |
| 4 | `WHERE team LIKE 'zzz%' AND 5 = team` | 0 rows | 0 rows |

**All four agree.** Note the direction of the fix on #1: the optimized leg used
to answer 0 rows and now ERRORS — i.e. `ChunkPruner::canSkipChunk`'s new
STRING-boundary decline plus the freeze made the loud leg the common one. That
is the right direction (the definition in `expr_totality.h` says `team = 5` is
evaluated on every row, so it must raise), and it is a user-visible behaviour
change on the optimized leg that no harness query covers.

---

## MEDIUM P5-1 — the definition has a THIRD hole that no screen guards, and it is
## not in `exprMayRaise` at all: `lowerInSubqueries` / `lowerExistsSubqueries`
## DELETE a conjunct from the WHERE and interpose a semi-join BELOW the filter,
## which changes the row set of every conjunct WRITTEN BEFORE IT.

`expr_totality.h` states the rule absolutely:

> Nothing may change that set for an expression that CAN RAISE: not a plan
> rewrite (predicate_pushdown.cc), not a storage-level chunk skip
> (chunk_pruner.h), not an engine's choice of when to be lazy.

Three passes do exactly that and none of them calls the screen.
`logical_plan.cc:1147-1183` splits the WHERE into conjuncts and hands the vector
to `lowerInSubqueries`, `lowerExistsSubqueries` and `lowerCorrelatedScalars`,
each of which REMOVES its conjunct from the vector and attaches a semi-join /
join to the spine — i.e. **below** the `LogicalFilter` the remaining conjuncts
end up in. Every surviving conjunct, including ones written EARLIER than the one
that was removed, is then evaluated on the semi-join's survivors.

Constructed, run, both outputs recorded (shipped `catalog.json`, columnar +
vectorized, `--no-cache`; identical on `--no-optimize`, and no other mode
supports the shape):

| conjunct 1 (raises on every row) | conjunct 2 (eliminates every row) | result |
|---|---|---|
| `lap_id * 9223372036854775807 > 0` | `AND driver_id > 999999` | **Error: integer overflow in `*`** |
| `lap_id * 9223372036854775807 > 0` | `AND driver_id IN (999999)` | **Error: integer overflow in `*`** |
| `lap_id * 9223372036854775807 > 0` | `AND driver_id IN (SELECT driver_id FROM drivers WHERE age > 999)` | **0 rows** |
| `l.lap_id * 9223372036854775807 > 0` | `AND EXISTS (SELECT 1 FROM drivers d WHERE d.driver_id = l.driver_id AND d.age > 999)` | **0 rows** |

Three spellings of "and nothing matches", one of which is a *constant* `IN` list
and therefore stays a conjunct. The first two raise, because the definition says
conjunct 1 is evaluated on every row. The last two do not, because the pass moved
the elimination underneath the filter. Same query, same rows, opposite error
behaviour, decided by which spelling of the second predicate the user chose.

The second-order consequence is sharper than the error itself: **for a lowered
conjunct, WRITTEN POSITION STOPS MATTERING.** `WHERE IN(subq) AND raiser` and
`WHERE raiser AND IN(subq)` both answer 0 rows — the semi-join is below the
filter either way — whereas for every other conjunct pair the order is exactly
what `firstMayRaise` exists to preserve. The rule the codebase now states as a
definition is true of four movers and false of three passes that predate it.

**Not a leg divergence.** All three lowerings run inside `LogicalPlanBuilder::build`,
before the `--no-optimize` gate, so `optimized == --no-optimize` in every cell and
`run_optimizer_invariant` cannot see it. That is precisely why it is worth
recording: round 4 raised this seam's contract from "the two legs agree" to "the
PLAN fixes the row set", and the second claim is strictly stronger than what the
code delivers. Ranked MEDIUM rather than HIGH because no answer is wrong and no
row is lost — the failure is that a documented absolute is not absolute.

