# Seam audit — optimizer result preservation (pass 4)

HEAD `b2bc70e`, branch `claude/phase5-week26-qomtkb`. Binary: `build/swiftql`,
newer than every file under `src/` (verified), so it is HEAD's build. No source
file touched; every measurement below is a single-query CLI invocation with
`--no-cache`.

Predecessors: `seam-optimizer-preservation-pass-1.md`, `-2.md`, `-3.md`.

Status: IN PROGRESS (written incrementally; summary block at end).

---

## BLOCKER-CLASS FINDING P4-1 — `mayRaise` is UNDER-approximate. The totality
## screen misses the whole class of TYPE-MISMATCHED COMPARISONS, and B3-2
## reopens in both directions, through two independent raise sites.

The fix round's precondition rests on one load-bearing sentence
(`predicate_pushdown.cc:378-384`):

> What it does NOT have to cover, because `inferExprType` (logical_plan.cc)
> decides it at PLAN time — in both legs, before any pass in this file runs — is
> every TYPE error: STRING arithmetic, a non-STRING LIKE or SUBSTRING operand, a
> mixed IN list, a CASE with mixed branches. Those raise identically in both legs
> no matter how the conjuncts are arranged.

**That list is missing the most common type error of all, and it is missing it
because `inferExprType` does not check it.** `logical_plan.cc:159-173`, the
`BinaryExpr` branch:

```cpp
TypeId l = inferExprType(bin->left.get(), schema);
TypeId r = inferExprType(bin->right.get(), schema);
const std::string& op = bin->op;
if (op == "+" || op == "-" || op == "*" || op == "/") {
    if (l == TypeId::STRING || r == TypeId::STRING)
        throw std::runtime_error("'" + op + "' requires numeric operands");
    ...
}
return TypeId::INT;   // comparison / AND / OR
```

For `=`, `!=`, `<`, `>`, `<=`, `>=` it recurses into both children and returns
INT **without ever comparing `l` against `r`**. So `team = 5` (STRING column,
INT literal) type-checks at plan time and raises PER ROW, from
`Value::checkMatchingType` / the `NUMERIC_COERCE` macro (`common/value.cc:33,57`
— *"Type mismatch in Value comparison"*). Confirmed standalone, both legs:

```
$ swiftql ... --query "SELECT team FROM laps WHERE team = 5 LIMIT 3"
Error: Type mismatch in Value comparison        (identical with --no-optimize)
```

`mayRaise` (`predicate_pushdown.cc:413-417`) answers **false** for that conjunct:
a `BinaryExpr` whose op is not arithmetic recurses, `ColumnRef` → false,
`Literal` → false. So `team = 5` is classified TOTAL, is freely permuted by
`orderByWork`, and is freely pushed below an inner join by `distribute`. A
missed subtype "answers total and is therefore the unsafe direction" — the
screen's own words, four lines above `mayRaise`.

### The four measured divergences

Shipped `catalog.json`, `--execution vectorized --storage columnar --no-cache`.

| # | query | optimized | `--no-optimize` |
|---|---|---|---|
| 1 | `SELECT team FROM laps WHERE team = 5 AND speed > 999999` | **0 rows** | **Error: Type mismatch in Value comparison** |
| 2 | `SELECT team FROM laps WHERE team > 'zzzzz' AND team = 5` | **Error: Type mismatch in Value comparison** | **0 rows** |
| 3 | `SELECT team FROM laps WHERE 5 = team AND speed = 333.3333` | **0 rows** | **Error: Type mismatch in Value comparison** |
| 4 | `SELECT team FROM laps WHERE team LIKE 'zzz%' AND 5 = team` | **Error: Type mismatch in Value comparison** | **0 rows** |

1 and 3 are the MASK direction; 2 and 4 are the INTRODUCE direction — the
optimizer turning a query that succeeds into one that fails. `--explain` shows
the mechanism directly in each case, e.g. for #2:

```
=== Logical Plan ===
  LogicalFilter [((team > zzzzz) AND (team = 5))]
=== Optimized Logical Plan ===
  LogicalFilter [((team = 5) AND (team > zzzzz))]
```

### TWO raise sites, not one, and the second is not in `evaluate()` at all

Pairs 1/2 and 3/4 fail through **different code**, which matters because a fix
aimed only at `mayRaise`'s `evaluate()` model closes half of it.

* **3 and 4 raise from `evaluate()`.** `5 = team` puts the Literal on the LEFT,
  so `ChunkPruner::collectSimplePredicates` (which requires ColumnRef-op-Literal,
  `chunk_pruner.h:61-71`) never collects it; the throw comes from the per-row
  `evalFallback` loop in `columnar_eval.cc:117-125`. `ExpressionExecutor` declines
  the shape on purpose (`expression_executor.cc:579-583`, *"STRING vs numeric
  throws in Value's comparison operators; decline so the fallback raises the same
  error from the same place"*), so the compiled kernel is not involved.

* **1 and 2 raise from `ChunkPruner::canSkipChunk`** (`chunk_pruner.h:76-86`):
  `if (op == "=") return val < mn || val > mx;` — `val` is `Value(5)`, `mn` is a
  STRING, and `Value::operator<` throws. This raise is **not reachable from
  `mayRaise`'s model at all**: it happens at SCAN time, on zone-map metadata,
  before a single row is evaluated, and the conjunct never had to be reached by
  the AND cascade. `ChunkPruner::shouldSkip` walks `preds` in **conjunct order**
  and returns on the FIRST predicate that proves a skip, so a prunable conjunct
  ordered ahead of `team = 5` short-circuits the throw and one ordered behind it
  does not. That is the entire mechanism of #1 and #2.

  Control, isolating it: with the SAME second conjunct, the outcome is decided by
  whether the FIRST conjunct proves a skip.

  | first conjunct | `... AND team = 5`, `--no-optimize` |
  |---|---|
  | `speed = 333.3333` (in range, proves nothing) | **Error** |
  | `speed > 999999` (proves skip) | 0 rows |
  | `lap_id < 0` (proves skip) | 0 rows |
  | `team = 'zzzz'` (proves skip) | 0 rows |
  | `lap_id = 999999` (proves skip) | 0 rows |
  | `sector_1 = 12345.5` (proves skip) | 0 rows |

  Note what this also means: the pruner raise is order-sensitive **even without
  the optimizer**. The optimizer is what makes the two legs disagree.

### Mode census (query #2, the INTRODUCE direction)

| mode | optimized | `--no-optimize` |
|---|---|---|
| `columnar` + `vectorized` | **Error** | 0 rows |
| `columnar` + `volcano` | 0 rows | 0 rows |
| `row` + `volcano` | Error | Error |
| `row` + `vectorized` | refused (needs columnar), both legs | — |

col-vec is again the only diverging mode, and for two reasons. `--no-optimize`
gates only the VECTORIZED optimizer (`main.cc:566`; the Volcano path plans
through `Planner::plan` and is unaffected by the flag), so neither Volcano row
is a differential at all. And Volcano's `evaluate()` is EAGER for AND
(`evaluator.cc:98-107` computes BOTH operands before the three-valued rule), so
`row`+`volcano` raises unconditionally — the same asymmetry pass 3 recorded for
`SUBSTRING`. `columnar`+`volcano` answers 0 rows because that path prunes the
chunks away before the predicate is reached.

### Ranking

Ranked **HIGH**, by the precedent pass 3 set and for the same reason it gave:
`optimized != --no-optimize` on CLI-typable queries on the shipped catalog, in
both directions, but the failing side is LOUD. It is not a wrong row. It is
recorded first because it is a **reopening of a finding the fix round declared
closed**, against a precondition that was written down and is false as written:
`inferExprType` does not decide "every TYPE error" at plan time.

### Where else the same conjunct now travels

The screen is applied at four sites, and a `mayRaise` miss defeats all four.
Measured, same catalog:

| site | query | optimized | `--no-optimize` |
|---|---|---|---|
| `orderByWork` (FILTER over SCAN) | table above | — | — |
| `distribute` (below an inner join) | `SELECT l.team FROM laps l JOIN drivers dr ON dr.driver_id = l.driver_id WHERE dr.nationality = 'Zzz' AND l.team = 5` | Error | Error (no divergence on this pair; the pruner sees it on both legs) |
| `distribute` under a LEFT join | `SELECT l.team FROM laps l LEFT JOIN drivers dr ON dr.driver_id = l.driver_id WHERE l.team = 5 AND l.speed > 999999` | **0 rows** | **Error** |
| `pushIntoDerived` (the NEW round-3 rule) | `SELECT d.team FROM (SELECT team, speed FROM laps) d WHERE d.team = 5 AND d.speed > 999999` | **0 rows** | **Error** |

So the fix round did not merely leave a gap open, it WIDENED the surface: the
derived-body entry rule is a new place a mis-classified conjunct can move to.

### Why the harnesses do not see it

`run_optimizer_invariant` catches an exception from either leg and records an
ERROR, so any of these four shapes WOULD be caught — if one were in the suite.
None is: no query in `python_tools/` compares a STRING column against a numeric
literal (or the reverse). TPC-H has no such comparison either, which is why the
gate is green. The exposure is a user typing `WHERE p_size = '5'`.

---

## HIGH P4-2 — the totality screen's over-approximation is NOT free. One
## arithmetic conjunct freezes every conjunct written after it, out of the join
## AND out of its scan, and `--explain` reports nothing. Measured **87×**.

`mayRaise` screens arithmetic **by operator, not by inferred type**
(`predicate_pushdown.cc:413-417`): any `+ - * /` answers true, and any
`UnaryExpr` answers true unconditionally. The screen's comment states the cost
and dismisses it:

> DOUBLE arithmetic cannot overflow, but deciding that needs a schema this
> function does not have, and post-folding an arithmetic conjunct in a WHERE is
> rare enough that the extra conservatism is free (no TPC-H query has one …).

"No TPC-H query has one" is **true** — I rendered all 22 through
`python_tools/tpch_queries.render` and checked every WHERE/AND clause; Q11's
`SUM(ps_supplycost * ps_availqty)` is in a HAVING (never pushed) and Q22's
`SUBSTRING(c_phone, 1, 2)` is constant-argument, so the screen's own carve-out
keeps it total. "Free" does not follow, and the reason is that `firstMayRaise`
freezes by **POSITION**: everything from the first raising conjunct onward is
frozen, so the cost is decided by where in the WHERE clause the user happened to
write the arithmetic.

Two queries, identical conjunct SETS, differing only in written order:

```sql
-- A: arithmetic first
SELECT l.team FROM laps l JOIN drivers dr ON dr.driver_id = l.driver_id
WHERE l.speed * 2 > 688 AND l.lap_id < 5 AND dr.age > 30
-- B: arithmetic last
SELECT l.team FROM laps l JOIN drivers dr ON dr.driver_id = l.driver_id
WHERE l.lap_id < 5 AND dr.age > 30 AND l.speed * 2 > 688
```

`--explain`, optimized:

```
A:  LogicalFilter [((((l.speed * 2) > 688) AND (l.lap_id < 5)) AND (dr.age > 30))]
      LogicalJoin [driver_id = driver_id]
        LogicalScan [laps, 4 columns]
        LogicalScan [drivers, 3 columns]

B:  LogicalFilter [((l.speed * 2) > 688)]
      LogicalJoin [driver_id = driver_id]
        LogicalFilter [(l.lap_id < 5)]
          LogicalScan [laps, 4 columns]
        LogicalFilter [(dr.age > 30)]
          LogicalScan [drivers, 3 columns]
```

`--explain-analyze`, `Execution:` line, three runs each, `build/swiftql`
(the debug build; see the note on build sensitivity in P4-3):

| | run 1 | run 2 | run 3 |
|---|---|---|---|
| A — frozen | 39944.7 µs | 38737.1 µs | 40773.0 µs |
| B — pushed | 455.2 µs | 465.3 µs | 430.9 µs |
| A, `--no-optimize` | 46908.9 µs | 47799.6 µs | — |

**87×**, on the same three predicates. Both return 0 rows, so no correctness
harness can report it, and `--explain` prints **no decline line at all** — A's
optimized plan is the written plan with nothing said about why.

This is a **regression introduced by the fix round**: before the screen existed,
`soleSlot` routed `l.lap_id < 5` to slot 0 and `dr.age > 30` to slot 1 and both
were pushed. The optimized leg is now within 15% of the `--no-optimize` leg on
this shape — i.e. the optimizer has effectively switched itself off, silently.

Two things make it worth a HIGH rather than a MEDIUM:

1. **The magnitude is larger than anything this seam has recorded.** Phase 5's
   previous largest measured plan-quality loss was B3-3 at 9.4× (pass 3) / 2.9×
   (the fixer, Release). This is 87× on a three-conjunct WHERE over a 10k-row
   table.
2. **The trigger is a shape users write constantly** — `WHERE price * qty > X
   AND ship_date < Y` — and DOUBLE arithmetic, the case that provably cannot
   raise, is the most common one. `evaluator.cc:141-155` takes the double branch
   without calling `checkedMul` at all when either operand is DOUBLE, so
   `l.speed * 2` (speed is DOUBLE in `catalog.json`) cannot overflow and the
   freeze buys nothing on it.

**The freeze arithmetic itself is SOUND — I checked it.** At all four sites the
partition is by index against `frozen`, so no conjunct ever crosses the boundary:
`pushIntoJoin` assigns `slot = (i >= frozen) ? -1 : soleSlot(c)` and re-sorts the
residual by written index; `pushIntoDerived` and the FILTER-over-PROJECT descent
both gate on `i < frozen`, so every moved conjunct has a strictly smaller written
index than every frozen one and cannot move ahead of one. The defect is the
CLASSIFIER's precision, not the partition.

---

## Part A — the fix round's precondition, attacked

### A.1 The soundness argument itself is CORRECT. The classifier is not.

The precondition as written — freely permute a prefix of total conjuncts, push
any of them below an inner join, freeze from the first raiser on — is right, and
I could not break it on its own terms:

* AND over TOTAL conjuncts is commutative under 3VL because a filter keeps only
  TRUE (`columnar_eval.cc:107-111`, `evaluate`'s AND at `evaluator.cc:105-118`
  returns FALSE when either side is FALSE regardless of the other being NULL), so
  the survivor set entering position *k* is a function of the SET
  `{C0..C_{k-1}}`. Checked, not assumed.
* σ_p(R ⋈ S) ≡ σ_p(R) ⋈ S for an INNER join makes the pushed case identical, and
  `distribute` tests `join_type == INNER && semantics == STANDARD` positively
  (`predicate_pushdown.cc:522-524`).
* The partition never lets a conjunct cross the boundary (see P4-2's last
  paragraph).

Everything that is wrong is in `mayRaise`. P4-1 is the class it misses.

### A.2 `mayRaise`, subtype by subtype, against every `throw` reachable per row

I enumerated every throw site an expression can reach at execution
(`evaluator.cc`, `common/value.cc`, `execution/checked_arith.h`,
`storage/chunk_pruner.h`) and asked, for each, whether `mayRaise` answers true.

| raise | site | `mayRaise` | verdict |
|---|---|---|---|
| `SUBSTRING: start position must be >= 1` / `length must be >= 0` | `evaluator.cc:61,63` | true unless BOTH `start` and `length` are `Literal` | **correct**, and the carve-out is real: `inferExprType` (`logical_plan.cc:234-249`) refuses a constant out-of-domain start/length AT PLAN TIME, in both legs, and skips a NULL literal — which `evaluate` then answers NULL for rather than raising. TPC-H Q22 stays total |
| INT overflow — `checkedAdd/Sub/Mul/Div/Negate` | `checked_arith.h:23` | true (by operator) | **correct, over-approximate** — and that over-approximation is P4-2 |
| `'op' requires numeric operands` (STRING in arithmetic) | `evaluator.cc:138` | true (arithmetic ⇒ true) | correct |
| `unary '-' requires a numeric operand` | `evaluator.cc:165` | true (`UnaryExpr` ⇒ true) | correct |
| `LIKE requires a STRING operand` | `evaluator.cc:205` | recurses into operand only | **safe**: `inferExprType` (`logical_plan.cc:188-192`) refuses a non-STRING LIKE operand at plan time, in both legs. A column's type is fixed, so the runtime type cannot vary per row |
| `SUBSTRING requires a STRING operand` | `evaluator.cc:230` | as above | safe, same reason (`logical_plan.cc:222-223`) |
| `IN: mixed STRING/numeric` | `value.cc` via `evaluator.cc:198` | recurses into `in->operand` | **safe**: `inferExprType` (`logical_plan.cc:174-186`) walks `in->values` and refuses the mixed shape at plan time. `InExpr::values` is `vector<Value>`, so it can hide no expression |
| CASE branch raises | `evaluator.cc:220-227` | screens EVERY arm, including untaken ones | correct, and deliberately stricter than `evaluate` |
| `IntervalLiteral` / `SubqueryExpr` unconditional throw | `evaluator.cc:238,256` | true (falls through to `return true`) | correct; unreachable anyway |
| **`Type mismatch in Value comparison`** — a comparison whose operands are STRING vs numeric | **`value.cc:33,57`, reached from `evaluator.cc:126-131` and from `chunk_pruner.h:79-83`** | **FALSE** | **THE HOLE. P4-1** |
| `Column not found in schema` | `evaluator.cc:92,182` | false | not data-dependent; the remappers are all-or-nothing and resolve before they write, so a moved conjunct always resolves where it lands |
| `std::bad_variant_access` from `asInt()` on a bare DOUBLE predicate (`WHERE speed`) | `value.cc:28` | false | reachable (`WHERE speed AND lap_id > 999999`) but I could not make the two legs disagree: both answered 0 rows on every arrangement I tried, because the DOUBLE-typed predicate is refused by `ExpressionExecutor` and the fallback loop reaches `asInt()` only on surviving rows in BOTH legs. Recorded as unbroken, not as proven safe |

**The sentence that has to change** is the screen's, not the code's alone:
`inferExprType` does not "decide every TYPE error at plan time". It decides every
type error in ARITHMETIC, LIKE, SUBSTRING, IN and CASE. It decides **none** in a
comparison — its `BinaryExpr` branch computes `l` and `r` and then returns
`TypeId::INT` without comparing them.

### A.3 Pushdown into derived bodies — "plain passthrough" is CHECKED

`remapThroughProject` (`predicate_pushdown.cc:291-313`) does
`dynamic_cast<const ColumnRef*>(project.exprs[i].get())` and returns false on
anything else, all-or-nothing, before writing. That is a check, not a belief.
Verified by execution — the computed-projection body refuses and the passthrough
body descends:

```
computed  (SELECT team AS t, speed*2 AS s2 ...)     passthrough (SELECT team AS t, speed AS s2 ...)
  LogicalDerived [d, 2 columns]                       LogicalDerived [d, 2 columns]
    LogicalFilter [(s2 > 688)]                          LogicalProject [t, s2]
      LogicalProject [t, s2]                              LogicalFilter [(speed > 688)]
        LogicalScan [laps, 2 columns]                       LogicalScan [laps, 2 columns]
```

The ENTRY rule's claim — safe for any body shape, because the filter is attached
above the body ROOT whose rows ARE the derived relation's rows — holds, and the
shape that would break it (descending past a `LIMIT`) is **structurally**
unreachable rather than merely untested: `LogicalPlanBuilder::build` adds
`LIMIT` LAST (`logical_plan.cc:1229-1233`), above the projection, so a body with
a `LIMIT` has `LIMIT` as its root and the FILTER-over-PROJECT rule never sees it.
A body with `ORDER BY` and no `LIMIT` has `PROJECT` over `SORT`, and descending
there is order-preserving. Confirmed by execution on seven body shapes
(passthrough, computed, `GROUP BY`, aggregate output, `DISTINCT`,
`ORDER BY … LIMIT`, joining body): all `optimized == --no-optimize`.

I also re-ran pass 3's two BLOCKER repros (B3-1 and B3-1b) and both of B3-2's
SUBSTRING shapes on this HEAD: **all four now agree between the legs.** Those
fixes are real.

### A.4 The 2.9× — reproduced in direction and mechanism, not in ratio

`--explain-analyze` on the B3-3 query, `build/swiftql`, three runs each:

| | median `Execution:` |
|---|---|
| derived form, optimized | 2.79 ms |
| derived form, `--no-optimize` | 19.2 ms |
| flat equivalent | 2.18 ms |

**6.9×** against the un-pushed leg, and the derived form is now within **1.3×**
of the flat query it is semantically identical to (pass 3 measured it 9.4× away).
The commit's 2.9× is a Release-build before/after; mine is HEAD's debug binary,
which is the same build-sensitivity the fixer already flagged in
`05bf47d` ("its absolute numbers are ~7x mine on the FLAT form too"). Direction,
mechanism and the pruning claim all reproduce: the optimized derived form now
prints `chunks_skipped=0/2` on the body's scan and pushes **174** rows through
the body's projection where the `--no-optimize` leg pushes **10000**, and that
projection is 87% of the un-pushed leg's runtime. **The measurement is sound; the
ratio is build-dependent and both parties said so.**

### A.5 LOW P4-3 — the argument for NOT stamping a refusal at the PROJECT
### boundary is false as written, and the DERIVED stamp has one live reason

`predicate_pushdown.cc:769-776`:

> A conjunct refused HERE is not stamped, and the reason it does not need to be
> is that its refusal is READABLE FROM THE PLAN: the filter is drawn directly
> above the project whose select list `--explain` prints on the same line, so
> `LogicalFilter [(s2 > 688)]` over `LogicalProject [team, s2]` says which column
> is computed.

It does not. `LogicalProject`'s explain line prints OUTPUT COLUMN NAMES, never
the expressions — the two plans in A.3 print the identical string
`LogicalProject [t, s2]` whether `s2` is `speed*2 AS s2` (refused) or a plain
passthrough (descended). What a reader can infer is only that *some* refusal
happened, and the same inference is available at the derived boundary for the
same reason: `filterOnto` returns the child unchanged when `staying` is empty
(`predicate_pushdown.cc:483`), so a `LogicalFilter` above a `LogicalDerived` in
the OPTIMIZED plan already means at least one conjunct was refused. The stated
asymmetry — "a filter above a LogicalDerived looks the same whether there was a
decision or not" — is not true of the optimized plan either, once the entry rule
exists.

So the stamp is right to exist and the non-stamp is under-argued; neither is a
wrong answer. Two smaller things in the same 20 lines:

* **One of the two refusal reasons appears unreachable.** `pushIntoDerived`'s
  `"column does not resolve against the body"` fires when
  `remapOntoDerivedBody` returns false, i.e. on a schema-size drift (impossible —
  `derivedRelationSchema` builds the relation's schema from the body root's,
  column for column) or on a `resolveInSchema` miss against the derived
  relation's own schema (the conjunct was bound against exactly that schema). I
  could not construct one. `"predicate can raise"` is the only reason I could
  make print. A decline string that cannot fire is the dead-assertion pattern
  `join_enumeration.cc:519-524` names in its own comment.
* **The reason is LAST-WINS, not first.** `decline` is overwritten on each
  refusing conjunct, and the raising ones are by construction the LAST ones, so a
  resolution refusal on conjunct 0 is reported as `predicate can raise`.

### A.6 `deterministicCut` — runs in both legs, and cannot become a divergence

**It runs in both legs.** `deterministicCut` is called from
`LogicalPlanBuilder::build` at `logical_plan.cc:1231`, and `build` runs at
`main.cc:554` — BEFORE the `if (!args.no_optimize)` block at `:566`. The Volcano
path has the same rule open-coded at `planner.cc:459-481`. So the flag cannot
reach it, and neither can any optimizer pass: it is decided on the written tree.

**It cannot itself diverge**, for three independent reasons I checked rather than
assumed:

1. `orderIsPlanStable` (`logical_plan.cc:961-976`) is a pure function of the
   pre-optimization tree, identical in both legs.
2. The sort it inserts is placed ABOVE the projection (build order:
   sort(ORDER BY) → project → distinct → cut+limit), so its input schema is the
   PROJECTED schema — a function of the SELECT list, not of the plan. For
   `SELECT *` the projection's `ColumnDef`s are copied from the child schema at
   BUILD time, i.e. in written order, with `relation_slot` intact
   (`logical_plan.cc:1199-1215`).
3. It has no declared keys, so `rowLess` falls straight through to
   `compareCanonical` over `tieBreakOrder(schema)`, which sorts by
   `(relation_slot, name)` — column IDENTITY, not position. That is exactly the
   property `JoinEnumeration::rebuild` preserves (`join_enumeration.cc:315-317`
   stamps `c.relation_slot = r` with the binder's written-order slot) and the one
   pass 3's B3-1 showed the old positional tie-break did not have.

The one residual `sort_comparator.h` flags — that `(relation_slot, name)` might
not be unique — I closed by execution rather than by argument. A derived relation
is the only construct that stamps several source relations' columns with one
slot, and it REFUSES a repeated name outright:

```
$ ... "SELECT * FROM (SELECT l.team, dr.team FROM laps l JOIN drivers dr ON …) d"
Error: derived table 'd': column 'team' is produced twice; give one of them an alias
```

With the alias list (`AS d(a,b)`) the names differ. So the pair is unique on
every schema a sort can see, and the `stable_sort` fallback is unreachable there.
Not a finding; recorded so pass 5 does not re-derive it.

---

## Part B — the passes, enumerated from source at this HEAD

### B.1 The census has grown from six ungated passes to EIGHT. The conclusion holds; the count printed by the harness does not.

`--no-optimize` still gates **exactly three** — `main.cc:566-588` (top level) and
`main.cc:133-138` (`runVectorizedToRows`), re-read this pass. Everything else
that rewrites a plan runs in both legs:

| # | pass | site | in both legs? | why |
|---|---|---|---|---|
| 1 | `PredicatePushdown` | `main.cc:571` / `:135` | **gated** | — |
| 2 | `JoinEnumeration` | `main.cc:583` / `:136` | **gated** | — |
| 3 | `CardinalityEstimator` | `main.cc:587` / `:137` | **gated** | — |
| 4 | `foldConstants` | `binder.cc:259` | ungated, identical BY CONSTRUCTION | runs inside `Binder::bind`, before `build`, before the gate |
| 5 | `substituteGroupKeyRefs` | `logical_plan.cc:1014` | ungated, identical BY CONSTRUCTION | **NOT in the harness's census** |
| 6 | derived normalization (`buildRelation`) | `logical_plan.cc:1018`, `:1034` | ungated, identical BY CONSTRUCTION | — |
| 7 | `lowerInSubqueries` | `logical_plan.cc:1110` | ungated, identical BY CONSTRUCTION | — |
| 8 | `lowerExistsSubqueries` | `logical_plan.cc:1116` | ungated, identical BY CONSTRUCTION | — |
| 9 | `lowerCorrelatedScalars` | `logical_plan.cc:1131` | ungated, identical BY CONSTRUCTION | — |
| 10 | `deterministicCut` | `logical_plan.cc:1231` | ungated, identical BY CONSTRUCTION | **NOT in the harness's census**; A.6 |
| 11 | `materializeSubqueries` | `main.cc:500-543` | ungated, **output IS gate-dependent** | executes the body through `runVectorizedToRows(..., args.no_optimize)` |

`INVARIANT_SCOPE` (`python_tools/test_new_queries.py:635-643`) says *"5 further
passes are identical in both legs BY CONSTRUCTION … The 6th,
materializeSubqueries"*. It is short by two — `substituteGroupKeyRefs` and
`deterministicCut`, both of which arrived after the sentence was written. Neither
changes the conclusion (both are identical by construction, for the same reason
as the other five), so this is **LOW P4-4** — but it is the exact mechanism this
codebase's own doctrine warns about: a pass absent from an enumerated list reads
as considered-and-dismissed. `deterministicCut` is the one that matters, because
it INSERTS A SORT NODE and is therefore the ungated pass most capable of changing
an answer.

Pass 3's three obligations for the invariant harness are all **discharged**, and
I re-checked each against source rather than the commit message:
`normalize_ordered` + `_invariant_compare` compare `ORDER BY` queries positionally
(`test_new_queries.py:560-608`); `TIE_STRADDLE_QUERIES` (`:1228+`) supplies
3-relation shapes on the 2-table `catalog.json` via a `drivers`/`drivers` self
join, so `MIN_ENUMERATED_RELATIONS = 3` is now reachable; and
`b31_tie_order_only_no_set_change` pins the ordered comparison against a
set-preserving order divergence.

### B.2 Precondition table, refreshed

| pass / rule | precondition for result preservation | CHECKED or BELIEVED | can a later shape reach it? |
|---|---|---|---|
| `distribute` — inner/standard only | pushed side not null-supplying, not a semi/anti body | **checked** (`:522-524`, spelled positively) | — |
| `distribute` — assumes WRITTEN order | pushdown must precede `JoinEnumeration` | **believed**, guaranteed only by call order at `main.cc:571/583` | — |
| never below AGGREGATE / SORT / DISTINCT / LIMIT | structural | **checked structurally** — `apply` rewrites only FILTER over JOIN / SCAN / DERIVED / PROJECT, and a `LIMIT` body has `LIMIT` as its ROOT so the PROJECT rule cannot see it | no |
| `pushIntoDerived` ENTRY | the body root's rows ARE the derived relation's rows | **checked structurally** (`derivedRelationSchema` renames and re-stamps only) | — |
| `remapThroughProject` DESCENT | every named column is a plain passthrough | **checked** (`dynamic_cast<ColumnRef*>`, all-or-nothing) | — |
| `remapOntoDerivedBody` | positional 1:1 between derived schema and body schema | **checked** (size guard + `resolveInSchema` per ref, all-or-nothing) | — |
| **the totality screen** | **every conjunct that CAN RAISE stays put; nothing crosses it** | partition **checked** (index-based at all four sites); **classifier `mayRaise` INCOMPLETE — P4-1** | **YES — an ill-typed comparison. Reachable today** |
| `mayRaise` precision | over-approximating is free | **BELIEVED, and FALSE — P4-2, 87×** | yes |
| `rebuild` — same relation | each edge consumed once + cross-product throw | **checked** | — |
| `rebuild` — no consumer reads schema POSITION | column identity, not position | **checked** since the tie-break moved to `(relation_slot, name)`; verified unique by execution (A.6) | — |
| `containsOuterJoin` / `slotDeclineReason` | declines outer and semi/anti spines | **checked**, over-declining | — |
| written-cost floor | bounds a misestimate | **checked** | — |
| `CardinalityEstimator::selectivity` | *decides conjunct ORDER, which is not shape-only* | **still not stated anywhere** — the screen bounds the damage to error behaviour, but the rule that `selectivity` may only affect plan quality is written down nowhere | yes |
| `foldConstants` | folded node has the same VALUE; no downstream consumer tests SHAPE unsafely | **believed**; census of 5 in `binder.cc` is correct as written but is now short by one — see B.4 | yes |
| `deterministicCut` | the inserted sort's input schema is plan-independent | **checked** (A.6) | — |
| `materializeSubqueries` | the body's result is plan-independent | **checked as of the tie-break fix** — B3-1b's repro now agrees | — |

### B.3 Idempotency and ordering, re-checked after round 3's edits

* **`PredicatePushdown` is still effectively idempotent**, and round 3's two new
  rules do not break it: a second `apply` over the rewritten tree sees
  `FILTER`(residual) over `JOIN` (every residual still has `soleSlot < 0` or is
  frozen), and the planted body filter is `FILTER` over `SCAN`/`JOIN`, handled by
  the existing branches. The one genuinely non-idempotent write is
  `LogicalDerived::pushdown_decision`, which is a string, is only ever
  overwritten with the same value, and is never read by the pass.
* **`JoinEnumeration` is still not idempotent** (pass 2's B-7), and round 3's
  `applyToSpineLeaves` was written specifically so `apply` does not hand a
  rebuilt spine back to `decompose` — the descent steps OVER spine joins and into
  their leaves only. I traced it: `children[1]` of a left-deep spine join is
  never a `JOIN` except for a semi/anti body, which `reorder` declines on its own
  account. Correct.
* **Ordering**: pushdown before enumeration is still the one load-bearing
  dependency, still guaranteed only by call order. `foldConstants` before
  `LogicalPlanBuilder::build` is the second, and both `binder.cc:189-191` and
  `logical_plan.cc:253-262` now say so explicitly. Nothing enforces either.

### B.4 LOW P4-5 — the folding census is complete as of when it was written, and the SAME ROUND added a sixth shape consumer that does not appear in it

`binder.cc:219-249` lists five downstream consumers that test SHAPE and closes
with:

> The obligation this comment carries forward, now stated against a census that
> is complete: a new rule that tests for a Literal in a position the user could
> have written an expression in must ask what the user WROTE, or gate itself
> ahead of this call.

`mayRaise` (`predicate_pushdown.cc:431-437`), added in the same fix round, is
exactly such a rule:

```cpp
const bool const_start = dynamic_cast<const Literal*>(sub->start.get()) != nullptr;
const bool const_len   = !sub->length
                       || dynamic_cast<const Literal*>(sub->length.get()) != nullptr;
if (!const_start || !const_len) return true;
```

It tests Literal-ness in a position the user can write an expression in, it runs
downstream of folding, and it does neither of the two things the obligation
demands. It is nonetheless **SAFE**, for a third reason its own comment states:
`inferExprType` refuses a constant out-of-domain start/length AT PLAN TIME in
both legs, so the only trees that reach `mayRaise` with a folded literal start
are ones already proven in-domain. Verified: `SUBSTRING(team, 1 + 1, 2)` is
accepted and treated as total; `SUBSTRING(team, 1 - 1, 2)` is refused at plan
time in both legs.

A second-order note worth recording: folding now decides **whether a conjunct
freezes**. `WHERE speed > 2020 + 4` folds and stays total; a fold that DECLINES
(an overflowing constant) leaves a `BinaryExpr` and freezes everything after it.
Both legs fold identically so this is not a divergence, but it is a new way for
`foldConstants` to change a PLAN, and neither file mentions it.

### B.5 MEDIUM P4-6 — the retracted folding sentence has a FOURTH copy, and it is the one in `development.md`

`binder.cc` withdrew *"folding cannot change results, so it is canonicalization
rather than a cost-based decision"* by execution (pass 2's B-5).
`constant_folding.h` was swept this round — it now carries the sentence only in
quotation marks, as a retraction (`constant_folding.h:40-47`). `join_enumeration.h`
was swept for the other retracted paragraph.

`development.md:604-607` still asserts it, verbatim and unqualified:

> `foldConstants` … is unconditional — **folding cannot change results, so it is
> canonicalization rather than a cost-based decision**, and both execution paths
> and `--no-optimize` get it.

This is the only surviving unqualified statement of the claim in the repository
(`grep -rn "cannot change results"` over `src/`, `docs/` and `*.md` returns
exactly three hits: the two retractions and this one). It matters more than an
ordinary stale doc for a reason the source itself spells out —
`logical_plan.cc:253-262` names the exact reader this sentence produces:

> A reader who took `constant_folding.h`'s old word "canonicalization" at face
> value and gated the pass would land here, on the `--no-optimize` leg only.

The header they would consult has been fixed. The prose document, which the same
paragraph in `development.md` is the canonical version of, has not. Ranked
MEDIUM, not LOW, because the consequence is specific and named: every TPC-H
interval predicate errors on the differential leg.

While here: `development.md:808` still reads *"The decline is silent, in the same
shape as the <3-relation one"* for `hasSlotOutsideRangeTable`, which `18af84f`
made false. Already recorded by the join-chain seam's pass 2; not re-counted.

### B.6 Silent declines — the complete list at this HEAD, and one is new and large

Phase 5 has found three. Enumerating every point where a pass refuses a move:

| decline | reported? | verdict |
|---|---|---|
| `reorder`, `n < 3` and `n > 32` | silent | honest — no decision existed |
| `containsOuterJoin` | `join-ordering=skipped (outer join)` | reported |
| `slotDeclineReason` (semi/anti, out-of-range slot) | `join-ordering=skipped (…)` | reported |
| `pushIntoDerived` entry refusal | `pushdown=skipped (…)` on `LogicalDerived` | reported; one of its two reasons appears unreachable (P4-3) |
| `remapThroughProject` descent refusal | **silent** | argued from a false premise (P4-3) |
| `distribute` leaving a null-supplying bucket | **silent** | the conjunct genuinely cannot move; the SCAN does lose its zone-map hint, but nothing better is available |
| **the `firstMayRaise` freeze** — at `orderByWork`, `pushIntoJoin`, `pushIntoDerived` and the PROJECT descent | **silent, at all four** | **NEW, and measured at 87× (P4-2).** `--explain` shows the written plan and says nothing. This is the largest unreported decline in the seam's history and it was introduced by the fix that closed B3-2 |

### B.7 What I checked this pass and found CLEAN

Recorded as results so pass 5 does not re-derive them.

* **Pass 3's two BLOCKERs are closed.** B3-1 (`ORDER BY n.n_regionkey LIMIT 5`
  over the 3-relation TPC-H join) and B3-1b (the same cut inside a scalar
  subquery) both return **identical output in both legs** on
  `data/tpch/sf0.01/catalog.json` at this HEAD. So do both of B3-2's SUBSTRING
  shapes on `catalog.json`.
* **B3-3's fix is real and its side claims hold**: the body's scan regains
  `chunks_skipped=0/2` and the body's projection carries 174 rows instead of
  10000 (A.4).
* **The freeze partition is sound at all four sites** — no conjunct crosses the
  boundary in either direction (P4-2, last paragraph).
* **`inferExprType` DOES decide, at plan time and in both legs, every type error
  in arithmetic, LIKE, SUBSTRING, IN and CASE** — verified case by case against
  `logical_plan.cc:159-247`. Only comparison is unguarded (A.2).
* **`deterministicCut` runs in both legs and in both engines**, and cannot
  diverge (A.6). `(relation_slot, name)` is unique on every schema a sort can
  see — `derivedRelationSchema` REFUSES a repeated name, verified by execution.
* **The derived-body pushdown preserves results on seven body shapes** —
  passthrough, computed projection, `GROUP BY` filtered on a key, `GROUP BY`
  filtered on an aggregate output, `DISTINCT`, `ORDER BY … LIMIT` (the shape
  where descending would be a wrong answer), and a joining body with an alias
  list. All `optimized == --no-optimize`.
* **13 further cross-shape queries agree between the legs** — 3-relation joins
  with `ORDER BY … LIMIT`, a derived relation inside a 3-relation join, a
  derived body that itself joins, a `LEFT JOIN` with a null-supplying-side
  predicate, an `IN` subquery beside a pushable conjunct, and an aggregate-bodied
  derived relation with two conjuncts.
* **`mayRaise` is complete for every raise EXCEPT the comparison class** — full
  enumeration in A.2, including the two I could not turn into a divergence
  (`asInt()` on a bare DOUBLE predicate; `Column not found in schema`).
* **TPC-H contains no conjunct that the freeze would catch** — all 22 rendered
  and checked; Q11's product is in a HAVING and Q22's SUBSTRING is
  constant-argument, so the screen's carve-out holds. This is why the gate is
  green and why P4-2 is invisible to it.
* **The invariant harness's three pass-3 obligations are discharged** (B.1).
* **`applyToSpineLeaves` cannot re-decompose a rebuilt spine** (B.3).
* **`--no-optimize` gates the VECTORIZED optimizer only.** The Volcano path
  plans through `Planner::plan` and ignores the flag entirely, so both Volcano
  rows of every mode census in this seam are not differentials at all. Worth
  stating once: a "4-mode" census contains two modes that cannot, in principle,
  show an optimizer divergence.

### B.8 Not reached

* I did not run any harness or the gate. Every number here is a single-query CLI
  invocation with `--no-cache` against `build/swiftql`, which was newer than
  every file under `src/`. No source file was touched and no build was started.
* I did not construct a WRONG-ROW divergence. P4-1's four repros are all
  error-vs-rows; I looked for a row divergence through the pruner (`shouldSkip`
  is order-independent except for the throw) and through the freeze (the
  partition is sound) and found none. Absence of a repro is not a proof.
* I did not measure P4-2 on a Release build. The 87× is the debug binary; A.4
  documents that ratios on this shape are build-sensitive by roughly 3×, so the
  Release figure should be independently taken before it is quoted.
* I did not re-audit `subquery_materialization.cc` or `subquery_decorrelation.cc`
  internals; the subquery-chain seam owns those.

---

## Summary

| severity | count | findings |
|---|---|---|
| **BLOCKER** | 0 | — |
| **HIGH** | 2 | **P4-1** — `mayRaise` is under-approximate: it classifies a type-mismatched COMPARISON (`team = 5`, `5 = team`) as total, and such a comparison raises per row from `Value`'s operators AND from `ChunkPruner::canSkipChunk`. Four measured divergences on the shipped catalog, both directions, through `orderByWork`, `distribute` under a LEFT join, and the new `pushIntoDerived`. The precondition the fix wrote down — *"`inferExprType` decides every TYPE error at plan time"* — is false: `inferExprType`'s comparison branch never compares its two operand types. **P4-2** — the screen's over-approximation is not free: any arithmetic conjunct freezes every conjunct written after it, out of the join and off its scan, with **no decline line anywhere**. Measured **87×** (39.9 ms vs 0.45 ms) between two queries whose only difference is the order the same three predicates were written in. A regression against the pre-fix code, invisible to every correctness harness |
| **MEDIUM** | 1 | **P4-6** — the retracted sentence *"folding cannot change results, so it is canonicalization"* has a fourth copy, in `development.md:606`, and it is now the only unqualified statement of it in the repo. `logical_plan.cc:253-262` names the exact failure this sentence causes |
| **LOW** | 3 | **P4-3** — the argument for not stamping a PROJECT-boundary refusal is false as written (`--explain` prints output NAMES, not expressions, so `LogicalProject [t, s2]` is byte-identical for a computed and a passthrough column); plus one apparently-unreachable refusal reason and a last-wins reason string. **P4-4** — the harness's `INVARIANT_SCOPE` census says six ungated passes; there are eight (`substituteGroupKeyRefs`, `deterministicCut`). Conclusion unaffected, count wrong. **P4-5** — folding's census, declared "complete", was made short by one in the same round: `mayRaise`'s Literal test is a new shape consumer that neither asks what the user wrote nor gates ahead of folding (it is safe, for a third reason) |

**Verdict: the seam does not hold, and the audit should not end here. Pass 3's
two BLOCKERs are genuinely closed — both repros now agree, and the tie-break's
move from column POSITION to column IDENTITY is the right fix, checked rather
than argued. What did not close is B3-2. The fix wrote down the precondition for
the first time, and the precondition is correct; the CLASSIFIER that implements
it is wrong in both directions at once. It is too coarse where arithmetic is
concerned — 87× and silent — and too fine where comparison is concerned, because
it rests on a sentence about `inferExprType` that is false for the one operator
class it names. That is the same failure this codebase has now hit four times: a
precondition stated as a comment at a moment when the reader believed it, and not
checked against the code it names. The difference this time is that the sentence
was never true, not that it expired.**
