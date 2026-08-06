#include "cost_model.h"
#include <algorithm>

double hashJoinCost(double build_rows, double build_width, double probe_rows) {
    build_rows = std::max(build_rows, 0.0);   // estimates can be 0 (empty build side)
    probe_rows = std::max(probe_rows, 0.0);
    double cpu    = build_rows * CPU_HASH_BUILD + probe_rows * CPU_HASH_PROBE;
    double memory = build_rows * build_width * MEM_PER_BYTE;
    return cpu + memory;
}

double simdLoopJoinCost(double build_rows, double build_width, double probe_rows) {
    build_rows = std::max(build_rows, 0.0);
    probe_rows = std::max(probe_rows, 0.0);
    double cpu    = build_rows * CPU_LOOP_BUILD + probe_rows * build_rows * CPU_SIMD_COMPARE;
    double memory = build_rows * build_width * MEM_PER_BYTE;  // payload rows persist, as in hash join
    return cpu + memory;
}

double joinOutputCost(double output_rows, double output_width) {
    // same clamps as the two above: estimates can be 0, and a negative one is a
    // corrupt statistic rather than a negative cost
    output_rows  = std::max(output_rows, 0.0);
    output_width = std::max(output_width, 0.0);
    return output_rows * output_width * CPU_MATERIALIZE_BYTE;
}
