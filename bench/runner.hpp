#pragma once

// Runs a command stream against the engine and turns the timings into the
// results file described in docs/benchmark-methodology.md.
//
// Two passes over the same commands, each on a fresh Book after its own
// warmup. One is timed per operation for the latency percentiles. The other
// is timed as a whole for throughput, so that figure doesn't carry the cost
// of two rdtscp reads per command. Every correct engine produces the same
// trades for the same commands (S7), so run() checks that the passes agree.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "engine/book.hpp"
#include "timing.hpp"
#include "workload.hpp"

namespace bench {

// Bump when the way numbers are produced changes, and say what changed in
// the methodology doc. Every results file records it.
inline constexpr int kMethodologyVersion = 1;

// The p99.9 bucket must hold at least this many samples.
inline constexpr std::size_t kMinTailSamples = 100;

// Same as the generator's book. Far above W1's ~3,000 resting orders.
inline constexpr engine::Index kBookCapacity = 1u << 16;

struct RunConfig {
  std::string label;
  std::uint64_t seed = 1;
  std::size_t warmup = 100000;
  std::size_t measured = 1000000;
  // p50 of an empty timed region on this machine, measured at the start of
  // the run (noop_run in noise.hpp). Recorded, not subtracted.
  double timer_overhead_ns = 0;
};

struct LatencyStats {
  std::size_t samples = 0;
  double p50 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;  // ns
};

struct RunResult {
  LatencyStats limit, cancel;
  double ops_per_sec = 0;  // batched: one rdtscp pair around all measured ops
  double per_op_mean_ns = 0;  // mean of the per-op samples, timer overhead included
  std::uint64_t trades = 0;  // in the measured part only
};

struct KindCounts {
  std::size_t limit = 0, cancel = 0;
};

inline KindCounts count_kinds(const std::vector<Cmd>& cmds, std::size_t from) {
  KindCounts k;
  for (std::size_t i = from; i < cmds.size(); ++i) {
    (cmds[i].kind == CmdKind::Cancel ? k.cancel : k.limit)++;
  }
  return k;
}

// With n samples the p99.9 bucket holds n / 1000 of them.
inline bool p999_has_enough_samples(std::size_t n) { return n / 1000 >= kMinTailSamples; }

// Nearest rank on sorted, non-empty input. per_10k is the quantile in units
// of 0.01%, so p99.9 is 9990.
inline std::uint64_t percentile(const std::vector<std::uint64_t>& sorted, std::uint32_t per_10k) {
  std::size_t rank = (sorted.size() * per_10k + 9999) / 10000;
  return sorted[rank - 1];
}

inline LatencyStats stats_from(std::vector<std::uint64_t>& ticks, const TscClock& clock) {
  LatencyStats s;
  s.samples = ticks.size();
  if (ticks.empty()) return s;
  std::sort(ticks.begin(), ticks.end());
  s.p50 = clock.to_ns(percentile(ticks, 5000));
  s.p99 = clock.to_ns(percentile(ticks, 9900));
  s.p999 = clock.to_ns(percentile(ticks, 9990));
  s.p9999 = clock.to_ns(percentile(ticks, 9999));
  s.max = clock.to_ns(ticks.back());
  return s;
}

// Returns the number of trades the command produced.
inline std::size_t apply(engine::Book& book, const Cmd& c) {
  if (c.kind == CmdKind::Cancel) {
    book.cancel_order(c.id);
    return 0;
  }
  return book.add_limit_order(c.id, c.side, c.price, c.qty, c.account).trades.size();
}

// cmds[0, warmup) warm the book and the caches and are not measured.
inline RunResult run(const std::vector<Cmd>& cmds, std::size_t warmup, const TscClock& clock) {
  const std::size_t measured = cmds.size() - warmup;
  RunResult r;

  std::uint64_t whole_trades = 0;
  {
    engine::Book book(kBookCapacity);
    for (std::size_t i = 0; i < warmup; ++i) apply(book, cmds[i]);
    const std::uint64_t start = tsc_now();
    for (std::size_t i = warmup; i < cmds.size(); ++i) whole_trades += apply(book, cmds[i]);
    const std::uint64_t end = tsc_now();
    r.ops_per_sec = static_cast<double>(measured) / (clock.to_ns(end - start) / 1e9);
  }

  const KindCounts n = count_kinds(cmds, warmup);
  std::vector<std::uint64_t> limit_ticks, cancel_ticks;
  limit_ticks.reserve(n.limit);
  cancel_ticks.reserve(n.cancel);
  {
    engine::Book book(kBookCapacity);
    for (std::size_t i = 0; i < warmup; ++i) apply(book, cmds[i]);
    for (std::size_t i = warmup; i < cmds.size(); ++i) {
      const std::uint64_t start = tsc_now();
      r.trades += apply(book, cmds[i]);
      const std::uint64_t end = tsc_now();
      (cmds[i].kind == CmdKind::Cancel ? cancel_ticks : limit_ticks).push_back(end - start);
    }
  }

  if (r.trades != whole_trades) {
    std::fprintf(stderr, "the two passes disagree on trades (%llu vs %llu): the engine is not deterministic\n",
                 static_cast<unsigned long long>(r.trades), static_cast<unsigned long long>(whole_trades));
    std::abort();
  }
  std::uint64_t total_ticks = 0;
  for (std::uint64_t t : limit_ticks) total_ticks += t;
  for (std::uint64_t t : cancel_ticks) total_ticks += t;
  r.per_op_mean_ns = clock.to_ns(total_ticks) / static_cast<double>(measured);

  r.limit = stats_from(limit_ticks, clock);
  r.cancel = stats_from(cancel_ticks, clock);
  return r;
}

// env_block is the "# key: value" text from environment_block().
inline std::string results_csv(const std::string& env_block, const RunConfig& cfg, const RunResult& r) {
  std::ostringstream out;
  out << env_block;
  out << "# methodology_version: " << kMethodologyVersion << '\n';
  out << "# label: " << cfg.label << '\n';
  out << "# workload: W1\n";
  out << "# seed: " << cfg.seed << '\n';
  out << "# warmup_ops: " << cfg.warmup << '\n';
  out << "# measured_ops: " << cfg.measured << '\n';
  out << "# latency_timing: per-op rdtscp+lfence, timer overhead not subtracted\n";

  char buf[160];
  std::snprintf(buf, sizeof buf, "# timer_overhead_p50_ns: %.1f\n", cfg.timer_overhead_ns);
  out << buf;

  out << "# throughput_timing: batched, one rdtscp pair around all measured ops\n";
  out << "# trades: " << r.trades << '\n';
  std::snprintf(buf, sizeof buf, "# throughput_ops_per_sec: %.0f\n", r.ops_per_sec);
  out << buf;

  out << "kind,samples,p50_ns,p99_ns,p999_ns,p9999_ns,max_ns\n";
  auto row = [&](const char* kind, const LatencyStats& s) {
    std::snprintf(buf, sizeof buf, "%s,%zu,%.1f,%.1f,%.1f,%.1f,%.1f\n", kind, s.samples, s.p50, s.p99, s.p999,
                  s.p9999, s.max);
    out << buf;
  };
  row("limit", r.limit);
  row("cancel", r.cancel);
  return out.str();
}

}  // namespace bench
