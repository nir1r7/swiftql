---
name: verify
description: SwiftQL full quality gate — build, C++ unit tests, SQLite correctness harness, and regression harness across all storage/execution modes. Run before declaring any change done, before commits, and after any planner/execution/storage edit.
---

# Verify

Run the complete SwiftQL verification loop. Do not declare work done until every gate passes. Report results as a pass/fail table, then diagnose failures autonomously (use the `bisect-stage` skill for correctness failures).

## Prerequisites

- `data/laps.csv` and `data/drivers.csv` must exist. If missing: `python3 python_tools/generate_data.py`
- `build/` must be configured. If missing: `mkdir -p build && cd build && cmake ..`

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
```

## Interpreting results

- Gate 2 failure → unit-level; the failing test name identifies the stage.
- Gate 3/4 failure → correctness drift vs SQLite; note WHICH mode failed (default vs vectorized vs optimizer-invariant) — that localizes the layer. Invoke `bisect-stage`.
- A query passing SQLite but failing the optimizer invariant means an optimizer pass is not result-preserving — always a bug in `predicate_pushdown.cc`, `cost_model.cc`, or `vectorized_plan_builder.cc`, never in the harness. Never weaken the harness to pass.

## Report format

| Gate | Result | Notes |
|---|---|---|
| Build | ✅/❌ | warnings? |
| Unit tests | ✅/❌ | failed test names |
| SQLite oracle | ✅/❌ | failing queries + mode |
| Regression harness | ✅/❌ | failing queries + mode |

If any gate fails: fix, then re-run ALL gates from the top (a fix can regress an earlier gate).
