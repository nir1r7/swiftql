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

