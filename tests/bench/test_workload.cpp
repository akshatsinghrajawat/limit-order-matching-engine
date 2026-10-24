// Checks W1 against docs/benchmark-methodology.md: a deterministic stream,
// the documented mix, and a stationary book near the target depth.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

#include "engine/book.hpp"
#include "workload.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace bench;

// FNV-1a over every field of every command.
static std::uint64_t stream_hash(const std::vector<Cmd>& cmds) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  auto mix = [&h](std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (8 * i)) & 0xff;
      h *= 0x100000001b3ull;
    }
  };
  for (const Cmd& c : cmds) {
    mix(static_cast<std::uint64_t>(c.kind));
    mix(static_cast<std::uint64_t>(c.side));
    mix(c.id);
    mix(static_cast<std::uint64_t>(c.price));
    mix(c.qty);
    mix(c.account);
  }
  return h;
}

constexpr std::size_t kOps = 400000;
constexpr std::size_t kWarmup = 100000;
constexpr std::size_t kEarly = 150000;  // book size is sampled here and at the end

static void same_seed_same_stream_and_pinned_by_golden_hash() {
  W1Params p;
  auto a = generate(p, 1, 200000);
  auto b = generate(p, 1, 200000);
  CHECK(stream_hash(a) == stream_hash(b));
  CHECK(stream_hash(generate(p, 2, 200000)) != stream_hash(a));
  // Fails on any change to the generator or to the engine's trades. If the
  // change is intended, regenerate the constant and say so in the commit:
  // older W1 numbers stop being comparable.
  CHECK(stream_hash(a) == 0xe6cc8941fc10572aull);
}

static void fields_stay_inside_the_documented_ranges(const std::vector<Cmd>& cmds) {
  W1Params p;
  std::set<OrderId> seen;
  for (const Cmd& c : cmds) {
    if (c.kind == CmdKind::Cancel) {
      CHECK(seen.count(c.id) != 0);
      continue;
    }
    CHECK(c.qty >= 1 && c.qty <= p.max_qty);
    CHECK(c.price >= p.mid - static_cast<Price>(p.max_offset));
    CHECK(c.price <= p.mid + static_cast<Price>(p.max_offset));
    CHECK(seen.insert(c.id).second);
  }
}

static void mix_matches_the_methodology_doc(const std::vector<Cmd>& cmds) {
  W1Params p;
  std::uint64_t cancels = 0, buys = 0, inserts = 0, rank1 = 0, rank2 = 0;
  for (std::size_t i = kWarmup; i < cmds.size(); ++i) {
    const Cmd& c = cmds[i];
    if (c.kind == CmdKind::Cancel) {
      ++cancels;
      continue;
    }
    ++inserts;
    buys += c.side == Side::Buy;
    std::int64_t offset = c.side == Side::Buy ? p.mid - c.price : c.price - p.mid;
    std::int64_t rank = offset + p.cross_shift;
    rank1 += rank == 1;
    rank2 += rank == 2;
  }
  double ops = static_cast<double>(cmds.size() - kWarmup);
  double cancel_share = cancels / ops;
  double buy_share = static_cast<double>(buys) / inserts;
  double zipf_ratio = static_cast<double>(rank1) / rank2;
  std::printf("cancel_share=%.4f buy_share=%.4f zipf_rank1_over_rank2=%.3f\n", cancel_share, buy_share, zipf_ratio);
  CHECK(cancel_share > 0.28 && cancel_share < 0.32);
  CHECK(buy_share > 0.49 && buy_share < 0.51);
  CHECK(zipf_ratio > 1.9 && zipf_ratio < 2.1);  // 1/r weights: rank 1 is twice rank 2
}

static std::size_t levels_on(const std::vector<engine::Book::RestingOrder>& s, Side side) {
  std::set<Price> prices;
  for (const auto& o : s) {
    if (o.side == side) prices.insert(o.price);
  }
  return prices.size();
}

static void replay_is_clean_and_the_book_is_stationary_near_target(const std::vector<Cmd>& cmds) {
  W1Params p;
  engine::Book book(1u << 16);
  std::uint64_t trades = 0, limits = 0;
  std::size_t resting_early = 0;

  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const Cmd& c = cmds[i];
    if (c.kind == CmdKind::Cancel) {
      CHECK(book.cancel_order(c.id));  // every cancel names a live order
    } else {
      auto r = book.add_limit_order(c.id, c.side, c.price, c.qty, c.account);
      CHECK(r.accepted && !r.stp_rejected && !r.pool_exhausted);
      trades += r.trades.size();
      ++limits;
    }
    if (i + 1 == kEarly) resting_early = book.snapshot().size();
  }
  CHECK(book.invariant_violation() == nullptr);

  auto s = book.snapshot();
  std::size_t bid_levels = levels_on(s, Side::Buy);
  std::size_t ask_levels = levels_on(s, Side::Sell);
  double trades_per_limit = static_cast<double>(trades) / limits;
  std::printf("resting early=%zu late=%zu bid_levels=%zu ask_levels=%zu trades_per_limit=%.3f\n",
              resting_early, s.size(), bid_levels, ask_levels, trades_per_limit);

  CHECK(bid_levels > 800 && bid_levels < 1400);
  CHECK(ask_levels > 800 && ask_levels < 1400);
  CHECK(s.size() > p.target_resting * 2 / 3 && s.size() < p.target_resting * 3 / 2);
  // Stationary: a book that is still growing or collapsing differs a lot
  // between the early sample and the end.
  CHECK(resting_early * 3 > s.size() * 2 && resting_early * 2 < s.size() * 3);
  CHECK(trades_per_limit > 0.4);  // a workload that never trades measures nothing
}

int main() {
  same_seed_same_stream_and_pinned_by_golden_hash();
  const auto cmds = generate(W1Params{}, 1, kOps);
  fields_stay_inside_the_documented_ranges(cmds);
  mix_matches_the_methodology_doc(cmds);
  replay_is_clean_and_the_book_is_stationary_near_target(cmds);
  std::puts("workload ok");
}
