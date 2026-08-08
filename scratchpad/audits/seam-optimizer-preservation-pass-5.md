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

---

## Part A — `exprMayRaise`, form by form

Every raise site reachable per row is in three files and nowhere else. I checked
this rather than inherited it: `grep -n "throw"` over `src/execution/columnar_eval.cc`
and `src/execution/expression_executor.cc` returns **no throw statements at all**
— every mention is a comment about declining a shape so `evaluate()` raises it.
So `evaluator.cc` + `common/value.cc` + `execution/checked_arith.h` is the whole
set, and modelling `evaluate()` is modelling all three engines. That is a real
strengthening of the argument since pass 4, and it is checkable in one command.

`ChunkPruner::canSkipChunk` is the FOURTH site, outside `evaluate()` entirely,
and round 4 guarded it directly (`chunk_pruner.h:150-155`: a STRING/non-STRING
zone-map/literal pair returns false). Verified: `WHERE team = 5 AND speed > 999999`
now errors on both legs where it used to answer 0 rows optimized.

### The table

Each row: the form, whether `evaluate()` can raise on it, what the screen
answers, and how I decided. "run" = constructed and executed on both legs.

| form | can raise? | screen | verdict |
|---|---|---|---|
| `Literal` | no | false | exact |
| `ColumnRef` | yes — *Column not found* (`evaluator.cc:92`) | `!staticTypeOf` | exact for a LOCAL ref; **wrong for a foreign ref — P5-2** |
| `+ - * /`, INT/INT | yes — `checkedAdd/Sub/Mul/Div`, and `INT64_MIN / -1` | true | exact-and-necessary. Cost measured, §P5-3 |
| `+ - * /`, any DOUBLE | **no** — `evaluator.cc:155-160` takes the plain-IEEE branch | false | **exact.** This is round 4's fix and it is right |
| `+ - * /` with a STRING operand | refused by `inferExprType` at plan time | true | over-approximate, free (unreachable) |
| unary `-`, INT | yes — `checkedNegate` | true | exact |
| unary `-`, DOUBLE | no | false | exact |
| `AND` / `OR` with a non-INT operand | yes — `asInt()` → `bad_variant_access` (`value.cc:28`) | `l != INT \|\| r != INT` | exact. `WHERE speed AND …` run both orders: agrees |
| comparison across the STRING boundary | yes — `NUMERIC_COERCE` (`value.cc:57`) | `(l==STRING) != (r==STRING)` | exact **as an expression**; P4-1's hole is closed. Run: 4/4 repros agree |
| comparison, INT vs DOUBLE | no — coerced | false | exact |
| comparison against a NULL `Literal` | **no** — `value.cc` returns false before the type check, which the screen's own comment states | reports `null_type` ⇒ can answer TRUE | **over-approximate, and it costs — §P5-4** |
| `IN` over a constant list | yes, same boundary | per-value STRING test | exact; `values` is `vector<Value>` so the list hides no expression. Run: agrees both orders |
| `LIKE` | yes — *LIKE requires a STRING operand* | operand must type STRING | exact. `pattern` is a `std::string`, not an Expr, so there is no computed-pattern case at all |
| `SUBSTRING`, constant bounds | no — `inferExprType` decides the domain at plan time, both legs | false | exact; TPC-H Q22 stays total. Verified `1 + 1` accepted, `1 - 1` refused |
| `SUBSTRING`, computed bounds | yes — `substringOf` | true | exact. Run: guard-first is total, raiser-first errors, **and still errors when a later conjunct eliminates every row** |
| `CASE` arms | yes, any arm | screens EVERY arm + each condition must type INT | over-approximate ON PURPOSE (`evaluate` short-circuits, `ExpressionExecutor` declines the node) — correct, and the stricter answer is the only one both engines can honour. Run: agrees both orders |
| `CASE` with a `case_operand` | — | — | **not a case**: `ast.h:128-137` says the simple form is not supported; searched CASE only |
| `NOT x` | — | — | **not a case**: the parser refuses a standalone `NOT` (*"NOT is supported only as NOT BETWEEN, NOT LIKE, NOT IN or IS NOT NULL"*). Negation lives on `InExpr::negated` / `LikeExpr::negated` / `IsNullExpr::is_not_null`, all screened through their operand |
| `IS NULL` | operand IS evaluated | recurses | exact |
| `AggregateExpr` | yes — output column absent | `!staticTypeOf` | exact; `evaluate` reads the precomputed column and recomputes nothing |
| a subquery inside a conjunct | `SubqueryExpr::evaluate` throws unconditionally | true (falls through) | correct and unreachable — but see **P5-1**, where the LOWERING of that subquery is the problem, not its evaluation |
| a correlated reference | — | bare-name fallback | same root cause as P5-2; see below |
| a UDF-like builtin | — | — | **`SUBSTRING` is the only one.** `ast.h:141-147` says so and prescribes a `FunctionExpr` + registry if a second arrives. If that happens it lands on the final `return true` — safe |

### The one class the table cannot close: `staticTypeOf` fails OPEN on an OPERATOR, not on a subtype

The file's stated defence (`expr_totality.h:44-46`) is:

> A MISSED Expr subtype here must answer TRUE, which the final `return true`
> delivers.

That is true at **subtype** granularity and **false at operator granularity**, in
three places, and it is the direct answer to Part B's "can it fire on a shape
added later than it was written":

1. `staticTypeOf`'s `BinaryExpr` arm ends `out = TypeId::INT; return true;` for
   anything that is not `+ - * /`. A new operator — string concatenation `||` is
   the obvious next one — is silently typed INT.
2. `exprMayRaise`'s `BinaryExpr` arm ends `return (l == STRING) != (r == STRING);`
   for the same set. `'a' || 'b'` would be typed STRING/STRING and answer
   **FALSE — total**, and a concat kernel that can raise (allocation, or a length
   cap) would be classified total from day one.
3. Neither function tests `un->op` at all. `UnaryExpr` is treated as *the negate
   node* by both. `ast.h:66-70` says `// "-"` and `Parser::parseUnary` enforces
   it, so this is correct today — and a `NOT` node added as a `UnaryExpr` would
   make `staticTypeOf` report the OPERAND's type (should be INT) and
   `exprMayRaise` answer FALSE for `NOT speed`.

Three files must stay in lockstep (`ast.h`'s comment, `parseUnary`, and these two
functions) and nothing asserts it. Recorded as **LOW P5-5**: not wrong today,
wrong by default on the next operator, and the header's own safety claim is what
would stop a reader from checking.

### The four movers, re-verified at this HEAD

Pass 4 found B3-2 had three causes where the audit named one, so I re-derived the
partition at every site from source rather than trusting the previous pass.

| mover | how the boundary is enforced | verdict |
|---|---|---|
| `orderByWork` (`:437-447`) | `std::stable_sort(begin, begin + frozen)` — the frozen tail is not in the range | sound. Screened against `scan_child->output_schema`, which is the schema the conjuncts were written against at every call site (`filterOnto` is the only caller) |
| `pushIntoJoin` (`:541-585`) | `int slot = (i >= frozen) ? -1 : soleSlot(c);` then the leftover buckets are re-merged carrying their WRITTEN index and `std::sort`ed by it | sound, and the re-sort is what makes "the residual is in written order" true by construction rather than by argument. A leftover bucket (LEFT / semi / anti) cannot land after a frozen conjunct |
| `distribute` (`:526-527`) | `join_type == INNER && semantics == STANDARD`, spelled positively so a future RIGHT/FULL is refused by omission | sound. Run: the LEFT-join repro that diverged in pass 4 now agrees |
| `pushIntoDerived` (`:623-641`) | `if (i >= frozen) … staying` | sound. A conjunct that fails `remapOntoDerivedBody` at `i < frozen` stays while a LATER total conjunct enters — that is a permutation of two TOTAL conjuncts and is exactly what the precondition permits |
| FILTER-over-PROJECT descent (`:727-762`) | `projection_total && i < frozen && remapThroughProject(...)` | sound, **and the two screens use two DIFFERENT schemas on purpose**: `frozen` over `project->output_schema` (where the conjuncts are written) and `projection_total` over `project->children[0]->output_schema` (where the select list is evaluated). Getting that pair backwards would be silent; it is right |

`projection_total` is all-or-nothing over the whole select list, which is
necessary — the filter lands in ONE place, so one raising sibling stops every
conjunct. Verified by execution: the `x.lap_id * 1000000000000000 AS big` body
that P4-B1 was written about now agrees between the legs, and the `speed * 2 AS s2`
body (DOUBLE, cannot raise) still descends.

**The partition is not where the seam is broken. Both remaining defects are in
the CLASSIFIER, and both are about which SCHEMA it is handed** — P5-2 (a schema
the expression was not written against) and P5-4 (a NULL literal's declared type
standing in for its behaviour).

---

## MEDIUM P5-3 — P4-2 is **half** closed. The DOUBLE spelling is fixed; the INT
## spelling still measures **90×**, and it is still silent in `--explain`.

The fix round reports "86× → 1.0×, byte-identical optimized plan". **I reproduced
that and it is true** — the two spellings of the DOUBLE query produce optimized
plans that `diff` reports as differing only in the *pre-optimization* section:

```
$ diff <(explain A) <(explain B)
3c3
<   LogicalFilter [((((l.speed * 2) > 688) AND (l.lap_id < 5)) AND (dr.age > 30))]
---
>   LogicalFilter [(((l.lap_id < 5) AND (dr.age > 30)) AND ((l.speed * 2) > 688))]
```

— the `=== Optimized Logical Plan ===` blocks are byte-identical, both pushing
all three conjuncts. That is a real fix and the right one.

**But `checkedMul` on INT/INT can genuinely overflow, so the INT spelling still
freezes**, and P4-2's whole mechanism is intact for it. Same query shape, `lap_id`
(INT) instead of `speed` (DOUBLE):

```sql
-- A: arithmetic first
… WHERE l.lap_id * 2 > 8 AND l.lap_id < 5 AND dr.age > 30
-- B: arithmetic last
… WHERE l.lap_id < 5 AND dr.age > 30 AND l.lap_id * 2 > 8
```

```
A:  LogicalFilter [((((l.lap_id * 2) > 8) AND (l.lap_id < 5)) AND (dr.age > 30))]
      LogicalJoin              ← nothing pushed, laps chunks_skipped=0/2
B:  LogicalFilter [((l.lap_id * 2) > 8)]
      LogicalJoin
        LogicalFilter [(l.lap_id < 5)]     ← laps chunks_skipped=1/2
        LogicalFilter [(dr.age > 30)]
```

**Method** (stated because `benchmark.py::run_once` greps only the `Execution:`
line and has reported regressed queries as faster): five runs each, debug
`build/swiftql`, `--no-cache --storage columnar --execution vectorized
--explain-analyze`, recording BOTH the `Execution:` line and the wall clock of
the whole process, so a shift between phases cannot masquerade as a speed-up.
Both spellings return 0 rows.

| | `Execution:` median | `Execution:` range | wall median |
|---|---|---|---|
| A — frozen | **38 847 µs** | 38 177 – 45 932 | 244 ms |
| B — pushed | **432 µs** | 412 – 506 | 203 ms |
| A, `--no-optimize` | 48 957 µs | 47 147 – 50 322 | 254 ms |

**90×**, and the wall-clock delta (41 ms) tracks the `Execution:` delta (38 ms),
so the number is not an artefact of the grep. Optimized-A is within **1.26×** of
its own `--no-optimize` leg — the optimizer has switched itself off on this
query.

The freeze here is **CORRECT** — `l.lap_id * 2` can overflow, and both other
conjuncts change the join's output set, so pushing either of them would change
the row set the raiser sees. This is the definition's price, not a defect. What
IS a defect is that the price is invisible: `--explain` prints A's optimized plan
as the written plan with **no decline line anywhere**, and the only clue is
`chunks_skipped=0/2` in `--explain-analyze`, which a reader has to know to look
for. Ranked MEDIUM rather than HIGH because, unlike pass 4's version, this is no
longer a regression against pre-fix code on the common (DOUBLE) shape — it is the
irreducible remainder, unreported.

**Reading "86× → 1.0×" as "P4-2 is closed" would be wrong**, and that is the
substance of this finding: the ratio was measured on the one arithmetic type the
fix addressed.

---

## MEDIUM P5-4 — the screen classifies a comparison against a **NULL literal** as
## may-raise. It provably cannot raise, the file's own comment says so, and it
## costs a measured **2.55×**.

`staticTypeOf` (`expr_totality.h:62-67`):

```cpp
// A NULL literal never raises in a comparison (Value's operators return
// false when either side is null, before the type check), but its
// declared type is what a consumer would compare against, so report it.
out = lit->value.isNull() ? lit->null_type : lit->value.type();
```

Reporting `null_type` is right for `ChunkPruner` (which compares the literal
against zone maps) and wrong for `exprMayRaise`'s comparison arm, which then
sees `STRING` vs `INT` and answers TRUE for an expression the comment has just
finished proving total. A zero-row scalar subquery is where this arrives:
`subquery_materialization.cc:491` sets `null_type` from the body's output schema,
so `l.team = (SELECT MAX(age) …)` over an empty result becomes
`STRING = NULL:INT`.

Constructed, run, `--explain` and `--explain-analyze` (median of 3, both 0 rows):

| | optimized plan | `Execution:` |
|---|---|---|
| `WHERE l.team = (SELECT MAX(dr2.age) FROM drivers dr2 WHERE dr2.age > 999) AND l.speed > 300 AND dr.age > 30` | `LogicalFilter [(((l.team = NULL) AND (l.speed > 300)) AND (dr.age > 30))]` over a bare `LogicalJoin` — **nothing pushed** | **93 594 µs** |
| the same three conjuncts, NULL comparison written last | both other conjuncts pushed to their own scans | **36 676 µs** |

**2.55×**, on a conjunct that cannot raise. And the query **answers** — 0 rows,
both legs, every mode — which is the whole point: the screen froze a plan for an
error that cannot happen.

The general statement is larger than the one case and is not written down
anywhere: **`evaluate()` propagates NULL before every raise site except `AND`/`OR`**
(`evaluator.cc:134`, and each of `IN`, `LIKE`, `SUBSTRING`, `CASE` short-circuits
its own NULL first). So a NULL *literal* in any operand position makes the whole
node total, statically decidable, at every one of those sites — not just
comparison. `lap_id + NULL` is screened as INT/INT-may-overflow today for the
same reason. The fix is one clause in each arm, or one `isNullLiteral(e)` test at
the top of `exprMayRaise`; I did not write it.

---

## Adjudication — the fixer disputed P4-B2, and **the fixer is right**. Do not
## make `inferExprType` refuse a STRING-vs-numeric comparison at plan time.

Pass 4's P4-B2 preferred fix was to have `inferExprType`'s `BinaryExpr` branch
compare `l` against `r` for `= != < > <= >=` and throw. The fixer refused it on
three grounds. I checked all three by execution and add a fourth that I think is
decisive.

**(1) "It converts queries that answer today into plan-time errors." TRUE, and I
have the example.** `expr_totality.h`'s own cascade rule means a conjunct is
evaluated only on the rows every earlier conjunct kept — including on *zero* rows:

```
SELECT team FROM laps WHERE team > 'zzzzz' AND team = 5     →  0 rows
```

verified on both legs and in every supported mode at this HEAD. Under P4-B2 this
becomes a hard plan-time error. That is not a marginal query; it is the exact
shape round 4 shipped the cascade rule to make well-defined.

**(2) "It newly throws on a NULL `Literal` from a zero-row scalar subquery."
TRUE, verified.**

```
SELECT team FROM laps WHERE team = (SELECT MAX(age) FROM drivers WHERE age > 999)
  →  0 rows
```

`logical_plan.cc:114` returns `lit->null_type` for a NULL literal, and
`subquery_materialization.cc:491` sets that from the body's schema — `INT` here,
against a `STRING` column. `inferExprType` would refuse a query that answers.
Every empty-scalar-subquery comparison whose types differ would become an error,
and the user cannot see the types differ because the value is NULL.

**(3) "It would be a third rule beside the SUBSTRING and overflow rules."** True
but the weakest of the three; symmetry is an argument about maintenance, not
about answers.

**(4) The argument neither party made, and the one I would decide on: P4-B2
CONTRADICTS THE DEFINITION ROUND 4 SHIPPED.** `expr_totality.h` says, as a
definition and not a description:

> PER-ROW EVALUATION IS NOT TOTAL. `evaluate()` can THROW on a row. So the SET OF
> ROWS an expression is evaluated on decides whether a query errors …

A plan-time refusal says the opposite — that whether `team = 5` errors is decided
by the SCHEMA, before any row exists. The two rules cannot both be the rule. Round
4 chose the row-set rule and built four movers, a chunk pruner and a LIMIT
placement on it; P4-B2 would make `team = 5` the one predicate whose error
behaviour is not a function of its row set. **Pass 4's P4-B2 is hereby withdrawn.**
The screen approach round 4 took instead is the correct design, and P5-2 is a bug
in it, not a reason to revisit it.

---

## The fixer's two open items, checked

### (a) "The new declines are still silent in `--explain`." CONFIRMED, and the count is now SIX.

`--explain` on the frozen P5-3 query prints the optimized plan **byte-identical
to the written plan** and nothing else. Only two decision fields exist in the
whole logical layer (`grep -n "decision" src/planner/logical_plan.h`):
`LogicalDerived::pushdown_decision` and `LogicalJoin::order_decision`.
`LogicalScan`, `LogicalProject` and `LogicalFilter` have none.

Complete silent-decline census at this HEAD (phase 5 has now found six; pass 4
said four):

| decline | reported? |
|---|---|
| `reorder`, `n < 3` / `n > 32` | silent — honest, no decision existed |
| `containsOuterJoin`, `slotDeclineReason` | `join-ordering=skipped (…)` |
| `pushIntoDerived` entry refusal | `pushdown=skipped (…)`; still last-wins, and *"column does not resolve against the body"* still appears unconstructible (P4-3, unchanged) |
| `remapThroughProject` descent refusal | **silent** |
| `projection_total` refusal (P4-B1's screen) | **silent** — and `LogicalProject` has no field to put it in, which that call site now says outright |
| the `firstMayRaise` freeze, at all four movers | **silent** — P5-3, 90× |
| **`collectSimplePredicates` stopping at a raiser** | **silent, NEW in round 4** |
| **`canSkipChunk`'s STRING-boundary refusal** | **silent, NEW in round 4** |

The last two are the ones that turn into P5-2 when they misfire, and neither has
any print at all — `--explain` shows `pruning=on` whether the hint yields
everything or nothing. That string is *honest* as documented
(`vec_scan_node.cc:104-105` says it means "a hint is attached", and
`--explain-analyze` prints the real `chunks_skipped=n/m`), so I am not counting
it as a defect — but it means the only way to observe either decline is to run
the plan.

### (b) "`--no-optimize` loses zone-map pruning on join queries, +21%." CONFIRMED — and the claim needs one refinement, and its proposed fix also closes P5-2.

**Refinement: the loss is not unconditional. It is decided by written order.**
`collectSimplePredicates` stops at the first conjunct it cannot type, so a
scan-local conjunct written *before* the cross-relation one still prunes.
Isolated measurement — same conjunct SET, same 398 rows, `--no-optimize` on both,
five runs each:

| `--no-optimize`, written order | `chunks_skipped` | `Execution:` median |
|---|---|---|
| `WHERE l.season = 2024 AND dr.nationality = 'British'` | **1/2** | 41 852 µs |
| `WHERE dr.nationality = 'British' AND l.season = 2024` | **0/2** | 49 263 µs |

**+17.7%**, same direction and nearly the same magnitude as the reported +21%,
and now attributable to a single decision rather than to the leg. (The 18× gap
between the legs on this query is the un-pushed join, not the pruning; quoting
the leg-to-leg ratio here would have been the `Execution:`-line trap.)

**The fix the fixer names is the fix for P5-2.** `chunk_pruner.h:50-57` proposes
threading the filter's child schema alongside the hint through
`pruningHintForPreservedSide`. With the WHERE's own schema in hand, `b.v` in
P5-2's repro resolves at slot 1 to the STRING column, `conjunctMayRaise` answers
TRUE, the walker stops, no chunk is skipped, and the `--no-optimize` leg raises
exactly as the optimized leg does. The same change restores the pruning above,
because `dr.nationality = 'British'` would then type correctly as total and the
walk would continue. **One change; a 17.7% recovery and a divergence closed.**
That the performance concession and the correctness hole are the same missing
argument is the finding worth carrying forward: a screen handed the wrong schema
does not fail safe, it fails in whichever direction the name table happens to
point.

---

## Part B — the passes, re-enumerated from source at `b14d086`

### B.1 The census is now **3 gated, 9 ungated**. `INVARIANT_SCOPE` says 5+1 and is short by THREE.

`--no-optimize` still gates exactly three, re-read at both sites
(`main.cc:574-588` top level, `main.cc:137-139` in `runVectorizedToRows`).
Everything else that rewrites a plan runs on both legs:

| # | pass | site | gated? | note |
|---|---|---|---|---|
| 1 | `PredicatePushdown` | `main.cc:576` / `:138` | **gated** | |
| 2 | `JoinEnumeration` | `main.cc:590` / `:139` | **gated** | |
| 3 | `CardinalityEstimator` | `main.cc:594` / `:140` | **gated** | |
| 4 | `foldConstants` | `binder.cc:259` | no | identical by construction; runs before `build` |
| 5 | `substituteGroupKeyRefs` | `logical_plan.cc:1063` | no | **absent from the harness census** |
| 6 | derived normalization (`buildRelation`) | `logical_plan.cc:1067` | no | |
| 7 | `lowerInSubqueries` | `logical_plan.cc:1159` | no | **violates the totality definition — P5-1** |
| 8 | `lowerExistsSubqueries` | `logical_plan.cc:1165` | no | **violates the totality definition — P5-1** |
| 9 | `lowerCorrelatedScalars` | `logical_plan.cc:1180` | no | clean — LEFT join, row-preserving (checked) |
| 10 | `deterministicCut` | `logical_plan.cc:1280` | no | **absent from the harness census**; inserts a SORT |
| 11 | **`applyLimit`** | `logical_plan.cc:1281` | no | **NEW in round 4, absent from the harness census.** Inserts a `LogicalLimit` in a different POSITION, conditionally on `exprMayRaise` |
| 12 | `materializeSubqueries` | `main.cc:550` | no, but its **output is gate-dependent** | threads the flag into the nested runner |

`python_tools/test_new_queries.py:635-642` still reads *"5 further passes are
identical in both legs BY CONSTRUCTION … The 6th, materializeSubqueries"*. That is
9, not 6 — pass 4's P4-4 named two of the three missing (`substituteGroupKeyRefs`,
`deterministicCut`) and was ranked LOW and not acted on, and round 4 then added a
third (`applyLimit`). **LOW P5-6**, unchanged in kind from P4-4 but now larger,
and `applyLimit` is the entry that matters most: it is the only ungated pass whose
firing is decided by `exprMayRaise`, so every defect in the screen is also a
plan-shape defect on every `LIMIT` query, in both legs at once, where no
differential can see it.

### B.2 Preconditions: checked, believed, or reachable by a later shape

| rule | precondition | CHECKED / BELIEVED | reachable by a later shape? |
|---|---|---|---|
| `distribute` inner/standard only | pushed side not null-supplying, not a semi/anti body | **checked**, positively | no |
| pushdown before enumeration | `distribute` reads WRITTEN spine order | **believed** — guaranteed only by call order at `main.cc:576/590`; nothing asserts it | yes |
| `foldConstants` before `build` | `inferExprType` throws on a surviving `IntervalLiteral` | **believed** — guaranteed by folding being last in `Binder::bind`; `binder.cc:190` says so in prose | yes |
| **`deterministicCut` before `applyLimit`** | reversed, the LIMIT would sink below the projection and the sort would then be inserted ABOVE it, cutting different rows | **believed** — two adjacent lines (`:1280`, `:1281`). `applyLimit`'s comment describes the interaction but neither asserts the order | **yes — NEW this round** |
| `applyLimit`'s PROJECT branch | `LIMIT n(π(R)) ≡ π(LIMIT n(R))` needs π 1:1 AND the child's order plan-stable | **checked, by an argument I had to reconstruct**: the branch can only fire when `deterministicCut` left the node a PROJECT, i.e. `orderIsPlanStable` was true, and `orderIsPlanStable` returns **false for JOIN** — so a reorderable child is structurally impossible here. Neither function says it is load-bearing for the other | no |
| never below AGGREGATE / SORT / DISTINCT / LIMIT | structural | **checked** — `apply` rewrites only FILTER over JOIN / SCAN / DERIVED / PROJECT, and a body with a LIMIT has LIMIT as its root | no |
| `remapThroughProject` | every named column a plain passthrough | **checked** (`dynamic_cast`, all-or-nothing) | no |
| `projection_total` (P4-B1) | no select-list SIBLING may raise | **checked**, against the projection's INPUT schema — the right one | no |
| **the totality screen — partition** | nothing crosses the frozen index | **checked at all five sites** (four movers + the pruner walk) | no |
| **the totality screen — classifier** | every conjunct that can raise is classified so | **CHECKED BY SCHEMA, and the schema can be the WRONG one — P5-2** | **yes, today** |
| **the totality rule vs. subquery lowering** | *"nothing may change that set … not a plan rewrite"* | **BELIEVED, and FALSE — P5-1**. Two lowerings interpose a row-reducing semi-join below the filter | yes |
| `CardinalityEstimator::selectivity` | may decide plan quality only, never order for a raiser | **now stated** (`cardinality_estimator.h:101-114`) and enforced by the caller. Pass 3's B.1 gap is closed | — |
| `deterministicCut` | inserted sort's input schema is plan-independent | **checked** (pass 4's A.6, re-read; `(relation_slot, name)` tie-break) | no |
| `materializeSubqueries` | body's result is plan-independent | **checked** — B3-1b's repro agrees at this HEAD | — |

### B.3 Idempotency and ordering

* **`PredicatePushdown` is still effectively idempotent**, including round 4's new
  `projection_total` screen: a second `apply` re-tests the same select list and
  refuses the same conjuncts. The only non-idempotent write remains
  `LogicalDerived::pushdown_decision`, a string overwritten with the same value
  and never read by the pass.
* **`JoinEnumeration` is still not idempotent** (pass 2's B-7); `applyToSpineLeaves`
  still cannot hand a rebuilt spine back to `decompose`.
* **`applyLimit` and `deterministicCut` run exactly once**, from `build`, so
  idempotency does not arise — but their ORDER now matters and is unasserted
  (B.2).
* **Ordering dependencies are now three, all guaranteed by adjacency alone**:
  pushdown→enumeration, folding→build, cut→limit. Each is documented in prose at
  one end and at neither in code.

### B.4 What I checked this pass and found CLEAN

Recorded as results so the record is complete.

* **Round 4's fixes are real.** All four of P4-1's divergences agree on both legs;
  `chunk_pruner.h`'s own documented repro (`lap_id * 92233… > 0 AND lap_id > 999999`)
  agrees across all six mode/leg cells where it used to split row-vs-columnar;
  both of P4-B1's derived-body shapes agree; the DOUBLE-arithmetic plans are
  byte-identical between spellings.
* **The raise-site enumeration is now closable in one command**: no `throw` exists
  in `columnar_eval.cc` or `expression_executor.cc`, so `evaluate()` + `value.cc`
  + `checked_arith.h` + `canSkipChunk` is the entire set.
* **A conjunct that raises only on rows a later predicate would eliminate DOES
  raise, in every mover.** Run for `SUBSTRING(name, age-34, 2)` guarded by
  `age > 99999` (drivers), for INT overflow guarded by `lap_id > 999999` (laps,
  where a zone map really can prune), through a join, through a LEFT join,
  through a derived body and through the PROJECT descent. The one place it does
  NOT raise is P5-1 (subquery lowering) and P5-2 (the mistyped pruner hint).
* **31 further cross-shape queries agree between the legs** in all three
  supported modes — CASE with a raising arm both orders, IN over constants,
  LIKE, bare-column predicates (`WHERE speed`, `WHERE lap_id`), unary minus over
  INT and over DOUBLE, `IS NULL` over a raising operand, OR-inside-AND both
  orders, GROUP BY / HAVING, a 3-relation self join both orders, derived bodies
  with computed and passthrough select lists, and `LIMIT` with a raising
  projection with and without `ORDER BY` and `DISTINCT`.
* **`orderIsPlanStable` returns false for JOIN**, which is what makes
  `applyLimit`'s new PROJECT branch unable to see a reorderable child (B.2).
* **TPC-H sf0.01 has NO shared column name at all** across its eight tables
  (checked programmatically), and `catalog.json`'s two shared names are
  type-matched — so P5-2 is unreachable by every query in every harness. The
  green gate is evidence about the corpus, not about the code.
* **`pruning=on` is honest**: `vec_scan_node.cc:104-105` documents it as "a hint
  is attached", and `--explain-analyze` prints the real `chunks_skipped=n/m`.
* **`lowerCorrelatedScalars` uses a LEFT join** and is therefore row-preserving,
  which is why it is not part of P5-1. Verified by execution and by `--explain`.

### B.5 Not reached

* **I ran no harness and no gate.** Every number here is a single-query CLI
  invocation against `build/swiftql`, which is newer than every file under `src/`.
  No source file was touched, no build was started, and the build lock was never
  taken.
* P5-3's 90× and P5-4's 2.55× are **debug-build** figures. Pass 4 recorded that
  ratios on these shapes are build-sensitive by roughly 3×; take the Release
  figure independently before quoting either.
* I did not construct a **wrong-row** divergence. P5-2 is error-vs-rows, P5-1 is
  error-vs-rows. I looked for a row divergence through the pruner (the
  mistyping changes only whether the WALK continues, never what is COLLECTED —
  the `localSlot() < 1` guard still rejects every foreign ref) and through the
  freeze (the partition is sound at all five sites), and found none. Absence of a
  repro is not a proof.
* I did not chase the correlated-ref instance of P5-2's root cause. A correlated
  `ColumnRef` also misses the slot-qualified lookup and takes the bare-name
  fallback in `staticTypeOf`, and `forEachLocalColumnRef` SKIPS it in both
  remappers, so a correlated ref inside a conjunct pushed into a derived body
  would be neither typed nor remapped. I could not construct a query that reaches
  it and did not want to report a shape I had not run.
* I did not re-audit `subquery_materialization.cc` or `subquery_decorrelation.cc`
  internals; the subquery-chain seam owns those. P5-1 is about WHERE the lowering
  puts its node, not about the lowering.

