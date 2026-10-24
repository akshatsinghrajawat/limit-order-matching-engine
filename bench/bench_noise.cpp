// Usage: bench_noise [cpu] > bench/results/<name>.csv
//
// Test 26. Run it on the machine and core that will produce the numbers,
// from the same kind of build as bench_w1 (-DENABLE_SANITIZERS=OFF,
// Release). Exits 1 if the p99 spread is over the limit, and no benchmark
// number from that setup should be trusted until it passes.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "env.hpp"
#include "noise.hpp"
#include "timing.hpp"

int main(int argc, char** argv) {
  const int cpu = argc > 1 ? std::atoi(argv[1]) : -1;

  if (std::string(BENCH_FLAGS).find("-fsanitize") != std::string::npos) {
    std::fputs("warning: sanitizers are on, this says nothing about the benchmark build\n", stderr);
  }
  if (cpu >= 0 && !bench::pin_to_cpu(cpu)) {
    std::fprintf(stderr, "could not pin to cpu %d\n", cpu);
    return 1;
  }

  const bench::TscClock clock;
  bench::noop_run(bench::kNoiseWarmup, clock);

  std::vector<bench::LatencyStats> runs;
  for (int i = 0; i < bench::kNoiseRuns; ++i) runs.push_back(bench::noop_run(bench::kNoiseOps, clock));

  const std::string csv = bench::noise_csv(bench::environment_block(clock), runs, bench::kNoiseOps);
  std::fputs(csv.c_str(), stdout);

  const double spread = bench::p99_spread_ns(runs);
  if (spread > bench::kMaxP99SpreadNs) {
    std::fprintf(stderr, "p99 moved by %.1f ns across runs, limit is %.1f ns\n", spread, bench::kMaxP99SpreadNs);
    return 1;
  }
}
