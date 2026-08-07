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

