# Week 32 — Round 4 audit (fix round responding to rounds 2 and 3)

Scope: the four tip commits 5dd74e4, 9b45466, 234ece8, 5c266e6, audited as new code.
Prior audits (week-32-round-2.md, week-32-round-3.md) read first; settled ground not re-reported.

Method: read-only. No build, no test run (a gate owns `build/`).

Tally: **0 blockers, 0 high, 1 medium, 2 low, 1 hunch.**

---

## Target 1 — the high-severity regression fix (`5dd74e4`)

`src/planner/subquery_materialization.cc:264-280`.

### Verified — no double materialization

The recursion is idempotent by the `has_subquery` guard, not by luck.
`materializeSubqueries` returns immediately at `:252` when
`stmt.has_subquery == false`, and the last statement of every successful pass is
`stmt.has_subquery = in_survives` (`:323`). A body whose only nested node is a
SCALAR/EXISTS therefore has the flag cleared on the first pass, so a second call
on the same `SelectStatement*` is a no-op — the body is never run twice and
`runOnce`'s per-call `ResultCache` (which is NOT shared across the recursion,
`:254`) cannot double-run it. Confirmed the flag is set per *directly* containing
block at bind time (`src/planner/binder.cc:247`, with `:261` recording that no
propagation is needed), so the guard does not swallow the recursion either. ✅

### Verified — no infinite recursion

Recursion is strictly downward: `materializeSubqueries(*sq->subquery, run)`
descends into a distinct `SelectStatement`. A cycle would need a body whose
`shared_ptr<SelectStatement>` reaches an ancestor; every `SelectStatement` in the
engine is allocated at parse time (`parser.cc:355/477/639`, per the note at
`subquery_materialization.cc:150-159`) and the parser builds strictly nested
blocks, so no `SubqueryExpr::subquery` can name an enclosing statement. Even a
self-referential shape smuggled in would terminate on the second visit via the
`has_subquery` clear above, since `in_survives` is recomputed from scratch each
call. ✅

### Verified — the restored recursion covers all three nestings

- **SCALAR inside an IN body.** `:277` recurses; the body's scalar is substituted
  by the normal `runOnce`/`buildReplacement` path. This is the shape the new test
  pins. ✅
- **EXISTS inside an IN body.** Same arm, same path — `visit` dispatches on
  `sq->kind` and only `Kind::IN` returns early, so an EXISTS in the body is
  materialized to a boolean literal exactly as it was pre-Week-32. ✅
- **IN inside an IN body.** The inner IN survives the recursive pass and sets the
  body's `has_subquery` to true. It is then lowered, not left dangling:
  `subquery_lowering.cc:25` plans the body with the *full* `LogicalPlanBuilder`,
  which runs `lowerInSubqueries` again on the body's own WHERE conjuncts. So the
  nested IN reaches a semi join rather than dispatch site 12. ✅

Coverage is therefore equal to the pre-Week-32 `runOnce` path for the body: the
same call, at the same "innermost first" point. Nothing else of `runOnce` (move,
limit cap, cache) is semantically owed to an IN body, since `planBody`
(`subquery_lowering.cc:20-26`) does the move itself and refuses a shared body via
`use_count()`.

### Verified — the new test is falsifiable

`tests/test_subquery.cc:258-292`. Deleting `:277` makes `EXPECT_EQ(runs, 1)`
read 0 and makes `asLiteral(body_pred->right.get())` return nullptr, tripping the
`ASSERT_NE(lit, nullptr)`. The test asserts the real nested shape (it reaches
through `sq->subquery->where` to the substituted literal `312.5`) rather than a
flattened scalar-outer stand-in, which is what the round-3 finding said the
previous version had degenerated into. ✅

### LOW-1 — the oracle entry added for this shape is `COUNT(*)`-shaped, which
### hides a row-set difference

`python_tools/compare_against_sqlite.py:550-559`:

```sql
SELECT COUNT(*) FROM drivers WHERE driver_id IN
  (SELECT driver_id FROM laps WHERE speed > (SELECT AVG(speed) FROM laps))
```

The C++ test proves the *substitution*; the oracle query is what proves the
*answer*. As written it compares a single scalar, so any semi-join defect that
preserves cardinality while returning the wrong `drivers` (e.g. an off-by-one in
the nested threshold, or the semi join matching on the wrong column) diffs
identically against SQLite. The sibling entry directly above it
(`:548-549`) has the same shape. Concrete blind spot: if the nested scalar were
substituted with `MIN(speed)` instead of `AVG(speed)` on a dataset where both
thresholds admit the same number of distinct `driver_id`s, this entry passes.
Selecting `name` and ordering would close it at no cost.

---

## Target 2 — `collectSlotTables` and `semantics == STANDARD` (`9b45466`)

`src/planner/vectorized_plan_builder.cc:117-145`.

### The "latent, not live" claim is CORRECT — but the commit's stated reason is
### not the one that makes it true

The commit and the `development.md` row justify "latent" with *"the widths
computed there are discarded before `setCostDecision`"*. That is the weaker half
and I could not confirm it: `rowWidth` (`:151-190`) feeds the Week 22 cost block,
which is exactly where the build-side decision is made.

The claim survives on the **second** reason, which is airtight: `rowWidth`'s only
consumption of the map is `slot_tables.find(col.relation_slot)` over
`child->output_schema.columns()` (`:181`). A semi/anti join's `output_schema` is
its **left child's** (asserted at `subquery_lowering.cc:74-86`), and the left
child is the outer spine, whose columns carry outer binder slots ≥ 0. No column
anywhere in that schema carries `-1`, so the pre-fix `out[-1] = <body table>`
entry was written and never read. The computed width is therefore **bit-identical
before and after the fix** — which is why no behavioural test can distinguish
them, and why this is a contract repair rather than a corrected cost decision.

So: **not a wrong cost decision.** But the reason recorded in
`development.md:738` and in the code comment at `:126-129` is the one that does
not hold up; the reason that does (no spine column carries slot `-1`) is stated
second in both, as a parenthetical. Worth swapping, since the next reader
inherits the weak argument.

### Verified — the fix loses no entry that was previously recorded

Walked the three reachable shapes:
- semi over a scan: `isSingleRelation(left)` is true, so
  `out[output_schema.column(0).relation_slot] = leafScanTable(left)` still fires
  and stamps slot 0 correctly (a semi join's output schema IS the scan's).
- semi over semi over a scan: `isSingleRelation` returns false on any JOIN
  (`:55-62`), so the walk recurses to the inner semi and lands on the scan.
- semi over a STANDARD 2-way join: the recursion reaches the standard join, which
  still stamps both `out[join_slot]` and the merged column-0 slot.
✅ in all three.

---

## Target 3 — `build_had_null_key_` → `build_had_unmatchable_key_` (`9b45466` era)

`src/execution/vec_hash_join_node.cc:84/118/218/239`,
`src/execution/key_encoding.h:88-91`.

### Verified — the probe logic itself is correct for `NOT IN`

Hand-checked the four-way table at `:236-241`:
`hit != (semantics_ == SEMI)` gives emit/skip correctly for SEMI-hit, SEMI-miss,
ANTI-hit, ANTI-miss. The NULL-probe-key arm (`:238-246`) emits only for
`ANTI && build_keys_.empty()`, which is `NULL NOT IN ()` → TRUE — the one case
SQL says survives. The `&& !build_had_unmatchable_key_` conjunct on `:239` is
unreachable-but-harmless: `:218` already `continue`d the outer loop for
`ANTI && build_had_unmatchable_key_`. ✅

### LOW-2 — the justification for collapsing NaN into NULL is wrong for the CSV
### path, though no shipped dataset reaches it

The collapse is defended at `vec_hash_join_node.cc:110-118` and
`key_encoding.h:86-87` with: *"SQLite never has the case at all (it stores NaN as
NULL), so dropping agrees with it too."* That is true for a **computed** NaN
(SQLite's `0.0/0.0` is NULL, and a bound NaN is stored as NULL), and I confirmed
the computed path really does agree end to end: SwiftQL short-circuits ANTI,
SQLite gets a NULL in the set and yields UNKNOWN — both return zero rows.

It is **not** true for a NaN that arrives through the loader.
`src/storage/csv_loader.cc:58` parses a DOUBLE field with `std::stod`, which
accepts the literal text `nan`, `NaN` and `infinity`. Concrete input — a CSV with
a DOUBLE column holding the cell `nan`:

- SwiftQL: `Value(NaN)` → `isUnmatchableKey` true (`key_encoding.h:90`) →
  `build_had_unmatchable_key_ = true` → `SELECT * FROM a WHERE x NOT IN
  (SELECT d FROM t)` emits **zero rows**.
- SQLite importing the same CSV: `nan` is not numeric text, so REAL affinity
  leaves it as the TEXT value `'nan'`, which is **not NULL**. `3.0 NOT IN (1.0,
  'nan')` is TRUE (a number never equals text), so SQLite emits **the row**.

Not reachable on committed data — `grep -li nan data/*.csv` returns nothing — so
this is a documentation defect, not a live wrong answer: the README limitation
and the two code comments claim an agreement with SQLite that only holds for
computed NaNs. Round 2's LOW-3 flagged the relational divergence (`3.0 NOT IN
{1.0, NaN}` is relationally TRUE); what is new here is that the *reason given for
dismissing it* is false on the one path by which a NaN can actually enter a
column.

---

## Target 4 — the deleted test (`NoLongerRefusesALargeInSet`)

`tests/test_subquery.cc:185-217` (the fold) vs the deleted body.

### Verified — no assertion was lost

The deleted test asserted exactly one thing: `EXPECT_NO_THROW(
materializeSubqueries(stmt, canned({oneCol("driver_id", TypeId::INT), {}})))`.
Both halves survive the fold:

- **No-throw** is still asserted, implicitly but really — GoogleTest fails a test
  on an escaped exception, and `LeavesAnInNodeForSemiJoinLowering` calls
  `materializeSubqueries` unguarded at `:203`. A reinstated cap that threw would
  fail the folded test.
- **The set size** is strictly strengthened, not weakened: the deleted test fed
  **zero** rows (below Week 31's 1024 cap, so it could not have caught the cap
  even if it had been live); the fold feeds **4096** (`:194-195`), above it.
- The `IN` query it used is byte-identical to the first of the two the fold
  iterates (`:196-197`), and the fold additionally covers `NOT IN`.

The one property genuinely not asserted at this layer is that a large set
*produces the right answer* — but that was never in the deleted test either, and
it is pinned in the oracle (`compare_against_sqlite.py`, the 10 000-distinct-value
query). ✅ Nothing narrowed.

---

## Target 5 — other risk in the fix round

### MEDIUM-1 — the `Kind::IN` arm recurses without consulting `runOnce`'s cache,
### so a body shared by two `IN` nodes is materialized twice

`src/planner/subquery_materialization.cc:277`.

`cloneExpr` shares the `shared_ptr<SelectStatement>` rather than deep-copying
(`:132-136`), so two `SubqueryExpr` nodes over one body is a state the engine
already reaches (the file's own example: `BETWEEN` clones its left operand). The
non-IN path guards this with the statement-keyed `ResultCache` at `:164`. The new
IN arm calls `materializeSubqueries(*sq->subquery, run)` **unconditionally**,
outside that cache.

Why it is MEDIUM and not a blocker: the second pass is a no-op *by the
`has_subquery` clear*, so no body is re-run and no answer changes today — and any
shape that reaches here is refused a few steps later anyway, by
`planBody`'s `use_count() > 1` check (`subquery_lowering.cc:21-24`). But that
refusal is in a **different file**, and the safety of `:277` currently depends on
it plus the flag clear, neither of which is named at the call site. The comment
at `:269-271` asserts "the body is NOT moved, NOT limit-capped and NOT cached"
and treats not-cached as free; it is free only because of a guard elsewhere. If a
future week's body legitimately survives with `has_subquery` still set (a nested
IN does exactly that — `:323` leaves the body's flag true), a second visit
re-enters and re-walks it. Concrete state: an IN body containing its own IN,
reached twice — the outer pass recurses, the body's flag stays true, and a second
visit recurses again. Today no walker produces a second visit; the invariant that
prevents it is unstated. Smallest fix: consult `cache` (a `count()` on
`sq->subquery.get()` guard) or state the dependency in the comment.

### HUNCH — `refuseUnloweredIn` is not applied to an IN body's own clauses

`src/planner/subquery_lowering.cc:98-107`. The tripwire is called on the leftover
WHERE conjunction and on HAVING of the block being planned. Because `planBody`
re-enters `LogicalPlanBuilder::build`, the body's own clauses should get the same
treatment on the recursive call — but I did not read `LogicalPlanBuilder::build`
to confirm the tripwire is invoked on every entry rather than once at the top.
If it is invoked once at the top level only, an IN nested in an IN body's HAVING
would reach dispatch site 12 with the wrong diagnostic. Not grounded — flagged as
a hunch for the next round.

---

## Not reached

- `LogicalPlanBuilder::build`'s call sites for `refuseUnloweredIn` (the hunch
  above).
- The Volcano `HashJoinNode` side of the NULL/NaN rule. `grep` shows
  `build_had_unmatchable_key_` exists **only** in `vec_hash_join_node.{h,cc}`,
  which is consistent with `WEEK32_SEMI_JOIN_VOLCANO_REJECTED` refusing semi
  joins on that path, but I did not verify the refusal is total.
- `setCostDecision`'s consumption of `rowWidth` (see Target 2) — not needed for
  the verdict, but it is why half the commit's stated reasoning is unconfirmed.
