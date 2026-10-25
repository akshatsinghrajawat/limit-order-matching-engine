// Checks the benchmark runner on a short W1 stream: percentile arithmetic,
// the tail-sample rule, that the counts and trades add up against an
// untimed replay, and the layout of the results file. It says nothing about
// speed.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "engine/book.hpp"
#include "runner.hpp"
#include "timing.hpp"
#include "workload.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace bench;

static void percentile_is_nearest_rank() {
  std::vector<std::uint64_t> v;
  for (std::uint64_t i = 1; i <= 100000; ++i) v.push_back(i);
  CHECK(percentile(v, 5000) == 50000);
  CHECK(percentile(v, 9900) == 99000);
  CHECK(percentile(v, 9990) == 99900);
  CHECK(percentile(v, 9999) == 99990);
  CHECK(percentile(v, 10000) == 100000);

  // Ranks round up: the median of three is the middle one, and the top
  // percentiles of a short list are its largest element.
  const std::vector<std::uint64_t> three = {10, 20, 30};
  CHECK(percentile(three, 5000) == 20);
  CHECK(percentile(three, 9999) == 30);
  CHECK(percentile({7}, 5000) == 7);
  CHECK(percentile({7}, 9999) == 7);
}

static void tail_rule_needs_a_hundred_samples_above_p999() {
  CHECK(!p999_has_enough_samples(0));
  CHECK(!p999_has_enough_samples(99999));
  CHECK(p999_has_enough_samples(100000));
}

constexpr std::size_t kWarmup = 5000;
constexpr std::size_t kMeasured = 30000;

static void counts_and_trades_match_an_untimed_replay(const std::vector<Cmd>& cmds, const RunResult& r) {
  const KindCounts n = count_kinds(cmds, kWarmup);
  CHECK(n.limit + n.cancel == kMeasured);
  CHECK(r.limit.samples == n.limit);
  CHECK(r.cancel.samples == n.cancel);

  engine::Book book(kBookCapacity);
  std::uint64_t trades = 0;
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    std::size_t t = apply(book, cmds[i]);
    if (i >= kWarmup) trades += t;
  }
  CHECK(r.trades == trades);
  CHECK(r.trades > 0);
}

static void stats_are_ordered_and_positive(const RunResult& r) {
  for (const LatencyStats& s : {r.limit, r.cancel}) {
    CHECK(s.p50 > 0);
    CHECK(s.p50 <= s.p99 && s.p99 <= s.p999 && s.p999 <= s.p9999 && s.p9999 <= s.max);
  }
  CHECK(r.ops_per_sec > 0);
  CHECK(r.per_op_mean_ns > 0);
  CHECK(r.per_op_mean_ns <= std::max(r.limit.max, r.cancel.max));
}

static std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string piece;
  std::istringstream in(s);
  while (std::getline(in, piece, sep)) out.push_back(piece);
  return out;
}

static void csv_has_the_documented_layout(const RunResult& r) {
  RunConfig cfg;
  cfg.label = "unit";
  cfg.seed = 1;
  cfg.warmup = kWarmup;
  cfg.measured = kMeasured;
  cfg.timer_overhead_ns = 24.84;
  const std::string csv = results_csv("# cpu_model: test\n", cfg, r);

  std::vector<std::string> lines = split(csv, '\n');
  CHECK(csv.back() == '\n');
  CHECK(lines.size() == 1 + 10 + 1 + 1 + 2);  // env, metadata, throughput, header, two rows

  CHECK(lines[0] == "# cpu_model: test");
  CHECK(lines[1] == "# methodology_version: 1");
  CHECK(lines[2] == "# label: unit");
  CHECK(lines[3] == "# workload: W1");
  CHECK(lines[4] == "# seed: 1");
  CHECK(lines[5] == "# warmup_ops: 5000");
  CHECK(lines[6] == "# measured_ops: 30000");
  CHECK(lines[7] == "# latency_timing: per-op rdtscp+lfence, timer overhead not subtracted");
  CHECK(lines[8] == "# timer_overhead_p50_ns: 24.8");
  CHECK(lines[9] == "# throughput_timing: batched, one rdtscp pair around all measured ops");
  CHECK(lines[10] == "# trades: " + std::to_string(r.trades));
  CHECK(lines[11].compare(0, 25, "# throughput_ops_per_sec:") == 0);
  CHECK(lines[12] == "kind,samples,p50_ns,p99_ns,p999_ns,p9999_ns,max_ns");

  for (int i = 13; i <= 14; ++i) {
    std::vector<std::string> f = split(lines[i], ',');
    CHECK(f.size() == 7);
    CHECK(f[0] == (i == 13 ? "limit" : "cancel"));
    CHECK(std::stoull(f[1]) == (i == 13 ? r.limit.samples : r.cancel.samples));
  }
}

int main() {
  percentile_is_nearest_rank();
  tail_rule_needs_a_hundred_samples_above_p999();

  const TscClock clock;
  const auto cmds = generate(W1Params{}, 1, kWarmup + kMeasured);
  const RunResult r = run(cmds, kWarmup, clock);
  counts_and_trades_match_an_untimed_replay(cmds, r);
  stats_are_ordered_and_positive(r);
  csv_has_the_documented_layout(r);
  std::puts("runner ok");
}
