# Seam audit — optimizer result preservation (pass 3, final)

HEAD `922ca15`, branch `claude/phase5-week26-qomtkb`.
Predecessors: `seam-optimizer-preservation-pass-1.md`, `-pass-2.md`.

Status: IN PROGRESS (written incrementally; summary block at end).

## BLOCKER B3-1 — `optimized` and `--no-optimize` return DIFFERENT ROWS. The fix-round-2 sort tie-break rests on a precondition `JoinEnumeration::rebuild` breaks by design.

This is the thing the seam exists to forbid, and it is reproducible from the CLI
on a shipped catalog. Run on `data/tpch/sf0.01/catalog.json`,
`--execution vectorized --storage columnar --no-cache`:

```sql
SELECT n.n_name, s.s_suppkey
FROM supplier s
JOIN nation n ON s.s_nationkey = n.n_nationkey
JOIN region r ON n.n_regionkey = r.r_regionkey
ORDER BY n.n_regionkey
LIMIT 5
```

```
optimized                     --no-optimize
n_name   s_suppkey            n_name      s_suppkey
ALGERIA  5                    ALGERIA     5
ALGERIA  25                   MOZAMBIQUE  17
ALGERIA  40                   MOROCCO     20
ALGERIA  46                   MOROCCO     23
ALGERIA  61                   ALGERIA     25
```

Four of five rows differ. Not a formatting difference, not an ordering
difference the harness's `sorted()` would absorb: the ROW SETS differ.

### The mechanism, and it is exactly the premise the fix wrote down

Drop the `LIMIT` and the two legs return **the same 100 rows in a different
order** (`diff` of both outputs sorted: identical; `head -8` of both, unsorted:
different from row 2 on). So the row set is preserved by join enumeration; what
is not preserved is the ORDER the sort produces, and `LIMIT` turns an order
difference into a set difference.

The order comes from the tie-break commit `7b84952` added
(`src/execution/sort_comparator.h:119-137`, `rowLess`):

```cpp
// every declared key ties — see the header comment
const int n = std::min(schema.size(), ...);
for (int i = 0; i < n; ++i) {
    int c = compareForTieBreak(a[i], b[i]);
    if (c != 0) return c < 0;
}
```

The tie-break is **positional**: it walks the sort input's schema in COLUMN
ORDER. Its header states its own precondition, in these words:

> THE PRECONDITION IT RESTS ON, STATED SO IT CAN BE CHECKED: the sort's INPUT
> row must be the same in every mode.

`JoinEnumeration::rebuild` (`src/planner/join_enumeration.cc:214-278`) breaks it,
and says so in its own comment:

```cpp
// merged schema: [left block] ++ [this relation's columns, stamped r].
// Order matches VecHashJoinNode's two contiguous output blocks — a
// slot-sorted "canonical" order is not available (invariant 1).
```

The merged schema is built **in the chosen order**, not the written one. For the
query above `--explain` reports
`order=nation@1,region@2,supplier@0 cost=303 (written=436) method=dp`, so:

| leg | sort input schema, in order |
|---|---|
| optimized | `n_nationkey, n_name, n_regionkey, r_regionkey, s_suppkey, s_nationkey` |
| `--no-optimize` | `s_suppkey, s_nationkey, n_nationkey, n_name, n_regionkey, r_regionkey` |

`ORDER BY n.n_regionkey` ties across the 20 suppliers of region 0. The tie-break
then compares `n_nationkey` first in one leg and `s_suppkey` first in the other.
Two different total orders, two different first five rows.

The row values are the same in both legs. The COLUMN ORDER of the row is not,
and the tie-break is a function of column order. The precondition says "the
same row"; what it needed to say is "the same row **in the same column order**",
and that is false for any query whose sort sits directly above a reordered join.

### Three controls, run, that isolate it to exactly this cause

| control | change | result |
|---|---|---|
| write the FROM list in the order the DP would choose (`FROM nation n JOIN region r JOIN supplier s`), so `order=nation@0,region@1,supplier@2` == written | no reorder, so no schema permutation | **SAME** |
| make the declared key a total order: `ORDER BY n.n_regionkey, s.s_suppkey LIMIT 5` | tie-break never reached | **SAME** |
| put a `GROUP BY` between the join and the sort (`SELECT n.n_name, COUNT(*) ... GROUP BY n.n_name ORDER BY COUNT(*) LIMIT 5`) | sort input is `buildAggregateSchema`'s output, which is plan-independent | **SAME** |

So the failing shape is precisely: **≥ 3 relations (so `JoinEnumeration` runs at
all, `MIN_ENUMERATED_RELATIONS = 3`) + the DP picks a non-written order + the
sort sits directly above that join (no aggregate between) + `ORDER BY` is not a
total order on the surviving columns + the tie is not immaterial.** `LIMIT` is
not required for divergence (the row ORDER already differs); it is required for
the divergence to survive a set comparison.

### Mode census

| mode | verdict |
|---|---|
| `columnar` + `vectorized` | **DIVERGE** |
| `columnar` + `volcano` | SAME — `Error: multi-way joins are not supported on the Volcano path` in both legs |
| `row` + `volcano` | SAME — same refusal |
| `row` + `vectorized` | SAME — `--execution vectorized requires --storage columnar`, refused in both legs |

col-vec is the only mode where a 3-relation join and `JoinEnumeration` coexist,
and it is the mode the invariant harness uses. There is no cross-ENGINE
divergence here, because Volcano cannot run the shape at all.

### Why 119 invariant checks and 0 divergences did not see it

Two independent reasons, both structural:

1. **`run_optimizer_invariant` compares SORTED rows.**
   `python_tools/test_new_queries.py:496-514` — `normalize()` ends
   `return sorted(tuple(...) for row in rows)`. A pure ORDER divergence is
   invisible to it by construction. Only the `LIMIT` variant, where the SET
   changes, would be caught.
2. **The invariant suite runs against `catalog.json`, which has TWO tables.**
   Every query in it that joins is a 2-relation join, and `reorder` returns
   unchanged at `join_enumeration.cc:444` (`n < MIN_ENUMERATED_RELATIONS`, = 3).
   A 3-relation shape needs a self-join or a derived relation; the ones present
   (`self_join_where_both`) are still 2 relations.

So the invariant's "0 divergences" is not evidence about this shape — the suite
contains no query that can reach the code path.

### Ranking

**BLOCKER.** `optimized != --no-optimize` on a query the CLI accepts, on a
catalog that ships in the repo, in the only mode where both the optimizer and
a multi-way join run. Every answer is legal SQL in isolation (SQL does not fix
the order when `ORDER BY` is not total) — and that is exactly the argument
`sort_comparator.h` already rejects, in its own words: *"The project asserts
`optimized == --no-optimize`, so that is a defect even though every one of those
answers is legal SQL."* The tie-break commit accepted that obligation and then
did not discharge it for the one rewrite that permutes the schema.

### What a fix has to decide (stated, not prescribed)

The tie-break needs an ordering of columns that is a function of the QUERY, not
of the plan. `rebuild` already stamps every merged column with its binder slot
(`c.relation_slot = order[k]`), and the written order is by definition slot
order — so `(relation_slot, position-within-relation)` is available at the sort
and is plan-independent. `rebuild`'s comment says a slot-sorted schema cannot be
MATERIALIZED (invariant 1: the join's two output blocks must stay contiguous),
but the tie-break does not need the schema reordered, only a comparison ORDER —
which is a permutation computed once in `rowLess`'s caller. That is a claim
about feasibility, not a patch; I touched no source.

### B3-1b — the amplification: `materializeSubqueries` turns the order divergence into a VALUE divergence, in a query with no `ORDER BY` and no `LIMIT` of its own

The "every answer is legal SQL" defence dies here. `data/tpch/sf0.01/catalog.json`,
col-vec:

```sql
SELECT n_name, n_nationkey FROM nation
WHERE n_nationkey = (
  SELECT n.n_nationkey
  FROM supplier s
  JOIN nation n ON s.s_nationkey = n.n_nationkey
  JOIN region r ON n.n_regionkey = r.r_regionkey
  WHERE s.s_suppkey > 0
  ORDER BY n.n_regionkey DESC
  LIMIT 1)
```

```
optimized                      --no-optimize
n_name  n_nationkey            n_name        n_nationkey
EGYPT   4                      SAUDI ARABIA  20
```

One row either way; a **different** row. The outer block has no `ORDER BY`, no
`LIMIT`, and nothing unspecified about it. `--explain` shows the mechanism
directly — the scalar subquery is materialized to a constant before planning
(`main.cc:500`, `materializeSubqueries`), and the constant differs:

```
optimized      LogicalFilter [(n_nationkey = 4)]
--no-optimize  LogicalFilter [(n_nationkey = 20)]
```

The nested runner threads the flag (`main.cc:519`,
`runVectorizedToRows(..., args.no_optimize)`) precisely so the sub-plan is
tested too — so the body is optimized in one leg and not the other, its
`LIMIT 1` cut lands on a different row for exactly the reason in B3-1, and the
materialized `Literal` carries the difference into a query that has no
order-dependence at all.

Body in isolation, `... ORDER BY n.n_regionkey DESC LIMIT 1`: optimized returns
`n_nationkey = 4`, `--no-optimize` returns `20`.

`sort_comparator.h`'s header already names this amplification as the reason the
tie-break was written — *"a scalar subquery of the same shape — materialized to
a `Literal` — turned that into `COUNT(*)` = 977 versus 1536"*. It closed the
cross-ENGINE instance and left the optimizer instance open.

Same BLOCKER, same root cause; recorded separately because it is the shape that
makes the severity unarguable.

## HIGH B3-2 — `orderByWork` reorders conjuncts on an estimate, and predicate evaluation is NOT total. The optimizer both MASKS and INTRODUCES a per-row error.

`src/planner/predicate_pushdown.cc:239-255`:

```cpp
// Order conjuncts most-selective-first (smallest keep-fraction). Stable so ties
// and stat-less predicates (fallback selectivity) keep their original order.
void orderByWork(std::vector<std::unique_ptr<Expr>>& conjuncts, ...) {
    std::stable_sort(conjuncts.begin(), conjuncts.end(), [&](a, b) {
        return CardinalityEstimator::selectivity(a.get(), ctx)
             < CardinalityEstimator::selectivity(b.get(), ctx); });
}
```

`orderByWork` is called only from `filterOnto` and `PredicatePushdown::apply`,
both inside the `--no-optimize` gate. So conjunct ORDER differs between the two
legs. The columnar predicate cascade short-circuits — `columnar_eval.cc:143-146`,
AND evaluates the right conjunct only over the left's survivors — and per-row
evaluation **can throw**. Reordering therefore decides whether a throwing
conjunct is ever reached.

Both directions are reachable from the CLI on the shipped `catalog.json`,
`--execution vectorized --storage columnar`.

**Direction 1 — the optimizer MASKS an error.**

```sql
SELECT team FROM laps
WHERE SUBSTRING(team, lap_id - lap_id, 2) = 'x' AND speed = 333.3333
```
```
optimized       (0 rows)
--no-optimize   Error: SUBSTRING: start position must be >= 1
```
`--explain`: written `[(SUBSTRING(...) = x) AND (speed = 333.3333)]`, optimized
`[(speed = 333.3333) AND (SUBSTRING(...) = x)]`. `speed = 333.3333` has a real
NDV so its selectivity is ~1e-4; the SUBSTRING conjunct is a `=` with no
`col op lit` shape, so it gets `FALLBACK_EQ_SELECTIVITY = 0.1`
(`cardinality_estimator.h:14`, used at `:159`). The speed conjunct sorts first,
keeps zero rows, and the SUBSTRING is never evaluated.

**Direction 2 — the optimizer INTRODUCES the error.**

```sql
SELECT team FROM laps
WHERE team LIKE 'zzz%' AND SUBSTRING(team, lap_id - lap_id, 2) = 'x'
```
```
optimized       Error: SUBSTRING: start position must be >= 1
--no-optimize   (0 rows)
```
`--explain`: written `[team LIKE 'zzz%' AND (SUBSTRING(...) = x)]`, optimized
`[(SUBSTRING(...) = x) AND team LIKE 'zzz%']`. `LIKE` deliberately has no
selectivity rule (`cardinality_estimator.cc:120`) and takes
`FALLBACK_SELECTIVITY = 0.5`; the SUBSTRING conjunct's 0.1 sorts ahead of it, so
a predicate that matched nothing is demoted below one that throws.

This is the second direction that matters: the first can be argued as the
optimizer being kinder, the second is the optimizer manufacturing a failure on
a query that succeeds without it.

### Mode census

| mode | optimized | `--no-optimize` |
|---|---|---|
| `columnar` + `vectorized` | **Error** | 0 rows |
| `columnar` + `volcano` | Error | Error |
| `row` + `volcano` | Error | Error |
| `row` + `vectorized` | refused (needs columnar), both legs | — |

Volcano evaluates the whole WHERE per row with no cascade, so it throws in both
legs; col-vec is where the divergence lives.

### The precondition, and that it is BELIEVED rather than checked

`orderByWork`'s comment argues only about COST — *"'Expected work' is modeled as
selectivity only — optimal when per-predicate eval cost is uniform"* — and
defers a cost-weighted ranking to Week 28. It never states the correctness
precondition a reordering needs, which is that **conjunct evaluation is total**:
that no conjunct can fail on a row another conjunct would have removed. That
precondition is false today and has been since Week 25 added `SUBSTRING`, whose
`substringOf` raises per row for a computed start < 1 (`evaluator.cc`; the
plan-time twin at `logical_plan.cc:234-243` catches only the *constant* case, and
says so: *"A computed position still raises at execution"*).

Every other per-row raise is in the same class. `checkedArith` overflow throws;
integer division by zero does **not** (it yields NULL — verified:
`SELECT 100 / (lap_id - lap_id) FROM laps LIMIT 1` prints `NULL`), so the
division case is safe by accident rather than by rule.

### Why the harnesses do not see it

`run_optimizer_invariant` catches an exception from either leg and records an
ERROR — so this shape WOULD be caught if it were in the suite. It is not: no
query in `test_new_queries.py` puts a computed-argument `SUBSTRING` in a `WHERE`
alongside a second conjunct. `grep -rn "SUBSTRING" python_tools/` finds the
function only in select lists and in fully-constant argument positions.

### Ranking

**HIGH, not BLOCKER.** `optimized != --no-optimize` on a CLI-typable query on
the shipped catalog, in both directions, and the failing side is loud rather
than silently wrong — an error is not a wrong answer. It is HIGH rather than
MEDIUM because direction 2 means the optimizer can turn a working query into a
failing one, which is the strongest form of "not result-preserving" short of a
wrong row.

## Part A — the fix-round-2 questions

### A.1 Is the new folding comment TRUE? Yes as to values. Its census of shape-consumers is WRONG — it says "there was exactly one", and there are four more.

`src/planner/binder.cc:174-207`. The narrow claim it now makes —

> for any expression it agrees to fold, the folded node evaluates to the same
> Value as the original would have, on every row

— is **true**, and earned rather than asserted. `foldNode` calls the same
`evaluate()` the per-row path calls with an empty `Row`/`Schema`
(`constant_folding.cc:89`, legitimate because a constant subtree reads neither),
declines on any `std::runtime_error` so overflow and ill-typing surface from
their usual site (`:92-97`), declines on a NULL result (`:98` — so folding never
manufactures the `Literal::null_type` case), never folds `IS NULL` (`:121-125`),
never folds an `AggregateExpr` (`:127-130`), and never descends into a
`SubqueryExpr` body (`:165-169`). The `foldDateInterval` special case routes its
one unguarded operation through `checkedNegate` (`:53`).

It also correctly names the gap — *"any consumer downstream of here that tests
the SHAPE of the tree rather than the value it computes"*. What it gets wrong is
the count: *"There was exactly one — the Validator's column-ordinal rule."*

**The exact shape delta folding can produce.** `isArithOp` is `+ - * /` only,
plus `foldDateInterval` on `+`/`-`; a comparison operator is never folded, and
`Literal` is the only node kind folding creates. So exactly two flips are
reachable: (i) *"is this a `Literal`?"* false → true, and (ii) *"is this a
`BinaryExpr` / `UnaryExpr` / `IntervalLiteral`?"* true → false. Folding can
neither create nor destroy a `ColumnRef`.

**Every remaining shape consumer downstream of `Binder::bind`, and its verdict.**

| # | site | flip | verdict |
|---|---|---|---|
| 1 | `validator.cc:359,366` ordinal rule | (i) | **FIXED** — reads `written_ordinal`, not the tree |
| 2 | `logical_plan.cc:106` `inferExprType` Literal case | (i)/(ii) | **safe, with an obligation.** Safe iff `evaluate`'s result type equals `inferExprType`'s promotion rule for the same tree. Both say INT⊗INT→INT, any DOUBLE→DOUBLE (`logical_plan.cc:163-170`). Confirmed: `SELECT 7 / 2 FROM laps LIMIT 1` prints `3`, i.e. the fold kept INT truncating division. Nobody states this obligation anywhere |
| 3 | `logical_plan.cc:234-243` SUBSTRING constant start/length refusal | (i) | **outcome-changing, LOW.** Folding ACTIVATES it. `SELECT SUBSTRING(team, 1 - 1, 2) FROM laps WHERE speed > 99999` → plan-time `Error: SUBSTRING: start position must be >= 1`; the value-identical non-constant `SUBSTRING(team, lap_id - lap_id, 2)` with the same WHERE → **0 rows, no error**. Folding converts a conditional per-row failure into an unconditional plan-time one. Identical in both legs, and the site's own comment says it is deliberate |
| 4 | `logical_plan.cc:250` `IntervalLiteral` → throw | (ii) | **safe, and it inverts the comment's framing.** Folding is the ONLY thing that removes an `IntervalLiteral`; without it every TPC-H `date ± interval` predicate is `Error: INTERVAL is only valid in constant date arithmetic`. So folding is load-bearing for CORRECTNESS, not "canonicalization". Anyone who moved `foldConstants` inside the `--no-optimize` gate on the strength of the word "canonicalization" would break every interval query on the `--no-optimize` leg |
| 5 | `logical_plan.cc:642` `substituteInto` early-return on `ColumnRef \|\| Literal` | (i) | **safe.** A select item that folds to a `Literal` is never substituted by a GROUP BY expression key — and does not need to be, because a `Literal` is row-independent and evaluates to the same value above the aggregate. Confirmed on the shape the ordinal fix newly UNBLOCKED: `SELECT 1 + 1, COUNT(*) FROM laps GROUP BY 1 + 1` → `2 | 10000` |
| 6 | `cardinality_estimator.cc:149-153` `col op lit` selectivity | (i) | **safe as a shape test** — folding strictly improves it. But see B3-2: what the estimate is USED for (conjunct order) is not plan-quality-only |
| 7 | `chunk_pruner.h:60` `collectSimplePredicates` | (i) | **safe.** Folding lets `speed > 300 + 40` prune. Pruning is value-based (`canSkipChunk`) and runs in BOTH legs (`pruning=on` appears under `--no-optimize`), so fold+prune is ungated end to end |
| 8 | `columnar_eval.cc:154` `col op lit` fast path | (i) | **safe here.** Folding routes more predicates to `scanColumn` instead of `evalFallback`; their agreement is the engine-divergence seam's subject, already audited |
| 9 | `subquery_decorrelation.cc:306` `constantOnly` | (i)/(ii) | **safe, and closed under folding.** It accepts `Literal`, `BinaryExpr`, `UnaryExpr` recursively, so anything it accepted pre-fold folds to a `Literal` it still accepts. Monotone |
| 10 | `expr_utils.h` `exprToString` | (ii) | **outcome-changing, LOW.** The OUTPUT COLUMN NAME comes from the folded tree: `SELECT 2020 + 4 FROM laps LIMIT 1` is headed `2024`; SQLite heads it `2020 + 4`. Both legs identical, and both harnesses compare `row.values()` rather than headers, so invisible by construction |
| 11 | `expr_utils.h` `exprKey` | (i)/(ii) | **safe.** The GROUP BY key and the select item are folded by the SAME `foldConstants(stmt)` call over the same statement, so their keys stay equal |
| 12 | `expr_utils.h` `cloneExpr` | — | total dispatcher, shape-preserving |
| 13 | `evaluator.cc:84`, `expression_executor.cc:489` | — | value-based |
| 14 | `join_condition.cc:13,58-60` join-key classification | (ii) | **safe by two independent facts.** It requires a `BinaryExpr` with a comparison op (never folded — `isArithOp` is arithmetic only) AND a `ColumnRef` on both sides (folding can neither create nor destroy one) |
| 15 | `parser.cc:43-58` `ordinalAsWritten`, `:68-84` `constantValue` | — | run BEFORE folding; not downstream |

**So: the comment's mechanism is right and its census is wrong.** Four more
consumers test shape, three of them provably safe for a stated reason and two
(#3 SUBSTRING, #10 column naming) actually changing an outcome — quietly, in
both legs. Neither is a divergence and neither is worth a fix on its own;
recorded because the comment now carries an obligation for future weeks
(*"a new rule that tests for a Literal … must ask what the user WROTE"*) and
that obligation is stated against a census that is already short by four.
Ranked **LOW**.

### A.2 `written_ordinal` — set on every path that needs it; it fails OPEN, and open is the safe direction

**Where it is set.** Three sites, all in the parser:
`parser.cc:226` and `:234` (ORDER BY — the first item and the comma loop, both
covered), and `parser.cc:700` (GROUP BY). The GROUP BY site sets it only in the
`else` branch, because the `if` branch is a plain `ColumnRef` and a `ColumnRef`
is never an ordinal. Correct by cases.

**Where a struct is synthesised instead of parsed.** Exactly one site in `src/`:
`subquery_decorrelation.cc:544`

```cpp
GroupByColumn{cr->table_name, cr->column_name, cr->id, nullptr}
```

Four positional initializers; `written_ordinal` is declared **last** in both
structs precisely so positional brace-inits stay valid (`ast.h:250-269`,
comment says so), and it defaults to `""`. No other `src/` site constructs
either struct. Test files build them positionally too
(`test_vectorized.cc:479-480`, `test_execution.cc:391`), same default.

**Which way it fails, and whether that is safe.** Empty = "the user wrote no
ordinal" = **accept**. It fails OPEN with respect to the refusal, and open is
correct: the rule exists to stop a USER silently sorting or grouping by a
constant. A key the planner synthesised was chosen deliberately by the planner,
so refusing it would be wrong — a fail-CLOSED default would refuse the very
`GroupByColumn` `subquery_decorrelation.cc:544` builds for TPC-H decorrelation.
`ast.h:240-241` states this: *"Only the parser sets this, so a hand-built
OrderByItem is never an ordinal, which is correct: nobody wrote it."* Verified
as written and verified as the right direction.

**Can a PARSED item lose the field before the Validator sees it?** I checked
every mutation site of `stmt.order_by` / `stmt.group_by`:
`constant_folding.cc:186-198` rewrites `item.expr` and `g.expr` and never
reconstructs the struct; `subquery_materialization.cc:408` walks
`GroupByColumn::expr` in place; the binder's alias substitution replaces the
`expr` only. Nothing rebuilds a parsed item, and struct copies carry the field.
**No path loses it.**

**Enforcement reaches nested blocks.** `Validator::validate` runs per statement
and nested bodies go through it — confirmed by execution:
`SELECT d.team FROM (SELECT team FROM laps ORDER BY 1) d LIMIT 3` →
`Error: ORDER BY 1: …`, and
`… WHERE driver_id IN (SELECT driver_id FROM drivers GROUP BY 1)` →
`Error: GROUP BY 1: …`. No derived-body or subquery-body hole.

**The boundary is where the comment says it is.** Confirmed by execution:
`ORDER BY 1`, `ORDER BY (1)`, `ORDER BY - -1` all still refused, all quoting
`ORDER BY 1`; `ORDER BY 1 + 1`, `SELECT 1 AS one … ORDER BY one` and
`GROUP BY 1 + 1` now accepted. **CLEAN.**

### A.3 Do the six ungated passes still behave identically in both legs? FIVE do. `materializeSubqueries` does NOT, and that is B3-1b.

Five of the six run strictly BEFORE the gate, on an input the flag cannot have
touched, so their output is identical by construction, not by argument:

| pass | site | argument |
|---|---|---|
| `foldConstants` | `binder.cc:207`, inside `Binder::bind` | `Binder::bind` runs before `LogicalPlanBuilder::build`, which runs before the `if (!no_optimize)` block (`main.cc:560` then `:566`). Same input, same code |
| `lowerInSubqueries` | `logical_plan.cc:1000` | inside `LogicalPlanBuilder::build`, same |
| `lowerExistsSubqueries` | `logical_plan.cc:1005` | same |
| `lowerCorrelatedScalars` | `logical_plan.cc:1022` | same |
| derived normalization (`buildRelation`) | `logical_plan.cc:908` | same |

I checked that none of the five reads anything the optimizer could have written:
they take no `estimated_rows`, no `TableStats`, and no plan annotation. Fix
round 2's edits to them (`reachesOutsideThisBody`, `hidden` synthetic columns,
the alias cleared off the aggregate, `SELECT *` not expanding over a synthetic
relation) are all pure statement rewrites at the same point, so they change
what both legs do, identically.

**The sixth does not, and it is by design.** `materializeSubqueries`
(`main.cc:500-543`) EXECUTES the nested query, and the runner is handed the flag:

```cpp
return runVectorizedToRows(std::move(body), catalog, std::move(tables),
                           args.no_optimize);   // main.cc:519
```

`runVectorizedToRows` then runs the same three gated passes on the body
(`main.cc:133-138`). So the pass is ungated but its OUTPUT is gate-dependent
whenever the body's result is plan-dependent. The comment at `main.cc:126-129`
argues this is right — a runner that always optimized *"would give both legs the
same subquery result and quietly stop testing the sub-plan"* — and it IS right:
the flag being threaded is what makes B3-1b visible instead of silent. But the
consequence is that pass-2's B-1 table has a sixth row that is not like the
other five, and B3-1b is that row firing.

**Correction to pass 2's B-1, therefore.** "Both legs of the differential run
identical code" holds for five of the six ungated passes, not all six.
`materializeSubqueries` runs identical code over a leg-dependent input and
produces a leg-dependent constant. That does not weaken B-1's point (the oracle
is blind to the five); it sharpens it.

## Part B — the passes, precondition by precondition

### B.1 The table this seam should have had from Week 21

For each pass: what makes it result-preserving, and is that CHECKED (the code
tests it) or BELIEVED (a comment asserts it)?

| pass | gated? | precondition for result preservation | checked or believed | can a LATER Phase-5 shape reach it? |
|---|---|---|---|---|
| `PredicatePushdown` — `distribute` | yes | pushed side is not null-supplying; not a semi/anti body; conjunct is single-slot and non-correlated | **checked** — `join_type == INNER && semantics == STANDARD` (`:301-303`), `soleSlot < 0` containment (`:341`) | verified again this pass; pass 2's B-6 stands |
| `PredicatePushdown` — never below AGGREGATE / SORT / DISTINCT / LIMIT | yes | structural: `apply` rewrites only `FILTER` over `JOIN` or `SCAN` (`:361-380`) | **checked, structurally** | no |
| `PredicatePushdown` — `filterOnto` over a `LogicalDerived` | yes | a conjunct on a derived relation is WRAPPED above it, never pushed into the body | **checked** (correctness) — but see B3-3, it is also an unstated performance decline | derived tables are Week 34, after the pass |
| `PredicatePushdown` — `distribute` assumes WRITTEN order | yes | pushdown must run BEFORE `JoinEnumeration` | **believed**, guaranteed only by call order at `main.cc:571/583` and `:135/136` | — |
| `PredicatePushdown` — `orderByWork` | yes | **conjunct evaluation must be TOTAL** — no conjunct may fail on a row another conjunct would have removed | **BELIEVED, and FALSE** | **YES — `SUBSTRING` (Week 25) raises per row. B3-2** |
| `JoinEnumeration` — `containsOuterJoin` / `slotDeclineReason` | yes | declines outer and semi/anti trees outright | **checked**, over-declining | — |
| `JoinEnumeration` — `rebuild` | yes | (a) the reordered tree computes the same RELATION — each edge consumed exactly once, cross-product throw; (b) **no consumer above depends on the merged schema's COLUMN ORDER** | (a) **checked**; (b) **BELIEVED, and FALSE** | **YES — the Week-37 sort tie-break reads schema order. B3-1** |
| `JoinEnumeration` — written-cost floor | yes | bounds a misestimate, does not need path-independent `rows` | **checked** | — |
| `CardinalityEstimator::estimate` | yes | every consumer of `estimated_rows` is a plan-SHAPE choice | **believed**; pass 2's A.3 traced all five consumers and they hold | — |
| `CardinalityEstimator::selectivity` | yes (via pushdown) | *nothing states one* — it now decides conjunct ORDER, which B3-2 shows is not shape-only | **not stated at all** | yes |
| `foldConstants` | **no** | folded node has the same value on every row; no downstream consumer tests SHAPE | **believed**, and the census is short by four (A.1) | yes — SUBSTRING (W25), intervals (TPC-H) |
| `lowerInSubqueries` / `lowerExistsSubqueries` / `lowerCorrelatedScalars` / derived normalization | **no** | run before the gate on an input the flag cannot touch | **checked, structurally** (A.3) | — |
| `materializeSubqueries` | **no**, but its RESULT is gate-dependent | the nested body's result must be plan-independent | **BELIEVED, and FALSE** | **YES — B3-1b** |

Two of the three "BELIEVED and FALSE" rows are new this pass; the third
(`selectivity` having no stated precondition at all) is the mechanism behind
one of them.

### B.2 Idempotency and ordering

- **`JoinEnumeration` is not idempotent** and its guard is half a guard — pass 2's
  B-7, re-confirmed, unchanged. Nothing calls `apply` twice.
- **`PredicatePushdown` IS effectively idempotent.** A second `apply` on the
  rewritten tree sees `FILTER`(residual) over `JOIN`; every residual conjunct
  still has `soleSlot < 0` so it stays residual, and the already-pushed
  conjuncts now live in `LogicalFilter`s *below* the join where `apply`'s
  `FILTER`-over-`SCAN` branch only re-orders them by the same key. The one
  non-idempotent operation is `restampSlots(c, 0)` (`:312`), and it is applied
  only to conjuncts newly routed to `children[1]`, so a second pass has none to
  restamp. No fixpoint hazard.
- **`CardinalityEstimator::estimate` is idempotent** — a pure function of the
  tree, overwriting `estimated_rows` with the same value.
- **Ordering is load-bearing in exactly one place and it is stated**: pushdown
  before enumeration (`distribute` needs written order). Confirmed at both call
  sites. **A second ordering dependency is NOT stated**: `foldConstants` must run
  before `LogicalPlanBuilder`, because `logical_plan.cc:250` throws on any
  surviving `IntervalLiteral` (A.1 #4). Today that holds because folding is at
  the end of `Binder::bind`, but nothing says the dependency exists.

### B.3 (MEDIUM) A third silent decline — predicate pushdown never enters a derived body at all, and it costs 9.4× on the simplest possible shape

Phase 5 has found two silent declines. Here is a third, measured.

`filterOnto` (`predicate_pushdown.cc:259-265`) attaches a conjunct **above** the
node it is routed to. When that node is a `LogicalDerived`, the predicate stops
there — permanently, in every shape, including the one with no join anywhere.

```sql
SELECT d.team, d.speed FROM (SELECT team, speed, season FROM laps) d
WHERE d.speed > 344 ORDER BY d.speed LIMIT 3
```

`--explain` (optimized):
```
LogicalFilter [(d.speed > 344)]            est=3333
  LogicalDerived [d, 3 columns]            est=10000
    LogicalProject [team, speed, season]   est=10000
      LogicalScan [laps, 3 columns]        est=10000
```

`--explain-analyze`, against the flat query that is its exact semantic
equivalent (`SELECT team, speed FROM laps WHERE speed > 344 ORDER BY speed LIMIT 3`):

| | derived form | flat form |
|---|---|---|
| execution | **18465 µs** | **1969 µs** |
| body `VecProject (materialize)` | `rows_in=10000 rows_out=10000`, 16221 µs (87.8%) | — |
| scan annotation | `VecScan [laps, 3 columns]` — no `chunks_skipped`, no `pruning=on` | `VecScan [laps, 2 columns] chunks_skipped=0/2` |

**9.4× slower**, and the cost is not the filter — it is the body's projection
materializing 10000 rows that the filter immediately discards down to 174.
Pushing a non-correlated conjunct through a derived relation's projection is
textbook and legal here (no aggregate, no `DISTINCT`, no `LIMIT` in the body);
declining it is a choice, and it costs the whole body.

Two aggravating details:

1. **The decline is silent.** `--explain` prints no `pushdown=skipped (derived)`
   line, unlike `join-ordering=skipped (outer join)` which `18af84f` added on
   exactly the "a decision was available and was refused" argument. Nothing in
   `--explain` distinguishes "there was nothing to push" from "there was, and we
   didn't".
2. **The chunk-pruning hint does not reach the body's scan either.** The flat
   form's scan prints `chunks_skipped=0/2`; the derived form's prints nothing.
   So the derived form loses zone-map pruning as well as pushdown.

Distinct from what is already recorded: pass 2's B-2 is about
`PredicatePushdown::apply` and `JoinEnumeration::apply` not RECURSING into their
own result when the outer block has a join. This one needs no join at all and
is not about recursion — `filterOnto` is reached, and wraps by design. Ranked
**MEDIUM**: pure plan quality, invisible to every correctness harness by
construction, measured at 9.4× on the simplest shape that exhibits it.

### B.4 The invariant harness, interrogated a third time

Beyond pass 2's B-1 (the oracle is blind to five ungated passes — corrected
count, see A.3), two properties of `run_optimizer_invariant`
(`python_tools/test_new_queries.py:560-584`) that decide what "119 checks, 0
divergences" means:

1. **It compares SORTED rows.** `normalize()` (`:496-514`) ends
   `return sorted(...)`. Every divergence that is an ORDER difference and not a
   SET difference is invisible. B3-1 without its `LIMIT` is exactly that shape,
   and the harness would pass it.
2. **It runs against `catalog.json`, which has two tables.** `JoinEnumeration`
   returns unchanged below `MIN_ENUMERATED_RELATIONS = 3`
   (`join_enumeration.cc:444`, `join_enumeration.h:99`). The suite's only
   multi-relation join shapes are 2-relation, including `self_join_where_both`.
   **No query in the invariant suite can make `reorder` change anything.** The
   pass with the most surface in this seam is exercised by zero of the 119
   checks.
3. It DOES catch an exception from either leg and record it as an ERROR, so
   B3-2's shape would be caught — if a query of that shape existed. None does:
   `SUBSTRING` appears in the harnesses only in select lists and with constant
   arguments.

The three coverage holes are independent, and each one alone is sufficient to
let B3-1 through.

**What the invariant would need to be worth its reputation**, stated as
obligations rather than a patch: compare rows **in order** when the query has an
`ORDER BY`; include at least one **3-relation** shape (a self-join on
`catalog.json` reaches `MIN_ENUMERATED_RELATIONS`, or point a leg at
`data/tpch/sf0.01/catalog.json`); and include one query whose `ORDER BY` is
deliberately **not a total order** with a `LIMIT` that cuts inside the tie.
