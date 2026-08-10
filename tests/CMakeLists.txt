add_executable(swiftql_tests
    test_common.cc
    test_catalog.cc
    test_storage.cc
    test_parser.cc
    test_planner.cc
    test_execution.cc
    test_vectorized.cc
    test_binder.cc
    test_logical_plan.cc
    test_subquery.cc
    test_vec_plan_builder.cc
    test_stats.cc
    test_cardinality.cc
    test_predicate_pushdown.cc
    test_join_enumeration.cc
    test_cost_model.cc
    test_simd_loop_join.cc
    test_sort_tiebreak.cc
    test_key_encoding_parity.cc
    test_int_double_materialization.cc
    test_int_double_type_through_division.cc
    test_int_double_arithmetic_result.cc
    test_chunk_pruner_hint_schema.cc
)

target_link_libraries(swiftql_tests swiftql_lib gtest gtest_main)

# Register with CTest so `ctest` in build/ actually runs the suite. The tests
# resolve "../catalog.json" relative to their working directory, which is why
# WORKING_DIRECTORY is the build root rather than build/tests.
add_test(NAME unit COMMAND swiftql_tests)
set_tests_properties(unit PROPERTIES WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
