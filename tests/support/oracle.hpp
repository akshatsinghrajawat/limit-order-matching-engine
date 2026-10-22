#pragma once

// Reference implementation of docs/spec.md, written to be correct by
// inspection rather than fast. It is the second, independent opinion the
// differential tests compare the engine against, so it shares nothing with
// the engine except the plain type aliases in engine/types.hpp: no pool, no
// handles, no intrusive lists, no price maps. State is one flat vector of
// resting orders, and every question is answered by scanning it.
//
// Do not "fix" a divergence by making this look more like book.hpp. If the
// two disagree, one of them misreads the spec, and the spec decides. The
// exceptions are the two points the spec leaves open, what a trade's seq_no
// holds and whether modify can cross the book: there this follows the
// engine, as test_oracle.cpp spells out.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "engine/types.hpp"

namespace oracle {

using engine::OrderId;
using engine::Price;
using engine::Qty;
using engine::SeqNo;
using engine::Side;
using engine::TimeInForce;

struct Trade {
  OrderId maker_order_id;
  OrderId taker_order_id;
  Price price;
  Qty qty;
  std::uint64_t trade_id;
  SeqNo seq_no;
};

struct Resting {
  Side side;
  Price price;
  OrderId order_id;
  Qty qty;
  std::uint32_t account;
  SeqNo seq_no;
};

struct Result {
  bool accepted = false;
  std::vector<Trade> trades;
  Qty resting_qty = 0;
  bool stp_rejected = false;
  bool pool_exhausted = false;
};

class Oracle {
 public:
  explicit Oracle(std::size_t capacity) : capacity_(capacity) {}

  Result add_limit(OrderId id, Side side, Price price, Qty qty,
                   std::uint32_t account, TimeInForce tif) {
    return submit(id, side, true, price, qty, account, tif);
  }

  // S5: no price bound, never rests.
  Result add_market(OrderId id, Side side, Qty qty, std::uint32_t account) {
    return submit(id, side, false, 0, qty, account, TimeInForce::IOC);
  }

  bool cancel(OrderId id) {
    std::size_t i = find(id);
    if (i == npos) return false;
    orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
  }

  // S2: only a pure quantity reduction at the same price keeps priority.
  // Anything else gets a fresh seq_no, which puts it behind every order
  // already at its (possibly unchanged) price. Never crosses the book.
  bool modify(OrderId id, Price new_price, Qty new_qty) {
    if (new_qty == 0) return false;
    std::size_t i = find(id);
    if (i == npos) return false;
    Resting& o = orders_[i];
    if (new_price == o.price && new_qty <= o.qty) {
      o.qty = new_qty;
      return true;
    }
    o.price = new_price;
    o.qty = new_qty;
    o.seq_no = next_seq_++;
    return true;
  }

  // Bids best-first then asks best-first, FIFO (seq_no) within a price.
  std::vector<Resting> snapshot() const {
    std::vector<Resting> out = orders_;
    std::sort(out.begin(), out.end(), [](const Resting& a, const Resting& b) {
      if (a.side != b.side) return a.side == Side::Buy;
      if (a.price != b.price) {
        return a.side == Side::Buy ? a.price > b.price : a.price < b.price;
      }
      return a.seq_no < b.seq_no;
    });
    return out;
  }

 private:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  std::size_t find(OrderId id) const {
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      if (orders_[i].order_id == id) return i;
    }
    return npos;
  }

  static bool crosses(Side taker, Price resting, Price limit) {
    return taker == Side::Buy ? resting <= limit : resting >= limit;
  }

  // Price priority for a resting order against the given taker side, then
  // time priority.
  static bool better(Side taker, const Resting& a, const Resting& b) {
    if (a.price != b.price) {
      return taker == Side::Buy ? a.price < b.price : a.price > b.price;
    }
    return a.seq_no < b.seq_no;
  }

  std::size_t best_opposite(Side taker, bool has_limit, Price limit) const {
    std::size_t best = npos;
    for (std::size_t i = 0; i < orders_.size(); ++i) {
      const Resting& o = orders_[i];
      if (o.side == taker) continue;
      if (has_limit && !crosses(taker, o.price, limit)) continue;
      if (best == npos || better(taker, o, orders_[best])) best = i;
    }
    return best;
  }

  // S3 + S5: what a FOK order could take. The sweep stops at the first
  // order of the taker's own account, so liquidity behind it does not count.
  std::uint64_t fillable_before_self_trade(Side taker, Price limit,
                                           std::uint32_t account) const {
    std::vector<Resting> candidates;
    for (const Resting& o : orders_) {
      if (o.side != taker && crosses(taker, o.price, limit)) candidates.push_back(o);
    }
    std::sort(candidates.begin(), candidates.end(),
              [taker](const Resting& a, const Resting& b) { return better(taker, a, b); });
    std::uint64_t total = 0;
    for (const Resting& o : candidates) {
      if (o.account == account) break;
      total += o.qty;
    }
    return total;
  }

  Result submit(OrderId id, Side side, bool has_limit, Price limit, Qty qty,
                std::uint32_t account, TimeInForce tif) {
    Result r;
    if (qty == 0 || find(id) != npos) return r;
    r.accepted = true;

    if (tif == TimeInForce::FOK && fillable_before_self_trade(side, limit, account) < qty) {
      return r;
    }

    while (qty > 0) {
      std::size_t m = best_opposite(side, has_limit, limit);
      if (m == npos) break;
      if (orders_[m].account == account) {
        r.stp_rejected = true;
        break;
      }
      Qty fill = std::min(qty, orders_[m].qty);
      r.trades.push_back({orders_[m].order_id, id, orders_[m].price, fill,
                          next_trade_id_++, next_seq_});
      qty -= fill;
      orders_[m].qty -= fill;
      if (orders_[m].qty == 0) orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(m));
    }

    if (tif == TimeInForce::GTC && has_limit && !r.stp_rejected && qty > 0) {
      if (orders_.size() >= capacity_) {
        r.pool_exhausted = true;
      } else {
        orders_.push_back({side, limit, id, qty, account, next_seq_++});
        r.resting_qty = qty;
      }
    }
    return r;
  }

  std::size_t capacity_;
  std::vector<Resting> orders_;
  SeqNo next_seq_ = 0;
  std::uint64_t next_trade_id_ = 0;
};

}  // namespace oracle
