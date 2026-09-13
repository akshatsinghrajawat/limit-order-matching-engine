#pragma once

#include "pool.hpp"
#include "types.hpp"

namespace engine {

// ADR-004. head/tail are raw indices, not Handles — an order's own
// next_idx/prev_idx only ever point at other slots currently linked into
// this same list, so there's nothing to validate a generation against.
struct Level {
  Price price = 0;
  Index head = kNilIndex;
  Index tail = kNilIndex;
};

// O(1). Caller guarantees order_idx is currently unlinked (next_idx ==
// prev_idx == kNilIndex on entry) — appending an already-linked order
// corrupts both lists it ends up half-in.
inline void level_append(Level& level, FixedPool<Order>& pool, Index order_idx) {
  Order& order = pool.unchecked(order_idx);
  order.prev_idx = level.tail;
  order.next_idx = kNilIndex;
  if (level.tail != kNilIndex) {
    pool.unchecked(level.tail).next_idx = order_idx;
  } else {
    level.head = order_idx;
  }
  level.tail = order_idx;
}

// O(1). Leaves order_idx's own links reset to kNilIndex so it's safe to
// level_append() it again immediately — this is deliberate: S2's
// qty-increase/price-change modify is unlink-then-append with no special
// case for landing back in the same level, because these two primitives
// already compose to do that correctly. A same-level modify is just
// level_unlink() followed by level_append() on the same Level — nothing
// else needs to know it was a "same-level" case.
inline void level_unlink(Level& level, FixedPool<Order>& pool, Index order_idx) {
  Order& order = pool.unchecked(order_idx);
  Index prev = order.prev_idx;
  Index next = order.next_idx;

  if (prev != kNilIndex) {
    pool.unchecked(prev).next_idx = next;
  } else {
    level.head = next;
  }
  if (next != kNilIndex) {
    pool.unchecked(next).prev_idx = prev;
  } else {
    level.tail = prev;
  }

  order.prev_idx = kNilIndex;
  order.next_idx = kNilIndex;
}

}  // namespace engine
