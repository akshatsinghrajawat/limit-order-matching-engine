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

// Hot/cold split per ADR-002. `timestamp` is deliberately absent: S7
// forbids wall-clock state anywhere in the core, so it cannot live here
// regardless of access frequency — ADR-002 listed it as a cold field
// before this was implemented; that was a spec error, corrected here.
//
// `level_idx` is also deliberately absent. The book's price-level
// container (ADR-003) isn't built yet, and its final shape (map vs. flat
// array vs. page table) determines how an order should reference its
// level — adding that field now would be a guess this struct would have
// to unlearn later.
struct Order {
  // hot — read/written on every crossing-loop iteration
  Price price = 0;
  Qty qty = 0;
  Index next_idx = kNilIndex;
  Index prev_idx = kNilIndex;
  Side side = Side::Buy;

  // cold — read only on cancel, modify lookup, or audit
  OrderId order_id = 0;
  std::uint32_t account = 0;  // STP compares this, not order_id — S3
  SeqNo seq_no = 0;
};

}  // namespace engine
