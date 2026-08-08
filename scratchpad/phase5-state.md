# Phase 5 orchestrator state
Current: seam-audit **FIX ROUND 2** in flight — 3 concurrent fixers. Pass 2 complete (all 5 seams).
  Fix round 1 GATED GREEN.
  Fixer A (subquery): owns compare_against_sqlite.py, subquery_decorrelation.cc, logical_plan.cc,
    planner.cc, schema.h (comment-only). **B-1 DONE + verified** by counterfactual: rebuilt with
    the fix removed, all 6 correlated-scalar entries fail (4 FAIL + 2 internal-error ERROR);
    restored, 10/10 pass in both vec modes, 809/809 C++ tests. 11 new oracle entries. B-2, B-3 next.
    Fix shape: mark the synthetic $scalarN columns `hidden` — hence the schema.h doc sweep, because
    `ColumnDef::hidden` had documented itself as "aggregate outputs referenced only in
    HAVING/ORDER BY", which the fix falsifies.
  Fixer B (constant folding + catalog + explain): owns validator.cc, ast.h, parser.cc, binder.cc,
    tests/test_logical_plan.cc, constant_folding, catalog.cc, vectorized_plan_builder.cc.
  Fixer C (deterministic tiebreak, E-1/E-1b): owns vec_sort_node, vec_limit_node, and the Volcano
    sort/limit in plan_nodes. **USER DECISION: deterministic tiebreak**, NOT unifying build-side
    selection (that would permanently constrain the optimizer). Where ORDER BY is not a total
    order, the surviving row must be fixed regardless of plan/engine/storage.
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
