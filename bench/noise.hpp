#pragma once

// Test 26, the noise floor. Times an empty region the same way the runner
// times a command, ten times over, and checks how far the p99 moves between
// runs of identical work. Whatever it moves by here, with nothing to
// measure, is the smallest p99 difference a real comparison can claim.
//
// The limit is an absolute number of nanoseconds, not a fraction. The
// no-op's p99 is about 30 ns, so a fraction of it says little about whether
// a 400 ns operation can be measured. See "Noise floor" in the methodology
// doc for where the number comes from.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "runner.hpp"
#include "timing.hpp"

namespace bench {

inline constexpr int kNoiseRuns = 10;
inline constexpr std::size_t kNoiseOps = 1000000;
inline constexpr std::size_t kNoiseWarmup = 100000;

// Largest allowed gap between the highest and lowest p99 across the runs.
inline constexpr double kMaxP99SpreadNs = 10.0;

inline LatencyStats noop_run(std::size_t ops, const TscClock& clock) {
  std::vector<std::uint64_t> ticks;
  ticks.reserve(ops);
  for (std::size_t i = 0; i < ops; ++i) {
    const std::uint64_t start = tsc_now();
    const std::uint64_t end = tsc_now();
    ticks.push_back(end - start);
  }
  return stats_from(ticks, clock);
}

// Highest p99 minus lowest. Needs at least one run.
inline double p99_spread_ns(const std::vector<LatencyStats>& runs) {
  double lo = runs[0].p99, hi = runs[0].p99;
  for (const LatencyStats& r : runs) {
    lo = std::min(lo, r.p99);
    hi = std::max(hi, r.p99);
  }
  return hi - lo;
}

// env_block is the "# key: value" text from environment_block().
inline std::string noise_csv(const std::string& env_block, const std::vector<LatencyStats>& runs, std::size_t ops) {
  const double spread = p99_spread_ns(runs);
  char buf[160];
  std::ostringstream out;
  out << env_block;
  out << "# methodology_version: " << kMethodologyVersion << '\n';
  out << "# test: 26, noise floor, empty timed region\n";
  out << "# ops_per_run: " << ops << '\n';
  out << "# timing: per-op rdtscp+lfence\n";
  std::snprintf(buf, sizeof buf, "# p99_spread_ns: %.1f\n", spread);
  out << buf;
  std::snprintf(buf, sizeof buf, "# max_p99_spread_ns: %.1f\n", kMaxP99SpreadNs);
  out << buf;
  out << "# verdict: " << (spread <= kMaxP99SpreadNs ? "pass" : "fail") << '\n';

  out << "run,p50_ns,p99_ns,p999_ns,max_ns\n";
  for (std::size_t i = 0; i < runs.size(); ++i) {
    const LatencyStats& s = runs[i];
    std::snprintf(buf, sizeof buf, "%zu,%.1f,%.1f,%.1f,%.1f\n", i + 1, s.p50, s.p99, s.p999, s.max);
    out << buf;
  }
  return out.str();
}

}  // namespace bench
