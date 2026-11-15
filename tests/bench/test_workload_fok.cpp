// Checks W2 against docs/benchmark-methodology.md: a deterministic stream,
// a fill-or-kill that always fills one order, and a book that is the same
// size after every round.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "engine/book.hpp"
#include "workload_fok.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace bench;

static std::uint64_t stream_hash(const W2Stream& s) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&h](std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (8 * i)) & 0xff;
      h *= 0x100000001b3ull;
    }
  };
  for (const auto* part : {&s.setup, &s.rounds}) {
    for (const FokCmd& c : *part) {
      mix(static_cast<std::uint64_t>(c.side));
      mix(static_cast<std::uint64_t>(c.tif));
      mix(c.id);
      mix(static_cast<std::uint64_t>(c.price));
      mix(c.qty);
      mix(c.account);
    }
  }
  return h;
}

static void run_scenario(const W2Params& p) {
  const std::size_t rounds = 2000;
  const W2Stream s = generate_fok(p, rounds);
  CHECK(s.setup.size() == std::size_t{2} * p.levels * p.per_level);  // both sides
  CHECK(s.rounds.size() == 2 * rounds);

  engine::Book book(1u << 16);
  engine::Book::AddResult result;
  result.trades.reserve(1u << 16);
  for (const FokCmd& c : s.setup) {
    book.add_limit_order(result, c.id, c.side, c.price, c.qty, c.account, c.tif);
    CHECK(result.accepted && result.trades.empty() && result.resting_qty == c.qty);
  }
  const std::size_t resting = book.snapshot().size();
  CHECK(resting == s.setup.size());
  CHECK(book.invariant_violation() == nullptr);

  for (std::size_t i = 0; i < s.rounds.size(); i += 2) {
    const FokCmd& fok = s.rounds[i];
    const FokCmd& refill = s.rounds[i + 1];
    CHECK(fok.tif == engine::TimeInForce::FOK && refill.tif == engine::TimeInForce::GTC);
    book.add_limit_order(result, fok.id, fok.side, fok.price, fok.qty, fok.account, fok.tif);
    CHECK(result.accepted && result.trades.size() == 1 && result.trades[0].qty == p.order_qty);
    CHECK(!result.stp_rejected && result.resting_qty == 0);
    CHECK(book.snapshot().size() == resting - 1);
    book.add_limit_order(result, refill.id, refill.side, refill.price, refill.qty, refill.account, refill.tif);
    CHECK(result.accepted && result.trades.empty() && result.resting_qty == p.order_qty);
    CHECK(book.snapshot().size() == resting);
  }
  CHECK(book.invariant_violation() == nullptr);
}

int main() {
  run_scenario(kW2Shallow);
  run_scenario(kW2Deep);

  CHECK(stream_hash(generate_fok(kW2Shallow, 1000)) == stream_hash(generate_fok(kW2Shallow, 1000)));
  CHECK(stream_hash(generate_fok(kW2Shallow, 1000)) != stream_hash(generate_fok(kW2Deep, 1000)));
  constexpr std::uint64_t kGoldenShallow = 0xd03fcaed81f8356dull;
  const std::uint64_t got = stream_hash(generate_fok(kW2Shallow, 1000));
  if (got != kGoldenShallow) {
    std::fprintf(stderr, "W2 shallow hash is 0x%016llx\n", static_cast<unsigned long long>(got));
    return 1;
  }
  std::puts("workload w2 ok");
}
