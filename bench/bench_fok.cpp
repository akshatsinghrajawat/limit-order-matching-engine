// Usage: bench_fok <label> [rounds] [cpu] > bench/results/<name>.csv
//
// Runs W2 (bench/workload_fok.hpp) on a shallow and on a deep book and
// reports the latency of the fill-or-kill command alone; the order that
// refills the book is not timed. Same build and machine conditions as
// bench_w1: -DENABLE_SANITIZERS=OFF -DCMAKE_BUILD_TYPE=Release, an idle core,
// and the core passed as the last argument.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "engine/book.hpp"
#include "env.hpp"
#include "runner.hpp"
#include "timing.hpp"
#include "workload_fok.hpp"

namespace {

void send(engine::Book& book, engine::Book::AddResult& result, const bench::FokCmd& c) {
  book.add_limit_order(result, c.id, c.side, c.price, c.qty, c.account, c.tif);
}

struct Scenario {
  const char* name;
  bench::W2Params params;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fputs("usage: bench_fok <label> [rounds] [cpu]\n", stderr);
    return 2;
  }
  const std::string label = argv[1];
  const std::size_t rounds = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 200000;
  const int cpu = argc > 3 ? std::atoi(argv[3]) : -1;

  if (std::string(BENCH_FLAGS).find("-fsanitize") != std::string::npos) {
    std::fputs("warning: sanitizers are on, these numbers are not publishable\n", stderr);
  }
  if (cpu >= 0 && !bench::pin_to_cpu(cpu)) {
    std::fprintf(stderr, "could not pin to cpu %d\n", cpu);
    return 1;
  }
  const bench::TscClock clock;

  std::ostringstream out;
  out << bench::environment_block(clock);
  out << "# methodology_version: " << bench::kMethodologyVersion << '\n';
  out << "# label: " << label << '\n';
  out << "# workload: W2\n";
  out << "# rounds: " << rounds << " per scenario, the first tenth are warmup\n";
  out << "# latency_timing: per-op rdtscp+lfence around the FOK only, timer overhead not subtracted\n";
  out << "scenario,orders_in_reach,fok_samples,p50_ns,p99_ns,p999_ns,max_ns\n";

  const Scenario scenarios[] = {{"shallow", bench::kW2Shallow}, {"deep", bench::kW2Deep}};
  for (const Scenario& sc : scenarios) {
    const bench::W2Stream stream = bench::generate_fok(sc.params, rounds);
    engine::Book book(bench::kBookCapacity);
    engine::Book::AddResult result;
    result.trades.reserve(bench::kBookCapacity);
    for (const bench::FokCmd& c : stream.setup) send(book, result, c);

    const std::size_t warmup = stream.rounds.size() / 10 / 2 * 2;  // whole rounds only
    std::vector<std::uint64_t> ticks;
    ticks.reserve(stream.rounds.size() / 2);
    std::uint64_t trades = 0;
    for (std::size_t i = 0; i < stream.rounds.size(); i += 2) {
      const std::uint64_t start = bench::tsc_now();
      send(book, result, stream.rounds[i]);
      const std::uint64_t end = bench::tsc_now();
      trades += result.trades.size();
      if (!result.accepted || result.trades.size() != 1) {
        std::fputs("a fill-or-kill did not fill one order: the workload and the engine disagree\n", stderr);
        return 1;
      }
      if (i >= warmup) ticks.push_back(end - start);
      send(book, result, stream.rounds[i + 1]);
    }
    const bench::LatencyStats s = bench::stats_from(ticks, clock);
    char row[200];
    std::snprintf(row, sizeof row, "%s,%zu,%zu,%.1f,%.1f,%.1f,%.1f\n", sc.name,
                  static_cast<std::size_t>(sc.params.levels) * sc.params.per_level, s.samples, s.p50, s.p99, s.p999,
                  s.max);
    out << row;
  }
  std::fputs(out.str().c_str(), stdout);
}
