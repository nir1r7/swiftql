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
