# A critique of this evaluation, and what was done about it

Written against our own paper. Each criticism is followed by the change made in
response, so this file is a worklist that was worked rather than a disclaimer.

---

## Criticism 1 — medians with no variance, supporting a 10% claim

**The problem.** Every latency in the paper is a median: 5 repetitions, 3 at
SF=1. No spread was ever reported. The headline "1.11x faster than PostgreSQL at
SF=1" rests on a geometric mean of 0.90 — a **10% margin** — and nothing in the
published data shows that 10% exceeds run-to-run variation on a thermally
unconstrained laptop.

The one repeatability check we did run covered **two queries** (q1 spanned
8.38-8.70 ms, q18 61.6-66.0 ms — 3% and 7%) and was then generalised to 66
query-engine cells across three scale factors. A 7% spread on a 10% claim is not
comfortable.

**What was done.** `compare_engines.py` now records every repetition, plus
`min`, `max` and `rel_spread = (max - min) / median`, for both `ms` and
`exec_ms`, in the JSON. A dedicated higher-repetition run at SF=1 tests whether
the PostgreSQL claim survives: `docs/week-38-variance-sf1.json`.

### Result: the criticism was correct and it cost us a headline claim

A 5-repetition run of SwiftQL against PostgreSQL at SF=1
(`docs/week-38-variance-sf1.json`, 21 queries):

| | value |
|---|---|
| geometric mean, medians | **0.931x** |
| SwiftQL relative spread | median **11.5%**, max 104.7% |
| PostgreSQL relative spread | median **9.2%**, max 78.2% |
| worst-case geomean (SwiftQL max vs PG min) | **1.079x — SwiftQL SLOWER** |
| best-case geomean (SwiftQL min vs PG max) | 0.804x |

**The interval spans parity, so "1.11x faster than PostgreSQL at SF=1" is not a
supported claim.** The honest statement is that SwiftQL and default PostgreSQL
are **indistinguishable at SF=1 on this machine**. The three-repetition run that
produced 0.90x, and the earlier one that produced 0.87x, were both inside the
noise the whole time; taking a second sample is what showed it.

Two claims DO survive, because their margins are far outside the spread:

- **vs single-threaded PostgreSQL: 0.54x**, a 1.9x margin against ~11% noise.
- **vs SQLite: 0.33x**, a 3.0x margin. And **vs DuckDB: 16.07x** against us.

The paper and README are corrected accordingly. This is the clearest possible
argument for the criticism that produced it: **the claim we were most pleased
with is the one that did not survive being measured twice.**

**What this still cannot fix.** The machine is a laptop. There is no CPU pinning,
no thermal control, and other processes exist. Reporting spread makes the noise
visible; it does not remove it. Median spreads of 9-12% mean no claim under about
1.3x should be stated as a win.

---

## Criticism 2 — load exclusion is a footnote, not a number

**The problem.** Every latency excludes load. That is correct and it is
defensible — SQLite, PostgreSQL and DuckDB load once into a persistent database,
so timing SwiftQL's per-invocation CSV parse would compare a text parser against
three storage engines.

But SwiftQL is the **only** engine with no persistence, so the exclusion removes
a cost that only it pays. At SF=1 a SwiftQL process spends ~37 seconds re-reading
1.1 GB of text before executing anything. A reader looking at "q19: SwiftQL
53.6 ms, PostgreSQL 13.2 ms" is comparing an engine that must do that first
against one that need not, and the results table said nothing about it.

**What was done.** The harness measures cold start explicitly — total wall time
for one invocation, the timed portion, and the difference — and prints it
directly above the latency table, as well as recording it in the JSON:

```
swiftql cold start: 21.9s wall, of which 21.9s is load EXCLUDED from every row below
```

Measured at SF=1. Note this also corrects a number the paper carried: the load
cost was quoted as 36.8 s from a single earlier observation, and the measured
figure is **21.9 s**. Both are large; only one was measured by this harness.

**What this cannot fix.** SwiftQL remains a query engine rather than a database.
The right comparison for a *system* would include load, and SwiftQL would lose
it by two orders of magnitude. The paper's Limitations section says so; now the
benchmark output says so too.

---

## Criticism 3 — the query port is an uncontrolled variable

**The problem.** Every engine runs SwiftQL's ported query text, which makes the
port a shared input to all four engines and therefore invisible to any
cross-engine check. Week 38 proved this is not hypothetical: **two of the 22
ports were wrong**, one of them (q20) having replaced the query's central
correlated predicate with a constant, and SQLite agreed with SwiftQL on both
because SQLite ran the same text.

We found them only because `dbgen` ships an answer set. That is luck, not
method. And two gaps remain:

1. The gated SF=0.01 suite still renders **deviated parameters** for q2 and q19
   (chosen because the spec's values are vacuous on the small synthetic
   dataset), so the queries the daily gate exercises are not the queries the
   official-answer check validates.
2. Nothing checks the remaining 20 ports structurally. They match the published
   answers at SF=1 under one parameter set, which is strong evidence but not the
   same as verifying that each port expresses the specification's predicate.

**What was done.** The published-answer check is now **gate 6** of the standing
verification, so a port defect cannot survive a routine run again, and the
`verify` skill records how to tell a port defect from an engine defect: ask
SQLite the same query on the same data — if SQLite agrees with SwiftQL, the port
is wrong; if it agrees with the answer file, the engine is.

**What this cannot fix.** Gate 6 only runs at SF=1 on `dbgen` data, because that
is the only configuration the published answers describe. The SF=0.01 gate
remains SQLite-only and therefore remains blind to the class of defect that
produced this criticism.

---

## Also true, and not fixed

- **q15 is excluded from every aggregate** because PostgreSQL returns 0 rows
  where the other three return 1 — an exact equality on a computed `DOUBLE`.
  That makes the SF=1 aggregates 21 queries, not 22, and the exclusion favours
  nobody in particular but does change the mean.
- **The mutation check neuters one predicate per query.** It bounds the
  "meaningful" figure from above and cannot certify a query. q20 is the proof:
  it was reported DISCRIMINATING while its central predicate was missing,
  because the check was aimed elsewhere.
- **`row-volcano` and `col-volcano` answer 5 of 22 queries**, so the
  four-mode correctness figure and the two-mode one are different measurements
  that both get called "22/22" in casual reading.
