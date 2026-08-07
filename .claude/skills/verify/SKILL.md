---
name: verify
description: SwiftQL full quality gate — build, C++ unit tests, SQLite correctness harness, regression harness across all storage/execution modes, and the TPC-H harness against its recorded baseline. Run before declaring any change done, before commits, and after any planner/execution/storage edit.
---

# Verify

Run the complete SwiftQL verification loop. Do not declare work done until every gate passes. Report results as a pass/fail table, then diagnose failures autonomously (use the `bisect-stage` skill for correctness failures).

## Prerequisites

- `data/laps.csv` and `data/drivers.csv` must exist. If missing: `python3 python_tools/generate_data.py`
- `build/` must be configured. If missing: `mkdir -p build && cd build && cmake ..`
- `data/tpch/sf0.01/` must exist (gate 5). `data/tpch/` is **gitignored**, so a fresh clone has none. The generator is seeded, so regenerating reproduces the exact files `docs/tpch-baseline.json` was recorded against: `python3 python_tools/generate_tpch.py --scale 0.01 --out-dir data/tpch/sf0.01`. Gate 5 exits non-zero and names this command if the catalog is absent — a gate that could not run is never reported as one that passed.

## Gates (run in order, from repo root)

```bash
# 1. Build — must be warning-clean for changed files
#    nproc on Linux, sysctl on macOS. Without the fallback the substitution is
#    empty on Linux and `-j` alone means unbounded parallelism, which OOMs a
#    small container.
cmake --build build -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)

# 2. C++ unit tests — MUST run from build/ (tests resolve ../catalog.json relative to CWD)
cd build && ./tests/swiftql_tests && cd ..

# 3. SQLite correctness oracle (repo root)
python3 python_tools/compare_against_sqlite.py

# 4. Regression harness — covers default mode, vectorized mode, and the
#    optimizer invariant (optimized == --no-optimize) internally
python3 python_tools/test_new_queries.py

# 5. TPC-H harness against its recorded baseline. ~5 MINUTES on sf0.01 — the
#    honest cost, see "Gate 5 runtime" below. --baseline is NOT optional: it is
#    what turns a report into a gate. Exits non-zero on a regression.
python3 python_tools/run_tpch.py \
    --catalog data/tpch/sf0.01/catalog.json \
    --baseline docs/tpch-baseline.json
```

## Gate 5 — TPC-H

Gates 1–4 never invoke `run_tpch.py`, so before this step a verifier could report
GREEN while TPC-H was broken. Phase 5's whole goal is TPC-H, so it gets its own
line in the verdict block and cannot be omitted by accident.

**Report the last line of the run verbatim.** The harness prints it as
`GATE tpch: ...` on purpose — copy it, never retype a count:

```
tpch:       PASS (20/22 meaningful vs SQLite: 5 in all four modes, 15 vectorized-only; 1 vacuous; 1 unported)
```

- **meaningful** — matched SQLite *and* survived the mutation check (neutering
  the query's characteristic predicate changes its answer). This is the figure
  Week 36 raises.
- **vacuous** — matched SQLite while asserting nothing (`EMPTY`, `ALL_NULL`,
  `INERT`). Counted separately, never folded into the headline.
- **unported** — a dialect or decorrelation gap; not a failure, it is Week 36's
  worklist.
- The mode split is load-bearing: `Planner::plan` builds one join, so most
  queries answer on the two vectorized modes only. A count without the split
  reads as coverage the engine does not have.

**Claim wording.** `dbgen` was unavailable; the generator reproduces the spec's
value domains but not its distributions, and `PROVENANCE.txt` records that the
published TPC-H answer set does not apply. SQLite over the same files is the only
oracle. Write **"matches SQLite"** — never "correct", never "TPC-H compliant".

**Three verdicts, matching the exit code:**

| Verdict | Meaning |
|---|---|
| `PASS` | No regression against `docs/tpch-baseline.json`. An `-- IMPROVED` suffix also passes: something newly answers, so refresh the baseline in the same commit as the improvement — a **second, separate run** with `--write-baseline docs/tpch-baseline.json --json docs/tpch-sf0.01-report.json` and *no* `--baseline`. `--json` is not optional there and the harness now refuses without it: the baseline **is** the report's `summary` key, so refreshing one alone leaves the published pair disagreeing about the headline figure — which is exactly how the report spent Week 36 saying 19/22 with q17 UNPORTED beside a baseline saying 20/22. The harness now writes the baseline only after the comparison and only when the run passed, so a regression can no longer be laundered into the file it was being gated against; a refresh run that also carries `--baseline` will simply refuse to write if anything failed. |
| `FAIL` | **RED.** A wrong answer, an unexplained error, a broken mutation check, or a regression: a query that stopped answering, one that became vacuous, or one that answers in fewer modes than the baseline records. |
| `NO-BASELINE` | `--baseline` was omitted. Not a pass — a run that cannot see a regression. Re-run it properly. |
| `PARTIAL-*` | `--queries` narrowed the run, so the figure is a **subset**, not a measurement of TPC-H. Never quotable in a verdict block whatever the suffix says. The line names how many of the 22 did not run. |

Never edit `docs/tpch-baseline.json` to make this gate pass. The baseline moves
**up** with a real improvement and never down; a drop in the honest number is the
finding, not the obstacle.

### Gate 5 runtime

**It is not fast: ~5 minutes** (22 queries × 4 modes = 88 `swiftql` invocations,
each reloading a 9 MB `lineitem.tbl`, since `main.cc` reloads every table per
invocation). It is not parallelised. sf0.01 is already the small dataset and the
default. A larger set is opt-in and is not the gate: generate it with
`--scale 0.1 --out-dir data/tpch/sf0.1` and pass
`--catalog data/tpch/sf0.1/catalog.json` only when deliberately testing scale.
`docs/tpch-baseline.json` was recorded at sf0.01, and the honest figure is a
property of the *data* — a different scale factor makes different queries
vacuous, so it needs its own baseline rather than being diffed against this one.

Run it anyway on any planner, execution, storage, or TPC-H-query change — that is
every change Phase 5 makes. While iterating on one query, narrow it with
`--queries q9,q17` (the baseline comparison scopes itself to the queries that ran),
but a verdict block may only report a **full 22-query run**. That is no longer
discipline alone: a narrowed run prints `PARTIAL-PASS` / `PARTIAL-FAIL` /
`PARTIAL-NO-BASELINE` and says in the same line how many of the 22 did not run,
so a partial figure cannot be quoted as a full one.

## Interpreting results

- Gate 2 failure → unit-level; the failing test name identifies the stage.
- Gate 3/4 failure → correctness drift vs SQLite; note WHICH mode failed (default vs vectorized vs optimizer-invariant) — that localizes the layer. Invoke `bisect-stage`.
- Gate 5 failure → same drift, on TPC-H shapes gates 3/4 do not reach. The grid printed above the summary gives the query and the mode; `bisect-stage` from there. A query that regressed in *some* modes but not others names the layer directly.
- A query passing SQLite but failing the optimizer invariant means an optimizer pass is not result-preserving — always a bug in `predicate_pushdown.cc`, `cost_model.cc`, or `vectorized_plan_builder.cc`, never in the harness. Never weaken the harness to pass.

## Report format

| Gate | Result | Notes |
|---|---|---|
| Build | ✅/❌ | warnings? |
| Unit tests | ✅/❌ | failed test names |
| SQLite oracle | ✅/❌ | failing queries + mode |
| Regression harness | ✅/❌ | failing queries + mode |
| TPC-H (gate 5) | ✅/❌ | the harness's `GATE tpch:` line, verbatim |

If any gate fails: fix, then re-run ALL gates from the top (a fix can regress an earlier gate).
