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
