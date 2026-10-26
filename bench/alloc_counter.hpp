#pragma once

// Counts calls to the global operator new. Only a program that links the
// alloc_counter library gets the counting operators (bench/alloc_counter.cpp);
// everything else keeps the default ones.
//
// Read it as a difference of two snapshots around the code of interest.
// Taking a snapshot doesn't allocate.
#include <cstdint>

namespace bench {

struct AllocSnapshot {
  std::uint64_t calls = 0;
  std::uint64_t bytes = 0;
};

AllocSnapshot alloc_snapshot();

}  // namespace bench
