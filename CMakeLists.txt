cmake_minimum_required(VERSION 3.20)
project(SwiftQL CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# GoogleTest via FetchContent (downloads automatically).
# Fetched over git rather than by archive URL: sandboxed CI/agent environments
# commonly proxy egress and reject the codeload archive endpoints with a 403
# while allowing the git protocol. Same pinned versions either way.
include(FetchContent)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.14.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

# nlohmann/json via FetchContent (downloads automatically)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# The engine as a library (everything in src/)
add_library(swiftql_lib
    src/common/value.cc
    src/common/schema.cc
    src/catalog/catalog.cc
    src/catalog/table_stats.cc
    src/storage/csv_loader.cc
    src/parser/lexer.cc
    src/parser/parser.cc
    src/planner/validator.cc
    src/planner/binder.cc
    src/planner/constant_folding.cc
    src/planner/join_condition.cc
    src/planner/logical_plan.cc
    src/planner/cardinality_estimator.cc
    src/planner/predicate_pushdown.cc
    src/planner/cost_model.cc
    src/planner/vectorized_plan_builder.cc
    src/planner/planner.cc
    src/planner/plan_nodes.cc
    src/execution/evaluator.cc
    src/execution/expression_executor.cc
    src/storage/csv_to_columnar.cc
    src/execution/vec_scan_node.cc
    src/execution/vec_filter_node.cc
    src/execution/vec_project_node.cc
    src/execution/columnar_eval.cc
    src/execution/vec_limit_node.cc
    src/execution/vec_sort_node.cc
    src/execution/vec_distinct_node.cc
    src/execution/vec_hash_aggregate_node.cc
    src/execution/vec_hash_join_node.cc
    src/execution/vec_simd_loop_join_node.cc
)
target_include_directories(swiftql_lib PUBLIC include src)

# Test binary. enable_testing() must precede add_subdirectory so the tests/
# CMakeLists' add_test() calls register — without it `ctest` in build/ printed
# "No tests were found!!!" and exited 0, which reads as a green gate.
enable_testing()
add_subdirectory(tests)

target_link_libraries(swiftql_lib 
    nlohmann_json::nlohmann_json
)

# CLI executable
add_executable(swiftql src/cli/main.cc)
target_link_libraries(swiftql swiftql_lib)

# Week 23.5 calibration harness (run from a Release build — see file header)
add_executable(calibrate_join_crossover benchmarks/calibrate_join_crossover.cc)
target_link_libraries(calibrate_join_crossover swiftql_lib)