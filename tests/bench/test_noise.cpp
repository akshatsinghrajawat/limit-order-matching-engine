// Checks the pieces of test 26: the spread arithmetic, the verdict, the
// layout of the file, and that an empty timed region measures as a handful
// of nanoseconds. The pass/fail decision on the machine itself is made by
// running bench_noise there, not here, because a loaded CI runner would make
// this test fail for reasons that say nothing about the code.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "noise.hpp"
#include "runner.hpp"
#include "timing.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace bench;

static LatencyStats with_p99(double p99) {
  LatencyStats s;
  s.samples = 1000;
  s.p50 = 25;
  s.p99 = p99;
  s.p999 = p99 + 3;
  s.max = 5000;
  return s;
}

static std::vector<LatencyStats> runs_with_p99s(const std::vector<double>& p99s) {
  std::vector<LatencyStats> runs;
  for (double v : p99s) runs.push_back(with_p99(v));
  return runs;
}

static void spread_is_highest_p99_minus_lowest() {
  CHECK(p99_spread_ns(runs_with_p99s({30})) == 0);
  CHECK(p99_spread_ns(runs_with_p99s({30, 30, 30})) == 0);
  CHECK(p99_spread_ns(runs_with_p99s({34, 28, 31, 30})) == 6);
}

static std::vector<std::string> lines_of(const std::string& s) {
  std::vector<std::string> out;
  std::string line;
  std::istringstream in(s);
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

static void csv_layout_and_verdict() {
  const std::vector<double> steady = {30, 31, 32, 30, 31, 33, 32, 31, 30, 34};
  auto lines = lines_of(noise_csv("# cpu_model: test\n", runs_with_p99s(steady), kNoiseOps));
  CHECK(lines.size() == 1 + 7 + 1 + 10);
  CHECK(lines[0] == "# cpu_model: test");
  CHECK(lines[1] == "# methodology_version: 1");
  CHECK(lines[2] == "# test: 26, noise floor, empty timed region");
  CHECK(lines[3] == "# ops_per_run: 1000000");
  CHECK(lines[4] == "# timing: per-op rdtscp+lfence");
  CHECK(lines[5] == "# p99_spread_ns: 4.0");
  CHECK(lines[6] == "# max_p99_spread_ns: 10.0");
  CHECK(lines[7] == "# verdict: pass");
  CHECK(lines[8] == "run,p50_ns,p99_ns,p999_ns,max_ns");
  CHECK(lines[9] == "1,25.0,30.0,33.0,5000.0");
  CHECK(lines[18].compare(0, 3, "10,") == 0);

  // Exactly at the limit passes, anything above fails.
  auto at_limit = lines_of(noise_csv("", runs_with_p99s({30, 40}), kNoiseOps));
  CHECK(at_limit[6] == "# verdict: pass");
  auto over = lines_of(noise_csv("", runs_with_p99s({30, 40.5}), kNoiseOps));
  CHECK(over[6] == "# verdict: fail");
}

static void an_empty_region_measures_as_a_few_nanoseconds() {
  const TscClock clock;
  const LatencyStats s = noop_run(50000, clock);
  CHECK(s.samples == 50000);
  CHECK(s.p50 > 0 && s.p50 < 1000);
  CHECK(s.p50 <= s.p99 && s.p99 <= s.p999 && s.p999 <= s.max);
}

int main() {
  spread_is_highest_p99_minus_lowest();
  csv_layout_and_verdict();
  an_empty_region_measures_as_a_few_nanoseconds();
  std::puts("noise ok");
}
