// Test 25: once the book is built and warm, no command allocates.
//
// Two streams. W1 is the benchmark workload, so this is the claim the
// benchmark numbers rest on. It only issues GTC limit orders and cancels,
// though, so the second stream drives every other way into the book through
// a small pool: IOC, FOK, market and modify orders, self-trade rejections,
// duplicate ids, and a pool that fills up.
//
// Counted with the allocation counter, which sees calls to the global
// operator new and nothing else (docs/benchmark-methodology.md).
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
using namespace engine;

namespace {

void w1_makes_no_allocation(std::uint64_t seed) {
  const std::size_t warmup = 100000, measured = 200000;
  const auto cmds = generate(W1Params{}, seed, warmup + measured);
  const AllocProfile p = profile_allocs(cmds, warmup);

  // Zero is only worth asserting if the stream reached every path.
  CHECK(p.limit_no_trade.commands > 0);
  CHECK(p.limit_trades.commands > 0);
  CHECK(p.cancel.commands > 0);
  CHECK(p.all.commands == measured);
  CHECK(p.limit_no_trade.commands + p.limit_trades.commands + p.cancel.commands == measured);

  CHECK(p.limit_no_trade.allocs == 0);
  CHECK(p.limit_trades.allocs == 0);
  CHECK(p.cancel.allocs == 0);
  CHECK(p.all.allocs == 0);
  CHECK(p.all.bytes == 0);
}

std::uint64_t mix(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

struct Seen {
  std::size_t trades = 0, stp = 0, exhausted = 0, duplicates = 0, modified = 0, killed = 0;
};

// Prices sit in 40 ticks and ids in a small domain, so orders cross each
// other constantly, ids collide with resting orders, and the 8-slot pool
// fills. Three accounts make self-trades common.
Seen drive(Book& book, Book::AddResult& result, std::uint64_t& s, std::size_t ops) {
  Seen seen;
  for (std::size_t i = 0; i < ops; ++i) {
    const OrderId id = mix(s) % 200;
    const Side side = (mix(s) & 1) ? Side::Buy : Side::Sell;
    const Price price = 100 + static_cast<Price>(mix(s) % 40);
    const Qty qty = 1 + static_cast<Qty>(mix(s) % 30);
    const std::uint32_t account = static_cast<std::uint32_t>(mix(s) % 3);

    switch (mix(s) % 10) {
      case 0:
      case 1:
      case 2:
        book.add_limit_order(result, id, side, price, qty, account);
        break;
      case 3:
        book.add_limit_order(result, id, side, price, qty, account, TimeInForce::IOC);
        break;
      case 4:
        book.add_limit_order(result, id, side, price, qty, account, TimeInForce::FOK);
        break;
      case 5:
        book.add_market_order(result, id, side, qty, account);
        break;
      case 6:
        seen.modified += book.modify_order(id, price, qty);
        continue;
      default:
        seen.killed += book.cancel_order(id);
        continue;
    }
    seen.trades += result.trades.size();
    seen.stp += result.stp_rejected;
    seen.exhausted += result.pool_exhausted;
    seen.duplicates += !result.accepted;
  }
  return seen;
}

void every_entry_point_makes_no_allocation(std::uint64_t seed) {
  Book book(8);
  Book::AddResult result;
  std::uint64_t s = seed;

  // The result's trade list grows to the longest sweep it has seen and then
  // stays there, so the longest sweeps have to happen before counting starts.
  drive(book, result, s, 50000);

  const AllocSnapshot before = alloc_snapshot();
  const Seen seen = drive(book, result, s, 150000);
  const AllocSnapshot after = alloc_snapshot();

  CHECK(seen.trades > 0);
  CHECK(seen.stp > 0);
  CHECK(seen.exhausted > 0);
  CHECK(seen.duplicates > 0);
  CHECK(seen.modified > 0);
  CHECK(seen.killed > 0);

  CHECK(after.calls == before.calls);
  CHECK(after.bytes == before.bytes);
  CHECK(book.invariant_violation() == nullptr);
}

}  // namespace

int main() {
  for (std::uint64_t seed = 1; seed <= 3; ++seed) w1_makes_no_allocation(seed);
  for (std::uint64_t seed = 1; seed <= 3; ++seed) every_entry_point_makes_no_allocation(seed);
  std::puts("zero allocation ok");
}
