// Usage: bench_allocs <label> [seed] [measured_ops] > bench/results/<label>-allocs.csv
//
// Counts allocations per command on W1. The counts are the same under
// sanitizers and in Release, and don't depend on the machine's timing, so
// this needs no pinned core or governor. Use the same seed and operation
// count as bench_w1 so the two files describe the same stream.
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "alloc_profile.hpp"
#include "env.hpp"
#include "runner.hpp"
#include "timing.hpp"
#include "workload.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs("usage: bench_allocs <label> [seed] [measured_ops]\n", stderr);
    return 2;
  }
  bench::RunConfig cfg;
  cfg.label = argv[1];
  if (argc > 2) cfg.seed = std::strtoull(argv[2], nullptr, 10);
  if (argc > 3) cfg.measured = std::strtoull(argv[3], nullptr, 10);

  const bench::TscClock clock;
  const auto cmds = bench::generate(bench::W1Params{}, cfg.seed, cfg.warmup + cfg.measured);
  const bench::AllocProfile profile = bench::profile_allocs(cmds, cfg.warmup);
  std::fputs(bench::allocs_csv(bench::environment_block(clock), cfg, profile).c_str(), stdout);
}
