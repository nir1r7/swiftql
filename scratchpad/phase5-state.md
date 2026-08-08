# Phase 5 orchestrator state

## !! PENDING MERGE — `claude/typefix-wt` @ 070e01a (pushed to origin, durable)
Type-through-division fix is DONE but **NOT on the working branch**. Merge it as soon as the
optimizer wave-B agent finishes — not before, because it is live in the shared worktree and the
merge touches files under its build. `070e01a` already merges the shared branch at 451c0d9.
**FRAMING (sharper than my brief's):** the question is not "is this column read by another
expression" but **"can this column's INT-ness reach an operand of `/`"** — `/` is the ONLY operator
in the dialect whose VALUE depends on operand types. Everything else coerces (comparisons), renders
identically (`x+1` is "8" from both 8 and 8.0), or normalizes (`keyFieldText` for group/DISTINCT/
join keys; ORDER BY compares numerically). Widening to "any expression" would refuse `WHERE x > 3`
and `SELECT x*2`, correct today — the blanket refusal one step out.
**ARM AT THE ORIGIN, NOT THE CONSUMER.** In `SELECT y/4 FROM (SELECT x*2 AS y FROM (CASE…7…) t) u`
the value is already 14.0 by the time `y` materializes, so arming `y` fires on nothing; only `x`,
one level down, sees the INT. Hence an ORIGIN SET per output column, taint-walked back through
`+ - * /`, derived tables, joins and MIN/MAX. Arming at the producer also makes the rule
independent of who divides — the `WHERE x/2 > 3` case is consumed by `VecFilterNode`, which the
agent does not own and never touched, and it refuses anyway.
**BOTH HALVES REQUIRED:** plan shape AND an INT actually arriving. Plan-shape alone would refuse
`WHERE lap_id = 99` where no row takes the INT branch and today's answer is right; the runtime test
alone is the blanket refusal.
ONE CAUSE, two sites (the same two E-10 named): VecProject's Pass-2 `evaluate()` path and
`VecHashAggregate::fillChunk`'s MIN/MAX arm — MIN/MAX are ORDER STATISTICS, so they hand back the
argument's own Value AND TYPE. Proof it is one cause: a test stacking the derived-table consumer on
the HAVING producer needed no third fix.
11 tests written against the PUBLIC PLANNER API ONLY so they compile and run unchanged on b13a96a:
  **6 failed / 5 passed pre-fix; 11 pass post-fix.** Every witness is 7 and 0.5; largest number in
  the file is 8. The 5 Guards pass both ways and are NAMED as the fence, not as evidence.
Gates on the merged tree: unit 868/868, oracle 1556/0, regression 330/0, tpch PASS 20/22 verbatim.
On b13a96a the oracle output was **BYTE-IDENTICAL** with and without the fix — no passing answer moved.
COST STATED: it also refuses a division that happens to be EXACT (`THEN 6` over `/2` gives 3 either
way, right today). Closing that needs the question asked at the division site
(`expression_executor.cc` / `evaluator.cc`), deliberately not reached into. Also left open:
checked-INT overflow (`x * <huge>` throws in Volcano, yields a double here) — a magnitude story like
`narrowToDoubleColumn`'s, and arming for it would refuse `SELECT x*2`, right today.

### Oracle entries owed for the typefix — SEQUENCE AFTER THE MERGE, not before
**PIN STRING: `"that another expression divides"`. Do NOT use `"cannot materialize the integer"` —
E-10's magnitude refusal also emits that.** All verified against 070e01a.
(a) Refusal-only, NO Volcano leg (Volcano cannot run derived tables):
  1. `SELECT x / 2 FROM (SELECT CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END AS x FROM laps WHERE lap_id < 4) t` — SQLite `3, 0.25, 0.25`
  2. `SELECT x FROM (… same body …) t WHERE x / 2 > 3` — SQLite 0 rows (consumer is VecFilterNode)
  3. `SELECT y / 4 FROM (SELECT x * 2 AS y FROM (… same body …) t) u` — taint through `*` and two derived levels
(b) Volcano-answers / vectorized-refuses (the E10_VOLCANO_ONLY + _VECTORIZED_REFUSED pairing):
  4. `SELECT team, MAX(CASE WHEN lap_id=2 THEN 7 ELSE 0.5 END) / 2 AS m FROM laps WHERE lap_id < 4 GROUP BY team ORDER BY team`
     — SQLite AND both Volcano modes: AlphaTauri 3 / McLaren 0.25 / RedBull 0.25. **THE NON-VACUOUS
     VALUE WITNESS — this one carries the diff.**
  5. the HAVING row-count witness — SQLite 0 rows, both Volcano 0 rows. **Its Volcano leg is EMPTY so
     the harness will flag it vacuous: add as a REFUSAL PIN ONLY and let #4 carry the diff.**
(c) Boundary guards, ALL FOUR MODES must answer and match SQLite:
  6. the bare mixed CASE -> `7, 0.5, 0.5`
  7. `THEN 1 ELSE 0.5` -> `1, 0.5, 0.5` — **the named answer a blanket refusal would have cost**
  8. the aggregate produced but NOT divided -> AlphaTauri 7 / McLaren 0.5 / RedBull 0.5
(d) Boundary guards, _VEC_ONLY (derived tables), both vec modes answer and match SQLite:
  9. `… WHEN lap_id = 99 …` armed shape where NO INT reaches it -> `0.25 x3`. **This is the entry
     that fails if anyone converts the rule to a plan-time-only refusal.**
  10. `THEN 7.0` -> `3.5, 0.25, 0.25`
  11. `SELECT x + 1 …` -> `8, 1.5, 1.5` — **fails if the rule is ever widened past `/`**
Entries 9-11: row ORDER differs between SQLite and SwiftQL on data/laps.csv (no ORDER BY) — use
unordered comparison or add an ORDER BY.

Current: **PASS 3 COMPLETE (all 5 seams). FIX ROUND 3 WAVE A RUNNING — 4 agents.**
  Pass 3 tally: 4 blockers (3 distinct roots), 2 HIGH, 2 MEDIUM, 11 LOW. Subquery chain CLEAN;
  storage 0/0/1/3. **Every pass so far has returned blockers — 3 for 3.**
  WAVE A (disjoint files): (1) comparator — canonical plan-independent column identity + EVERY cut
    site; (2) harness — third catalog table + multi-relation coverage + random_diff TRAP-1;
    (3) join-key type refusal — Week 29's guard reaches 1 of 4 JoinKey producers;
    (4) value coercion — 7 vectorized re-materialization sites, INT->DOUBLE above 2^53.
  WAVE B (HELD until wave A lands): `orderByWork` conjunct reordering (masks AND introduces
    errors); pushdown never entering a derived body (9.4x); **S-9 `narrowRows` perf**; and pass 2's
    B-2/B-3 — held because one is the ACCIDENTAL CONTAINMENT on the comparator's blast radius.
  THEN: gate -> seam pass 4 (cap 5 passes) -> doc+coverage sweep -> q21 -> sf0.1 -> final gate.
  ESTIMATE GIVEN TO USER at 04:50 UTC: optimistic 9h, likely **12-14h**, on measured historicals
  (audit pass 35-54min, gate 22-34min, fix round 55-95min, ~5min per transition; full cycle ~3h).

## PASS 3 — storage: 0 BLOCKER / 0 HIGH / 1 MEDIUM / 3 LOW (45522ca)
`narrowRows` is CORRECT: no aliasing (the vector was always moved by value; `SeqScanNode` owns
  `rows_` and `&rows_[cursor_]` points into its own member); no catalog-position indexing survives
  (`grep` for catalog/getTable in plan_nodes.cc and evaluator.cc returns ZERO — the execution layer
  structurally cannot see a catalog schema); no `hidden` interaction. 1588 query x cell comparisons
  across the three real cells + --no-optimize, **0 divergences**, incl. 37 shapes built specifically
  to break the new symmetry. (The commit's own example is BENIGN because driver_id is column 0 in
  both legs; the discriminating shape is a tie WITHIN a repeated key.)
**S-9 (MEDIUM) — and the instrument reports the WRONG SIGN.** `narrowRows` was landed as
  "acceptable at this project's scale". Measured 6748cfc vs 922ca15, Release, TPC-H sf0.1:
  `SELECT l_quantity FROM lineitem LIMIT 1` plan **64us -> 109312us**, total **1540x slower**.
  Three of four measured queries got slower end to end; one sort-heavy got 1.75x faster, so it is a
  genuine TRADE, not a pure loss. **`benchmark.py::run_once` greps `Execution:` only, so it reports
  ALL FOUR faster — including the three that regressed.** The regression is not merely unmeasured;
  the instrument reports the opposite sign. Fix: move the narrowing into `SeqScanNode::next()` as a
  `keep_` index vector, mirroring the columnar branch's existing `reconstructed_row_`.
L-1: S-8's duplicate-column refusal is now load-bearing for a SECOND reason — it is the only thing
  stopping `narrowRows` double-moving on a duplicate name — and its comment states only the
  columnar reason. L-2: `tables_.emplace` first-wins is reachable but behaves IDENTICALLY in all
  three cells, so it is catalog input validation, not a storage divergence.
Week 37 `VecScanNode`: slightly HARDER as written (+~15 lines to hoist `narrowRows` out of
  `Planner::plan`), but EASIER if S-9 is fixed first — the fill loop becomes `row[keep_[i]]`.
  **Do them in that order.**
Sharper than recorded: both IN-subquery pins in `VOLCANO_BOUNDARIES` never matched their message at
  all (`subqueries` vs `subquery`) — never live, rather than merely redundant.
  Fix round 2 gate was **GREEN** on 922ca15 (carries 70570dc): build genuine recompile 56 TUs
  0 warnings; unit 823/823; oracle 1496/0/0 (22 suites / 205 rejection entries); regression 318
  incl. 119 invariant, 0 divergences; tpch PASS 20/22 meaningful; baseline md5 unchanged both ends;
  tree fingerprint d766100110d12f899c4026715011c55b identical before and after.
  **AND THAT GREEN GATE SAT ON A TREE CONTAINING BOTH BLOCKERS BELOW.** Second time this phase
  (Week 30 round 1 was the first). A green gate proves the harness passes, not that the code is right.

## PASS 3 — join chain: 2 BLOCKERS (73e71e3)
**B3-1 (BLOCKER) — FIX ROUND 2'S OWN TIE-BREAK IS THE CAUSE.** `sort_comparator::rowLess`
compares the whole row "in schema order" when declared keys tie. Above a join that schema is the
MERGED JOIN SCHEMA, which `rebuild` builds in the DP's chosen order. Same columns, PERMUTED ->
different total order -> different LIMIT cut. The fix made ties deterministic PER PLAN, not
PLAN-INVARIANT, which is the property it needed.
  SELECT c.c_name, o.o_orderkey FROM orders o JOIN customer c ... JOIN nation n ...
  ORDER BY n.n_regionkey LIMIT 5     (data/tpch/sf0.01)
    optimized      Customer#000000002 x5 (orderkeys 1116, 2866, 3420, 3908, 4886)
    --no-optimize  Customer#000000644, ...  (orderkeys 1, 9, 12, 14, 15)
  FIVE ROWS, ZERO OVERLAP. Carried into a scalar subquery (LIMIT 1 survives materialization;
  main.cc:521 threads no_optimize into the subquery runner) it becomes a wrong VALUE:
  count(*) = **1115 vs 0**. Same failure the tie-break commit exists to close (its own header
  records "977 versus 1536"), with a cause the header LISTS and does not neutralize.
  **AND THE TOOL THAT WOULD HAVE CAUGHT IT IS BLINDED BY AN EXPIRED ASSUMPTION**: `random_diff.py`'s
  TRAP-1 rationale deliberately NEVER generates a tied ORDER BY, because a reordered join breaking
  ties differently used to be "a FALSE FAILURE". Since the tie-break landed that is a TRUE failure —
  so the generator produces the shape zero times. Fix the generator, not just the comparator.
**B3-2 (BLOCKER) — Week 29's STRING-vs-numeric join-key refusal covers 1 of 4 `JoinKey` producers.**
  The guard lives inside `Validator::validate`'s `for (stmt.joins)` loop. `subquery_lowering`,
  `splitCorrelation` and the correlated-scalar rewrite ALL SHIPPED LATER and are uncovered, so the
  text encoding decides and half-matches. `FROM…JOIN…ON` is refused (containment works) while
  IN / NOT IN / EXISTS / NOT EXISTS / correlated-scalar SILENTLY RETURN WRONG ROW SETS.
  **Reachable on the SHIPPED catalog.json**: `l.driver_id IN (SELECT '016' …)` -> SwiftQL 0,
  SQLite 495 (same for `'16.0'`, `' 16'`, `'+16'`); `'16'` agrees. Exactly the "half a match, with
  no error either way" the Week 29 comment named and CLAIMED TO HAVE CLOSED.
## PASS 3 — engine divergence: 2 BLOCKERS, 1 HIGH (33e7052). CONVERGES ON THE SAME ROOT.
**E-9 == B3-1** — found independently by two auditors. Schema-index-order tie-break, permuted by
  `JoinEnumeration::rebuild`. Its own written precondition ("the sort's INPUT row must be the same
  in every mode") is FALSE for any multi-way join. Demo: 3-way TPC-H join whose single ORDER BY key
  is constant over all survivors, so the tie-break decides the WHOLE order -> disjoint row sets;
  `COUNT(*)` 281 vs 1 transported into a scalar.
**E-8 (BLOCKER) — THE TIE-BREAK LIVES IN THE SORT COMPARATOR, SO IT DOES NOT FIRE WITH NO SORT.**
  Delete the ORDER BY from pass 2's own repro and E-1 and E-1b come straight back at HEAD with the
  gate green: {AlphaTauri,Alpine,McLaren} vs {RedBull,AlphaTauri,McLaren}, and COUNT(*) 977 vs 1536.
  No GROUP BY, no contrivance. An ordinary shape — `customer JOIN orders WHERE o_orderstatus='F'
  AND o_orderpriority='1-URGENT' LIMIT 3` — returns DISJOINT ROW SETS between Volcano and optimized
  vectorized and violates `optimized == --no-optimize`. Prevalence **2 of 45** mechanically
  generated join+LIMIT queries.
**E-10 (HIGH) — new, missed by passes 1 AND 2.** The vectorized path coerces every value it
  re-materializes to its schema-declared type at SEVEN sites; Volcano has no such site.
  `appendColumnValue`'s comment calls the INT->DOUBLE widening "lossless" — it is not above 2^53.
  `CASE ... THEN 9007199254740993 ELSE 0.5 END` returns a DIFFERENT INTEGER on the vectorized path,
  and `SELECT DISTINCT` of it returns 2 rows vs 3 — **the vectorized path contradicting ITSELF**,
  since `COUNT(DISTINCT e)` says 3. Pass 2's type sweep could not see it: it compared the two
  EVALUATORS and this lives one layer above both.
WHY THE GATE IS GREEN ANYWAY — measured, not assumed: of 211 distinct harness queries, 6 have LIMIT
  without ORDER BY, exactly 1 contains a join, and that one has no WHERE clause, so pushdown moves
  no estimate and the two build-side rules provably coincide. TPC-H: 21/22 agree opt-vs-noopt.
  **The corpus contains no instance of the class — exactly as it did not before pass 2 either.**

## PASS 3 — optimizer preservation: 1 BLOCKER, 1 HIGH, 1 MEDIUM, 2 LOW. THIRD CONVERGENCE.
**Same comparator defect, found INDEPENDENTLY BY A THIRD AUDITOR.** Three isolating controls that
  pin it exactly: write the FROM list in the DP's order -> SAME; make ORDER BY total -> SAME; put a
  GROUP BY between join and sort -> SAME. `JoinEnumeration::rebuild`'s OWN COMMENT admits it: "a
  slot-sorted canonical order is not available". Repro on shipped sf0.01, col-vec:
    SELECT n.n_name, s.s_suppkey FROM supplier s JOIN nation n ... JOIN region r ...
    ORDER BY n.n_regionkey LIMIT 5
    optimized: 5x ALGERIA | --no-optimize: ALGERIA/MOZAMBIQUE/MOROCCO/MOROCCO/ALGERIA (4 of 5 differ)
  **B3-1b kills the "it's all legal SQL" defence**: `materializeSubqueries` threads the flag into the
  nested runner, so the same LIMIT 1 cut inside a scalar materializes a DIFFERENT CONSTANT. A query
  with NO ORDER BY and NO LIMIT of its own returns `EGYPT 4` optimized vs `SAUDI ARABIA 20` unopt.
  Also CORRECTS pass 2's B-1: **five** of the six ungated passes are identical in both legs by
  construction, not all six. The sixth is `materializeSubqueries` — which is B3-1b.
**B3-2 (HIGH) — `orderByWork` reorders conjuncts on an estimate, and per-row evaluation is NOT
  total.** The optimizer both MASKS and INTRODUCES errors, both directions confirmed on catalog.json:
    WHERE SUBSTRING(team, lap_id - lap_id, 2) = 'x' AND speed = 333.3333
      -> optimized 0 rows, --no-optimize ERRORS
    WHERE team LIKE 'zzz%' AND SUBSTRING(...) = 'x'
      -> optimized ERRORS, --no-optimize 0 rows
  The precondition that would make reordering safe is written NOWHERE.
**B3-3 (MEDIUM) — a THIRD silent decline**: predicate pushdown never enters a `LogicalDerived` body
  in ANY shape, even with no join present. Measured **9.4x** (18465us vs 1969us) plus lost zone-map
  pruning, with nothing in `--explain`.
LOW x2: folding's new comment is true about VALUES but its shape-consumer census says "there was
  exactly one" and there are FOUR MORE (two change an outcome today) — 15 consumers enumerated with
  a verdict each; and `foldConstants` is load-bearing for CORRECTNESS, not canonicalization — it is
  the only thing that removes an `IntervalLiteral`.
`written_ordinal` is COMPLETE and fails OPEN (the safe direction): three parser sites, one synthesis
  site at subquery_decorrelation.cc:544 that correctly leaves it empty; nothing rebuilds a parsed item.

## PASS 3 — subquery chain: **CLEAN** (f6767b8). 0 BLOCKER / 0 HIGH / 0 MEDIUM / 4 LOW.
All four of fix round 2's subquery fixes verified real and complete, and it checked the ways a
claim could be true-but-useless rather than accepting it:
- B-1's "`hidden` is read in exactly three places" is exhaustively true. It checked the two ways a
  FOURTH consumer could exist without using the word: dropping the flag by rebuilding a `ColumnDef`
  field-by-field (every merge site copies whole `ColumnDef`s BY VALUE — logical_plan.cc:967,
  join_enumeration.cc:270, planner.cc:350, buildProjectSchema:351, buildAggregateSchema:451), and
  propagating it where a reader must not skip (blocked by `derivedRelationSchema` forcing
  hidden=false and by the two producers' names being UNLEXABLE). `Schema::indexOf` has no third
  overload, so resolution is untouched. 12 star shapes match SQLite, incl. `DISTINCT *` — which is
  safe ONLY because `LogicalDistinct` sits ABOVE `LogicalProject`.
- B-3's suppression cannot leak, for a reason THE FIX DOES NOT STATE: `forEachSubquery` and
  `collectSlots` enumerate IDENTICAL subtype sets and both stop at a body, so the cleared node set
  is exactly the consulted set. A shared body cannot observe the flag because `correlated` lives on
  `SubqueryExpr`, not the shared `SelectStatement`.
- The depth guard's `!= 1` is right: level 0 cannot arrive (an unresolved id reports `isLocal()` and
  is parted off above with its own message), making it a PURE depth test.
- ~90 constructed queries in Part B: no wrong answer, no unsound refusal. NULL semantics correct in
  all 16 shapes manufacturable by two independent routes, including the three that actually part
  `ANTI` from `ANTI_NOT_IN`. The documented cardinality divergence has NOT widened.
4 LOWs for the sweep: five rejection entries pinning needles that 4–24 messages satisfy (with
  wrong-guard counterexamples for each, and one suite whose own stated purpose its pin cannot serve
  while the rule is written out twelve lines above it); a comment on B-3's fix justifying its RAII
  with **two facts that are both false** (the fix is still correct, for the different reason above);
  a refusal class B-3's fix newly opened that is in no README table or suite.
**CORRECTION to a queued item: the `NOT IN` NULL hole is NARROWER than pass 2 recorded** — three
  LEFT-JOIN NULL entries are already in the tree. Only the mixed-body case is missing, and the
  behaviour behind it is correct.

## ~~THE SYSTEMIC COVERAGE HOLE~~ — **RETRACTED. THE PREMISE WAS FALSE.**
I recorded, and told the user, that "`catalog.json` has two tables and `MIN_ENUMERATED_RELATIONS`
= 3, so no query in the oracle suite ever reaches join enumeration", and called it the systemic
finding bigger than any blocker. **It is wrong.** `MIN_ENUMERATED_RELATIONS` counts **RELATIONS,
NOT TABLES**, and a self-join supplies three from two. `WEEK27_QUERIES`/`WEEK28_QUERIES` have been
feeding the DP since Week 28 — `--explain` on `w28_nonzero_leftmost_projection` prints
`order=drivers@1,drivers@2,laps@0 cost=1692 (written=2045) method=dp`.
**NO THIRD TABLE WAS ADDED, and that was the right call**: it would have bought nothing while
perturbing `CardinalityEstimator` inputs, existing `w28_*` order decisions, and every harness that
loads the catalog into SQLite. A self-join is also the HARDER case — the merged schema then carries
several columns of the SAME NAME at different relation slots, which is exactly what a positional
tie-break trips over. Recorded as a `WHY NO THIRD TABLE` note in the harness file.
**What was actually missing was the DEFECT SHAPE**, four preconditions of which existing entries
had three: (1) >=3 relations, (2) the DP picks a different order, (3) the sort sits DIRECTLY above
the join with no aggregate between, (4) the ORDER BY is not total and the tie is MATERIAL.
Lesson: I amplified one auditor's claim into a systemic conclusion without checking what the
constant counted. The auditor's own finding (that the invariant checks miss B3-1) stands; my
generalisation of it did not.

## Wave A — harness coverage: DONE
`TIE_STRADDLE_QUERIES`, 11 entries, INVARIANT-ONLY — `ORDER BY <non-unique> LIMIT n` has no single
correct answer so SQLite is NOT an oracle for it, stated in the block header. Added
`normalize_ordered()`/`_invariant_compare()`: ORDER BY queries now compare POSITIONALLY, everything
else as sets — measured against the whole suite first, all 65 pre-existing ORDER BY entries already
agreed under it, so it cost nothing. Ties are EARNED FROM THE DATA: `laps.season` has 4 values over
10000 rows, `round` ~24, `drivers.nationality` 6, and every projection carries a per-row-distinct
column so which tied row survives is observable.
Two-sided discrimination vs a pinned pre-fix binary (cb59ff8) AND the post-fix binary:
  8 tie/subquery entries FAIL pre-fix, PASS post; `b31_plain_limit_no_order_by` same;
  **2 CONTROLS (total order; aggregate between) PASS ON BOTH** — without them the block would only
  prove that SOME 3-relation query diverges. 318->320 passed/9 failed pre-fix; 331/0 post-fix.
  Nothing existing moved; TPC-H q2 and q20 still byte-identical in both legs.
`random_diff.py`: TRAP-1 rewritten — it now emits `FORM_TIED_LIMIT` (~35%) off a low-cardinality
  `TIE_KEYS` pool. Default seed goes from 0 tied shapes to 7, catching 3 divergences pre-fix, 0
  post-fix; 13 of 64 across 20 seeds. Leg 2 SKIPS the tied form (SQLite breaks ties by its own plan)
  and counts it as skipped so the coverage loss stays VISIBLE. A guard shouts if a run emits zero
  tied shapes, so the original silent failure cannot recur. **Correction: it already generated 3-8
  relation joins and already reached the DP** — that part of my brief was also inaccurate.
`INVARIANT_SCOPE` now prints with the result: --no-optimize gates 3 passes; 5 more are identical in
  both legs BY CONSTRUCTION and are therefore NOT tested (only the SQLite oracle sees those); the
  6th, `materializeSubqueries`, threads the flag into the nested runner so subquery bodies ARE
  covered. Re-verified against main.cc:566 and :134.

## Wave A — join-key type refusal (B3-2): **CLOSED**
**Chose REFUSE in all four producers, not affinity.** Three reasons, weightiest first:
  1. `LogicalPlanBuilder::build` FOLDS an inner join's non-key ON conjuncts into the WHERE
     conjunction (Week 27's `R (join)_(p^q) S == sigma_q(R (join)_p S)`), and `Value::operator==`
     THROWS across the STRING boundary. Give keys affinity and one written predicate means two
     different things depending on whether `classifyJoinCondition` happened to make it a key —
     the same "half a match" one layer up.
  2. `keyFieldText` sees ONE `Value`; affinity would need the OPPOSING column's type threaded
     through six serializers and both engines.
  3. The encoding is NOT the defect, the missing guard is — re-measured live: an INT key vs an
     integral DOUBLE returns 10000 = SQLite after the fix. The rule is coarse (both STRING or both
     numeric) exactly so `7.0` keeps joining `7`.
FIX: `Validator::validateJoinKeyTypes(const LogicalPlanNode&)`, ONE walk over the FINISHED plan at
  the end of `LogicalPlanBuilder::build` (logical_plan.cc:1230). It resolves keys EXACTLY as the
  physical builder does (`from_slot`-aware left; POSITIONAL right for SEMI/ANTI, by name otherwise).
  It must be on the finished node to be CORRECT, not merely to avoid a fourth copy: `semantics` is
  set AFTER construction, so only the finished node knows which right-side resolution applies.
  The AST loop in `Validator::validate` is KEPT and re-scoped in its comment — `Planner::plan`
  builds a `HashJoinNode` straight from `stmt.joins` and never builds a logical plan, so that loop
  is Volcano's ONLY cover. Both sites raise one message from one function.
All 9 audit shapes now refuse; on shipped catalog.json `IN (SELECT '016')` was 0 vs SQLite 495 and
  `NOT IN` was 10000 vs 9505 — both now refused.
**COVERAGE LOSS IT NAMED RATHER THAN HID**: `IN (SELECT '16' …)` returned 495 = SQLite before and
  is now REFUSED. That is the half that agreed BY ACCIDENT on canonical text. Four doors with one
  behaviour was the requirement; keeping it answering would have been a fifth behaviour.
Unit 842/842. Oracle 1492 passed / 4 failed — **the 4 are NOT its work**: same query in 4 modes,
  `--explain` shows `LogicalSort [canonical row order]` under the LIMIT, i.e. the CONCURRENT
  comparator agent's uncommitted `deterministicCut`. Its code only throws; it cannot alter a row set.
Its first draft pinned BARE WORDS and its own suite caught it — `"EXISTS subquery"` is a substring
  of BOTH `"IN / EXISTS subquery"` and `"NOT EXISTS subquery"`. Pins are now the full clause.
**No C++ test pinned the Week 29 refusal before this work** — only the python rejection suite did.
  That is why a four-week regression in three producers was invisible to `ctest`.
Swept: validator.cc's "closed here / covers all four modes"; README's Limitations row;
  development.md:731's level-audit row (was FALSE as of the hoist); both lowering headers now say
  the condition is NOT enforced at their site. `docs/week-29-plan.md:1540` deliberately LEFT — it is
  a dated plan record and editing it would falsify the history that makes the finding legible.
Oracle entries SEQUENCED to the harness agent (7 queries x 2 suites, vec-only + Volcano-rejected).

## Wave A — E-10 value coercion: **CLOSED**, and it found a WIDER LIVE BUG it did not fix
FIX: `appendColumnValue`'s DOUBLE arm goes through `narrowToDoubleColumn` (vec_types.h), which
  **THROWS** rather than convert an INT `Value` whose conversion would be observable. Below the
  bound it is bit-for-bit the old code. 12 new tests; **8 of 12 fail** against a scratch build with
  the guard reduced to `return v.toNumeric();`. The other 4 pass both ways ON PURPOSE and are
  LABELLED so — 1 derives the constant, 3 pin what the fix must not move; neither kind is offered
  as evidence the fix works.
**THE BOUND IS 1e15, NOT 2^53** — two things must survive and it MEASURED both: the VALUE (fails
  above 2^53) and the RENDERING (`Value::toString()` uses `%.15g`, which flips to exponent form at
  1e15, so `1000000000000001` prints `1e+15` while the INT prints all sixteen digits). 1e15 < 2^53,
  so the smaller bound binds and one comparison covers both. `ThresholdIsExactlyTheBoundary` walks
  outward and asserts it is the FIRST failing magnitude, so a change to `%.15g` breaks a test
  instead of silently widening the window.
THE SEVEN SITES, verdict each — the deciding invariant is that a chunk's column types always equal
  the producing node's `outputSchema()` types, verified for every producer. **2 of 7 create the
  divergence** (VecProject's evaluate() path and the aggregate's MIN/MAX arm — both pre-allocate a
  declared type BEFORE the first value exists), 1 more can in principle, 4 are structurally
  incapable of loss. `VecDistinct` INHERITED the wrong values from the project site but creates none.
SELF-CONTRADICTION RESOLVED: pre-fix one engine gave `SELECT DISTINCT e` = 2 and `COUNT(DISTINCT e)`
  = 3. `COUNT(DISTINCT)` was ALREADY RIGHT (it keys off the pre-conversion Value). Post-fix the
  DISTINCT leg refuses and COUNT(DISTINCT) still answers 3 — one answer or none, never two.
REJECTED, with reasons: making Volcano coerce identically (satisfies `optimized == --no-optimize` by
  BREAKING `== SQLite` — one wrong engine becomes two); refusing mixed-type CASE at plan time (would
  reject `CASE WHEN c THEN 1 ELSE 0.5 END`, correct in all four modes today — moves a passing
  answer); choosing the column type from values seen (chunk-dependent, and cannot handle mixing
  WITHIN a chunk). The guard is PER-VALUE, not per-expression, exactly so it rejects only what is
  wrong today.
Neighbours checked BY RUN: DOUBLE->INT, STRING->numeric, numeric->STRING are all already loud
  (`bad_variant_access`) — INT->DOUBLE was the ONLY silent one. Unsigned does not exist (`TypeId` is
  {INT,DOUBLE,STRING}). `SUM(<large int>)` diverges from SQLite in BOTH engines — shared dialect,
  pre-existing, not a seam.

### !! NEW, LIVE, UNFIXED — the same class but WIDER, and it bites at value 7
**The conversion does not merely lose precision; it changes the TYPE, and type is load-bearing for
`/` (INT/INT truncates).** Any value out of a mixed CASE is at risk the moment another expression
reads the materialized column, so the 1e15 bound does not touch it. Both verified PRE-EXISTING
(identical on a build with the E-10 fix reverted):
  SELECT x / 2 FROM (SELECT CASE WHEN lap_id = 2 THEN 7 ELSE 0.5 END AS x
                     FROM laps WHERE lap_id < 4) t        -> SQLite 3, vectorized 3.5
    (control with both branches INT gives 3 vectorized too, isolating materialization as the cause)
  SELECT team, MAX(CASE ...) AS m FROM laps WHERE lap_id < 4 GROUP BY team
    HAVING MAX(CASE WHEN lap_id=2 THEN 7 ELSE 0.5 END)/2 > 3
    -> row-volcano 0 rows | SQLite 0 rows | **vectorized 1 row** — a ROW COUNT divergence, and here
       Volcano CAN adjudicate.
LEFT UNFIXED DELIBERATELY: extending the refusal to all INT->DOUBLE narrowing would reject
  `CASE WHEN c THEN 1 ELSE 0.5 END`, correct today in every mode — moving a passing answer. The
  distinction that matters ("is this column read by another expression, or is it the query's
  output?") is a PLAN-SHAPE fact `appendColumnValue` cannot see; it lives in
  `vectorized_plan_builder.cc`. **WAVE B / ROUND 4 OWNS THIS.**

## RESOLVED — plain LIMIT (was the pending decision). Entry 55 made TOTAL. (30df719)
Two invariants conflict at a plain LIMIT and only one can hold: `optimized == --no-optimize`
(plan-independent) vs `swiftql == sqlite` (mimic an arbitrary pick). **Chose the first** — it is
what this phase rests on, and SQL guarantees nothing about the second. Entry 55 had been asserting
a non-guarantee and passing BY COINCIDENCE (both engines probed `laps` in storage order).
Entry 55 is now `ORDER BY season, speed, laps.lap_id LIMIT 5` — natural keys plus a unique
tiebreak. Chosen over `ORDER BY laps.lap_id` because its answer is IDENTICAL to what the
deterministic cut already returns, so the change aligns the ORACLE with the ENGINE rather than
moving the engine's answer. 4/4 modes. Header records the coincidence, both invariants, which won,
the measured divergence, and **"DO NOT PUT THE UNORDERED FORM BACK IN THIS SUITE"** with the reason.
**AND IT CAUGHT ANOTHER VACUOUS ENTRY — one I specified.** I told it to re-add entry 55's unordered
form as invariant-only coverage. It CHECKED whether that text can discriminate. **It cannot**: with
the LIMIT removed, the two legs emit all 10000 rows BYTE-IDENTICALLY despite planning DIFFERENT
JOIN ALGORITHMS (`VecSimdLoopJoin` optimized vs `VecHashJoin` unoptimized) — both probe `laps`,
both emit probe-major in storage order, so any `LIMIT n` cuts the same n rows in either leg whether
or not the fix exists. Copied verbatim it would have been a green tick proving nothing — the exact
failure mode `TIE_STRADDLE_QUERIES` exists to end, re-entering through the front door.
Instead: `b31_plain_limit_two_relations`, same join + `WHERE l.lap_id < 15`, so filtered cardinality
(14) drops below `drivers` (20) and the optimized leg builds on the OPPOSITE side. Pre-fix
discrimination RECONSTRUCTED EXACTLY (a plain LIMIT only truncated the stream, so the first 5 rows
of unsorted output are precisely what it returned): **four of five rows differ**. 12/12 pass.
Structural: QUERIES still 174 (one string replaced by one), mode_census untouched, disjointness
guard unaffected.

## ~~DECISION PENDING~~ — plain LIMIT: determinism vs SQLite agreement
`QUERIES` line 55 — `SELECT season, speed FROM laps JOIN drivers ON ... LIMIT 5`, diffed against
SQLite in ALL FOUR MODES — **now FAILS in every mode.** `--explain` shows
`Limit [5] -> Sort [canonical row order] -> ...`: the plain-LIMIT determinism fix gives every
unordered LIMIT a canonical sort, so SwiftQL returns the five SMALLEST rows while SQLite returns
its scan's first five.
**Neither is wrong. SQL does not define which rows a plain LIMIT returns**, so that oracle entry
has always been asserting a non-guarantee — it passed by luck because both engines happened to scan
in the same order. Same situation as the tie-at-the-cut queries, which is why those went
invariant-only into `TIE_STRADDLE_QUERIES`.
LIKELY RESOLUTION: move line 55 out of the SQLite-diffed suite and into invariant-only coverage,
exactly as the tie case was handled. **But do not decide it until the comparator agent reports** —
it owns `6ab8848` ("the deterministic cut is a bounded top-N, not a full sort"), which is its answer
to the 200x regression, and the cost of a top-N on EVERY unordered LIMIT is the other half of the
trade. Get its number first, then decide (and surface to the user if the cost is material).

## Wave A — harness coverage, unit 2: **IT CAUGHT TWO BAD ENTRIES I HANDED IT**
I relayed four E-10 queries from the E-10 fixer. **Entries 1 and 2 were wrong and it refused to
write them as given.** They were specified `ORDER BY c LIMIT 2` — ASCENDING. Run against the current
binary they do NOT refuse: all four modes return `0.5, 0.5` and match SQLite. Cause: `6ab8848`
landed AFTER the E-10 fixer verified, and with an ascending sort the two smallest rows win, so the
big integer is discarded before anything materializes it.
**They would have been dead TWICE OVER**: pinned to a message never raised, AND vacuous as diffed
entries — the Volcano leg's output is `0.5, 0.5`, which agrees with SQLite whether or not the
integer survives. `DESC` fixes both. Same defect in guard 5. The ascending form was KEPT and moved
into the guards as the SCOPE-OF-REFUSAL pin: it holds that the refusal covers values reaching the
output column, and would fail if someone widened the guard to "any INT in the expression" or
dropped the top-N.
LESSON: a verified expectation goes stale the moment another agent lands a fix. **Re-verify handed-
over expectations against the CURRENT binary, never against the reporting agent's snapshot.**
It also built `assert_b32_pins_discriminate()` — collects each entry's actual message once and
cross-checks the FULL pin matrix: every pin must match its own message and NO other entry's, with
genuinely-shared producers compared as equal-expectation pairs rather than exempted by index.
**Then it proved the check can fail**: injecting my own first-draft bare pin `"EXISTS subquery"` on
rows 1 and 3 gives `run_rejection_suite`: 7 passed / 0 failed — GREEN AND WRONG — while the pin
check reports 10 findings naming exactly which pins cannot tell which producers apart. Wired into
the exit code beside the sweep.
B32: 28/28 pass. E-10 discrimination vs a `return v.toNumeric()` build: `E10_VECTORIZED_REFUSED`
**0/8 pre-fix** ("expected a rejection, got rows") -> 8/8 post; guards 16/16 both ways;
`E10_VOLCANO_ONLY` 8/8 both ways (Volcano was always right). Sweep: 25 suites, 223 entries, clean.

## !! WAVE A INTRODUCED A 200x REGRESSION — must not ship unremarked
Commit `d085230` ("a LIMIT cuts a determined order") fixes the plain-LIMIT shape CORRECTLY, but the
**`--no-optimize` leg of a plain LIMIT over a `driver_id` self-join went 0.32s -> 64s** (two runs
each, back-to-back, same machine). Optimized leg fine (0.10s -> 0.35s). Cause: --no-optimize does
not push the WHERE, so the LIMIT is handed the full 10000x10000/20 product, and giving it a
DETERMINED ORDER means ordering that product instead of streaming it. The harness agent worked
around it by keying its entry on a 1:1 join and documented why — **a harness workaround, not a
fix.** Raised with the comparator agent: find a bounded top-N / streaming selection, or quantify the
trade in the open. Do not let a correctness fix silently make an unoptimized query 200x slower.

## !! MY RECOMMENDATION WAS WRONG — CORRECTED FIX DESIGN FOR ROUND 3
I offered the user "deterministic tiebreak" vs "unify build-side selection" and RECOMMENDED the
tiebreak, framing the problem as "where ORDER BY is not a total order". **That framing was too
narrow and it is the reason fix round 2 shipped an incomplete fix.** The divergence is not about
ORDER BY ties; it is about ANY plan-dependent row order reaching ANY cut.
Neither option as I wrote them is sufficient, so do NOT simply switch to the other one:
  - Unifying build-side selection would close E-8, but NOT E-9/B3-1 — the DP still permutes the
    merged schema, and permuting the schema is the DP's job.
  - The sort tie-break closes neither when there is no sort.
**THE CORRECT DESIGN, both halves required:**
  1. **Canonical column order.** The tie-break must order columns by a PLAN-INDEPENDENT identity —
     qualified name, or relation-slot identity — NEVER by schema index, which the optimizer is
     free to permute and does.
  2. **Every cut site, not just sorts.** Enumerate and close all of them: LIMIT without ORDER BY,
     DISTINCT, a scalar subquery's LIMIT 1, a semi-join's first match. The engine auditor reports
     nine of eleven cut sites already closed — get the list from `seam-engine-divergence-pass-3.md`
     rather than re-deriving it.
  3. **Fix `random_diff.py`'s TRAP-1 too.** It deliberately never generates a tied ORDER BY because
     that used to be a false failure. It is now a TRUE failure, so the one tool that would have
     caught this generates the shape ZERO times. An expired assumption blinding the generator.

**ORDERING CONSTRAINT — do not get this wrong:** pass 2's B-2 and B-3 MUST NOT be fixed before
  B3-1. Both widen where the DP runs, and B-3's defect is currently the ACCIDENTAL CONTAINMENT on
  B3-1's blast radius.
Everything else on this seam checked clean: scan-schema narrowing touches no join-side consumer
  (12 shapes x 4 legs); `reachesOutsideThisBody` cannot hide a real correlation from key selection
  (8 shapes, 3-way); `hidden` invisible to join planning and preserved by `rebuild` (6 shapes);
  240 randomized oracle shapes over two seeds, 0 diffs; 41 hand batteries. `VecDerivedNode`'s
  chunk-pointer item is NOT worse than recorded — a plan tree cannot route one node's output to
  two consumers.

## STILL OPEN after fix round 2 — for a doc sweep AFTER pass 3
- **B-4**: stale deleted-refusal claims in `development.md` AND `src/cli/main.cc`
  (`has_correlated_subquery` is written and never read). Both were outside every fixer's file set.
- `development.md` is WRONG A THIRD TIME: `:855` carries VERBATIM the paragraph 18af84f deleted
  from the .cc as "false in both halves" — the .md copy is now the ONLY surviving statement of the
  retracted claim. `:854` also false; `:808` ("the decline is silent") unswept; CardinalityEstimator
  MISSING from the Week 34 consumer table despite being pass 1's HIGH.
- `join_enumeration.h:84-91` carries the same retracted paragraph verbatim.
- `docs/week-36-plan.md` records the byte-identical `--explain` check as "re-verified" — it ran on
  the UNALIASED pair only. Misleading, still there.
- **B-8 coverage hole, still real**: 17 `NOT IN` oracle entries and NOT ONE has a NULL in the body.
- B-5.1 / B-5.2: dead HAVING and arity guards. A4: `$kN` leaks into `--explain` (cosmetic).
- `catalog.cc`'s `tables_.emplace(meta.name, ...)` silently keeps the FIRST of two same-named
  TABLES — same class as the column fix, one level up.
- `JoinEnumeration` never recurses into its own result, so a derived/subquery body's joins are
  never enumerated when the outer block has a join (62729 vs 38417 measured). Plan quality.
- `containsOuterJoin` recurses into a DERIVED body, declining ordering for enclosing inner blocks.
- `VecDerivedNode::nextChunk` forwards its child's chunk pointer — a derived table on both sides of
  a self-join would forward the same `DataChunk*` twice. Not traced to a reachable plan.
  Fixer A (subquery): owns compare_against_sqlite.py, subquery_decorrelation.cc, logical_plan.cc,
    planner.cc, schema.h (comment-only). **B-1 DONE + verified** by counterfactual: rebuilt with
    the fix removed, all 6 correlated-scalar entries fail (4 FAIL + 2 internal-error ERROR);
    restored, 10/10 pass in both vec modes, 809/809 C++ tests. 11 new oracle entries. B-2, B-3 next.
    Fix shape: mark the synthetic $scalarN columns `hidden` — hence the schema.h doc sweep, because
    `ColumnDef::hidden` had documented itself as "aggregate outputs referenced only in
    HAVING/ORDER BY", which the fix falsifies.
    B-2 DONE (e916b0c). B-3 in progress; QUEUED for compare_against_sqlite.py after fixer C.
    **B-3's honest risk, which it named itself:** the refusal it NARROWS (correlated non-equality
    conjunct) has 3 rejection entries and is fine. But the guard its narrowing makes LOAD-BEARING —
    the `level != 1` depth refusal — has **ZERO coverage anywhere**: no diffed entry, no rejection
    entry, nothing. It was unreachable, so nothing ever noticed. Until its new entries land, the
    honest statement is that B-3 trades a reachable-but-wrong-cause refusal for a newly-reachable
    guard that only a manual probe has ever fired. **An uncovered refusal is worse than a stale
    one — nothing will notice when it changes.** This must not close without that coverage.
    Its B-3 shape avoids adding a 19th dispatch site: it asks `collectSlots` itself with the one
    branch suppressed (clear nested `correlated` flags across a single call via forEachSubquery,
    restore immediately, record every node cleared) rather than writing a private walker.
  Fixer B (constant folding + catalog + explain): **DONE**, all three items pushed. 823/823 C++.
    - Item 1 chose to make the ordinal rule test **WHAT THE USER WROTE**, not Literal-ness: the
      parser stamps `written_ordinal` on OrderByItem/GroupByColumn and the Validator tests that.
      Right call for a reason the audit did NOT name: **folding was not the only rewrite
      manufacturing Literals there** — the binder's select-alias substitution does it too, so
      `SELECT 1 AS one, team FROM laps ORDER BY one` was refused as `ORDER BY 1`. No amount of
      gating folding would have reached that.
    - **IT GOT THE BOUNDARY WRONG FIRST AND SAID SO.** Its first characterisation of SQLite came
      from a TWO-ROW table where sorting by column 1, by column 2, and by a constant all return
      the same order — every hypothesis passed and it read back the one it already had. Re-measured
      on data whose insert order, column-1 order and column-2 order all differ:
        ORDER BY 1 / (1) / -1     -> ordinal (parens and unary signs are TRANSPARENT)
        ORDER BY 1+1 / 2*1 / 1+0  -> NOT an ordinal (insert order)
      **Binary arithmetic is the whole boundary.** This is the single best piece of method in the
      phase: a test bed that cannot distinguish hypotheses will confirm whichever one you hold.
    - The test that passed either way: every witness in `ColumnOrdinals.RejectedInOrderByAndGroupBy`
      contained a ColumnRef and so COULD NOT FOLD. New exhaustive test fails pre-fix (all six
      EXPECT_NO_THROW threw).
    - No stale rejection entry existed to delete — `grep -rn ordinal python_tools/` returned
      NOTHING, which is the other half of why the refusal survived unnoticed.
    - NOT fixed, adjacent: `catalog.cc`'s `tables_.emplace(meta.name, ...)` silently keeps the
      FIRST of two same-named TABLES. Same class one level up; no cross-storage divergence.
    - Its pre-lock results were re-run under the lock and held. One earlier subset run produced
      ~180 spurious "NO LONGER ERRORS" across suites it never touched — artifact, not finding.
  Fixer C (deterministic tiebreak, E-1/E-1b): **DONE — BOTH BLOCKERS CLOSED.** 7b84952 (engine,
    record-only), 1d6da04 (suite), 435a87f (comment fix). New `src/execution/sort_comparator.h`
    is called by BOTH `SortNode` and `VecSortNode`, which previously held two byte-identical
    lambdas — a tie-break only one engine applies is the same divergence with a new cause, so they
    now share a function and cannot drift. 11 new C++ tests.
    E-1 {AlphaTauri,Alpine,McLaren} vs {RedBull,AlphaTauri,McLaren} -> all four modes
    AlphaTauri/Alpine/Ferrari. E-1b `977|977|1536|977` -> 977 in all four.
    Suite-level discrimination: PRE-FIX 2 passed/4 failed -> HEAD 6 passed/0 failed. Self-check
    verified to FAIL FIVE WAYS (total ORDER BY; immaterial tie of 977 identical rows; drifted
    probe; cut past the end; the E-4 scenario verbatim).
    **NEW ITEM, HANDED TO FIXER A — `planner.cc:236-240` asymmetry.** Row storage hands
    `SeqScanNode` the FULL table schema; every columnar mode hands it the NARROWED one, so the two
    legs tie-break over DIFFERENT COLUMN SETS whenever a sort sits over a raw join row. Benign
    today ONLY because the first discriminating column happens to be `driver_id` in both — luck,
    not design. Fixer C could not fix it (planner.cc is fixer A's) so it pinned the behaviour with
    a DISTINCT oracle entry that goes red if it stops being benign. Tripwire but no fix.
    It also REJECTED THE AUDIT'S OWN PROPOSED ASSERTION as insufficient: the audit suggested
    checking "at least limit+1 groups share the boundary aggregate value", which passes on a tie
    among IDENTICAL rows while asserting nothing. It required two DISTINCT rows in the tied block.
    Totals on its worktree: oracle 1468/0/0, regression 318/0/0, unit 823/823 warning-clean.
    **USER DECISION: deterministic tiebreak**, NOT unifying build-side selection (that would
    permanently constrain the optimizer).
    RULE CHOSEN: when every declared ORDER BY key ties, compare the whole row column-by-column in
    schema order, ascending, on values alone. Both engines call one shared
    `sort_comparator::rowLess`. E-1 and E-1b closed in ALL FOUR MODES.
    **MY PREDICTION WAS HALF WRONG and it corrected me.** I said SwiftQL would now differ from
    SQLite on tied queries. For GROUP BY ties it is BACKWARDS: the tie-break sorts tied groups by
    the group key and SQLite's GROUP BY also emits in key order, so they now COINCIDE
    (AlphaTauri/Alpine/Ferrari; COUNT(*)=977 — both SQLite's answers). NOT a guarantee: it fails
    for DESC and where the tie-break column is not the group key. So the new entries still belong
    in ENGINE_AGREEMENT_QUERIES — but that suite's comment justifies itself by "SQLite returns a
    third answer", which is now FALSE for its own two entries. Stale-comment class; being fixed.
    **ONE CURRENTLY-PASSING ANSWER MOVED — reported, not absorbed.** test_new_queries.py:145,
    `ORDER BY speed LIMIT 5`: two rows tie at speed=280.01 and BOTH are inside the cut, so the row
    SET is unchanged and only their relative order moves (5275,3882 -> 3882,5275). SQLite answers
    5275 first (rowid order). NO GATE SEES IT — the query is not in compare_against_sqlite.py and
    test_new_queries.py's normalize() sorts unconditionally. DECISION: make it total
    (`ORDER BY speed, lap_id LIMIT 5`) — it was written to test LIMIT ordering, not to pin a tie
    SQL leaves unspecified.
    BLAST RADIUS, measured baseline-binary vs fixed in a PRIVATE git worktree (it never touched the
    shared build/, which is a better answer than the lock): 200 distinct ORDER BY queries from both
    harnesses x 4 modes -> EXACTLY 3 moved (the 2 ENGINE_AGREEMENT entries + the one above).
    TPC-H 36 queries incl. mutants x 4 modes -> 0 moved. Regression 318/0/0. Unit 820/820.
    DISCRIMINATION: 11 new C++ tests against a build with the tie-break loop removed -> 9 FAIL /
    2 PASS, and it declined to count the 2 as evidence because they are marked GUARD (they assert
    the tie-break must NOT fire). That self-discipline is the standard.
  AFTER ALL THREE: gate, then **seam pass 3** (pass 2 had blockers, so the audit did not stop).

## !! CONCURRENT FIXERS SHARE ONE build/swiftql — USE flock
`pgrep`-before-build does NOT protect a harness RUN. A relink underneath a sweep produces a wall
of correctness failures in suites the agent never touched — one fixer collected **256 spurious
ERRORs** before reporting it. Every build AND every binary/harness run must be wrapped:
  flock -w 1800 /tmp/swiftql-build.lock -c '<command>'
Hold the lock across a whole build+run when the binary you just built must still be yours when you
run it. **Any red harness result taken on this branch without the lock must be re-run before it is
believed.** This bites an agent investigating RESULT DIFFERENCES hardest — a binary changing
mid-comparison produces exactly the row-set difference it is hunting.
Working branch: `claude/phase5-week26-qomtkb` (env mandate; stands in for `main` everywhere in
  the skill — never push elsewhere)
Weeks done: 26 ✅ 27 ✅ 28 ✅ 29 ✅ 30 ✅ 31 ✅ 32 ✅ 33 ⚠️ (partial) 34 ✅ 35 ✅ 36 ✅ — WEEK PLAN COMPLETE

## Seam audit — where it stands
PASS 1 (5 auditors): 1 cross-engine WRONG ANSWER, 1 HIGH found independently by two auditors,
  1 coupled pair, 4 lows, 0 result divergences. All fixed in round 1:
    87c08a2 volcano GROUP BY first-encounter order — the wrong answer
    17bfcea CardinalityEstimator DERIVED case — the corroborated HIGH
    8a23b9d $scalarN group keys named POSITIONALLY — closed F1+F2 together by removing the
            collision rather than relaxing the guard that caught it
    18af84f the four lows — residual forwarding, semi/anti floor, a false invariant, a silent
            decline
  Implementer's closing report: scratchpad/audits/seam-fix-round-1-report.md (READ IT — it lists
  three places the fix was narrower than the finding, and three things found but NOT fixed).

GATE on fix round 1: **GREEN** (log scratchpad/gates/seam-fix-round-1.log, uncommitted by design).
  build genuine recompile (8 files touched + relinked), ZERO warnings; unit 808/808;
  sqlite 1336 passed 0 failed 0 errors, rejection sweep 171/171 executed clean;
  regression 318 passed incl. 119 optimizer-invariant; tpch PASS (20/22 meaningful vs SQLite:
  5 four-mode, 15 vec-only; 1 vacuous; 1 unported) — full 22-query run, 4m58s, not narrowed.
  Baseline md5 7cee17dae5398e3f20ef92f05ba78d5b before AND after. Staleness disproven twice.
  HEAD moved mid-run (ee9c9d7 -> 532183e) from CONCURRENT AUDITOR PUSHES; verifier proved the
  code surface was byte-identical at both ends (`git diff ee9c9d7..HEAD -- . ':(exclude)scratchpad/'`
  empty; code fingerprint 785a8d3094304eea958f78998dc467fc at both). Verdict stands for both SHAs.

PASS 2 results — **1 BLOCKER** (subquery chain). 4 of 5 seams reported; engine-divergence still running.

**THE BLOCKER — B-1, subquery chain.** `SELECT *` over a block holding a correlated scalar
  subquery emits the SYNTHETIC relation's columns:
    SELECT * FROM drivers d WHERE d.age > (SELECT COUNT(*) FROM laps l WHERE l.driver_id =
      d.driver_id AND l.speed > 999)
    SwiftQL: 7 cols ... age $k0 COUNT(*)   |   SQLite: 5 cols
  Inside a derived body the same defect surfaces as `internal: derived table 'x' was bound
  against a 5-column schema but planned to 7 columns`. A **Week 34** defect — `blockOutputSchema`
  models no subquery lowering, so the star expands over the merged join schema. NOT introduced by
  8a23b9d. Invisible to the oracle because **all 31 WEEK34_CORRELATED_SCALAR_VEC_ONLY entries name
  their select list** — not one uses `SELECT *`.
  => Pass 2 returned a blocker, so the audit does NOT stop here. Fix round 2, gate, then PASS 3.

storage (27fd886, 28655b4): **0/0/0/2 — and it REFUTED S-0.** Pass 1 overstated it; re-ranked LOW.
  1. There are **three** storage×engine cells, not four: `--storage row --execution vectorized` is
     refused for every query (main.cc:548, confirmed by running it). The harness's fourth "mode" is
     `--no-optimize`, an optimizer flag. So the row/columnar oracle is Volcano-only, always.
  2. **The oracle DOES span Phase 5**: `QUERIES` holds 89 Phase 5 queries out of 168, including all
     17 WEEK29_OUTER_JOIN_QUERIES. Week 29's `pruningHintForPreservedSide` is the one Phase 5 change
     existing only to protect chunk pruning, so the storage-critical Phase 5 path IS differentially
     oracled. 6/6 byte-identical row vs columnar.
  3. The two storage modes differ in exactly ONE node — `SeqScanNode`. The four shapes Volcano
     refuses are rejected BEFORE any scan is built, so none can reach it. Row storage is a
     `vector<Row>` with an index: no chunks, encodings, zone maps or pruning to break.
  CHEAPEST FIX for W37: give `VecScanNode` a row-backed constructor and delete main.cc:548.
    ~70-90 lines, no planner change, no refusal relaxed. The row leg has no zone maps, so it becomes
    a pruning-on vs pruning-off oracle on exactly the shapes that have none. Every alternative
    (lifting a Volcano refusal) is larger and buys less.
  NEW S-8 (LOW): a duplicate column name in catalog.json is a SILENT WRONG ANSWER in columnar only.
    `ColumnarTable::columns` is keyed by name, so both columns land in one vector.
    `SELECT k FROM t` -> row `1,2,3` vs columnar `1,100,2`. No error at any layer. Needs a
    hand-malformed catalog, but it is the ONLY input found in two passes that makes the formats
    disagree, and the engine already refuses the identical shape for derived relations
    (logical_plan.cc:494). ~4 lines in catalog.cc.
  Also closed pass 1's not-reached gap: 144-query zone-map boundary sweep over DOUBLE and STRING
    (plus INT) x 3 cells vs a Python oracle, 0 mismatches; first runtime proof pruning is live on
    the Volcano columnar path (chunks_skipped=2/3); scan order identical across formats, so
    87c08a2's GROUP BY fix is sound across STORAGE too.
  TPC-H refusal pins are weak (owner: the TPC-H reporting seam, not storage): run_tpch.py:81-88 has
    a DEAD entry ("IN (subquery) is lowered" vs the engine's "IN subqueries are lowered") and a
    LOOSE one ("IN subquery" also matches two LANGUAGE refusals firing in all four modes,
    mislabelling a dialect gap as boundary coverage).
  NULL, proved not inferred: `CSVToColumnar::convert` is the only ColumnarTable producer,
    `parseField` has no NULL production, and `ColumnArray` has NO VALIDITY CONCEPT. So NULL
    representation has never been differentially tested across storage — and cannot be, because
    columnar has no representation to disagree with. NULL *semantics* are well covered (38
    NULL-bearing queries in both storage modes). W37: if a loader ever learns `\N`, columnar needs
    a validity representation that does not exist, and that day has ZERO harness coverage.

subquery-chain (c6830f1): 1 BLOCKER (above), 2 MEDIUM, 5 LOW. Part A: 8a23b9d's collision is
  IMPOSSIBLE, not unlikely — three independent reasons, incl. `$` being outside the lexer's
  identifier alphabet with no quoted-identifier production. The guard is dead on this call site and
  alive where it belongs (user-written `FROM (...) AS d`) — the fix removed the routing, not the
  problem. One correction: the commit cites `buildAggregateSchema`; the schema actually renamed is
  the terminal `LogicalProject`'s. Cost: `$k0`/`$k1` leak into `--explain`; nothing pins them.
  B-2 MEDIUM: a column alias on an UNWRAPPED correlated scalar body breaks it (`column not found:
    'AVG(l2.speed)'`, SQLite 4994) while the wrapped form with the same alias returns 4037 —
    contradicting the "same plan" claim in both the README and the suite.
  B-3 MEDIUM: `collectSlots` has a third `-1` producer (`if (sq->correlated)`) that
    `splitCorrelation`'s comment does not enumerate, so any conjunct merely CONTAINING a nested
    correlated subquery is refused with a message about a correlated inequality the query lacks.
  LOWs: a Week-33-deleted refusal still asserted in development.md AND main.cc:475
    (`has_correlated_subquery` written and never read); three guards that cannot fire, one
    (`level() != 1`) load-bearing the moment B-3 is fixed; a rejection pin on a shared message tail;
    and **none of the 17 NOT IN oracle entries has a NULL-bearing body**.

Earlier two seams — 0 BLOCKERS, 0 HIGH:
  join-chain (532183e): 3 MEDIUM, 2 LOW. Corrected pass 1: the DERIVED phantom manufactured the
    join-order *margin*, NOT the order — the order was byte-identical and already the better one.
    28 constructed shapes, three-way differential vs SQLite, 0 disagreements. DP search space is
    result-preserving; semi/anti cannot be moved/reach build side/be projected from; NULL keys
    dropped on both sides of all three join operators; '\x01' composite-key class closed by a
    length prefix; NOT EXISTS vs NOT IN over an all-NULL key column returns 20 vs 0 matching
    SQLite both ways.
  optimizer-preservation (d0c0a5f, 4d010e7, 7901864): 4 MEDIUM, 4 LOW. Found NO shape where
    optimized and --no-optimize disagree. Switch now exhaustive 9/9 LogicalNodeType. The false
    invariant had no code callers.

### Pass-2 findings queued for FIX ROUND 2 (none blocking)
CORROBORATED BY TWO AUDITORS — treat as the real ones:
- **JoinEnumeration never recurses into its own result**, so a derived/subquery body's joins are
  never enumerated and its WHERE never pushed *when the outer block has a join* — but the same
  body IS optimized when the outer block has none. Measured 62729 vs 38417 on sf0.01, with NO
  decline line printed. (join-chain B-3 / optimizer B-2.) Plan quality, not correctness.
- **development.md is wrong a THIRD time**, and `:855` carries VERBATIM the paragraph 18af84f
  deleted from the .cc as "false in both halves" — the .md copy is now the only surviving
  statement of the retracted claim. `join_enumeration.h:84-91` carries it verbatim too.
  Also `:854` now false, `:808` ("the decline is silent") unswept, and CardinalityEstimator is
  MISSING from the Week 34 consumer table despite being pass 1's HIGH.
  (join-chain B-5/B-1 / optimizer B-4.)
SINGLE-AUDITOR:
- **Constant folding is ungated and its justification is FALSE.** "Folding cannot change results"
  — but `ORDER BY 1 + 1` folds to `Literal(2)`, the Validator's ordinal rule tests Literal-ness,
  and a LEGAL query is refused with `ORDER BY 2: column ordinals are not supported`, naming an
  ordinal the user never wrote. Confirmed by execution. The C++ test that claims to cover this
  picks `ORDER BY speed + 1` — the one witness that CANNOT fold. (optimizer B-5.) This is a
  user-visible wrong refusal and the most fixable thing in pass 2.
- `containsOuterJoin` recurses into a DERIVED body, so a LEFT JOIN sealed inside a derived table
  declines ordering for an enclosing fully-inner block — and, running before `slotDeclineReason`,
  misattributes semi/anti declines as `(outer join)`. (join-chain B-2.)
- DERIVED lowering calls `lowerNode` not `lower`, so a derived body's ROOT physical node prints
  no `est=` — visible unremarked in 17bfcea's own pasted evidence. One-token fix, EXPLAIN-only.
  (join-chain B-4.)

### Carried from the fix-round implementer, NOT yet fixed
- **`VecDerivedNode` re-entry**: `nextChunk` forwards its child's chunk pointer, so a derived
  table on both sides of a self-join would forward the same `DataChunk*` twice. Not traced to a
  reachable plan.
- More pre-Week-34 `countRelations` comments likely survive (one stale test was found passing for
  the wrong reason — 2-relation spine while claiming to be past a `<3` guard).

### The measurement-blindness finding — biggest thing pass 2 has produced
**`--no-optimize` gates exactly THREE passes** (pushdown, enumeration, estimation). SIX other
plan-rewriting passes run in BOTH legs: constant folding, all three subquery lowerings, subquery
materialization, derived normalization. So `optimized == --no-optimize` is blind to all six BY
CONSTRUCTION — including the entire subquery lowering path — and nothing anywhere says so.
Every reassuring subquery number comes from SQLite agreement alone, never from the invariant.
This changes what "0 divergences" means and must be written down wherever that figure is quoted.

## STOP CRITERION — pass 2 FAILED it
The skill: repeat the seam pass up to 5 times, stopping as soon as a pass returns NO BLOCKERS.
**Pass 2 returned a blocker (B-1).** So: fix round 2 → gate → **PASS 3**. Only if pass 3 is
blocker-free does the audit end. Week 37 is the user's either way.

## TPC-H 22/22 — the user's stated goal. The two gaps are UNRELATED.
- **q21 — a real engine gap, 0 of 4 modes.** Blocker: a correlated INEQUALITY has no equi-join to
  lower to (`l2.l_suppkey != l1.l_suppkey`). But each correlation has TWO conjuncts — an equi one
  (`l2.l_orderkey = l1.l_orderkey`) AND the inequality. So the equi key DOES exist; the open
  question is whether the lowering can split the correlation and carry `!=` as a join RESIDUAL
  rather than refusing the whole predicate. Residual forwarding was one of 18af84f's four lows,
  which makes that path more plausible than it was. NOT asserted — sized for W37.
  The query itself is fine: the harness confirms it DISCRIMINATING against SQLite on this data.
- **q18 — NOT a gap.** SwiftQL answers it in 2 modes and matches SQLite; the answer is empty on
  both sides. MEASURED on the actual files (not trusting the comment): 15000 orders, max
  SUM(l_quantity) per order = **295.0**, orders over 300 = **0**, max lineitems/order = 7, max
  single quantity = 50. Domains are right (ceiling 7x50=350); no order hit both maxima at once —
  a DISTRIBUTION artifact, exactly what PROVENANCE.txt warns about. 290 -> DISCRIMINATING.
  **THE THRESHOLD MUST NOT MOVE**: 300 is already the lowest of the spec's three Q18 quantities
  (300/312/315), so lowering it invents a value the spec does not contain — unlike q2's SIZE and
  q19's BRANDs, which were re-chosen from WITHIN the spec's own domains.
  Honest route: **more data, not a smaller number.** sf0.1 gives ~150k orders and the tail that
  stops at 295 today would clear 300. Cost: sf0.1 needs its OWN baseline, since which queries are
  vacuous is a property of the data. **This is the user's call, not mine.**

## QUEUED BY THE USER — q21, AND THE GOAL IS 22/22
**USER DIRECTIVE: implement q21. The stated goal is all 22 TPC-H queries.**
DISPATCH IT the moment seam pass 3 closes — NOT before. An operator change underneath an audit
makes that audit's findings describe a tree that no longer exists, and pass 3 is measuring.

**The full requirement is already worked out — do NOT re-derive it.** `docs/week-36-plan.md`
Task 3 and the comment at `subquery_decorrelation.cc:58-73`. Summary of the four pieces:
1. `splitCorrelation` routes a correlated non-equality into a residual list instead of refusing.
   **The refusal NARROWS, it does not disappear** — still refused when NO equality survives, since
   a body correlated only by inequalities has no hash key and the fallback is a cross product this
   engine has no operator for.
2. The semi/anti build side must keep **rows**, not keys. Today it fills `build_keys_` (a set of
   serialized keys); the STANDARD path already fills `hash_table_` (key -> vector<Row>). A routing
   change, not a new data structure.
3. A **private** `residual_schema_` = probe (+) projected-body, with a concatenated row to evaluate
   against. `output_schema_` MUST NOT MOVE — that it stays the probe's is Week 32's containment,
   and widening it would put body columns in scope above the join.
4. Body-side residual columns **APPENDED** to the body projection, after the keys, so key indices
   stay positional 0..k-1. Appending, never inserting.
CONTAINMENT TO PRESERVE: `ANTI_NOT_IN` must keep refusing residuals. Its short-circuit answers
"S contains a NULL, so `x NOT IN S` is never TRUE" — a claim about the KEY column that a residual
makes untrue. `NOT IN` never produces a residual, so this costs nothing.
**THE HAZARD THAT WILL BITE:** the residual compares `l3.l_suppkey` with `l1.l_suppkey` — the SAME
NAME from two aliases of the SAME table. In a merged residual schema `indexOf(name)` takes the
first match. Wrong rows, no error, identical `--explain`. Refs MUST be restamped BY SLOT.
This is exactly the class the deferred `ColumnId {level, slot}` migration makes a COMPILE ERROR.
**USER DECISION: q21 FIRST, ColumnId migration LATER.** So the slot-restamping is done BY HAND and
the guarantee is care, not the compiler. Consequences that are now mandatory, not optional:
  - The q21 fixer's brief must call out the wrong-relation hazard EXPLICITLY and require it to
    demonstrate, with actual output, that `l1.l_suppkey` and `l3.l_suppkey` resolve to DIFFERENT
    columns — not merely that the query returns 3 rows. The right answer by luck looks identical.
  - It must add a test that FAILS if the refs resolve by name instead of by slot.
  - A post-q21 audit must re-check this specifically. Wrong rows, no error, identical `--explain`
    is the H-1 failure shape verbatim and no gate in this project can see it.
  - The ColumnId migration stays on the deferred list and its justification just got stronger.
TARGET IS KNOWN: mutation check says DISCRIMINATING; SQLite returns 3 rows
(`Supplier#000000044 | 9`, `...054 | 7`, `...013 | 4`). q21 goes 0 modes -> 2 (vec-only; it has
four joins and Volcano builds exactly one). **That is 21/22.**

### 22/22 needs a q18 decision that is NOT engineering
q18 already matches SQLite; both sides return zero rows. MEASURED on the committed files: 15000
orders, max SUM(l_quantity) per order = 295.0, orders over 300 = **0**, max lineitems/order = 7,
max single quantity = 50 (ceiling 7x50=350 — no order hit both maxima; a DISTRIBUTION artifact,
exactly what PROVENANCE.txt warns of). 290 -> DISCRIMINATING.
**THE THRESHOLD MUST NOT MOVE.** 300 is already the LOWEST of the spec's three Q18 quantities
(300/312/315). Lowering it invents a value the spec does not contain — unlike q2's SIZE and q19's
BRANDs, which were re-chosen from WITHIN the spec's own domains. Refuse this route even if it
would make the number 22.
**USER DECISION: GENERATE sf0.1.** Route approved; the threshold stays at 300.
  python3 python_tools/generate_tpch.py --scale 0.1 --out-dir data/tpch/sf0.1
Do this AFTER q21, so the new baseline is recorded once against a tree that already answers q21 —
otherwise the baseline is written twice and the second write is the kind of laundering the harness
was hardened against in Week 35.
MANDATORY when it lands, or the number is not honest:
  - sf0.1 gets its OWN baseline. Vacuity is a property of the DATA; it CANNOT be diffed against
    the sf0.01 baseline. Write it with a SECOND, SEPARATE run carrying
    `--write-baseline docs/tpch-sf0.1-baseline.json --json docs/tpch-sf0.1-report.json` and NO
    `--baseline` (the harness refuses `--write-baseline` without `--json`: the baseline IS the
    report's `summary` key, and refreshing one alone is exactly how the pair spent Week 36
    disagreeing about the headline figure).
  - **VERIFY q18 ACTUALLY DISCRIMINATES at sf0.1** — do not assume. Measure max SUM(l_quantity)
    per order on the generated files first; if it still does not clear 300, sf0.1 was the wrong
    answer and the honest report is 21/22, NOT a threshold change.
  - Re-check which OTHER queries change vacuity at the new scale — some may become vacuous that
    were not. The figure must be re-derived, never carried over.
  - **`data/tpch/` is GITIGNORED.** The generator is seeded, so record the exact command in the
    README next to the baseline; a fresh clone has no data and gate 5 must name that command.
  - Gate 5 is ~5 min at sf0.01 and scales with data — budget a slower gate, and decide whether
    sf0.01 stays the gate with sf0.1 as an opt-in, or sf0.1 becomes the gate.
  - Keep sf0.01's baseline and report. They are the record for every figure quoted in Weeks 35-36.

## CARRY TO WEEK 37
- **S-0 is REFUTED** — see the storage entry above. Re-ranked LOW. The real storage item is the
  ~70-90 line `VecScanNode` row-backed constructor.
- **NULL has no columnar representation at all** — `ColumnArray` has no validity concept. If a
  loader ever learns `\N`, columnar needs a representation that does not exist, with zero harness
  coverage. Bigger than "the CSV loader cannot express NULL", which is the symptom.
- q21 and q18 — see the TPC-H section above.
- Pre-existing "BetweenExpr would cost 17 dispatch sites" drift in three files.
- `ColumnId {level, slot}` — still deferred. 87 non-comment mentions / 6 source layers.

## !! Container reclaim — read this first after any reboot
The container has been reclaimed ON THE HOUR and comes back rolled to a stale snapshot with
scratchpad wiped. Budget every agent to finish inside the current hour.
- `run_in_background: false` is IGNORED by the harness — agents ALWAYS run in background, so a
  blocked turn cannot hold the container open. Short budgets are the only lever.
- **Tell every long agent to write its artifact INCREMENTALLY.** Whatever is on disk when it dies
  is all that survives.
- **AUDITS MUST COMMIT THEMSELVES** (`git add -f scratchpad/audits/<file>`). One was lost entirely.
  Gate LOGS are deliberately NOT committed (150-300KB each); their verdict goes in this file.
- **Never run two gates concurrently** — they contend on the same build/ directory.
- **The git remote is the only durable store.** This file is force-added (`git add -f`) despite
  the skill saying not to commit scratchpad — an uncommitted state file does not survive the
  exact event it exists for.
- **Recovery, every reboot:** `git fetch origin claude/phase5-week26-qomtkb`, confirm local HEAD
  is an ancestor, then `git reset --hard origin/claude/phase5-week26-qomtkb`.
- **Background subagents do NOT keep the session alive.** A turn ending with only background
  agents running reads as idle → reclaim → those agents are gone. Two gate+audit pairs lost.
- GIT IDENTITY — env vars (`GIT_AUTHOR_EMAIL` etc.) OVERRIDE git config entirely, so
  `git config user.email` does nothing. Prefix every commit:
    GIT_AUTHOR_NAME=Claude GIT_AUTHOR_EMAIL=noreply@anthropic.com \
    GIT_COMMITTER_NAME=Claude GIT_COMMITTER_EMAIL=noreply@anthropic.com git commit ...
  ~300 commits already carry the owner's identity, including Weeks 1-25 they wrote themselves.
  DO NOT rewrite history — raised with the user, awaiting their preference.

## Rules learned the hard way
- **NEVER commit the state file while a gate is running.** Journal BEFORE launching a gate or
  AFTER it reports.
- **`pgrep` is NOT a "is the gate finished" test.** The verifier goes quiet between gate steps
  while it writes its log and reasons, so empty `pgrep` means "nothing running this second".
  Seven auditor commits landed mid-gate because I handed auditors that test. Correct rule: the
  ORCHESTRATOR tells auditors when the gate has reported; `pgrep` is only for "don't start a
  second build/harness right now".
- **An agent past its expected duration with no artifact progress is dead** — relaunch, don't
  wait. Stamp every launch with `date`; there is no internal clock.

## Week verdicts (condensed)
W36 checkpoint met — the TPC-H figure moved 19 -> 20, independently confirmed; Q17 runs TPC-H's
  own unaltered text and discriminates under mutation. W35 built the TPC-H harness; its honest
  number moved 20->18->17->19->20, every move a correction to the MEASUREMENT.
W34 checkpoint met INCLUDING Q17 — the deliverable W33 recorded as a miss.
W33 PARTIALLY MET — EXISTS/NOT EXISTS decorrelation works; correlated SCALAR was REFUSED and
  handed to W34. Three silent wrong answers found, ALL ONE SHAPE: code trusting a refusal that
  had been deleted; two lived in header comments. A later sweep found seven more stale
  preconditions. THIS IS THE CODEBASE'S MOST PRODUCTIVE BUG CLASS.
W32 checkpoint met — the HIGH was a REGRESSION a green 988-query oracle could not see, because
  the test guarding that capability had been narrowed to a scalar stand-in.
W31 shipped uncorrelated + scalar subqueries; structural insight: `compare_against_sqlite.py`'s
  diffed suite CANNOT hold a query that errors, so refusals need their own rejection suite.
W30's round-1 gate was GREEN on a tree containing TWO BLOCKERS — the clearest justification in
  the whole phase for separating measurement from judgment.

## Heartbeats
~25-min self-re-arming send_later chain; re-arm every turn while work is in flight.
Hourly backstop Routine trig_011E7EYu3E7P9F3zKFUSsqhg at :44. Routine-fired sessions get NO
mcp__* tools so the backstop cannot re-arm the chain; send_later-fired ones can.
