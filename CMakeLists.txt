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
  # UBSan reports and keeps going unless told otherwise, which leaves a
  # diagnostic in a log that nothing fails on.
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                      -fno-omit-frame-pointer)
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
  add_test(NAME differential_fuzz_boundary COMMAND fuzz_diff 10 20000 1 8 boundary)

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

  add_executable(test_zero_alloc tests/bench/test_zero_alloc.cpp)
  target_link_libraries(test_zero_alloc PRIVATE bench_support alloc_counter)
  add_test(NAME zero_allocation COMMAND test_zero_alloc)

  # Links alloc_counter because one of its checks is that the map never
  # allocates after construction.
  add_executable(test_id_map tests/unit/test_id_map.cpp)
  target_link_libraries(test_id_map PRIVATE test_support alloc_counter)
  add_test(NAME unit_id_map COMMAND test_id_map)

  add_executable(test_journal tests/unit/test_journal.cpp)
  target_link_libraries(test_journal PRIVATE test_support)
  add_test(NAME unit_journal COMMAND test_journal WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  # Replaces global operator new to fail allocations, so it does not link
  # alloc_counter.
  add_executable(test_fail_stop tests/unit/test_fail_stop.cpp)
  target_link_libraries(test_fail_stop PRIVATE test_support)
  add_test(NAME unit_fail_stop COMMAND test_fail_stop WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

  add_executable(test_replay tests/replay/test_replay.cpp)
  target_link_libraries(test_replay PRIVATE test_support)
  add_test(NAME replay_deterministic COMMAND test_replay)

  # Replays the journal committed in tests/golden. Regenerate the three files
  # with `test_golden --update` when the output changes on purpose.
  add_executable(test_golden tests/replay/test_golden.cpp)
  target_link_libraries(test_golden PRIVATE test_support)
  target_compile_definitions(test_golden PRIVATE GOLDEN_DIR="${CMAKE_SOURCE_DIR}/tests/golden")
  add_test(NAME golden_replay COMMAND test_golden)

  add_executable(test_price_levels tests/unit/test_price_levels.cpp)
  target_link_libraries(test_price_levels PRIVATE test_support alloc_counter)
  add_test(NAME unit_price_levels COMMAND test_price_levels)

  # Undefined behavior has to end the process. The probe overflows an int and
  # passes only if the sanitized build dies; without sanitizers there is
  # nothing to detect it.
  if(ENABLE_SANITIZERS)
    add_executable(test_ubsan_death tests/unit/test_ubsan_death.cpp)
    add_test(NAME ubsan_aborts COMMAND test_ubsan_death)
    set_tests_properties(ubsan_aborts PROPERTIES WILL_FAIL TRUE)
  endif()

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
