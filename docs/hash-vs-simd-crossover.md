# Week 23.5 — Hash Join vs SIMD Loop Join Crossover

Calibration of `CPU_SIMD_COMPARE` in `src/planner/cost_model.h`: the measured
build size at which the hash join's wall-clock overtakes the SIMD loop join's,
which is the point the cost model must reproduce for the optimizer to pick the
right algorithm.

## Method

`benchmarks/calibrate_join_crossover.cc` constructs `VecHashJoinNode` and
`VecSimdLoopJoinNode` (SIMD and scalar reference paths) directly over synthetic
two-INT-column tables — no CLI, parse, or result-cache noise. Every probe key
matches exactly one build row (FK-join shape, matching how `driver_id` behaves
in the generated data). Timing covers `open()` (the build phase) through drain.
1 warmup + average of 5 runs, Release build (`-O3`), Apple M4 Pro (ARM NEON,
2 × int64 lanes per compare).

```
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target calibrate_join_crossover -j
./build-release/calibrate_join_crossover
```

## Results (avg ms, probe = 100k rows)

| Build rows | Hash | SIMD loop | Scalar loop |
|---|---|---|---|
| 2 | 8.89 | 5.37 | 4.89 |
| 4 | 6.25 | 5.03 | 4.94 |
| 8 | 6.77 | 5.21 | 5.12 |
| 16 | 6.70 | 5.62 | 5.69 |
| 32 | 6.90 | **5.83** | 6.13 |
| 64 | **6.90** | 7.22 | 6.93 |
| 128 | 6.92 | 8.99 | 8.99 |
| 256 | 6.90 | 12.23 | 14.11 |
| 512 | 7.57 | 19.39 | 20.93 |
| 1024 | 7.53 | 30.08 | 33.28 |
| 2048 | 7.91 | 54.46 | 57.50 |
| 4096 | 8.22 | 101.97 | 107.28 |

At probe = 1M rows the picture is the same: SIMD wins through 32 build rows
(58.5 vs 69.7 ms), loses from 64 (74.8 vs 68.3 ms).

## Crossover and constant

Linear interpolation between the 32- and 64-row samples puts the crossover at
**≈ 57 build rows** (100k probe) and **≈ 52** (1M probe). That the crossover
barely moves across a 10× probe-size change validates the model form: setting
`hashJoinCost = simdLoopJoinCost` (memory terms cancel — both retain the same
payload) gives

```
2B + P = B·CPU_LOOP_BUILD + B·P·CPU_SIMD_COMPARE
B* = P / (c·P − 1) ≈ 1 / CPU_SIMD_COMPARE      for c·P ≫ 1
```

— probe-independent for realistic probe sizes. `CPU_SIMD_COMPARE = 0.02`
models B* = 50, matching the measured 52–57. (The placeholder 0.05 before
calibration modelled B* = 20 — the loop join was being under-selected.)

## Observations

- **The hash join is nearly flat in build size** (6.2–8.2 ms from 4 to 4096
  build rows; the 2-row sample reads 8.9 ms, but it is also the first
  measurement of the whole run, so warm-up effects can't be excluded): its
  cost is probe-dominated, and each probe pays a
  `Value::toString()` heap allocation plus an `unordered_map` lookup. That
  per-probe overhead is what the loop join deletes, and why it wins below the
  crossover even though it does strictly more comparisons.
- **SIMD vs scalar is modest** (~5–13% at larger builds, a wash below ~32
  rows where per-probe-row overhead dominates the key scan). NEON compares
  only two int64 lanes per instruction, and match emission + payload
  materialization are shared costs. The flat contiguous buffer — not the
  vector width — is most of the win over hashing.
- **AVX2 path status:** compile-checked via x86_64 cross-compilation only;
  it has not executed on x86 hardware. The `SimdMatchesScalarAcrossTailSizes`
  test gates it the day it runs for real.
- **Misestimate risk (accepted, by design):** selection uses estimated
  cardinalities. An underestimated build side can pick the O(B×P) loop join
  on a large B — a performance regression, never a correctness one (the
  operator is exact at any size; `w23_5_*` harness queries pin this). There is
  deliberately no hard row cap: the quadratic cost term is the restriction,
  and `EXPLAIN ANALYZE`'s est-vs-actual column is the designed way to spot
  the miss.
