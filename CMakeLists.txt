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

# The engine library target is added here once include/engine and src/
# actually contain sources (M1). Intentionally not stubbed out with a
# placeholder .cpp — an empty library target that "builds" proves nothing
# and would need to be deleted the moment real code lands.
#
# add_library(engine ...)
# target_include_directories(engine PUBLIC include)

if(BUILD_TESTS)
  enable_testing()
  # add_subdirectory(tests) — wired up once tests/conformance has content
  # and the oracle exists (M1-M2), matching the milestone exit criteria in
  # README.md rather than the reverse.
endif()
