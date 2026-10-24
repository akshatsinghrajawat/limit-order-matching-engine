#pragma once

// Cycle-counter timing for the benchmark harness.
//
// rdtscp waits for earlier instructions to finish before it reads the
// counter, and the lfence behind it keeps later instructions from starting
// until the read is done. Plain rdtsc does neither, so the work being timed
// can slide across the timestamp.
//
// The counter ticks at a fixed rate whatever the core clock does, so ticks
// only mean something once TscClock has measured that rate against
// steady_clock. env.hpp records whether the CPU promises the rate is fixed.
#include <chrono>
#include <cstdint>
#include <limits>

#if !defined(__x86_64__)
#error "bench/timing.hpp needs x86-64 for rdtscp"
#endif
#include <cpuid.h>
#include <x86intrin.h>

namespace bench {

inline std::uint64_t tsc_now() {
  unsigned aux;
  std::uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

// CPUID leaf 0x80000007, EDX bit 8.
inline bool tsc_is_invariant() {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!__get_cpuid(0x80000007u, &a, &b, &c, &d)) return false;
  return ((d >> 8) & 1u) != 0;
}

struct TscSample {
  std::uint64_t ticks;
  std::int64_t ns;  // steady_clock, since its epoch
};

// One counter read paired with a steady_clock time taken at the same
// moment. The counter read is sandwiched between two clock reads and the
// pair with the narrowest gap wins, so a preemption between the reads costs
// one discarded try instead of skewing the sample.
inline TscSample sample_pair() {
  using clock = std::chrono::steady_clock;
  auto ns_of = [](clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
  };

  TscSample best{0, 0};
  std::int64_t best_gap = std::numeric_limits<std::int64_t>::max();
  for (int i = 0; i < 16; ++i) {
    std::int64_t before = ns_of(clock::now());
    std::uint64_t ticks = tsc_now();
    std::int64_t after = ns_of(clock::now());
    if (after - before < best_gap) {
      best_gap = after - before;
      best = {ticks, before + (after - before) / 2};
    }
  }
  return best;
}

class TscClock {
 public:
  // Spins for about 20 ms. It spins instead of sleeping so the core is awake
  // and at full clock when the first measurement starts.
  TscClock() {
    constexpr std::int64_t kWindowNs = 20'000'000;
    const TscSample start = sample_pair();
    TscSample end = start;
    while (end.ns - start.ns < kWindowNs) end = sample_pair();
    ticks_per_ns_ = static_cast<double>(end.ticks - start.ticks) / static_cast<double>(end.ns - start.ns);
  }

  double ticks_per_ns() const { return ticks_per_ns_; }
  double to_ns(std::uint64_t ticks) const { return static_cast<double>(ticks) / ticks_per_ns_; }

 private:
  double ticks_per_ns_ = 0;
};

}  // namespace bench
