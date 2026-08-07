# Week 35 audit — round 1 (TPC-H data + harness)

Range: `75bb4ce..HEAD` (branch `claude/phase5-week26-qomtkb`), excluding `chore:` commits touching only `scratchpad/`.
Auditing the harness **as an instrument**: does the number it reports mean what it says.

Status: IN PROGRESS (appended as findings are confirmed).

## Findings

### 1. BLOCKER — q16 is a second vacuous pass, unnamed. The NOT IN anti-join it exists to exercise filters nothing.

`python_tools/tpch_queries.py:302-311` (q16 template), counted MATCH in `col-vec`/`col-vec-noopt`
in `docs/tpch-sf0.01-report.json` and reported as one of the 20 answered.

The template's headline feature is Week 32's `NOT IN (subquery)` -> anti-join:

```
AND ps_suppkey NOT IN (SELECT s_suppkey FROM supplier
                       WHERE s_comment LIKE '%Customer%Complaints%')
```

On `data/tpch/sf0.01`, that inner SELECT returns **0 rows** — `generate_tpch.py` synthesises
`s_comment` from a word pool that contains neither "Customer" nor "Complaints", so no supplier
can ever match. Measured against the same SQLite oracle the harness uses:

```
q16 with the NOT IN : 305 rows
q16 with the whole NOT IN predicate DELETED : 305 rows, byte-identical
supplier rows matching the inner LIKE : 0
```

So q16 returns the same rows whether the anti-join works, returns everything, or is not run at
all. It is exactly the property the report calls a "vacuous pass", and it is counted in the 20
without qualification. This is the *provenance* claim biting: the spec's `dbgen` deliberately
seeds ~"Customer...Complaints" comments so this predicate is selective; the synthetic generator
reproduces the *domain* (a comment string) but not that seeded content, and the harness has no
check that a subquery predicate was selective.

`run_tpch.py:398-402` is the only vacuity detector and it tests `len(oracle) == 0` — a
whole-query empty result. It cannot see a non-empty result whose *feature* was inert, so q18
was flagged and q16 was not. The claim "1 vacuous pass named" is therefore false as stated;
at minimum it is 2, and the detector cannot rule out more of the same shape.

Same probe run against the other feature-bearing templates, for the record — these are NOT
vacuous on this data (feature deleted vs. present gives different results):
q2 (correlated MIN) 4 vs 6 rows; q4 (EXISTS semi-join) 545 -> 501 orders survive;
q11 (HAVING + scalar subquery) 79 vs 80; q13 (LEFT JOIN outer-ness) 26 vs 25, and its
`o_comment NOT LIKE` is load-bearing; q15 (`= (SELECT MAX ...)`) 1 vs 100;
q20 (nested IN) 1 vs 4; q22 (NOT EXISTS) differs, 500 customers have no orders.
q18 is genuinely empty (`SUM(l_quantity) > 300` unreachable at SF=0.01), as declared.

**Concrete fix shape:** assert per-query that the feature predicate is selective — e.g. run the
subquery body through the oracle and require a non-empty, non-total result — or record q16 in
the same `VACUOUS PASSES` block as q18. Do not leave it inside the unqualified 20.
