# Week 35 audit — round 2 (fix round + gate wiring)

Range: `806d1df..HEAD` on `claude/phase5-week26-qomtkb`, excluding `chore:` commits touching only
`scratchpad/`. Prior audit: `scratchpad/audits/week-35-round-1.md` (its blocker, q16 vacuous, is fixed).

Auditing the mutation check, the vacuous classifications, the unported pair, the fifth gate step,
and the recovered WIP — **as a measuring instrument**.

Status: IN PROGRESS (appended as findings are confirmed).

## Findings

### 1. BLOCKER (same class as round 1's q16) — q2's INERT is a *parameter* artifact, not vacuity. A one-line param change makes it discriminate.

`python_tools/tpch_queries.py:493-498` (the q2 mutation), verdict `INERT` in
`docs/tpch-sf0.01-report.json` (`mutations.q2`), and therefore q2 is subtracted from the
headline in `run_tpch.py:388-390` (`meaningful = [q for q in answered if q not in vacuous]`).
17/22 rather than 18/22 rests on this.

The classification is *mechanically* correct — I reproduced it. With
`VALIDATION_PARAMS['q2'] = {SIZE: '15', TYPE: 'BRASS', REGION: 'EUROPE'}`
(`tpch_queries.py`, VALIDATION_PARAMS block), the outer filter
`p_size = 15 AND p_type LIKE '%BRASS' AND r_name = 'EUROPE'` already narrows to **exactly one
(part, supplier) pair** on `data/tpch/sf0.01`:

```
q2 base                       : 1 row  (Supplier#000000008 / p_partkey 607)
q2 with the correlated MIN(ps_supplycost) subquery DELETED : 1 row, identical
```

With one candidate row there is nothing for `ps_supplycost = (SELECT MIN(...))` to eliminate,
so the correlated scalar subquery — the entire point of the query — filters nothing.

**But this is a data/parameter fault, not a property of Q2.** I swept all 50 `p_size` values
x 5 regions (250 combos) through the same base-vs-mutant comparison the harness uses:

```
172 of 250 (SIZE, REGION) combinations DISCRIMINATE.
e.g. SIZE=1  REGION=EUROPE  ->  7 rows base vs 8 rows mutant
     SIZE=3  REGION=EUROPE  ->  3 rows base vs 4 rows mutant
     SIZE=2  REGION=ASIA    ->  7 rows base vs 8 rows mutant
```

The spec's validation parameter for Q2 is SIZE=15/BRASS/EUROPE, but that parameter was chosen
against `dbgen` at SF=1; against this generator at SF=0.01 it collapses the candidate set to
one row. This is precisely the failure round 1 named for q16 — "the generator reproduces the
*domain* but not the seeded content/scale, and the harness has no check that the feature was
selective" — and q16 was *fixed* (`f7c6cdb` seeded the anti-join phrase) rather than written
off. q2 is being written off.

**Effect on the instrument:** the reported figure UNDERSTATES the engine by one query
(17/22 should be 18/22), and — more importantly for Week 36 — the correlated-scalar-subquery
mechanism is currently carried by **no** meaningfully-answered query on the col-vec path. q17
(the other correlated-scalar query) is unported; q2 is vacuous. Week 36 would inherit "the
correlated scalar subquery is exercised by nothing" without that being stated anywhere.

**Concrete fix:** change `VALIDATION_PARAMS['q2']['SIZE']` from `'15'` to `'1'` (or any of the
172 discriminating combos) and record in the template that the spec parameter does not
discriminate at this scale. Verified: `SIZE='1'` yields base 7 / mutant 8 -> DISCRIMINATING.

### 2. MAJOR — q19's ALL_NULL is also a parameter artifact. 19 of 25 brand choices make it non-NULL, and 18 of those discriminate.

`docs/tpch-sf0.01-report.json` `mutations.q19 = ALL_NULL`; the rule is
`run_tpch.py:261` (`all(v is None for row in base for v in row.values())`), and it fires before
the mutant is ever run. `docs/week-35-plan.md:115-116` writes it off as "no row matches any of
the three OR arms". True, but not a property of Q19 — a property of `VALIDATION_PARAMS['q19']`
at this scale.

Arm-by-arm on `data/tpch/sf0.01`, every domain the query needs is present and the conjunction
survives to 1-3 candidate lineitems before the quantity band empties it:

```
                       parts(brand+container)  +p_size band  +join,shipmode,shipinstruct  full
arm1 Brand#12 SM/1-5             7                  1                   1                  0
arm2 Brand#23 MED/1-10           5                  1                   1                  0
arm3 Brand#34 LG/1-15            8                  3                   2                  0
lineitem l_shipinstruct='DELIVER IN PERSON': 14829   l_shipmode IN ('AIR','REG AIR'): 17155
```

Swept all 25 generated brands through the harness's own base-vs-mutant comparison:

```
19 of 25 brand choices give a NON-NULL base; 18 of those DISCRIMINATE.
e.g. Brand#25 -> revenue 200627.36 base, NULL mutant   (mutation removes arms 2 and 3)
     Brand#31 -> revenue  64323.74 base, NULL mutant
     Brand#42 -> revenue 117767.37 base, NULL mutant
(Brand#14 gives a non-NULL base but the mutant is identical -> would be INERT, not a fix.)
```

So q19 is one parameter change away from meaningful. Same shape as finding 1 and as round 1's
q16: the spec's validation parameters were chosen against `dbgen` at SF=1, and at SF=0.01 with
this generator the three-arm conjunction is empty by luck, not by construction.

**Effect:** q19 is one of only five 4-mode queries (`summary.modes.q19 = 4`), so the vacuity
costs the headline its most-covered query. 17/22 with "4 in all four modes" would be 18/22 with
"5 in all four modes" if q19's brand parameters were re-chosen against this data.

The classification itself (`ALL_NULL`) is mechanically correct — I reproduced `base = [(None,)]`
— and the ALL_NULL rule cannot misfire in the other direction (it requires *every* value in
*every* row to be NULL). The defect is the write-off, not the detector.

### 3. MINOR — q18's EMPTY is genuine as a classification but is also parameter-reachable; the note "unreachable at SF=0.01" is right for 300 and wrong as a general statement.

`docs/week-35-plan.md:114` says `SUM(l_quantity) > 300` is "unreachable at SF=0.01". Measured:

```
MAX per-order SUM(l_quantity) on data/tpch/sf0.01 = 295.0   (max 7 lineitems x max qty 50)
q18 QUANTITY=300 : base 0   mutant 100   (EMPTY -- short-circuits at run_tpch.py:257)
q18 QUANTITY=290 : base 2   mutant 100   DISCRIMINATING
q18 QUANTITY=280 : base 5   mutant 100   DISCRIMINATING
```

`EMPTY` is the *right* verdict for an empty answer regardless — a zero-row match asserts nothing
about the engine, and the short-circuit at `run_tpch.py:257-258` correctly returns before the
mutant runs. So this is not a misclassification. But it is worth recording that the
`IN (subquery with HAVING)` semi-join *is* exercisable on this data at a 290 threshold, i.e. the
data is not the obstacle it reads as; only fidelity to the spec's 300 is. Lower severity than
findings 1 and 2 because 300 is already the lowest of the spec's three Q18 parameters, so
changing it is a real deviation rather than a free fix.

### 4. Mutation check — three verdicts verified by hand; the multiset hardening holds; no second positional flaw found.

Verified by re-running the fragments through SQLite myself, independently of the harness:

- **q2** — reproduced `INERT` exactly (see finding 1). The verdict is mechanically right.
- **q18** — reproduced `EMPTY` (base 0 rows) and confirmed the mutant is *not* empty (100 rows),
  i.e. the short-circuit is what produced EMPTY, not a failed mutation.
- **q19** — reproduced `ALL_NULL` (`base = [(None,)]`) and confirmed the mutant is also NULL, so
  even without the short-circuit this pair would read INERT rather than falsely DISCRIMINATING.

Fragment-application soundness: `python3 python_tools/tpch_queries.py` exits 0 with
"22 templates render ... and 22 mutations apply", and `render_mutant`
(`tpch_queries.py:601-608`) raises unless the fragment occurs **exactly once** in the collapsed
template — so a silently-non-applying mutation (which would report every query DISCRIMINATING)
is impossible, and `MUTATION_BROKEN` is a gate failure (`run_tpch.py:474`, `gate_line`
`run_tpch.py:566`). Verified `mutation_broken` is empty in the shipped report.

The hardened comparison (`run_tpch.py:275-278`) sorts both sides by `repr` and compares as a
multiset, so ORDER-BY reshuffling can no longer read as discrimination. I looked for the same
class of flaw elsewhere in the check and did **not** find one:

- Values only, never column names: `tuple(r.values())`. A mutation that changes a select-list
  expression (q14, q20) therefore cannot register as different merely because SQLite renamed the
  column.
- No `normalize()` on either side, so no float tolerance is applied — but both sides are SQLite,
  so the values are bit-identical for an unchanged row and no tolerance is needed. A mutation
  producing a genuinely different float would have to collide bit-exactly to read INERT; that
  direction of error would *understate*, not overstate.
- `key=repr` is a total order over mixed `None`/`str`/`float`, so the sort cannot raise and fall
  through to a wrong branch.
- LIMIT interaction: q2 and q15 carry `LIMIT 100`. A mutation that only reorders rows *within*
  the limit is caught by the multiset; one that changes *which* rows the limit admits is a real
  content change. Neither direction inflates.

**Bounding note, already stated by the harness** (`run_tpch.py:439-442`): the check neuters ONE
predicate. For q7 and q8 the mutated predicate is a filter *inside* the derived body, so the
`LogicalDerived` feature itself is present in both base and mutant and is not what the verdict
covers. The labels say so honestly ("the p_type filter inside the derived body"), but a reader
of the 17/22 should not take it as "the derived-table rewrite is exercised by 2 queries".

### 5. MAJOR — `--baseline` and `--write-baseline` on the same path makes the gate pass unconditionally, and the harness's own IMPROVED message tells you to type that path.

`python_tools/run_tpch.py:695-699` writes the baseline, and `:701-707` compares against it —
**in that order, in the same `main()`**. Nothing forbids the two flags naming the same file. So:

```
python3 python_tools/run_tpch.py --catalog data/tpch/sf0.01/catalog.json \
    --baseline docs/tpch-baseline.json --write-baseline docs/tpch-baseline.json
```

overwrites `docs/tpch-baseline.json` with this run's summary at `:696`, then at `:703` compares
that summary against the file it just wrote. `lost` and `gained` are empty by construction and
every `modes` comparison is an equality, so `compare_baseline` returns `ok=True` for *any* run,
including one where half the queries stopped answering. The gate prints
`tpch: PASS (…)` and exits 0.

This is not hypothetical operator error: `compare_baseline`'s own IMPROVED line
(`run_tpch.py:508-509`) and `.claude/skills/verify/SKILL.md`'s PASS row both instruct
"rerun with `--write-baseline docs/tpch-baseline.json`", and SKILL.md's gate-5 block shows
`--baseline docs/tpch-baseline.json` as the standing invocation. A verifier following both
instructions in one command silently disarms the gate. `SKILL.md` also says "Never edit
`docs/tpch-baseline.json` to make this gate pass" — this is that edit, performed by the harness
itself, with a PASS printed over it.

**Fix:** reject the combination when the paths resolve to the same file, or move the
`--write-baseline` write to *after* the comparison and after the exit-code decision.

### 6. MAJOR — the headline denominator is the *run's* query subset, so a narrowed run prints a clean-looking PASS with a 100% figure.

`run_tpch.py:552` (`f"{len(summary['meaningful'])}/{len(qids)} meaningful vs SQLite"`) takes
`qids` from `--queries` (`run_tpch.py:635`), and `compare_baseline` deliberately scopes itself to
the queries that ran (`:497-499`, `ran = set(summary["modes"])`). Both behaviours are individually
right; together they let a narrowed run emit a gate line that is indistinguishable in shape from a
full one. Exercised directly against the shipped baseline:

```
summary for a --queries q1,q6,q12,q14 run, compared to docs/tpch-baseline.json:
  tpch:       PASS (4/4 meaningful vs SQLite: 4 in all four modes; 0 vacuous; 0 unported)
```

"4/4 meaningful, 4 in all four modes, 0 vacuous, 0 unported" is a stronger-reading claim than the
true `17/22 … 3 vacuous; 2 unported`, and it is a genuine PASS with exit code 0. Nothing in the
line records that 18 queries were not run. `SKILL.md`'s "a verdict block may only report a full
22-query run" is a *discipline*, enforced by nothing — and the same section instructs narrowing
with `--queries q9,q17` while iterating, so both strings are in front of the same reader.

**Fix:** have `gate_line` carry the subset explicitly, e.g. `4/4 of a 4-query SUBSET (full set
is 22)`, so a partial figure cannot be copied into a verdict block as a full one.

### 7. The regression semantics of the fifth gate DO fire. Verified mechanically, three ways.

Drove `compare_baseline` and `gate_line` directly against the shipped `docs/tpch-baseline.json`
with synthetic summaries (pure Python; no build, no engine run):

| injected state | result |
|---|---|
| q9 meaningful -> vacuous (INERT) | `ok=False`, `REGRESSION: q9 was meaningfully answered, now VACUOUS (INERT)` |
| q1 answers in 2 modes, baseline 4 | `ok=False`, `REGRESSION: q1 answers in 2 modes, was 4` |
| q9 stops answering (0 modes) | `ok=False`, both `no longer answers` and `answers in 0 modes, was 2` |

All three propagate: `main` does `ok = ok and base_ok` (`run_tpch.py:709`), `gate_line` renders
`FAIL` (`:576-578`), and the process exits 1 (`:717`). The three failure inputs the target named
(stops answering / becomes vacuous / fewer modes) are all genuinely fatal.

`NO-BASELINE` cannot be read as a pass. Verified: with `baseline_lines=None` and
`mismatched=['q9']`, `gate_line` prints

```
tpch:       NO-BASELINE (17/22 meaningful vs SQLite: …) -- rerun with --baseline docs/tpch-baseline.json to gate it -- WRONG ANSWERS ['q9']
```

and the exit code is 1 because `ok` is already False from `render_report` (`:474`). One
inaccuracy in the docstring at `run_tpch.py:578` ("The verdict always agrees with the exit code"):
a no-baseline run with a wrong answer exits 1 while printing `NO-BASELINE`, not `FAIL`. The
failure is still named in the same line and `SKILL.md`'s verdict table calls NO-BASELINE "Not a
pass", so this is a stale comment rather than a hole — noted, not filed as a defect.

Vacuous and unported cannot be absorbed into the headline: `meaningful` subtracts every vacuous
query (`:389`), and `unported` is derived from cells with `correct_modes[q] == 0` (`:305-307`), so
a query cannot appear in both. `gate_line` prints all three counts unconditionally (`:552-556`).

### 8. Target 3 — q17/q21 are genuinely refused and pinned by message. But the README still records Q17 as supported, and the actual Q17 text does not run.

The refusals are real, not skips. All four cells for each query carry a non-zero exit and a
message, and `classify` (`run_tpch.py:210-224`) only reaches UNPORTED/REFUSED_EXPECTED via
`rc != 0`:

```
q17 row-volcano / col-volcano  REFUSED_EXPECTED  "correlated subqueries are decorrelated to a semi-join"
q17 col-vec / col-vec-noopt    UNPORTED          "correlated subquery: a correlated scalar subquery is
                                                  decorrelated only when its select list is a single aggregate"
q21 row-volcano / col-volcano  REFUSED_EXPECTED  "multi-way joins are not supported on the Volcano path"
q21 col-vec / col-vec-noopt    UNPORTED          "correlated subquery: only an equality between two columns
                                                  can become a join key"
```

Each message is emitted by name from `src/planner/subquery_decorrelation.cc:187-192` (q17) and
`:234` / the equality-key check (q21), and `internal:` is checked first at `run_tpch.py:213` so a
crash cannot be absorbed into either bucket. `docs/week-35-plan.md:117-124` records both reasons
verbatim, including "the standard text is `0.2 * AVG(l_quantity)`". **That note is accurate.**

**The gap is NOT what the README claims, however.** `subquery_decorrelation.cc:186-192` requires
`found.size() == 1 && found[0] == body.select_list[0].get()` — the select-list expression must
*be* the aggregate node. TPC-H Q17's spec text puts the constant **inside** the subquery
(`SELECT 0.2 * AVG(l_quantity) …`), which is a `*` node wrapping the aggregate, so it is refused.
Week 33/34 built and validated the constant-**outside** form — `docs/week-33-plan.md:719` states
the target shape as `WHERE l.speed > 0.2 * (SELECT AVG(l2.speed) …)`. The two forms are
semantically identical; only the parenthesis position differs. So the mechanism works and the
spec's Q17 does not, and three README lines do not say so:

- `README.md:70` — "a correlated **scalar** subquery over an aggregate body decorrelates … (Week
  34, **TPC-H Q17**)"
- `README.md:1768-1769` — "**Checkpoint:** … ✅ — and **Q17 with them**, which Week 33 recorded as
  a checkpoint miss and handed here."
- `README.md:2139` — "| 34 | … | **Q17's correlated scalar supported ✅** |"

Against a harness that now reports q17 as 0-mode UNPORTED, these read as a met checkpoint for a
query that does not run. Week 35 corrected the *harness* figure (20 -> 17) but left the Week 34
checkpoint text uncorrected. This is exactly the overstatement the round-2 brief asks about: the
mechanism is real, the named query is not covered by it.

**Fix (either, not both):** amend the three README lines to "the Q17 *shape* (constant outside the
subquery); TPC-H's Q17 text puts the constant inside and is refused", or lift the
`found[0] == select_list[0]` restriction to allow an aggregate under a constant-only expression
tree — at which point q17's `0.2 * AVG(...)` decorrelates and the figure becomes 18/22.

### 9. Target 5 — the recovered WIP (`cc3e4ce`) left nothing incoherent.

`cc3e4ce` committed `docs/tpch-baseline.json` plus the mutation-check edits unvalidated, with the
commit body stating "Nobody has checked it." Checked the recovery for residue:

- `python3 python_tools/tpch_queries.py` exits 0: all 22 templates render and all 22 fragments
  apply exactly once. No half-edited fragment survived.
- `docs/tpch-baseline.json` and `docs/tpch-sf0.01-report.json` agree: same 17 `meaningful`, same
  `modes` map, same `vacuous` {q2, q18, q19}, same `unported` {q17, q21}. The baseline was
  regenerated from a real run at `c1ff8ae`, not carried forward from the WIP.
- The baseline holds exactly the seven keys `compare_baseline` and `gate_line` read
  (`meaningful, mismatched, modes, mutation_broken, unexplained, unported, vacuous`) — no orphan
  key, no missing key that `.get()` would silently default.
- The one thing `cc3e4ce` flagged as unchecked — "17 … Nobody has checked it" — I reproduced
  independently for the three vacuity verdicts (finding 4). The arithmetic is sound; findings 1
  and 2 are about *which* queries belong in the vacuous set, not about the recovery.

No incoherence found.

## Severity tally

- **Blocker: 1** — finding 1 (q2's INERT is a parameter artifact; 172/250 param combos
  discriminate; the figure understates by one query and leaves the correlated-scalar mechanism
  exercised by nothing).
- **Major: 3** — finding 2 (q19 ALL_NULL, likewise parameter-fixable, costs a 4-mode query),
  finding 5 (`--baseline` + `--write-baseline` on one path = unconditional PASS),
  finding 6 (narrowed run prints a full-shaped PASS with a 100% figure).
- **Major, documentation of a measured claim: 1** — finding 8 (README records Q17 as supported;
  the spec's Q17 text is refused, and only the constant-outside shape works).
- **Minor: 1** — finding 3 (q18's "unreachable at SF=0.01" is true only for the 300 threshold).
- **Verified clean:** the mutation check's hardened multiset comparison and fragment-application
  guard (4), the fifth gate's three regression semantics and NO-BASELINE handling (7), q17/q21
  refusals genuinely pinned by message (8, first half), the recovered WIP (9).

## Not reached

- Per-query hand verification beyond q2, q18 and q19 (the other 19 verdicts were checked
  structurally — fragment uniqueness, comparison semantics, LIMIT interaction — not re-derived
  from SQLite one at a time).
- No engine run of any kind: a gate owns `build/`, so every measurement above is SQLite-only or
  pure-Python. The `cells` grid in `docs/tpch-sf0.01-report.json` is taken as given (round 1
  re-derived it and re-ran two queries against the binary).
- `python_tools/generate_tpch.py`'s Week 35 change (`f7c6cdb`, the seeded Q16 phrase) beyond
  confirming the week-35 plan's claim that q16 now discriminates.
- The `--time` path and `--fingerprint-all`.
