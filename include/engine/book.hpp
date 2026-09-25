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
// Scope: limit orders (GTC/IOC/FOK) and market orders -- insert, cross,
// partial fill, cancel, modify (S2), self-trade prevention (S3), S5's
// time-in-force variants. Not implemented yet: the oracle/differential
// fuzzer (M2), journal/replay (M3+).
class Book {
 public:
  explicit Book(Index capacity) : pool_(capacity) {}

  // S3: reject the incoming aggressive quantity when it would self-match
  // (same account); the resting order is untouched. "Reject the quantity,"
  // not "skip this maker" -- so the entire crossing attempt stops the
  // instant a self-match is reached, and whatever incoming qty remains is
  // rejected outright, never rested. This is "cancel newest," the
  // simplest deterministic policy: no relinking of the resting side is
  // needed to prove correct, because the resting side is never touched.
  struct AddResult {
    bool accepted = false;  // false only means "duplicate order_id" or "qty == 0" today
    std::vector<Trade> trades;
    Qty resting_qty = 0;  // 0 if fully filled, STP-rejected, pool-exhausted, or non-marketable-and-rested-elsewhere
    bool stp_rejected = false;  // true if a self-match stopped crossing early
    bool pool_exhausted = false;  // true if a residual couldn't be rested --
        // ADR-001/pool.hpp's "clean rejection, not a condition to retry"
        // contract; see test 19. Any crossing already executed before this
        // point stands (S8: a command that's run can't be unwound) -- this
        // only means the leftover residual is dropped instead of resting.
  };

  // S5. tif defaults to GTC (rests any residual). IOC/FOK never rest --
  // see TimeInForce's doc comment in types.hpp.
  //
  // FOK's pre-check and the crossing that follows must agree on where
  // crossing would stop, or the "atomic" guarantee is a lie -- a pre-check
  // that only summed qty-at-price would say yes, then cross() would say
  // stp_rejected on hitting a self-match order, and the order rests 0 but
  // already-executed trades happened anyway (not atomic). So
  // available_before_self_trade() below walks the book the *same way*
  // cross() does -- same price bound, same per-order self-match stop --
  // and both S8 (nothing can insert between pre-check and fill, this
  // engine is single-threaded and a command runs to completion) and this
  // shared-walk agreement are what make the atomicity claim actually true,
  // not just asserted (test 14b exercises the disagreement this would
  // cause if the pre-check ignored STP).
  AddResult add_limit_order(OrderId order_id, Side side, Price price, Qty qty,
                             std::uint32_t account,
                             TimeInForce tif = TimeInForce::GTC) {
    AddResult result;
    if (qty == 0 || id_map_.count(order_id) != 0) return result;

    if (tif == TimeInForce::FOK) {
      Qty available = (side == Side::Buy)
          ? available_before_self_trade(asks_, price, account,
                [](Price resting, Price taker) { return resting <= taker; })
          : available_before_self_trade(bids_, price, account,
                [](Price resting, Price taker) { return resting >= taker; });
      if (available < qty) {
        result.accepted = true;  // valid, well-formed order -- rejected on
        return result;           // liquidity grounds, book untouched (S5)
      }
    }

    Order incoming{};
    incoming.order_id = order_id;
    incoming.side = side;
    incoming.price = price;
    incoming.qty = qty;
    incoming.account = account;

    if (side == Side::Buy) {
      result.stp_rejected = cross(asks_, incoming, result.trades,
            [](Price resting, Price taker) { return resting <= taker; });
    } else {
      result.stp_rejected = cross(bids_, incoming, result.trades,
            [](Price resting, Price taker) { return resting >= taker; });
    }

    result.accepted = true;
    bool may_rest = (tif == TimeInForce::GTC) && !result.stp_rejected;
    if (may_rest && incoming.qty > 0) {
      if (rest(side, price, incoming)) {
        result.resting_qty = incoming.qty;
      } else {
        result.pool_exhausted = true;  // resting_qty stays 0 -- dropped,
      }                                 // not corrupted into the book
    }
    return result;
  }

  // S5. No price bound -- crosses() is unconditionally true, so the sweep
  // runs until incoming.qty is exhausted or the opposite side empties out.
  // Never rests (there's no price to rest *at*); an unfilled remainder is
  // simply not rested, same as IOC. STP still applies -- a self-match still
  // stops the sweep and whatever's left goes unfilled rather than crossing
  // through it (S3 makes no TIF exception).
  AddResult add_market_order(OrderId order_id, Side side, Qty qty,
                              std::uint32_t account) {
    AddResult result;
    if (qty == 0 || id_map_.count(order_id) != 0) return result;

    Order incoming{};
    incoming.order_id = order_id;
    incoming.side = side;
    incoming.qty = qty;
    incoming.account = account;

    if (side == Side::Buy) {
      result.stp_rejected = cross(asks_, incoming, result.trades,
            [](Price, Price) { return true; });
    } else {
      result.stp_rejected = cross(bids_, incoming, result.trades,
            [](Price, Price) { return true; });
    }

    result.accepted = true;
    result.resting_qty = 0;  // market orders never rest -- S5
    return result;
  }

  // S2: modify never frees the pool slot -- handle and order_id stay
  // stable for the order's whole lifetime. Never crosses the book, even
  // if the new price would be marketable (spec is silent on that case by
  // omission, not oversight: modify only repositions, matching is
  // add_limit_order's job alone).
  //
  // Rejection reuses cancel_order's "unknown or already gone" path
  // exactly -- a modify on an order_id that was cancelled, or one whose
  // slot was freed via a fill in cross(), fails the same id_map_ lookup
  // (test 8b, 8c).
  bool modify_order(OrderId order_id, Price new_price, Qty new_qty) {
    if (new_qty == 0) return false;

    auto it = id_map_.find(order_id);
    if (it == id_map_.end()) return false;

    Handle h = it->second;
    Order* order = pool_.get(h);
    if (order == nullptr) {  // see cancel_order's identical guard
      id_map_.erase(it);
      return false;
    }

    // Qty-reduce at an unchanged price keeps time priority -- update in
    // place, no unlink. Everything else (qty-increase, price-change, or
    // both) loses priority per S2 and goes through unlink+append as one
    // operation -- including landing back in the same level, which
    // level_unlink()/level_append() already compose correctly for (see
    // level.hpp; this is the path test 12b exercises).
    if (new_price == order->price && new_qty <= order->qty) {
      order->qty = new_qty;
      return true;
    }

    Level& old_level = level_for(order->side, order->price);
    level_unlink(old_level, pool_, h.index());
    if (old_level.head == kNilIndex) erase_level(order->side, order->price);

    order->price = new_price;
    order->qty = new_qty;
    order->seq_no = next_seq_++;  // S6: loses priority, gets a fresh seq_no

    Level& new_level = level_for_insert(order->side, new_price);
    level_append(new_level, pool_, h.index());
    return true;
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

  // Returns true iff a self-match stopped the sweep before `incoming` was
  // exhausted -- the caller must not rest the residual in that case (S3).
  template <typename PriceMap, typename Crosses>
  bool cross(PriceMap& resting_side, Order& incoming,
             std::vector<Trade>& trades, Crosses crosses) {
    while (incoming.qty > 0 && !resting_side.empty()) {
      auto level_it = resting_side.begin();
      if (!crosses(level_it->first, incoming.price)) break;
      Level& level = level_it->second;

      while (incoming.qty > 0 && level.head != kNilIndex) {
        Index maker_idx = level.head;
        Order& maker = pool_.unchecked(maker_idx);

        if (maker.account == incoming.account) return true;  // S3

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
    return false;
  }

  // FOK pre-check. Walks the same levels, in the same order, under the
  // same price bound as cross() would -- summing qty until either the
  // book/price-bound runs out (return what was found) or an order whose
  // account matches `account` is reached (stop and return the running
  // total *without* counting that order, mirroring cross()'s "self-match
  // stops the sweep, that order is never touched" rule). Read-only: no
  // trades, no mutation, nothing to unwind if the caller decides to
  // reject.
  template <typename PriceMap, typename Crosses>
  Qty available_before_self_trade(const PriceMap& resting_side, Price limit_price,
                                   std::uint32_t account, Crosses crosses) const {
    Qty total = 0;
    for (const auto& [level_price, level] : resting_side) {
      if (!crosses(level_price, limit_price)) break;
      Index idx = level.head;
      while (idx != kNilIndex) {
        const Order& order = pool_.unchecked(idx);
        if (order.account == account) return total;  // S3 boundary
        total += order.qty;
        idx = order.next_idx;
      }
    }
    return total;
  }

  // Returns false if the pool is exhausted -- ADR-001/pool.hpp's "clean
  // rejection, not a condition to retry" contract, finally wired up here.
  // Before this fix, an invalid Handle's index() (kNilIndex, i.e.
  // 0xFFFFFFFF) flowed straight into level_append()'s pool_.unchecked()
  // unchecked -- an out-of-bounds write past the pool's backing vector.
  // seq_no is only burned on success: an order that never entered the
  // book shouldn't consume a sequence number. level_for_insert() may have
  // just created a fresh empty Level for a price with no prior resting
  // orders -- clean that up on the failure path so a full pool doesn't
  // also leak empty levels into the price-level maps.
  bool rest(Side side, Price price, Order& incoming) {
    Level& level = level_for_insert(side, price);
    Handle h = pool_.allocate(incoming);
    if (!h.valid()) {
      if (level.head == kNilIndex) erase_level(side, price);
      return false;
    }
    pool_.get(h)->seq_no = next_seq_++;  // S6: assigned by the core, on accept
    level_append(level, pool_, h.index());
    id_map_[incoming.order_id] = h;
    return true;
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
