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

  # Benchmark-side code lives in bench/, kept apart from include/ for the
  # same reason as test_support: nothing in the engine may depend on it.
  add_library(bench_support INTERFACE)
  target_include_directories(bench_support INTERFACE bench)
  target_link_libraries(bench_support INTERFACE engine)

  # What the environment block records as compiler and flags. The flags come
  # from the directory property so the sanitizer and warning options added
  # with add_compile_options are included, not just CMAKE_CXX_FLAGS.
  get_directory_property(bench_options COMPILE_OPTIONS)
  list(JOIN bench_options " " bench_options)
  string(TOUPPER "${CMAKE_BUILD_TYPE}" bench_build_type)
  string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${bench_build_type}} ${bench_options}" bench_flags)
  target_compile_definitions(bench_support INTERFACE
    BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}"
    BENCH_FLAGS="${bench_flags}")

  add_executable(test_workload tests/bench/test_workload.cpp)
  target_link_libraries(test_workload PRIVATE bench_support)
  add_test(NAME bench_workload_w1 COMMAND test_workload)

  add_executable(test_timing tests/bench/test_timing.cpp)
  target_link_libraries(test_timing PRIVATE bench_support)
  add_test(NAME bench_timing COMMAND test_timing)

  add_executable(test_runner tests/bench/test_runner.cpp)
  target_link_libraries(test_runner PRIVATE bench_support)
  add_test(NAME bench_runner COMMAND test_runner)

  add_executable(test_noise tests/bench/test_noise.cpp)
  target_link_libraries(test_noise PRIVATE bench_support)
  add_test(NAME bench_noise_pieces COMMAND test_noise)

  add_executable(test_overhead tests/bench/test_overhead.cpp)
  target_link_libraries(test_overhead PRIVATE bench_support)
  add_test(NAME bench_overhead_pieces COMMAND test_overhead)

  # Replaces the global operator new with a counting one. Kept as its own
  # library so only the programs that measure allocations get it; linking it
  # anywhere else would change what their allocations cost.
  add_library(alloc_counter STATIC bench/alloc_counter.cpp)
  target_include_directories(alloc_counter PUBLIC bench)

  add_executable(test_alloc_counter tests/bench/test_alloc_counter.cpp)
  target_link_libraries(test_alloc_counter PRIVATE bench_support alloc_counter)
  add_test(NAME bench_alloc_counter COMMAND test_alloc_counter)

  # Not tests, run them by hand. See the usage comment in each source for
  # the build they need to produce numbers worth keeping.
  add_executable(bench_w1 bench/bench_w1.cpp)
  target_link_libraries(bench_w1 PRIVATE bench_support)

  add_executable(bench_noise bench/bench_noise.cpp)
  target_link_libraries(bench_noise PRIVATE bench_support)

  add_executable(bench_overhead bench/bench_overhead.cpp)
  target_link_libraries(bench_overhead PRIVATE bench_support)

  add_executable(bench_allocs bench/bench_allocs.cpp)
  target_link_libraries(bench_allocs PRIVATE bench_support alloc_counter)
endif()
