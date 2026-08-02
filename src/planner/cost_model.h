#pragma once

// Week 22 — abstract cost model for physical plan selection.
// Costs are unitless and comparable only to each other (like System R's
// page/CPU weights), never wall-clock. Consumed by VectorizedPlanBuilder for
// the hash-join build-side choice; hashJoinCost is intended for reuse by
// Week 28 join enumeration.

// per-row CPU weights. Build (allocate + insert) is dearer than probe (lookup),
// which is what pushes the smaller input onto the build side.
constexpr double CPU_HASH_BUILD = 2.0;   // cost to insert one row into the hash table
constexpr double CPU_HASH_PROBE = 1.0;   // cost to probe one row against it

// hash-table memory footprint penalty, applied per build-side byte
constexpr double MEM_PER_BYTE   = 0.001;

// cost of building `build_rows` (each `build_width` bytes) and probing with
// `probe_rows`. Order-sensitive: swapping the two sides yields the other build
// assignment, so the caller costs both and keeps the cheaper.
double hashJoinCost(double build_rows, double build_width, double probe_rows);

// Week 23.5 — SIMD small-build loop join costing. The probe term is quadratic
// (every probe row scans every build key), which is what confines SIMD
// selection to small build sides — no hard row cap needed. CPU_SIMD_COMPARE
// is calibrated so the modelled hash/SIMD crossover matches the measured
// on-device crossover (docs/hash-vs-simd-crossover.md); for large probes the
// crossover build size is ~ 1 / CPU_SIMD_COMPARE rows.
constexpr double CPU_LOOP_BUILD   = 1.0;    // append key + payload row to flat buffers
constexpr double CPU_SIMD_COMPARE = 0.02;   // one probe-key-vs-build-key comparison — calibrated:
                                            // measured crossover ≈ 52-57 build rows on Apple M4 Pro
                                            // at both 100k and 1M probe rows; 0.02 models B* = 50

// cost of loop-joining `build_rows` keys (each row `build_width` bytes of
// retained payload) against `probe_rows`. Same unitless scale as hashJoinCost
// so the two are directly comparable.
double simdLoopJoinCost(double build_rows, double build_width, double probe_rows);
