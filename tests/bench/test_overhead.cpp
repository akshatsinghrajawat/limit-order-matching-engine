// Checks the arithmetic and file layout of test 27, and that a short real
// measurement produces sensible fields. The figures it reports on a real
// machine come from running bench_overhead there.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "overhead.hpp"
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

static OverheadRep rep_of(double batched, double per_op, double empty) {
  OverheadRep r;
  r.batched_ns_per_op = batched;
  r.per_op_mean_ns = per_op;
  r.empty_p50_ns = empty;
  return r;
}

static std::vector<std::string> lines_of(const std::string& s) {
  std::vector<std::string> out;
  std::string line;
  std::istringstream in(s);
  while (std::getline(in, line)) out.push_back(line);
  return out;
}

static void share_is_empty_cost_over_batched_cost() {
  CHECK(rep_of(200, 230, 25).share_percent() == 12.5);
  CHECK(rep_of(100, 100, 1).share_percent() == 1.0);
}

static void csv_layout_and_the_one_percent_rule() {
  RunConfig cfg;
  cfg.seed = 7;
  cfg.warmup = 5000;
  cfg.measured = 30000;

  auto heavy = lines_of(overhead_csv("# cpu_model: test\n", cfg,
                                     {rep_of(200, 230, 25), rep_of(180, 215, 25), rep_of(210, 240, 24)}));
  CHECK(heavy.size() == 1 + 8 + 1 + 3);
  CHECK(heavy[0] == "# cpu_model: test");
  CHECK(heavy[1] == "# methodology_version: 1");
  CHECK(heavy[2] == "# test: 27, measurement overhead");
  CHECK(heavy[3] == "# workload: W1");
  CHECK(heavy[4] == "# seed: 7");
  CHECK(heavy[5] == "# warmup_ops: 5000");
  CHECK(heavy[6] == "# measured_ops: 30000");
  CHECK(heavy[7] == "# limit_percent: 1.0");
  CHECK(heavy[8] == "# one_percent_rule: exceeded");
  CHECK(heavy[9] == "rep,batched_ns_per_op,per_op_mean_ns,empty_p50_ns,empty_share_percent");
  CHECK(heavy[10] == "1,200.0,230.0,25.0,12.5");

  // One repetition over the line is enough to say exceeded; exactly 1% is within.
  auto mixed = lines_of(overhead_csv("", cfg, {rep_of(5000, 5000, 25), rep_of(200, 200, 25)}));
  CHECK(mixed[7] == "# one_percent_rule: exceeded");
  auto light = lines_of(overhead_csv("", cfg, {rep_of(2500, 2500, 25)}));
  CHECK(light[7] == "# one_percent_rule: within");
}

static void a_short_real_measurement_has_sensible_fields() {
  const TscClock clock;
  const auto cmds = generate(W1Params{}, 1, 35000);
  const OverheadRep r = measure_overhead(cmds, 5000, clock);
  CHECK(r.batched_ns_per_op > 0);
  CHECK(r.per_op_mean_ns > 0);
  CHECK(r.empty_p50_ns > 0 && r.empty_p50_ns < 1000);
  CHECK(r.share_percent() > 0);
}

int main() {
  share_is_empty_cost_over_batched_cost();
  csv_layout_and_the_one_percent_rule();
  a_short_real_measurement_has_sensible_fields();
  std::puts("overhead ok");
}
