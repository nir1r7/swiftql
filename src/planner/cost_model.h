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
// RECALIBRATED IN WEEK 37, AND THE MEASUREMENT NO LONGER SHOWS A CROSSOVER.
//
// Week 23.5 measured B* ≈ 52-57 build rows and set 0.02 to model B* = 50. That
// was correct against the hash join AS IT THEN WAS. Week 37 rewrote
// VecHashJoinNode — column-wise build store, chained index, INT64 key path,
// late-materialized output — making it roughly 3x faster, and re-running
// benchmarks/calibrate_join_crossover.cc afterwards shows the hash join winning
// at EVERY build size the harness tests, down to 2 rows:
//
//     probe=100k   build=2    hash 0.607ms   simd 1.134ms
//     probe=1M     build=8    hash 5.951ms   simd 12.160ms
//     probe=1M     build=1024 hash 6.346ms   simd 295.283ms
//
// So B* < 2 and the regime where the loop join paid has been engineered away.
// 0.5 models B* = 2, which leaves the operator reachable only for a
// single-row build side and is the closest honest expression of the data.
//
// THIS CONSTANT IS DERIVED FROM ANOTHER OPERATOR'S PERFORMANCE, so optimizing
// that operator invalidates it. That is not hypothetical: leaving 0.02 in place
// after the Week 37 rewrite made the optimizer pick the loop join on a 20-row
// build side and run 95.7ms where --no-optimize ran 32.6ms — the optimizer
// making a query 2.9x SLOWER, invisible to every gate, because both plans are
// correct and the gates assert answers. Re-run the calibration harness after any
// change to either join operator.
constexpr double CPU_SIMD_COMPARE = 0.5;    // one probe-key-vs-build-key comparison — calibrated:
                                            // Week 37 re-measurement finds no crossover above
                                            // build=1; 0.5 models B* = 2

// cost of loop-joining `build_rows` keys (each row `build_width` bytes of
// retained payload) against `probe_rows`. Same unitless scale as hashJoinCost
// so the two are directly comparable.
double simdLoopJoinCost(double build_rows, double build_width, double probe_rows);

// Week 28 — data-volume (bytes-materialized) term. Deferred here from Week 22,
// which noted it "cannot change the decision" at single-join scope: output rows
// and output width are invariant under a build-side swap, so the term cancels in
// every Week 22 comparison. It first discriminates in Week 28, where two join
// ORDERINGS produce different intermediate results.
//
// Deliberately NOT folded into hashJoinCost/simdLoopJoinCost. Those two are
// compared against each other, and CPU_SIMD_COMPARE is calibrated against
// measured on-device crossover (docs/hash-vs-simd-crossover.md); adding a term
// to both changes no decision but invalidates the calibration's provenance and
// inflates every cost= string --explain has printed since Week 23.
//
// Derivation: both join operators materialize every output row (VecHashJoinNode
// builds a Row per match, then copies it into a DataChunk). Anchor the weight so
// that materializing one ~40-byte row costs the same as probing one row
// (CPU_HASH_PROBE = 1.0): 1.0 / 40 = 0.025. Re-derive from profiling in Week 37
// if TPC-H orderings disagree with measurement.
constexpr double CPU_MATERIALIZE_BYTE = 0.025;

// cost of emitting `output_rows` rows of `output_width` bytes each. Applied by
// join enumeration once per join in a candidate ordering; symmetric under a
// build-side swap, which is why it lives outside the two algorithm costs.
double joinOutputCost(double output_rows, double output_width);
