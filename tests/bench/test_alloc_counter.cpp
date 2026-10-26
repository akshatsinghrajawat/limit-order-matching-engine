// Checks the allocation counter on allocations it can predict, and that the
// per-command attribution adds up on a short W1 stream. It does not pin how
// many allocations the engine makes: that number is what the optimization
// work changes, and test 25 is the check that will require zero.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <sstream>
#include <string>
#include <vector>

#include "alloc_counter.hpp"
#include "alloc_profile.hpp"
#include "engine/book.hpp"
#include "runner.hpp"
#include "workload.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace bench;

// A store through a volatile pointer is something the compiler has to
// perform, so it can't drop an allocation whose result is otherwise unused.
static void* volatile g_sink;

static void counts_each_form_once() {
  AllocSnapshot s = alloc_snapshot();

  g_sink = new int(1);
  CHECK(alloc_snapshot().calls == s.calls + 1);
  CHECK(alloc_snapshot().bytes == s.bytes + sizeof(int));
  delete static_cast<int*>(g_sink);

  s = alloc_snapshot();
  g_sink = new char[100];
  CHECK(alloc_snapshot().calls == s.calls + 1);
  CHECK(alloc_snapshot().bytes == s.bytes + 100);
  delete[] static_cast<char*>(g_sink);

  s = alloc_snapshot();
  g_sink = ::operator new(64, std::align_val_t{64});
  CHECK(alloc_snapshot().calls == s.calls + 1);
  CHECK((reinterpret_cast<std::uintptr_t>(g_sink) & 63) == 0);
  ::operator delete(g_sink, std::align_val_t{64});

  s = alloc_snapshot();
  g_sink = ::operator new(8, std::nothrow);
  CHECK(g_sink != nullptr);
  CHECK(alloc_snapshot().calls == s.calls + 1);
  ::operator delete(g_sink, std::nothrow);
}

static void frees_and_snapshots_are_not_counted() {
  g_sink = new int(1);
  const AllocSnapshot before = alloc_snapshot();
  delete static_cast<int*>(g_sink);
  const AllocSnapshot after = alloc_snapshot();
  CHECK(after.calls == before.calls);
  CHECK(after.bytes == before.bytes);

  const AllocSnapshot again = alloc_snapshot();
  CHECK(again.calls == after.calls);
}

static void a_vector_growth_is_counted() {
  const AllocSnapshot s = alloc_snapshot();
  std::vector<int> v;
  v.reserve(1000);
  g_sink = v.data();
  CHECK(alloc_snapshot().calls == s.calls + 1);
  CHECK(alloc_snapshot().bytes == s.bytes + 1000 * sizeof(int));
}

constexpr std::size_t kWarmup = 5000;
constexpr std::size_t kMeasured = 30000;

static void attribution_adds_up(const std::vector<Cmd>& cmds, const AllocProfile& p) {
  const KindCounts n = count_kinds(cmds, kWarmup);
  CHECK(p.cancel.commands == n.cancel);
  CHECK(p.limit_no_trade.commands + p.limit_trades.commands == n.limit);
  CHECK(p.all.commands == kMeasured);

  // The region total is counted around the loop, the parts per command.
  CHECK(p.limit_no_trade.allocs + p.limit_trades.allocs + p.cancel.allocs == p.all.allocs);
  CHECK(p.limit_no_trade.bytes + p.limit_trades.bytes + p.cancel.bytes == p.all.bytes);

  CHECK(p.limit_no_trade.max_per_command <= p.all.max_per_command);
  CHECK(p.limit_trades.max_per_command <= p.all.max_per_command);
  CHECK(p.cancel.max_per_command <= p.all.max_per_command);
  CHECK(p.all.allocs >= p.all.max_per_command);
}

static void trade_split_matches_an_untimed_replay(const std::vector<Cmd>& cmds, const AllocProfile& p) {
  engine::Book book(kBookCapacity);
  std::size_t traded = 0, rested_only = 0;
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const std::size_t trades = apply(book, cmds[i]);
    if (i < kWarmup || cmds[i].kind == CmdKind::Cancel) continue;
    (trades == 0 ? rested_only : traded)++;
  }
  CHECK(p.limit_trades.commands == traded);
  CHECK(p.limit_no_trade.commands == rested_only);
  CHECK(traded > 0 && rested_only > 0);
}

static void the_counts_repeat(const std::vector<Cmd>& cmds, const AllocProfile& p) {
  const AllocProfile q = profile_allocs(cmds, kWarmup);
  CHECK(q.all.allocs == p.all.allocs);
  CHECK(q.all.bytes == p.all.bytes);
  CHECK(q.limit_trades.allocs == p.limit_trades.allocs);
  CHECK(q.limit_no_trade.allocs == p.limit_no_trade.allocs);
  CHECK(q.cancel.allocs == p.cancel.allocs);
  CHECK(q.all.max_per_command == p.all.max_per_command);
}

static void file_layout(const AllocProfile& p) {
  RunConfig cfg;
  cfg.label = "layout-test";
  cfg.seed = 7;
  cfg.warmup = kWarmup;
  cfg.measured = kMeasured;
  const std::string csv = allocs_csv("# cpu_model: test\n", cfg, p);

  std::istringstream in(csv);
  std::string line;
  std::vector<std::string> lines;
  while (std::getline(in, line)) lines.push_back(line);

  CHECK(lines.size() >= 11);
  CHECK(lines[0] == "# cpu_model: test");
  CHECK(csv.find("# label: layout-test\n") != std::string::npos);
  CHECK(csv.find("# seed: 7\n") != std::string::npos);
  CHECK(csv.find("# warmup_ops: 5000\n") != std::string::npos);
  CHECK(csv.find("# measured_ops: 30000\n") != std::string::npos);
  CHECK(csv.find("# workload: W1\n") != std::string::npos);

  const std::size_t last = lines.size();
  CHECK(lines[last - 5] == "kind,commands,allocs,allocs_per_command,max_per_command,bytes");
  CHECK(lines[last - 4].rfind("limit_no_trade,", 0) == 0);
  CHECK(lines[last - 3].rfind("limit_trades,", 0) == 0);
  CHECK(lines[last - 2].rfind("cancel,", 0) == 0);
  CHECK(lines[last - 1].rfind("all,", 0) == 0);

  std::size_t commas = 0;
  for (char c : lines[last - 1]) commas += c == ',';
  CHECK(commas == 5);
  CHECK(lines[last - 1].rfind("all,30000,", 0) == 0);
}

int main() {
  counts_each_form_once();
  frees_and_snapshots_are_not_counted();
  a_vector_growth_is_counted();

  const std::vector<Cmd> cmds = generate(W1Params{}, 1, kWarmup + kMeasured);
  const AllocProfile p = profile_allocs(cmds, kWarmup);
  attribution_adds_up(cmds, p);
  trade_split_matches_an_untimed_replay(cmds, p);
  the_counts_repeat(cmds, p);
  file_layout(p);

  std::puts("alloc counter ok");
}
