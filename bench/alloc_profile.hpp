#pragma once

// Counts the allocations the engine makes while it runs W1, and attributes
// them to the kind of command that made them. The first half of what test 25
// will assert: this reports, test 25 will require zero.
//
// Only calls to the global operator new are counted, which is every
// allocation the engine makes through std containers. The profile is
// useful only in a program that links the alloc_counter library.
//
// Warmup commands run first and aren't counted, the same split the latency
// runs use. The counts don't depend on timing, so no quiet core is needed.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "alloc_counter.hpp"
#include "engine/book.hpp"
#include "runner.hpp"
#include "workload.hpp"

namespace bench {

struct AllocStats {
  std::size_t commands = 0;
  std::uint64_t allocs = 0;
  std::uint64_t bytes = 0;
  std::uint64_t max_per_command = 0;

  double per_command() const { return commands == 0 ? 0.0 : static_cast<double>(allocs) / static_cast<double>(commands); }
};

struct AllocProfile {
  AllocStats limit_no_trade, limit_trades, cancel;
  // The whole measured region, counted around the loop and not summed from
  // the three above, so the two can be checked against each other.
  AllocStats all;
};

inline AllocProfile profile_allocs(const std::vector<Cmd>& cmds, std::size_t warmup) {
  engine::Book book(kBookCapacity);
  for (std::size_t i = 0; i < warmup; ++i) apply(book, cmds[i]);

  AllocProfile p;
  const AllocSnapshot region_start = alloc_snapshot();
  for (std::size_t i = warmup; i < cmds.size(); ++i) {
    const AllocSnapshot before = alloc_snapshot();
    const std::size_t trades = apply(book, cmds[i]);
    const AllocSnapshot after = alloc_snapshot();

    AllocStats& s = cmds[i].kind == CmdKind::Cancel ? p.cancel : (trades == 0 ? p.limit_no_trade : p.limit_trades);
    const std::uint64_t calls = after.calls - before.calls;
    s.commands++;
    s.allocs += calls;
    s.bytes += after.bytes - before.bytes;
    s.max_per_command = std::max(s.max_per_command, calls);
    p.all.max_per_command = std::max(p.all.max_per_command, calls);
  }
  const AllocSnapshot region_end = alloc_snapshot();

  p.all.commands = cmds.size() - warmup;
  p.all.allocs = region_end.calls - region_start.calls;
  p.all.bytes = region_end.bytes - region_start.bytes;
  return p;
}

// env_block is the "# key: value" text from environment_block().
inline std::string allocs_csv(const std::string& env_block, const RunConfig& cfg, const AllocProfile& p) {
  std::ostringstream out;
  out << env_block;
  out << "# methodology_version: " << kMethodologyVersion << '\n';
  out << "# label: " << cfg.label << '\n';
  out << "# workload: W1\n";
  out << "# seed: " << cfg.seed << '\n';
  out << "# warmup_ops: " << cfg.warmup << '\n';
  out << "# measured_ops: " << cfg.measured << '\n';
  out << "# counted: calls to the global operator new\n";

  char buf[160];
  out << "kind,commands,allocs,allocs_per_command,max_per_command,bytes\n";
  auto row = [&](const char* kind, const AllocStats& s) {
    std::snprintf(buf, sizeof buf, "%s,%zu,%llu,%.3f,%llu,%llu\n", kind, s.commands,
                  static_cast<unsigned long long>(s.allocs), s.per_command(),
                  static_cast<unsigned long long>(s.max_per_command), static_cast<unsigned long long>(s.bytes));
    out << buf;
  };
  row("limit_no_trade", p.limit_no_trade);
  row("limit_trades", p.limit_trades);
  row("cancel", p.cancel);
  row("all", p.all);
  return out.str();
}

}  // namespace bench
