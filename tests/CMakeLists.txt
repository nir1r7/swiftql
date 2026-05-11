add_executable(swiftql_tests
    test_common.cc
    test_catalog.cc
    test_storage.cc
    test_parser.cc
    test_planner.cc
    test_execution.cc
)

target_link_libraries(swiftql_tests swiftql_lib gtest gtest_main)