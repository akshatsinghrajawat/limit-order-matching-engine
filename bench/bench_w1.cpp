// Usage: bench_w1 <label> [seed] [measured_ops] [cpu] > bench/results/<name>.csv
//
// Numbers worth keeping come from a build with -DENABLE_SANITIZERS=OFF
// -DCMAKE_BUILD_TYPE=Release, on a core that is otherwise idle. Pass the
// core as the last argument so the process pins itself to it; without one
// the results file says "not pinned".
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "env.hpp"
#include "runner.hpp"
#include "timing.hpp"
#include "workload.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs("usage: bench_w1 <label> [seed] [measured_ops] [cpu]\n", stderr);
    return 2;
  }
  bench::RunConfig cfg;
  cfg.label = argv[1];
  if (argc > 2) cfg.seed = std::strtoull(argv[2], nullptr, 10);
  if (argc > 3) cfg.measured = std::strtoull(argv[3], nullptr, 10);
  const int cpu = argc > 4 ? std::atoi(argv[4]) : -1;

  if (std::string(BENCH_FLAGS).find("-fsanitize") != std::string::npos) {
    std::fputs("warning: sanitizers are on, these numbers are not publishable\n", stderr);
  }
  if (cpu >= 0 && !bench::pin_to_cpu(cpu)) {
    std::fprintf(stderr, "could not pin to cpu %d\n", cpu);
    return 1;
  }

  // Calibrated after pinning so the rate is measured on the core being used.
  const bench::TscClock clock;
  const auto cmds = bench::generate(bench::W1Params{}, cfg.seed, cfg.warmup + cfg.measured);

  const bench::KindCounts n = bench::count_kinds(cmds, cfg.warmup);
  if (!bench::p999_has_enough_samples(n.limit) || !bench::p999_has_enough_samples(n.cancel)) {
    std::fprintf(stderr,
                 "%zu measured ops leave %zu limit and %zu cancel samples; the p99.9 bucket needs at least %zu in each "
                 "(%zu samples per kind)\n",
                 cfg.measured, n.limit, n.cancel, bench::kMinTailSamples, bench::kMinTailSamples * 1000);
    return 1;
  }

  const bench::RunResult result = bench::run(cmds, cfg.warmup, clock);
  std::fputs(bench::results_csv(bench::environment_block(clock), cfg, result).c_str(), stdout);
}
