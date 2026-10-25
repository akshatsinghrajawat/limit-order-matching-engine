// Usage: bench_overhead [seed] [measured_ops] [cpu] > bench/results/<name>.csv
//
// Test 27. Run it on the machine and core that will produce the numbers,
// from the same kind of build as bench_w1 (-DENABLE_SANITIZERS=OFF,
// Release). It reports and exits 0; the decision it informs is in the
// methodology doc.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "env.hpp"
#include "overhead.hpp"
#include "runner.hpp"
#include "timing.hpp"
#include "workload.hpp"

int main(int argc, char** argv) {
  bench::RunConfig cfg;
  if (argc > 1) cfg.seed = std::strtoull(argv[1], nullptr, 10);
  if (argc > 2) cfg.measured = std::strtoull(argv[2], nullptr, 10);
  const int cpu = argc > 3 ? std::atoi(argv[3]) : -1;

  if (std::string(BENCH_FLAGS).find("-fsanitize") != std::string::npos) {
    std::fputs("warning: sanitizers are on, these numbers say nothing about the benchmark build\n", stderr);
  }
  if (cpu >= 0 && !bench::pin_to_cpu(cpu)) {
    std::fprintf(stderr, "could not pin to cpu %d\n", cpu);
    return 1;
  }

  const bench::TscClock clock;
  bench::noop_run(bench::kNoiseWarmup, clock);
  const auto cmds = bench::generate(bench::W1Params{}, cfg.seed, cfg.warmup + cfg.measured);

  std::vector<bench::OverheadRep> reps;
  for (int i = 0; i < bench::kOverheadReps; ++i) reps.push_back(bench::measure_overhead(cmds, cfg.warmup, clock));

  std::fputs(bench::overhead_csv(bench::environment_block(clock), cfg, reps).c_str(), stdout);
}
