#pragma once

// W1, the baseline workload from docs/benchmark-methodology.md.
//
// generate() is a pure function of (params, seed, ops) and builds the whole
// sequence up front, so no generation happens inside a timed region. The
// golden hash in tests/bench/test_workload.cpp pins the stream.
//
// Integer arithmetic only. The Zipf table uses floor(2^40 / rank) instead
// of pow(), so it comes out the same on every libm.
//
// The generator runs its own Book to know which orders are still resting,
// so a cancel always hits a live order. Every correct engine produces the
// same trades for the same commands (S7), so the stream doesn't depend on
// which engine version is being measured.
//
// The cancel probability scales with the book size. A fixed probability
// has nothing pulling the book back to a target: 40% grew without bound
// and 45% collapsed to a few dozen orders. See "How W1 holds its depth" in
// the methodology doc.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "engine/book.hpp"
#include "engine/types.hpp"

namespace bench {

using engine::OrderId;
using engine::Price;
using engine::Qty;
using engine::Side;

enum class CmdKind : std::uint8_t { Limit, Cancel };

struct Cmd {
  CmdKind kind = CmdKind::Limit;
  Side side = Side::Buy;
  OrderId id = 0;
  Price price = 0;
  Qty qty = 0;
  std::uint32_t account = 0;
};

struct W1Params {
  Price mid = 100000;
  // Prices fall in [mid - max_offset, mid + max_offset], a 10,000 tick span.
  std::uint32_t max_offset = 5000;
  // Buys rest at mid - (rank - cross_shift), sells at mid + (rank -
  // cross_shift), rank drawn from Zipf(s=1) over 1..max_offset. Ranks up to
  // cross_shift land across mid and are marketable. At 50 about half of
  // all limit orders trade, and the cancel share comes out at 30%.
  std::uint32_t cross_shift = 50;
  std::uint32_t cancel_percent = 30;
  // Book size at which the cancel probability equals cancel_percent.
  // About 1,000 levels per side at the defaults.
  std::uint32_t target_resting = 3000;
  Qty max_qty = 100;
};

// Same splitmix64 as tests/support/diff.hpp. Duplicated because bench/ must
// not depend on tests/.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }

 private:
  std::uint64_t state_;
};

// Cumulative weights of 1/rank over ranks 1..n, in integers. The total is
// about 2^40 * ln(5000), far below 2^64.
inline std::vector<std::uint64_t> zipf_cdf(std::uint32_t n) {
  std::vector<std::uint64_t> cdf(n);
  std::uint64_t sum = 0;
  for (std::uint32_t r = 1; r <= n; ++r) {
    sum += (std::uint64_t{1} << 40) / r;
    cdf[r - 1] = sum;
  }
  return cdf;
}

inline std::vector<Cmd> generate(const W1Params& p, std::uint64_t seed, std::size_t ops) {
  constexpr std::uint64_t kPpm = 1000000;
  constexpr std::uint64_t kMaxCancelPpm = 900000;  // keeps inserts flowing if the book overshoots
  constexpr engine::Index kBookCapacity = 1u << 16;

  Rng rng(seed);
  const std::vector<std::uint64_t> cdf = zipf_cdf(p.max_offset);
  engine::Book book(kBookCapacity);

  // live is dense so a random pick and a swap-removal are both O(1). The
  // two maps are lookup-only, never iterated (S7).
  std::vector<OrderId> live;
  std::unordered_map<OrderId, std::size_t> where;
  std::unordered_map<OrderId, Qty> remaining;
  auto forget = [&](OrderId id) {
    std::size_t i = where[id];
    live[i] = live.back();
    where[live[i]] = i;
    live.pop_back();
    where.erase(id);
    remaining.erase(id);
  };

  std::vector<Cmd> out;
  out.reserve(ops);
  OrderId next_id = 1;

  for (std::size_t i = 0; i < ops; ++i) {
    Cmd c;
    std::uint64_t cancel_ppm = std::min<std::uint64_t>(
        kMaxCancelPpm, std::uint64_t{p.cancel_percent} * (kPpm / 100) * live.size() / p.target_resting);

    if (!live.empty() && rng.below(kPpm) < cancel_ppm) {
      c.kind = CmdKind::Cancel;
      c.id = live[rng.below(live.size())];
      book.cancel_order(c.id);
      forget(c.id);
    } else {
      c.kind = CmdKind::Limit;
      c.side = rng.below(2) == 0 ? Side::Buy : Side::Sell;
      std::uint64_t u = rng.below(cdf.back());
      auto rank = static_cast<std::int64_t>(std::upper_bound(cdf.begin(), cdf.end(), u) - cdf.begin()) + 1;
      std::int64_t offset = rank - static_cast<std::int64_t>(p.cross_shift);
      c.price = c.side == Side::Buy ? p.mid - offset : p.mid + offset;
      c.qty = static_cast<Qty>(rng.below(p.max_qty) + 1);
      c.id = next_id++;
      // One account per order so STP never fires; W1 is about matching.
      c.account = static_cast<std::uint32_t>(c.id);

      auto r = book.add_limit_order(c.id, c.side, c.price, c.qty, c.account);
      for (const auto& t : r.trades) {
        Qty& left = remaining[t.maker_order_id];
        left -= t.qty;
        if (left == 0) forget(t.maker_order_id);
      }
      if (r.resting_qty > 0) {
        where[c.id] = live.size();
        live.push_back(c.id);
        remaining[c.id] = r.resting_qty;
      }
    }
    out.push_back(c);
  }
  return out;
}

}  // namespace bench
