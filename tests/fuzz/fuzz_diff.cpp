// Test 20: seeded differential fuzzer. The same random operation stream is
// applied to the engine and to the oracle; any difference in a result, an
// emitted trade, or the resting book fails the run and prints the seed, the
// operation index and the last operations so the failure can be replayed.
//
//   fuzz_diff [seeds=10] [ops_per_seed=20000] [first_seed=1] [stride=1]
//
// Pool capacity cycles through {2, 8, 32, 128} by seed so exhaustion is
// exercised as often as a deep book. Every operation's result and trades are
// compared; the structural invariants and the full book diff run every
// `stride` operations, because the book diff is O(resting orders) and
// dominates the cost. A state-only divergence is therefore reported up to
// stride-1 operations late; rerun with stride 1 on the printed seed to
// pinpoint it.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "diff.hpp"

int main(int argc, char** argv) {
  int seeds = argc > 1 ? std::atoi(argv[1]) : 10;
  std::uint64_t ops = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 20000;
  std::uint64_t first = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 1;
  std::uint64_t stride = argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1;
  if (stride == 0) stride = 1;
  static const engine::Index capacities[] = {2, 8, 32, 128};

  diff::Stats total;
  for (int n = 0; n < seeds; ++n) {
    std::uint64_t seed = first + static_cast<std::uint64_t>(n);
    engine::Index capacity = capacities[seed % 4];
    diff::Pair pair(capacity);
    diff::Generator gen(seed);
    diff::Op recent[8];

    for (std::uint64_t i = 0; i < ops; ++i) {
      diff::Op op = gen.next();
      recent[i % 8] = op;
      std::string err = pair.apply(op);
      if (err.empty() && (i % stride == stride - 1 || i + 1 == ops)) err = pair.check_state();
      if (err.empty()) continue;

      std::fprintf(stderr, "DIVERGENCE seed=%llu capacity=%u op_index=%llu\n  %s\n",
                   (unsigned long long)seed, capacity, (unsigned long long)i, err.c_str());
      std::uint64_t shown = i + 1 < 8 ? i + 1 : 8;
      for (std::uint64_t k = shown; k > 0; --k) {
        std::fprintf(stderr, "  op %llu: %s\n", (unsigned long long)(i + 1 - k),
                     diff::describe(recent[(i + 1 - k) % 8]).c_str());
      }
      std::fprintf(stderr, "reproduce: fuzz_diff 1 %llu %llu 1\n", (unsigned long long)ops,
                   (unsigned long long)seed);
      return 1;
    }
    total.add(pair.stats);
  }

  std::printf("fuzz_diff ok: seeds=%d ops/seed=%llu total_ops=%llu\n", seeds,
              (unsigned long long)ops, (unsigned long long)total.ops);
  std::printf("  trades=%llu stp=%llu fok_rejected=%llu pool_exhausted=%llu modify_ok=%llu "
              "cancel_ok=%llu invalid_rejected=%llu crossed_checks=%llu\n",
              (unsigned long long)total.trades, (unsigned long long)total.stp,
              (unsigned long long)total.fok_rejected, (unsigned long long)total.pool_exhausted,
              (unsigned long long)total.modify_ok, (unsigned long long)total.cancel_ok,
              (unsigned long long)total.invalid_rejected, (unsigned long long)total.crossed_states);

  // A run that never reached a path says nothing about that path.
  if (total.ops >= 10000 &&
      (total.trades == 0 || total.stp == 0 || total.fok_rejected == 0 || total.pool_exhausted == 0 ||
       total.modify_ok == 0 || total.cancel_ok == 0 || total.invalid_rejected == 0)) {
    std::fprintf(stderr, "fuzz_diff: a path was never exercised; the run is vacuous\n");
    return 2;
  }
  return 0;
}
