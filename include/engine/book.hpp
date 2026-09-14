#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <vector>

#include "level.hpp"
#include "pool.hpp"
#include "types.hpp"

namespace engine {

struct Trade {
  OrderId maker_order_id;
  OrderId taker_order_id;
  Price price;  // S5: always the maker's price, never the taker's
  Qty qty;
  std::uint64_t trade_id;
  SeqNo seq_no;
};

// Book owns the order pool, the two price-level containers (ADR-003's
// std::map baseline -- correctness first, not yet the campaign's flat
// array/page-table replacement), and the order_id -> handle map used for
// cancel (ADR-005).
//
// Scope: limit orders only -- insert, cross, partial fill, cancel. Not
// implemented yet: modify (S2), self-trade prevention (S3), market/IOC/FOK
// (S5). `account` is stored on Order but nothing reads it yet.
class Book {
 public:
  explicit Book(Index capacity) : pool_(capacity) {}

  struct AddResult {
    bool accepted = false;  // false only means "duplicate order_id" today
    std::vector<Trade> trades;
    Qty resting_qty = 0;  // 0 if fully filled or rejected
  };

  AddResult add_limit_order(OrderId order_id, Side side, Price price, Qty qty,
                             std::uint32_t account) {
    AddResult result;
    if (qty == 0 || id_map_.count(order_id) != 0) return result;

    Order incoming{};
    incoming.order_id = order_id;
    incoming.side = side;
    incoming.price = price;
    incoming.qty = qty;
    incoming.account = account;

    if (side == Side::Buy) {
      cross(asks_, incoming, result.trades,
            [](Price resting, Price taker) { return resting <= taker; });
    } else {
      cross(bids_, incoming, result.trades,
            [](Price resting, Price taker) { return resting >= taker; });
    }

    result.accepted = true;
    result.resting_qty = incoming.qty;
    if (incoming.qty > 0) rest(side, price, incoming);  // S5: only rests if residual > 0
    return result;
  }

  // False covers both "never existed" and "already cancelled/filled" --
  // S1/test-8 deliberately don't distinguish those at this layer.
  bool cancel_order(OrderId order_id) {
    auto it = id_map_.find(order_id);
    if (it == id_map_.end()) return false;

    Handle h = it->second;
    Order* order = pool_.get(h);
    if (order == nullptr) {  // stale handle -- shouldn't happen if id_map_
      id_map_.erase(it);     // stays in sync, but don't trust that blindly
      return false;
    }

    Level& level = level_for(order->side, order->price);
    level_unlink(level, pool_, h.index());
    if (level.head == kNilIndex) erase_level(order->side, order->price);
    pool_.free(h);
    id_map_.erase(it);
    return true;
  }

  bool has_bid() const { return !bids_.empty(); }
  bool has_ask() const { return !asks_.empty(); }
  Price best_bid() const { return bids_.begin()->first; }
  Price best_ask() const { return asks_.begin()->first; }

  // Exposed for tests that need to walk a level's FIFO order directly --
  // not part of the engine's own matching logic.
  const Order& order_at(Index idx) const { return pool_.unchecked(idx); }
  Index head_at(Side side, Price price) const {
    return (side == Side::Buy ? bids_.at(price) : asks_.at(price)).head;
  }

 private:
  using BidLevels = std::map<Price, Level, std::greater<Price>>;  // best = begin()
  using AskLevels = std::map<Price, Level, std::less<Price>>;     // best = begin()

  template <typename PriceMap, typename Crosses>
  void cross(PriceMap& resting_side, Order& incoming,
             std::vector<Trade>& trades, Crosses crosses) {
    while (incoming.qty > 0 && !resting_side.empty()) {
      auto level_it = resting_side.begin();
      if (!crosses(level_it->first, incoming.price)) break;
      Level& level = level_it->second;

      while (incoming.qty > 0 && level.head != kNilIndex) {
        Index maker_idx = level.head;
        Order& maker = pool_.unchecked(maker_idx);
        Qty fill = std::min(incoming.qty, maker.qty);

        trades.push_back({maker.order_id, incoming.order_id, maker.price,
                           fill, next_trade_id_++, next_seq_});
        incoming.qty -= fill;
        maker.qty -= fill;

        if (maker.qty == 0) {
          OrderId maker_id = maker.order_id;  // read before free() below
          level_unlink(level, pool_, maker_idx);
          id_map_.erase(maker_id);
          pool_.free(pool_.handle_of(maker_idx));
        }
        // maker.qty > 0: partial fill, stays at the head of the level --
        // it already has the earliest seq_no there, nothing to relink.
      }

      if (level.head == kNilIndex) resting_side.erase(level_it);
    }
  }

  void rest(Side side, Price price, Order& incoming) {
    Level& level = level_for_insert(side, price);
    incoming.seq_no = next_seq_++;  // S6: assigned by the core, on accept
    Handle h = pool_.allocate(incoming);
    level_append(level, pool_, h.index());
    id_map_[incoming.order_id] = h;
  }

  Level& level_for_insert(Side side, Price price) {
    if (side == Side::Buy) {
      auto [it, inserted] = bids_.try_emplace(price);
      if (inserted) it->second.price = price;
      return it->second;
    }
    auto [it, inserted] = asks_.try_emplace(price);
    if (inserted) it->second.price = price;
    return it->second;
  }

  Level& level_for(Side side, Price price) {
    return side == Side::Buy ? bids_.at(price) : asks_.at(price);
  }

  void erase_level(Side side, Price price) {
    if (side == Side::Buy) bids_.erase(price); else asks_.erase(price);
  }

  FixedPool<Order> pool_;
  BidLevels bids_;
  AskLevels asks_;
  std::unordered_map<OrderId, Handle> id_map_;
  SeqNo next_seq_ = 0;
  std::uint64_t next_trade_id_ = 0;
};

}  // namespace engine
