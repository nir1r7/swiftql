// Week 23.5 — measure the hash-join vs SIMD-loop-join wall-clock crossover.
//
// For each build size B (fixed probe size P), times VecHashJoinNode against
// VecSimdLoopJoinNode (SIMD and scalar reference) on synthetic two-INT-column
// tables where every probe key matches exactly one build row (FK-join shape).
// Emits CSV: probe_rows,build_rows,algo,avg_ms — 1 warmup + 5 timed reps.
//
// Run from a Release build (intrinsics under -O0 measure nothing real):
//   cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
//   cmake --build build-release --target calibrate_join_crossover -j
//   ./build-release/calibrate_join_crossover
//
// The measured crossover B* feeds CPU_SIMD_COMPARE ≈ 1 / B* in cost_model.h
// (docs/hash-vs-simd-crossover.md walks through the inversion).

#include "execution/vec_hash_join_node.h"
#include "execution/vec_simd_loop_join_node.h"
#include "execution/vec_scan_node.h"
#include "storage/columnar_table.h"
#include "common/schema.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

Schema twoIntSchema(const std::string& key, const std::string& val) {
    return Schema({{key, TypeId::INT}, {val, TypeId::INT}});
}

// key column = i % key_mod (every probe key hits exactly one unique build key
// when the build table uses key_mod == n), val column = payload
ColumnarTable makeIntTable(const Schema& schema, int n, int key_mod) {
    ColumnarTable ct(schema, n);
    std::vector<int64_t> keys, vals;
    keys.reserve(n);
    vals.reserve(n);
    for (int i = 0; i < n; ++i) {
        keys.push_back(i % key_mod);
        vals.push_back(i);
    }
    ct.columns[schema.column(0).name] = std::move(keys);
    ct.columns[schema.column(1).name] = std::move(vals);
    return ct;
}

enum class Algo { HASH, SIMD, SCALAR };

const char* algoName(Algo a) {
    switch (a) {
        case Algo::HASH: return "hash";
        case Algo::SIMD: return "simd";
        case Algo::SCALAR: return "scalar";
    }
    return "?";
}

// One full join execution: node construction (table copies) is outside the
// timed region; open() — which runs the build phase — and the drain are inside.
double timedRunMs(Algo algo, const Schema& probe_s, const ColumnarTable& probe_t,
                  const Schema& build_s, const ColumnarTable& build_t,
                  const Schema& out_s) {
    auto probe = std::make_unique<VecScanNode>("p", probe_t, probe_s);
    auto build = std::make_unique<VecScanNode>("b", build_t, build_s);

    std::unique_ptr<VecPlanNode> join;
    if (algo == Algo::HASH) {
        join = std::make_unique<VecHashJoinNode>(
            std::move(probe), std::move(build), "pkey", "bkey", out_s);
    } else {
        join = std::make_unique<VecSimdLoopJoinNode>(
            std::move(probe), std::move(build), "pkey", "bkey", out_s,
            /*swapped=*/false, /*use_simd=*/algo == Algo::SIMD);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    join->open();
    int64_t rows = 0;
    while (DataChunk* chunk = join->nextChunk()) rows += chunk->num_rows;
    join->close();
    double ms = std::chrono::duration<double, std::milli>(
        std::chrono::high_resolution_clock::now() - t0).count();

    if (rows == 0) std::fprintf(stderr, "warning: join produced 0 rows\n");
    return ms;
}

void sweep(int probe_rows, const std::vector<int>& build_sizes, bool include_scalar) {
    Schema probe_s = twoIntSchema("pkey", "pval");
    Schema build_s = twoIntSchema("bkey", "bval");
    Schema out_s({{"pkey", TypeId::INT}, {"pval", TypeId::INT},
                  {"bkey", TypeId::INT}, {"bval", TypeId::INT}});

    for (int b : build_sizes) {
        ColumnarTable probe_t = makeIntTable(probe_s, probe_rows, b);
        ColumnarTable build_t = makeIntTable(build_s, b, b);

        for (Algo algo : {Algo::HASH, Algo::SIMD, Algo::SCALAR}) {
            if (algo == Algo::SCALAR && !include_scalar) continue;
            timedRunMs(algo, probe_s, probe_t, build_s, build_t, out_s);  // warmup
            double total = 0.0;
            constexpr int REPS = 5;
            for (int r = 0; r < REPS; ++r)
                total += timedRunMs(algo, probe_s, probe_t, build_s, build_t, out_s);
            std::printf("%d,%d,%s,%.3f\n", probe_rows, b, algoName(algo), total / REPS);
            std::fflush(stdout);
        }
    }
}

} // namespace

int main() {
    std::printf("probe_rows,build_rows,algo,avg_ms\n");
    // full sweep with the scalar reference at the primary probe size
    sweep(100000, {2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096}, true);
    // second probe size to check the crossover is probe-independent (model
    // validation); scalar skipped — quadratic wall-clock adds nothing here
    sweep(1000000, {8, 16, 32, 64, 128, 256, 512, 1024}, false);
    return 0;
}
