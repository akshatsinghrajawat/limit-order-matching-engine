#pragma once

// W2, a fill-or-kill workload. W1 never sends one, so it cannot show what the
// FOK pre-check costs. docs/benchmark-methodology.md describes it.
//
// Both sides of the book hold `levels` price levels of `per_level` orders of
// `order_qty` lots, and no order shares an account with another or with the
// taker. Every round sends a fill-or-kill for exactly one order's quantity,
// priced to reach the far end of the opposite side, then rests one order of
// that quantity at the price the fill emptied from. The FOK always fills, it
// fills one maker, and the book is the same size after every round. The
// pre-check has to decide a quantity that the first order already covers
// while the book offers `levels * per_level` orders in reach, so the cost
// of walking past the boundary is the cost the workload measures.
//
// Sides alternate. generate_fok() is a pure function of its parameters; it
// takes no seed because nothing in it is random.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/types.hpp"

namespace bench {

struct FokCmd {
  engine::Side side = engine::Side::Buy;
  engine::TimeInForce tif = engine::TimeInForce::GTC;
  engine::OrderId id = 0;
  engine::Price price = 0;
  engine::Qty qty = 0;
  std::uint32_t account = 0;
};

struct W2Params {
  engine::Price mid = 100000;
  std::uint32_t levels = 2;
  std::uint32_t per_level = 2;
  engine::Qty order_qty = 10;
};

// A shallow and a deep book. The deep one has 4,000 orders per side in reach.
inline constexpr W2Params kW2Shallow{100000, 2, 2, 10};
inline constexpr W2Params kW2Deep{100000, 500, 8, 10};

struct W2Stream {
  std::vector<FokCmd> setup;   // rests the book
  std::vector<FokCmd> rounds;  // FOK, replenish, FOK, replenish, ...
};

// The taker's account is above every id, so it matches no resting order.
inline constexpr std::uint32_t kW2TakerAccount = 0xFFFFFFF0u;

inline W2Stream generate_fok(const W2Params& p, std::size_t round_count) {
  using engine::Side;
  using engine::TimeInForce;
  W2Stream s;
  engine::OrderId next_id = 1;
  auto order = [&](Side side, engine::Price price, TimeInForce tif, std::uint32_t account) {
    FokCmd c;
    c.side = side;
    c.tif = tif;
    c.id = next_id++;
    c.price = price;
    c.qty = p.order_qty;
    c.account = account;
    return c;
  };

  for (std::uint32_t level = 1; level <= p.levels; ++level) {
    for (std::uint32_t k = 0; k < p.per_level; ++k) {
      FokCmd ask = order(Side::Sell, p.mid + level, TimeInForce::GTC, 0);
      ask.account = static_cast<std::uint32_t>(ask.id);
      s.setup.push_back(ask);
      FokCmd bid = order(Side::Buy, p.mid - level, TimeInForce::GTC, 0);
      bid.account = static_cast<std::uint32_t>(bid.id);
      s.setup.push_back(bid);
    }
  }

  const engine::Price reach = static_cast<engine::Price>(p.levels);
  for (std::size_t r = 0; r < round_count; ++r) {
    const bool buy = r % 2 == 0;
    s.rounds.push_back(order(buy ? Side::Buy : Side::Sell, buy ? p.mid + reach : p.mid - reach, TimeInForce::FOK,
                             kW2TakerAccount));
    FokCmd refill = order(buy ? Side::Sell : Side::Buy, buy ? p.mid + 1 : p.mid - 1, TimeInForce::GTC, 0);
    refill.account = static_cast<std::uint32_t>(refill.id);
    s.rounds.push_back(refill);
  }
  return s;
}

}  // namespace bench
