# Seam audit — engine divergence (Volcano vs vectorized), PASS 3

Repo `/home/user/swiftql`, branch `claude/phase5-week26-qomtkb`, HEAD `922ca15`.
Predecessors read in full: `seam-engine-divergence-pass-1.md`, `seam-engine-divergence-pass-2.md`.

Status: IN PROGRESS — written incrementally, appended as each item is confirmed.

Gate state at start (reported by the orchestrator, on this exact tree): build 56 TUs / 0 warnings;
unit 823/823; SQLite oracle 1496 passed / 0 failed / 0 errors; regression 318 incl. 119
`optimized == --no-optimize` checks, 0 divergences; TPC-H 20/22 meaningful, baseline md5 unchanged.

---

## Part A — verifying pass 2's fixes

### A0. What the two commits actually changed

- `7b84952` (record-only; content landed in `733596e`/`119bf75`) — `src/execution/sort_comparator.h`,
  called by `SortNode` (plan_nodes.cc:528) and `VecSortNode` (vec_sort_node.cc:48) and by nothing
  else. When every declared `ORDER BY` key ties, `rowLess` compares the whole row column by column
  in schema order, ascending, over `min(schema.size(), a.size(), b.size())` columns, with
  `compareForTieBreak` (a widened `compareForSort` that orders any number before any string instead
  of throwing).
- `70570dc` — `Planner::plan` now applies `narrowRows` on the row-storage legs so both storage legs
  hand `SeqScanNode` the same `buildScanSchema` result. Plus two comment sweeps.

Operator order is identical in both builders — `planner.cc:409-455` and `logical_plan.cc:1098-1134`
both produce `... -> Sort -> Project -> Distinct -> Limit`.

---

### A-1 FINDING **E-8 (BLOCKER)** — the fix is in the sort comparator, so it does not fire when
### there is no sort. **Delete the `ORDER BY` from pass 2's own repro and E-1 and E-1b both come
### straight back, at HEAD, with the gate green.**

Pass 2's E-1/E-1b repro carried `ORDER BY MIN(l.season)`. That clause does no work in the query —
every surviving group has `MIN(season) = 2022`, which is exactly why it was a *tie* at the cut.
Removing it removes the only node that calls `sort_comparator::rowLess`, and the cut then lands
directly on `HashAggregateNode`/`VecHashAggregateNode` first-encounter order, which is the join's
probe order, which is the build-side choice the two engines still make by different rules
(`planner.cc:392` raw counts vs `vectorized_plan_builder.cc:527` post-pushdown estimates).

**Run at HEAD `922ca15`:**

```sql
SELECT d.team, MIN(l.season)
FROM drivers d JOIN laps l ON d.driver_id = l.driver_id
WHERE l.season = 2022 AND l.season = 2022 AND l.season = 2022
  AND l.season = 2022 AND l.season = 2022 AND l.season = 2022
GROUP BY d.team LIMIT 3
```

```
row-volcano          AlphaTauri 2022 | Alpine     2022 | McLaren 2022
columnar-volcano     AlphaTauri 2022 | Alpine     2022 | McLaren 2022
columnar-vectorized  RedBull    2022 | AlphaTauri 2022 | McLaren 2022   <-- different row SET
columnar-vec-noopt   AlphaTauri 2022 | Alpine     2022 | McLaren 2022
```

`{AlphaTauri, Alpine, McLaren}` vs `{RedBull, AlphaTauri, McLaren}` — byte-for-byte pass 2's E-1,
unfixed.

And the E-1b transport is also intact — the same body as a scalar subquery, `LIMIT 1`, no `ORDER BY`:

```sql
SELECT COUNT(*) FROM laps
WHERE team = (SELECT d.team FROM drivers d JOIN laps l ON d.driver_id = l.driver_id
              WHERE l.season = 2022 AND ... (x6)
              GROUP BY d.team LIMIT 1)
```

```
row-volcano          COUNT(*)  977
columnar-volcano     COUNT(*)  977
columnar-vectorized  COUNT(*) 1536      <-- inner scalar resolved to 'RedBull', not 'AlphaTauri'
columnar-vec-noopt   COUNT(*)  977
SQLite                          977
```

**Why this is a BLOCKER and not a re-report.** It is a *different* failing shape (no `ORDER BY`
anywhere in the query) reaching the *same* consequence, and it is a direct violation of
`optimized == --no-optimize` — the invariant this project gates on with 119 entries in
`run_optimizer_invariant` — expressed as a single scalar differing by 559, not as a row order SQL
leaves unspecified. `normalize()` sorts, so a set difference of this kind IS visible to the
existing harness; it is missed only because no query of this shape is in any list.

**Why the fix missed it.** The fix was chosen at the sort site because that is where pass 2's repro
exhibited it. But `sort_comparator.h`'s own header states the general defect correctly —
"`std::stable_sort` propagates its INPUT order, and input order is a function of the PLAN" — and
then closes only the stable_sort instance. The cut is `LimitNode`/`VecLimitNode`, and nothing
requires a `SortNode` to sit under it. `LIMIT` without `ORDER BY` is a first-class SQL shape.

