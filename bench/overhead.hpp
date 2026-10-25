#pragma once

// Test 27, measurement overhead. Puts a number on what two rdtscp reads
// around every command cost, next to what the commands themselves cost.
//
// Three figures per repetition, all from the same stream:
//   batched   ns per operation with one rdtscp pair around the whole
//             measured region, so the timer cost is spread over every
//             operation and vanishes.
//   per-op    mean of the per-operation samples, timer cost included.
//   empty     p50 of an empty timed region, the cost of the timer alone.
//
// This quantifies; it doesn't pass or fail. What it feeds is the decision
// recorded under "Timing" in the methodology doc.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "noise.hpp"
#include "runner.hpp"
#include "timing.hpp"
#include "workload.hpp"

namespace bench {

inline constexpr int kOverheadReps = 5;

// The methodology's line: past this share of the operation cost, per-op
// timing needs a documented answer.
inline constexpr double kOverheadLimitPercent = 1.0;

struct OverheadRep {
  double batched_ns_per_op = 0;
  double per_op_mean_ns = 0;
  double empty_p50_ns = 0;

  double share_percent() const { return 100.0 * empty_p50_ns / batched_ns_per_op; }
};

inline OverheadRep measure_overhead(const std::vector<Cmd>& cmds, std::size_t warmup, const TscClock& clock) {
  const RunResult r = run(cmds, warmup, clock);
  OverheadRep rep;
  rep.batched_ns_per_op = 1e9 / r.ops_per_sec;
  rep.per_op_mean_ns = r.per_op_mean_ns;
  rep.empty_p50_ns = noop_run(kNoiseOps, clock).p50;
  return rep;
}

// env_block is the "# key: value" text from environment_block().
inline std::string overhead_csv(const std::string& env_block, const RunConfig& cfg,
                                const std::vector<OverheadRep>& reps) {
  bool over = false;
  for (const OverheadRep& r : reps) over = over || r.share_percent() > kOverheadLimitPercent;

  char buf[160];
  std::ostringstream out;
  out << env_block;
  out << "# methodology_version: " << kMethodologyVersion << '\n';
  out << "# test: 27, measurement overhead\n";
  out << "# workload: W1\n";
  out << "# seed: " << cfg.seed << '\n';
  out << "# warmup_ops: " << cfg.warmup << '\n';
  out << "# measured_ops: " << cfg.measured << '\n';
  std::snprintf(buf, sizeof buf, "# limit_percent: %.1f\n", kOverheadLimitPercent);
  out << buf;
  out << "# one_percent_rule: " << (over ? "exceeded" : "within") << '\n';

  out << "rep,batched_ns_per_op,per_op_mean_ns,empty_p50_ns,empty_share_percent\n";
  for (std::size_t i = 0; i < reps.size(); ++i) {
    const OverheadRep& r = reps[i];
    std::snprintf(buf, sizeof buf, "%zu,%.1f,%.1f,%.1f,%.1f\n", i + 1, r.batched_ns_per_op, r.per_op_mean_ns,
                  r.empty_p50_ns, r.share_percent());
    out << buf;
  }
  return out.str();
}

}  // namespace bench
