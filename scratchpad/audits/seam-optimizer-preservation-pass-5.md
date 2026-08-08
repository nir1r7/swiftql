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

Two passes do exactly that and neither calls the screen.
`logical_plan.cc:1147-1183` splits the WHERE into conjuncts and hands the vector
to `lowerInSubqueries`, `lowerExistsSubqueries` and `lowerCorrelatedScalars`.
The first two REMOVE their conjunct from the vector and attach a **semi-join** to
the spine — i.e. **below** the `LogicalFilter` the remaining conjuncts end up in
— and a semi-join is row-REDUCING. Every surviving conjunct, including ones
written EARLIER than the one that was removed, is then evaluated on the
semi-join's survivors.

(`lowerCorrelatedScalars` is the third and it is **clean**, checked rather than
assumed: it attaches a `LogicalLeftJoin` to a `LogicalDerived` aggregate, which
is row-PRESERVING, so nothing upstream loses a row. Verified by execution —
`WHERE l.lap_id * 9223372036854775807 > 0 AND l.speed > (SELECT AVG(d.age) FROM
drivers d WHERE d.driver_id = l.driver_id AND d.age > 999)` still raises even
though the correlated aggregate matches nothing, and `--explain` shows the
`LogicalLeftJoin [driver_id = $k0]` that is the reason. The choice of LEFT over
INNER there was made for scalar-subquery NULL semantics, and it happens to be
what keeps this pass on the right side of the totality rule; nothing in either
file says the two are connected.)

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

---

## HIGH P5-2 — `optimized != --no-optimize`, constructed and run. The totality
## screen's ColumnRef **bare-name fallback** makes a FOREIGN relation's conjunct
## type as TOTAL against the scanning relation's schema, so
## `collectSimplePredicates` walks straight past a raiser and a later conjunct
## prunes the rows it was owed. P4-1's second raise site is NOT closed.

### The mechanism

`staticTypeOf`'s ColumnRef arm (`expr_totality.h:64-75`) resolves slot-first with
a **bare-name fallback**:

```cpp
int idx = (col->id.isResolved() && col->id.isLocal())
    ? schema.indexOf(col->column_name, col->id.localSlot("staticTypeOf")) : -1;
if (idx < 0) idx = schema.indexOf(col->column_name);
```

That is the right rule for `evaluate()` — it is copied from
`resolveColumnIndex` — but `ChunkPruner` calls the screen with **the scanning
node's own schema** (`vec_scan_node.cc:22`, `plan_nodes.cc:67` pass `schema_`),
and on the `--no-optimize` leg the hint is the WHOLE un-pushed WHERE, which names
the other relation. A `b.v` ref carrying slot 1 misses the slot-qualified lookup
against relation `a`'s scan schema and then **falls back to the bare name and
finds `a.v`**. If `a.v` and `b.v` have different types, the screen types the
conjunct against the wrong column and can answer TOTAL for a conjunct that
raises.

`collectSimplePredicates` then does not stop, reaches a later conjunct, and that
one proves a zone-map skip. `chunk_pruner.h:29-34` states the case it is
supposed to refuse, in its own words:

> `j < k` — C_j IS evaluated on rows of that chunk, and skipping removes them.
> Unsound; this walker stops before reaching C_k.

It does not stop.

### The failing shape, constructed and run both ways

Two tables sharing a column NAME with different TYPES — the shipped
`catalog.json` has no such pair (`team` is STRING/STRING, `driver_id` INT/INT),
so this needs a catalog, written to scratch and not into the repo:

```
a(k INT, v INT)      20 000 rows, k = 1..20000, v = k % 7   (3 chunks)
b(k INT, v STRING)   20 rows,     v = 's1'..'s20'
```

`b.v = 5` is a STRING-vs-INT comparison: it raises per row from
`Value`'s `NUMERIC_COERCE` and both legs agree it must
(`SELECT b.k FROM a JOIN b ON b.k = a.k WHERE b.v = 5` → *Error: Type mismatch in
Value comparison*, every mode).

```sql
SELECT a.k FROM a JOIN b ON b.k = a.k
WHERE a.v = 5 AND b.v = 5 AND a.k > 999999
```

| mode | optimized | `--no-optimize` |
|---|---|---|
| columnar + vectorized | **Error: Type mismatch in Value comparison** | **0 rows** |
| columnar + volcano | 0 rows | 0 rows |
| row + volcano | Error | Error |
| row + vectorized | refused (needs columnar) | — |

`optimized != --no-optimize` in the col/vec cell — the same single differential
cell every finding in this seam has landed in, and for the reason pass 4 stated
(the flag gates only the vectorized optimizer). Note the second divergence in the
same table: **columnar/volcano answers 0 rows where row/volcano errors**, on an
identical plan, which isolates the cause to the zone-map skip and nothing else.

Control, isolating the skip: change only the threshold so no chunk proves
anything —

```sql
... WHERE a.v = 5 AND b.v = 5 AND a.k > 0     -- Error, both legs, every mode
```

### Why each leg does what it does

* **optimized.** `firstMayRaise` over the JOIN schema types `b.v` correctly
  (slot 1 resolves) → frozen = 1. `a.v = 5` alone is pushed to `a`'s scan and
  becomes that scan's hint; `a.v = 5` proves no skip (v ∈ 0..6). The residual
  `b.v = 5 AND a.k > 999999` stays above the join and is evaluated on real rows.
  Raises. `--explain` shows exactly this:

  ```
  LogicalFilter [((b.v = 5) AND (a.k > 999999))]
    LogicalJoin [k = k]
      LogicalFilter [(a.v = 5)]
        LogicalScan [a, 2 columns]
      LogicalScan [b, 2 columns]
  ```

* **`--no-optimize`.** Nothing is pushed, so the whole WHERE reaches `a`'s scan
  as the hint, with `a`'s schema. The walker: `a.v = 5` total, collected, proves
  nothing; **`b.v = 5` typed against `a.v` (INT) → total → walk CONTINUES**;
  `a.k > 999999` collected and skips every chunk. `a` yields no rows, the join is
  empty, the filter never runs.

### Ranking

**HIGH**, on the precedent pass 4 set for P4-1, which is the same shape: two legs
disagree on a CLI-typable query, the failing side is loud, no row is wrong. It is
recorded as the headline because it is a **reopening of the fix round 4 shipped
for exactly this raise site** — the screen was made schema-aware, and being
schema-aware is what introduced a way to consult the WRONG schema. The
`chunk_pruner.h` header now carries a paragraph explaining that a conjunct the
scan's schema cannot type is treated as may-raise; that is true of a conjunct
whose NAME is absent, and false of one whose name collides.

Two properties make it worse than a corner case:

1. **Shared column names with differing types are ordinary.** `id`, `name`,
   `code`, `status`, `value`, `type` — a two-table schema where one is INT and
   the other STRING is a routine shape, and the conjunct that triggers it
   (`b.v = 5` against a STRING column) is the very typo P4-1 was written about.
2. **No harness can reach it.** `compare_against_sqlite.py` and the regression
   suite run against `catalog.json` and TPC-H sf0.01, and neither has a
   cross-table name collision with a type mismatch. The gate being green is not
   evidence here — it is evidence that the corpus has no such catalog.

The narrow fix is in the screen, not the pruner: `staticTypeOf` should answer
"don't know" (return false) for a ColumnRef whose slot-qualified lookup MISSES,
rather than falling back to the bare name, **when the caller is the pruner** —
i.e. the fallback belongs to `evaluate`'s resolution rule and not to a screen
that is deliberately asked about a schema the expression was not written against.
Sharing one function between a resolver and a screen is what merged the two
rules. That is a fix note, not a fix; I touched no source.

