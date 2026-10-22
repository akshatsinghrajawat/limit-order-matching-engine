#pragma once

// Differential harness: runs every operation on the engine and on the
// oracle and reports the first place they disagree. Kept separate from the
// fuzzer so any driver can reuse it.
//
// Failures are returned as strings rather than asserted so the tests still
// verify under -DNDEBUG and can print a reproducing seed and the last few
// operations.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "engine/book.hpp"
#include "oracle.hpp"

namespace diff {

using engine::OrderId;
using engine::Price;
using engine::Qty;
using engine::Side;
using engine::TimeInForce;

// splitmix64. Hand-rolled because <random>'s distributions are not
// specified to produce the same sequence across standard libraries, and a
// seed that only reproduces on one machine is not a reproducible seed.
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

enum class Kind : std::uint8_t { Limit, Market, Cancel, Modify };

struct Op {
  Kind kind = Kind::Limit;
  OrderId id = 0;
  Side side = Side::Buy;
  Price price = 0;
  Qty qty = 0;
  std::uint32_t account = 0;
  TimeInForce tif = TimeInForce::GTC;
};

inline std::string describe(const Op& op) {
  static const char* kinds[] = {"limit", "market", "cancel", "modify"};
  static const char* tifs[] = {"GTC", "IOC", "FOK"};
  char buf[160];
  std::snprintf(buf, sizeof buf, "%s id=%llu side=%s price=%lld qty=%u acct=%u tif=%s",
                kinds[static_cast<int>(op.kind)], static_cast<unsigned long long>(op.id),
                op.side == Side::Buy ? "buy" : "sell", static_cast<long long>(op.price),
                op.qty, op.account, tifs[static_cast<int>(op.tif)]);
  return buf;
}

// Counts of the interesting paths taken, so a run can show it was not
// vacuous (a fuzzer that never self-trades proves nothing about STP).
struct Stats {
  std::uint64_t ops = 0, trades = 0, stp = 0, fok_rejected = 0, pool_exhausted = 0;
  std::uint64_t modify_ok = 0, cancel_ok = 0, invalid_rejected = 0, crossed_states = 0;

  void add(const Stats& o) {
    ops += o.ops; trades += o.trades; stp += o.stp; fok_rejected += o.fok_rejected;
    pool_exhausted += o.pool_exhausted; modify_ok += o.modify_ok; cancel_ok += o.cancel_ok;
    invalid_rejected += o.invalid_rejected; crossed_states += o.crossed_states;
  }
};

class Pair {
 public:
  explicit Pair(engine::Index capacity) : book(capacity), ref(capacity) {}

  engine::Book book;
  oracle::Oracle ref;
  Stats stats;

  // Empty string means the engine and the oracle agreed on this operation.
  std::string apply(const Op& op) {
    ++stats.ops;
    switch (op.kind) {
      case Kind::Limit:
        return compare_add(op, book.add_limit_order(op.id, op.side, op.price, op.qty, op.account, op.tif),
                           ref.add_limit(op.id, op.side, op.price, op.qty, op.account, op.tif));
      case Kind::Market:
        return compare_add(op, book.add_market_order(op.id, op.side, op.qty, op.account),
                           ref.add_market(op.id, op.side, op.qty, op.account));
      case Kind::Cancel: {
        bool a = book.cancel_order(op.id);
        bool b = ref.cancel(op.id);
        if (a != b) return flag("cancel", a, b);
        stats.cancel_ok += a;
        return {};
      }
      case Kind::Modify: {
        bool a = book.modify_order(op.id, op.price, op.qty);
        bool b = ref.modify(op.id, op.price, op.qty);
        if (a != b) return flag("modify", a, b);
        stats.modify_ok += a;
        return {};
      }
    }
    return {};
  }

  // Structural invariants, then a full book comparison.
  std::string check_state() {
    if (const char* v = book.invariant_violation()) return std::string("invariant: ") + v;

    auto a = book.snapshot();
    auto b = ref.snapshot();
    if (a.size() != b.size()) {
      return "resting order count engine=" + std::to_string(a.size()) +
             " oracle=" + std::to_string(b.size());
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      bool same = a[i].side == b[i].side && a[i].price == b[i].price &&
                  a[i].order_id == b[i].order_id && a[i].qty == b[i].qty &&
                  a[i].account == b[i].account && a[i].seq_no == b[i].seq_no;
      if (!same) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "book[%zu] engine{id=%llu px=%lld qty=%u acct=%u seq=%llu} "
                      "oracle{id=%llu px=%lld qty=%u acct=%u seq=%llu}",
                      i, (unsigned long long)a[i].order_id, (long long)a[i].price, a[i].qty,
                      a[i].account, (unsigned long long)a[i].seq_no,
                      (unsigned long long)b[i].order_id, (long long)b[i].price, b[i].qty,
                      b[i].account, (unsigned long long)b[i].seq_no);
        return buf;
      }
    }
    if (book.has_bid() && book.has_ask() && book.best_bid() >= book.best_ask()) ++stats.crossed_states;
    return {};
  }

 private:
  static std::string flag(const char* what, bool engine_v, bool oracle_v) {
    return std::string(what) + " result engine=" + (engine_v ? "true" : "false") +
           " oracle=" + (oracle_v ? "true" : "false");
  }

  template <typename E, typename O>
  std::string compare_add(const Op& op, const E& a, const O& b) {
    if (!a.accepted) ++stats.invalid_rejected;
    if (a.stp_rejected) ++stats.stp;
    if (a.pool_exhausted) ++stats.pool_exhausted;
    if (op.tif == TimeInForce::FOK && op.kind == Kind::Limit && a.accepted && a.trades.empty()) {
      ++stats.fok_rejected;
    }
    stats.trades += a.trades.size();

    if (a.accepted != b.accepted) return flag("accepted", a.accepted, b.accepted);
    if (a.stp_rejected != b.stp_rejected) return flag("stp_rejected", a.stp_rejected, b.stp_rejected);
    if (a.pool_exhausted != b.pool_exhausted) {
      return flag("pool_exhausted", a.pool_exhausted, b.pool_exhausted);
    }
    if (a.resting_qty != b.resting_qty) {
      return "resting_qty engine=" + std::to_string(a.resting_qty) +
             " oracle=" + std::to_string(b.resting_qty);
    }
    if (a.trades.size() != b.trades.size()) {
      return "trade count engine=" + std::to_string(a.trades.size()) +
             " oracle=" + std::to_string(b.trades.size());
    }
    for (std::size_t i = 0; i < a.trades.size(); ++i) {
      const auto& x = a.trades[i];
      const auto& y = b.trades[i];
      bool same = x.maker_order_id == y.maker_order_id && x.taker_order_id == y.taker_order_id &&
                  x.price == y.price && x.qty == y.qty && x.trade_id == y.trade_id &&
                  x.seq_no == y.seq_no;
      if (!same) {
        char buf[256];
        std::snprintf(buf, sizeof buf,
                      "trade[%zu] engine{maker=%llu taker=%llu px=%lld qty=%u tid=%llu seq=%llu} "
                      "oracle{maker=%llu taker=%llu px=%lld qty=%u tid=%llu seq=%llu}",
                      i, (unsigned long long)x.maker_order_id, (unsigned long long)x.taker_order_id,
                      (long long)x.price, x.qty, (unsigned long long)x.trade_id,
                      (unsigned long long)x.seq_no, (unsigned long long)y.maker_order_id,
                      (unsigned long long)y.taker_order_id, (long long)y.price, y.qty,
                      (unsigned long long)y.trade_id, (unsigned long long)y.seq_no);
        return buf;
      }
    }
    return {};
  }
};

// Random operation stream. Prices sit in a narrow band so levels collide
// and orders cross constantly; four accounts make self-trades common; ids
// are mostly fresh, sometimes reused (duplicates, reuse after a fill), and
// cancels and modifies mostly target recently issued ids, which are often
// already gone.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : rng_(seed) {}

  Op next() {
    Op op;
    op.account = 1 + static_cast<std::uint32_t>(rng_.below(4));
    op.side = rng_.below(2) ? Side::Buy : Side::Sell;
    std::uint64_t r = rng_.below(100);

    if (r < 62) {
      op.kind = r < 56 ? Kind::Limit : Kind::Market;
      op.tif = r < 40 ? TimeInForce::GTC : r < 48 ? TimeInForce::IOC : TimeInForce::FOK;
      if (op.kind == Kind::Market) op.tif = TimeInForce::IOC;
      op.id = fresh_or_reused();
      op.price = pick_price();
      op.qty = 1 + static_cast<Qty>(rng_.below(8));
      remember(op.id, op.price);
    } else if (r < 79) {
      op.kind = Kind::Cancel;
      op.id = issued_.empty() ? 0 : issued_[pick_known()];
    } else if (r < 97) {
      op.kind = Kind::Modify;
      std::size_t k = pick_known();
      op.id = issued_.empty() ? 0 : issued_[k];
      op.price = (!issued_.empty() && rng_.below(10) < 4) ? price_[k] : pick_price();
      op.qty = 1 + static_cast<Qty>(rng_.below(10));
      if (!issued_.empty()) price_[k] = op.price;
    } else {
      std::uint64_t which = rng_.below(3);
      if (which == 0) {
        op.kind = Kind::Limit;
        op.id = fresh_or_reused();
        op.price = pick_price();
        op.qty = 0;
      } else if (which == 1) {
        op.kind = Kind::Modify;
        op.id = issued_.empty() ? 0 : issued_[pick_known()];
        op.price = pick_price();
        op.qty = 0;
      } else {
        op.kind = Kind::Limit;
        op.id = issued_.empty() ? 1 : issued_[pick_known()];
        op.price = pick_price();
        op.qty = 1 + static_cast<Qty>(rng_.below(8));
      }
    }
    return op;
  }

 private:
  Price pick_price() {
    if (rng_.below(100) < 5) return 80 + static_cast<Price>(rng_.below(41));
    return 95 + static_cast<Price>(rng_.below(11));
  }

  OrderId fresh_or_reused() {
    if (!issued_.empty() && rng_.below(100) < 4) return issued_[pick_known()];
    return ++next_id_;
  }

  std::size_t pick_known() {
    if (issued_.empty()) return 0;
    std::size_t n = issued_.size();
    if (rng_.below(100) < 85) {
      std::size_t window = n < 64 ? n : 64;
      return n - 1 - static_cast<std::size_t>(rng_.below(window));
    }
    return static_cast<std::size_t>(rng_.below(n));
  }

  void remember(OrderId id, Price price) {
    issued_.push_back(id);
    price_.push_back(price);
  }

  Rng rng_;
  OrderId next_id_ = 0;
  std::vector<OrderId> issued_;
  std::vector<Price> price_;
};

}  // namespace diff
