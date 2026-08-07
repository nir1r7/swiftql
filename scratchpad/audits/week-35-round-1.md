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

### 2. Re-derived census — the 20/22 is genuinely computed, and it reproduces.

`run_tpch.py:285-308` derives `answered`, `by_count`, `unported` and `pinned` from the `cells`
dict built by the run loop (`run_tpch.py:394-418`); nothing is typed. Re-derived independently
from `docs/tpch-sf0.01-report.json` with my own script (not the harness's printer):

```
22 queries, 88 cells: MATCH 50, REFUSED_EXPECTED 34, UNPORTED 4
answered (>=1 MATCH): 20   4-mode: 5  [q1 q6 q12 q14 q19]
                           2-mode: 15 [q2 q3 q4 q5 q7 q8 q9 q10 q11 q13 q15 q16 q18 q20 q22]
not answered: [q17, q21]
```

**I get the same 20/22, 5 / 15, 34 pinned.** Re-executed `run_tpch.py --queries q1,q18` against
the shipped binary and data: identical cells. So the arithmetic and the mechanism are sound.
The overstatement is not in the counting — it is that one of the 20 (q16, finding 1) asserts
nothing about the feature it exists to test.

A refusal cannot be miscounted as an answer: `classify` (`run_tpch.py:203-229`) returns MATCH
only on `rc == 0` plus a successful `rows_equal`, and every non-zero exit falls through the
`internal:` / boundary / unported ladder. The reverse (an answer miscounted as a refusal) is
also not reachable — a zero exit never enters that ladder.

### 3. Tolerance — `rel_tol=1e-9` does not mask a wrong answer here. Justification checks out.

`compare_against_sqlite.py:1874`: `abs(x-y) > max(abs_tol, rel_tol*max(|x|,|y|))`, with
`rel=1e-9, abs=1e-6` for TPC-H only; the 168 pre-existing diffs keep `rel_tol=0.0, abs_tol=1e-5`
(the default in the signature at `:1840`), so nothing pre-existing is loosened. Verified by
reading the call sites — `run_tpch.py:226` is the only caller passing `rel_tol`.

The masking question: at SF=0.01 the largest TPC-H aggregate is order 1e8, so the relative
budget is at most ~1e-1 absolute. The smallest *genuine* error this engine can make in such a
sum is dropping or adding one lineitem's contribution, and the minimum
`l_extendedprice*(1-l_discount)` in this data is order 1e3 — four orders above the budget. A
wrong `1-l_discount` is a percent-level error. I could not construct a wrong answer that passes
at 1e-9 and I do not believe one exists at this scale.

The stated justification also checks out: `Value::toString` uses `%.15g` (one digit short of a
double round trip) so the text already carries ~1e-15 relative error, and on a 1e8 sum that is
~1e-7 absolute — already at the old 1e-5 threshold before summation-order noise, which over
~60k rows is far larger. Absolute 1e-5 would indeed reject a correct Q1.

**One grounded caveat, input not demonstrated:** `compare_against_sqlite.py:1874` compares
non-finite floats with `abs(x-y) > tol`. For `x = y = nan`, or for two infinities of the same
sign, `abs(x-y)` is `nan` and `nan > tol` is **False**, so the pair compares EQUAL. `normalize`'s
`coerce` (`:1830`) will produce these, because `float("nan")` and `float("inf")` both parse. I
did not produce a TPC-H input that reaches it (SQLite yields NULL, not nan, for 0/0, which would
mismatch against SwiftQL's nan), so this is a latent hole in the comparator rather than a
demonstrated miscount. Grounded in the code, not in an input.

### 4. Q22 fix — correct, and the sibling sweep found no second instance.

`src/planner/subquery_materialization.cc:305-311` now recurses into `stmt.from` and every
`stmt.joins[i].relation` derived body **before** the `has_subquery` fast path at `:313`. Order
matters and the comment is right: the flag is per-block, so an outer query whose only subquery
sits in a derived body has it clear, and recursing after the early return would leave the bug.
`needsSubqueryMaterialization` (`:268-283`) is the whole-tree predicate `main.cc` needed. Both
match the shape `collectQueryTables` (`:118-128`) already had.

Swept every other pass over `SelectStatement` for the same one-updated-sibling shape:
- `foldConstants` (`src/planner/constant_folding.cc:185-201`) walks select_list / where / having
  / order_by / group_by / join conditions and **does not** recurse into a derived body — the
  identical textual shape. **Tested rather than assumed**: `SELECT COUNT(*) FROM (SELECT
  o_orderkey FROM orders WHERE o_orderdate < DATE '1994-01-01' + INTERVAL '1' YEAR) AS d`
  returns 6751, the same as the top-level form and the same as the pre-folded literal. The body
  is planned as its own block and folded there, so this is not a live defect. Recorded so a
  future reader does not re-flag it.
- `substituteGroupKeyRefs` / `buildScanSchema` / `blockOutputSchema` are per-block by
  construction (they take the block's own `table_schema`), so the pairing does not apply.
No second instance found.

### 5. `random_diff.py` — it does discriminate, with one narrowing worth naming.

The `--self-check` mode (`random_diff.py:236-256`) proves the comparator FAILS on row count, a
changed value, column count and **column order**, and passes on row-order-only and float noise.
That is the right answer to "is this differ tuned only to stop crying wolf" — it is not, and
the self-check is the evidence. `run_tsv` compares raw `--format tsv` **positionally**
(`:170-171`), never through `normalize()`'s name-keyed dict, so the Week 18 column-identity
blind spot genuinely does not apply to it. The three traps are handled: no ORDER BY is emitted,
no LIMIT is emitted (`:141-152`), and the row-count bound replaces LIMIT as the cap.

**Narrowing, grounded:** `generate_query` projects only from `rels[:3]`
(`random_diff.py:113-117`) and only the columns `driver_id` and `team`. `driver_id` is also the
join key in every generated join (`:109-114`), so **every projected `rN.driver_id` is equal
across relations by construction** — a plan defect that emitted r5's `driver_id` where r2's
belonged produces identical output. Only `team` discriminates, and only on a laps/drivers pair
(a drivers-drivers join may key on `team`, equalising that too). With 3-8 relations, relations
4-8 are never projected at all. The 40/40 result-preserving figure is real but its
column-identity reach is narrower than "positional comparison" suggests. Not a blocker for the
Week 35 claim; it bounds what the 40/40 licenses Week 36 to say.

### 6. Provenance claim — accurate, and nothing upgrades it.

`data/tpch/sf0.01/PROVENANCE.txt` states the domains are the spec's and the distributions are
not, that the published answer set does not apply, and that SQLite over the same files is the
only oracle. That is accurate for `generate_tpch.py`, and finding 1 is a direct consequence of
it. The harness echoes the file verbatim on every run (`run_tpch.py:383-387, 272-273`) and
repeats "the published TPC-H answer set does not apply to it" in the blind-spot block
(`:344-345`). Report language is "answered correctly" against a named SQLite oracle, and the
`WHAT THIS HARNESS CANNOT CHECK` block is printed before the Q22 section, not buried. I found
no place that says "TPC-H compliant", cites the published answer set, or calls a result correct
without the oracle named. The plan's `## Progress` and README wording match. **No issue.**

## Severity tally

- **Blocker: 1** — finding 1 (q16 vacuous, counted unqualified in the 20).
- **Grounded-in-code, input not demonstrated: 1** — finding 3's nan/inf comparator hole.
- **Bounding note, not a defect: 1** — finding 5's column-identity narrowing.
- Verified clean: census mechanism (2), tolerance (3), Q22 fix + sibling sweep (4), provenance (6).

## Not reached

- Per-cell re-execution of all 88 cells (ran q1/q18 only; the rest re-derived from the
  committed report JSON, which the two re-runs corroborate).
- The `--time` path and the RSS/compression figures.
- `generate_tpch.py`'s referential-integrity claims beyond the two probes above.
