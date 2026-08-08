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

The residual: where `(relation_slot, name)` is NOT unique — a body that projects
two identically-named columns through a `LogicalDerived`, which stamps them both
slot 0 — `tieBreakOrder`'s `stable_sort` falls back to schema order. That is
still plan-independent, because `rebuild` copies a leaf's columns in their own
order and a derived leaf's block stays contiguous, so the duplicates keep the
same relative position on both legs. Checked by execution on
`(SELECT l.team, dr.team FROM laps l JOIN drivers dr ON …) AS d(a,b)` with an
`ORDER BY … LIMIT` straddling: identical. Not a finding, recorded so pass 5 does
not re-derive it.
