#pragma once

// The structural checks behind Book::invariant_violation(), written against
// the three containers rather than against Book, so a test can build each
// broken state out of the containers' own interfaces and see the message it
// produces.
//
// A snapshot only follows next_idx, so a bad prev_idx, a stale level tail, a
// leaked pool slot or a dangling id map entry would pass a snapshot diff;
// these walk all of them. The book is never crossed: crossing orders trade
// on arrival, and a modify that would cross is rejected (ADR-009).
//
// The walk is bounded by the pool capacity. "cycle in level list" is
// defence in depth and cannot be reached: a list that loops back reaches a
// node whose prev_idx is not the node just visited, and the prev_idx check
// ahead of the bound reports that first. The test builds the loops to show
// it.
#include <cstddef>

#include "level.hpp"
#include "types.hpp"

namespace engine::invariants {

// nullptr when every invariant holds, otherwise a description of the first
// violation found. `linked` counts the orders reached through the levels.
template <typename Pool, typename Levels, typename Ids>
const char* check_side(const Pool& pool, const Ids& ids, const Levels& levels, Side side,
                       std::size_t& linked) {
  for (std::size_t rank = 0; rank < levels.size(); ++rank) {
    const Level& level = levels.by_rank(rank);
    const Price price = level.price;
    if (rank > 0 && !Levels::better(levels.by_rank(rank - 1).price, price)) {
      return "price levels not strictly ordered best first";
    }
    if (level.head == kNilIndex) return "empty level left in price levels";
    Index prev = kNilIndex;
    SeqNo last_seq = 0;
    for (Index idx = level.head; idx != kNilIndex; idx = pool.unchecked(idx).next_idx) {
      if (idx >= pool.capacity()) return "next_idx out of range";
      const Order& o = pool.unchecked(idx);
      if (o.prev_idx != prev) return "prev_idx does not match traversal";
      if (o.side != side) return "order on the wrong side";
      if (o.price != price) return "order price != level price";
      if (o.qty == 0) return "resting order with qty 0";
      if (prev != kNilIndex && o.seq_no <= last_seq) return "seq_no not increasing within level";
      const Handle* mapped = ids.find(o.order_id);
      if (mapped == nullptr || mapped->index() != idx || !pool.validate(*mapped)) {
        return "id_map_ entry missing or pointing at another slot";
      }
      if (++linked > pool.capacity()) return "cycle in level list";
      prev = idx;
      last_seq = o.seq_no;
    }
    if (level.tail != prev) return "level.tail is not the last linked order";
  }
  return nullptr;
}

template <typename Pool, typename Bids, typename Asks, typename Ids>
const char* violation(const Pool& pool, const Bids& bids, const Asks& asks, const Ids& ids) {
  if (!bids.empty() && !asks.empty() && bids.best().price >= asks.best().price) return "book is crossed";
  std::size_t linked = 0;
  if (const char* v = check_side(pool, ids, bids, Side::Buy, linked)) return v;
  if (const char* v = check_side(pool, ids, asks, Side::Sell, linked)) return v;
  if (linked != ids.size()) return "id_map_ size != orders linked into levels";
  std::size_t in_use = static_cast<std::size_t>(pool.capacity()) - pool.free_slots() - pool.retired_slots();
  if (in_use != linked) return "pool slots in use != orders linked into levels";
  return nullptr;
}

}  // namespace engine::invariants
