cmake_minimum_required(VERSION 3.20)
project(SwiftQL CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# GoogleTest via FetchContent (downloads automatically)
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
FetchContent_MakeAvailable(googletest)

# nlohmann/json via FetchContent (downloads automatically)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# The engine as a library (everything in src/)
add_library(swiftql_lib
    src/common/value.cc
    src/common/schema.cc
    src/catalog/catalog.cc
    src/storage/csv_loader.cc
    src/parser/lexer.cc
    src/parser/parser.cc
    src/planner/validator.cc
)
target_include_directories(swiftql_lib PUBLIC include src)

# Test binary
add_subdirectory(tests)

target_link_libraries(swiftql_lib 
    nlohmann_json::nlohmann_json
)