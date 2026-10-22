cmake_minimum_required(VERSION 3.16)
project(limit-order-matching-engine CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# S7 requires every test build to run under ASan+UBSan. This is opt-out for
# non-test builds (e.g. release benchmark binaries), not opt-in — the
# default is sanitized.
option(ENABLE_SANITIZERS "Build with -fsanitize=address,undefined" ON)
option(BUILD_TESTS "Build the test suite" ON)

if(ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# The engine is header-only so far (M1) — an interface target keeps
# include paths and sanitizer flags consistent for anything that uses it,
# without pretending there's a compiled library yet.
add_library(engine INTERFACE)
target_include_directories(engine INTERFACE include)

if(BUILD_TESTS)
  enable_testing()
  add_executable(test_basic tests/conformance/test_basic.cpp)
  target_link_libraries(test_basic PRIVATE engine)
  add_test(NAME conformance_basic COMMAND test_basic)

  # Test-only helpers. Kept out of include/ so nothing in the engine can
  # depend on them.
  add_library(test_support INTERFACE)
  target_include_directories(test_support INTERFACE tests/support)
  target_link_libraries(test_support INTERFACE engine)

  add_executable(test_oracle tests/oracle/test_oracle.cpp)
  target_link_libraries(test_oracle PRIVATE test_support)
  add_test(NAME oracle_self_check COMMAND test_oracle)

  add_executable(fuzz_diff tests/fuzz/fuzz_diff.cpp)
  target_link_libraries(fuzz_diff PRIVATE test_support)
  add_test(NAME differential_fuzz COMMAND fuzz_diff 20 20000 1 8)

  add_executable(exhaustive_diff tests/fuzz/exhaustive_diff.cpp)
  target_link_libraries(exhaustive_diff PRIVATE test_support)
  add_test(NAME differential_exhaustive_shallow COMMAND exhaustive_diff 3 3 4)
endif()
