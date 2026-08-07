# Week 35 — TPC-H Data + Harness

> **Teaching plan, not an implementation.** No code in this document is meant to
> be pasted into the tree. Every snippet is illustrative and written in this
> project's existing patterns and naming so that the shape is recognisable when
> you do write it.

**README bullets for this week:**

- Add the TPC-H schema, pipe-delimited loader, and scale-factor workflow
- Add parameterized queries, warmups, repetitions, and reference comparison

**Checkpoint:** TPC-H data generation and automated query runs are reproducible.

**What makes this week pivotal.** Every week from 26 onward has treated "TPC-H
22/22" as design context while being unable to *measure* it. Week 36 is the week
that measures it. So the harness built here is what every Week 36 claim rests
on, and a harness that reports a number without reporting the conditions under
which the number was obtained is worse than no harness: it converts four known,
written-down capability boundaries into an unqualified pass.

---

## Task list

1. [The TPC-H schema in a three-type engine](#task-1--the-tpc-h-schema-in-a-three-type-engine) — eight tables, no `DATE`, no `DECIMAL`, and a second catalog rather than a second copy of the first
2. [The pipe-delimited loader](#task-2--the-pipe-delimited-loader) — `CSVLoader` hard-codes a comma *and* unconditionally eats line 1; `.tbl` has neither a header nor a clean line end
3. [The scale-factor workflow](#task-3--the-scale-factor-workflow) — making a 500-row fixture a first-class artifact instead of a second catalog to keep in sync
4. [Parameterized queries, warmups, repetitions](#task-4--parameterized-queries-warmups-repetitions) — the run harness, and what its timers must exclude
5. [Reference comparison, and what the oracle cannot check](#task-5--reference-comparison-and-what-the-oracle-cannot-check) — a catalog-driven `load_sqlite`, numeric tolerance, and two named blind spots
6. [Reporting mode coverage honestly](#task-6--reporting-mode-coverage-honestly) — the per-query mode matrix that stops "22/22" from being a lie, and Q22's provenance
7. [Randomized result differencing at SF-small](#task-7--randomized-result-differencing-at-sf-small) — Week 28's deferred gap, with its two named traps built in from the start
8. [The behavioural rejection sweep and the standing sweep rule](#task-8--the-behavioural-rejection-sweep-and-the-standing-sweep-rule) — Week 34's harness lesson, automated

---

*(sections follow)*
