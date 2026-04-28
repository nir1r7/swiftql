cmake_minimum_required(VERSION 3.20)
project(SwiftQL CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# GoogleTest via FetchContent (downloads automatically)
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# The engine as a library (everything in src/)
add_library(swiftql_lib
    src/common/value.cc
    src/common/schema.cc
)
target_include_directories(swiftql_lib PUBLIC include src)

# Test binary
add_subdirectory(tests)