#pragma once

#include <cstdint>
#include <limits>

namespace engine {

using OrderId = std::uint64_t;
using Price   = std::int64_t;   // integer ticks — S1, never floating point
using Qty     = std::uint32_t;  // integer lots, min 1 — S1
using SeqNo   = std::uint64_t;  // assigned by the core on dequeue, never by
                                 // a producer — S6
using Index   = std::uint32_t;

inline constexpr Index kNilIndex = std::numeric_limits<Index>::max();

enum class Side : std::uint8_t { Buy, Sell };

// Whether the byte holds a side that exists. journal::Reader accepts the same
// two values and no others (ADR-011).
inline bool valid(Side s) { return s == Side::Buy || s == Side::Sell; }

// S5. GTC (default) rests any residual. IOC and FOK never rest -- an
// unfilled remainder is cancelled, not left in the book. FOK additionally
// pre-checks available qty and rejects the whole order, untouched, if it
// can't be fully filled -- IOC takes whatever's marketable and cancels
// the rest.
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

inline bool valid(TimeInForce t) {
  return t == TimeInForce::GTC || t == TimeInForce::IOC || t == TimeInForce::FOK;
}

// Hot/cold split per ADR-002. `timestamp` is deliberately absent: S7
// forbids wall-clock state anywhere in the core, so it cannot live here
// regardless of access frequency — ADR-002 listed it as a cold field
// before this was implemented; that was a spec error, corrected here.
//
// `level_idx` is also deliberately absent. The price levels are a sorted
// array (ADR-003) and an insert or erase away from the touch shifts every
// level behind it, so a stored index would go stale; an order's level is
// found from its side and price instead.
struct Order {
  // hot — read/written on every crossing-loop iteration
  Price price = 0;
  Qty qty = 0;
  Index next_idx = kNilIndex;
  Index prev_idx = kNilIndex;
  Side side = Side::Buy;

  // cold — read only on cancel, modify lookup, or invariant checks
  OrderId order_id = 0;
  std::uint32_t account = 0;  // STP compares this, not order_id — S3
  SeqNo seq_no = 0;
};

}  // namespace engine
