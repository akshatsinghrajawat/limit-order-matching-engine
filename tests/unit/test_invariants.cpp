// Every check in invariants.hpp, one broken state each, compared by the exact
// message. The states are built from the containers' own interfaces, so a
// check that stops firing, fires on the wrong state or reports another
// message shows up here and not only as a difference in a long fuzz run.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "engine/id_map.hpp"
#include "engine/invariants.hpp"
#include "engine/level.hpp"
#include "engine/pool.hpp"
#include "engine/price_levels.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace engine;

namespace {

constexpr Index kCap = 8;

// The three containers a Book is made of, filled through their own
// interfaces. rest() links the order itself rather than through level.hpp,
// so the walk is checked against an independent writer of the list.
template <std::uint32_t MaxGen = std::numeric_limits<std::uint32_t>::max()>
struct Rig {
  FixedPool<Order, MaxGen> pool{kCap};
  PriceLevels<Best::High> bids{kCap + 1};
  PriceLevels<Best::Low> asks{kCap + 1};
  IdMap ids{kCap};
  SeqNo next_seq = 0;

  Index rest(Side side, Price price, OrderId id, Qty qty = 1) {
    Order o;
    o.side = side;
    o.price = price;
    o.order_id = id;
    o.qty = qty;
    o.seq_no = next_seq++;
    const Handle h = pool.allocate(o);
    CHECK(h.valid());
    Level* level = side == Side::Buy ? bids.try_get_or_insert(price) : asks.try_get_or_insert(price);
    CHECK(level != nullptr);
    if (level->tail == kNilIndex) {
      level->head = h.index();
    } else {
      pool.unchecked(level->tail).next_idx = h.index();
      pool.unchecked(h.index()).prev_idx = level->tail;
    }
    level->tail = h.index();
    CHECK(ids.insert(id, h));
    return h.index();
  }

  const char* violation() const { return invariants::violation(pool, bids, asks, ids); }
};

// Bids 100: {1, 2}, 99: {3}; ask 103: {4}. The slots are returned so a test
// can break one of them.
struct Slots {
  Index first, second, third, ask;
};

template <std::uint32_t MaxGen>
Slots fill(Rig<MaxGen>& r) {
  Slots s;
  s.first = r.rest(Side::Buy, 100, 1);
  s.second = r.rest(Side::Buy, 100, 2);
  s.third = r.rest(Side::Buy, 99, 3);
  s.ask = r.rest(Side::Sell, 103, 4);
  return s;
}

void is(const char* got, const char* want) {
  if (got == nullptr || std::strcmp(got, want) != 0) {
    std::fprintf(stderr, "wanted \"%s\", got \"%s\"\n", want, got != nullptr ? got : "(none)");
    std::abort();
  }
}

void a_valid_book_has_no_violation() {
  Rig<> empty;
  CHECK(empty.violation() == nullptr);
  Rig<> r;
  fill(r);
  CHECK(r.violation() == nullptr);
}

void the_book_is_crossed() {
  Rig<> r;
  fill(r);
  r.rest(Side::Sell, 100, 5);  // an ask at the best bid, never traded
  is(r.violation(), "book is crossed");
}

void levels_are_out_of_order() {
  Rig<> r;
  fill(r);
  r.bids.find(99)->price = 100;
  is(r.violation(), "price levels not strictly ordered best first");
}

void a_level_is_empty() {
  Rig<> r;
  fill(r);
  r.bids.try_get_or_insert(98);
  is(r.violation(), "empty level left in price levels");
}

void a_link_points_outside_the_pool() {
  Rig<> r;
  const Slots s = fill(r);
  r.pool.unchecked(s.first).next_idx = kCap + 100;
  is(r.violation(), "next_idx out of range");
}

void a_back_link_is_wrong() {
  Rig<> r;
  const Slots s = fill(r);
  r.pool.unchecked(s.second).prev_idx = kNilIndex;
  is(r.violation(), "prev_idx does not match traversal");
}

void an_order_is_on_the_wrong_side() {
  Rig<> r;
  const Slots s = fill(r);
  r.pool.unchecked(s.first).side = Side::Sell;
  is(r.violation(), "order on the wrong side");
}

void an_order_is_at_another_price() {
  Rig<> r;
  const Slots s = fill(r);
  r.pool.unchecked(s.first).price = 101;
  is(r.violation(), "order price != level price");
}

void an_order_has_no_quantity() {
  Rig<> r;
  const Slots s = fill(r);
  r.pool.unchecked(s.first).qty = 0;
  is(r.violation(), "resting order with qty 0");
}

void sequence_numbers_do_not_increase() {
  Rig<> r;
  const Slots s = fill(r);
  r.pool.unchecked(s.second).seq_no = r.pool.unchecked(s.first).seq_no;
  is(r.violation(), "seq_no not increasing within level");
}

void the_id_map_has_no_entry_for_an_order() {
  Rig<> r;
  fill(r);
  CHECK(r.ids.erase(1));
  is(r.violation(), "id_map_ entry missing or pointing at another slot");
}

void the_id_map_entry_points_at_another_slot() {
  Rig<> r;
  const Slots s = fill(r);
  CHECK(r.ids.erase(1));
  CHECK(r.ids.insert(1, r.pool.handle_of(s.third)));
  is(r.violation(), "id_map_ entry missing or pointing at another slot");
}

void the_level_tail_is_not_the_last_order() {
  Rig<> r;
  const Slots s = fill(r);
  r.bids.find(100)->tail = s.first;
  is(r.violation(), "level.tail is not the last linked order");
}

void the_id_map_holds_an_extra_entry() {
  Rig<> r;
  const Slots s = fill(r);
  CHECK(r.ids.insert(999, r.pool.handle_of(s.first)));
  is(r.violation(), "id_map_ size != orders linked into levels");
}

void a_pool_slot_is_leaked() {
  Rig<> r;
  fill(r);
  CHECK(r.pool.allocate(Order{}).valid());
  is(r.violation(), "pool slots in use != orders linked into levels");
}

// "cycle in level list" cannot be reached. A list that loops back reaches a
// node whose prev_idx is not the node just left, and that check comes first.
void a_loop_in_a_level_is_reported_by_its_back_link() {
  {
    Rig<> r;
    const Slots s = fill(r);
    r.pool.unchecked(s.second).next_idx = s.second;  // an order that follows itself
    is(r.violation(), "prev_idx does not match traversal");
  }
  {
    Rig<> r;
    const Slots s = fill(r);
    r.pool.unchecked(s.second).next_idx = s.first;  // the tail links back to the head
    is(r.violation(), "prev_idx does not match traversal");
  }
  {
    Rig<> r;
    const Slots s = fill(r);
    r.pool.unchecked(s.second).next_idx = s.first;
    r.pool.unchecked(s.first).prev_idx = s.second;  // and the head agrees
    is(r.violation(), "prev_idx does not match traversal");
  }
}

// A retired slot is neither free nor in use, so it must not read as a leak.
void a_retired_slot_is_not_a_leak() {
  Rig<2> r;
  const Handle a = r.pool.allocate(Order{});
  r.pool.free(a);
  const Handle b = r.pool.allocate(Order{});
  CHECK(b.index() == a.index());
  r.pool.free(b);  // the second free of the slot is its last generation
  CHECK(r.pool.retired_slots() == 1);
  fill(r);
  CHECK(r.violation() == nullptr);
}

}  // namespace

int main() {
  a_valid_book_has_no_violation();
  the_book_is_crossed();
  levels_are_out_of_order();
  a_level_is_empty();
  a_link_points_outside_the_pool();
  a_back_link_is_wrong();
  an_order_is_on_the_wrong_side();
  an_order_is_at_another_price();
  an_order_has_no_quantity();
  sequence_numbers_do_not_increase();
  the_id_map_has_no_entry_for_an_order();
  the_id_map_entry_points_at_another_slot();
  the_level_tail_is_not_the_last_order();
  the_id_map_holds_an_extra_entry();
  a_pool_slot_is_leaked();
  a_loop_in_a_level_is_reported_by_its_back_link();
  a_retired_slot_is_not_a_leak();
  std::puts("invariants ok");
}
