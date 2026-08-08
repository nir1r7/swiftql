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

