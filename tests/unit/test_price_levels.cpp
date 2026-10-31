// PriceLevels against std::map as the reference, for both sides, plus the
// properties the reference can't show: that the array stays sorted best
// first, that levels keep their contents when neighbours shift, and that
// nothing allocates after construction.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <type_traits>

#include "alloc_counter.hpp"
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

std::uint64_t mix(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// The reference orders best first by construction.
template <Best B>
using Reference = std::map<Price, Level, std::conditional_t<B == Best::High, std::greater<Price>, std::less<Price>>>;

// A level's head and tail are set from its price so a level that is copied
// to the wrong slot, or not copied at all, shows up when it is read back.
Index head_for(Price p) { return static_cast<Index>(p * 7 + 1); }
Index tail_for(Price p) { return static_cast<Index>(p * 13 + 2); }

template <Best B>
void check_same(const PriceLevels<B>& levels, const Reference<B>& ref) {
  CHECK(levels.size() == ref.size());
  CHECK(levels.empty() == ref.empty());
  std::size_t rank = 0;
  for (const auto& [price, level] : ref) {
    const Level& got = levels.by_rank(rank++);
    CHECK(got.price == price);
    CHECK(got.head == level.head && got.tail == level.tail);
  }
  if (!ref.empty()) {
    CHECK(levels.best().price == ref.begin()->first);
    CHECK(levels.best().head == ref.begin()->second.head);
  }
}

template <Best B>
void better_is_strict_and_directional() {
  CHECK(PriceLevels<B>::better(10, 9) == (B == Best::High));
  CHECK(PriceLevels<B>::better(9, 10) == (B == Best::Low));
  CHECK(!PriceLevels<B>::better(10, 10));
}

template <Best B>
void insert_find_erase_basics() {
  PriceLevels<B> levels(8);
  CHECK(levels.empty());
  CHECK(levels.find(100) == nullptr);

  levels.get_or_insert(100).head = 1;
  levels.get_or_insert(105).head = 2;
  levels.get_or_insert(95).head = 3;
  CHECK(levels.size() == 3);

  // Asking again for an existing price returns that level, not a new one.
  CHECK(levels.get_or_insert(100).head == 1);
  CHECK(levels.size() == 3);

  CHECK(levels.find(105)->head == 2);
  CHECK(levels.at(95).head == 3);
  CHECK(levels.find(96) == nullptr);

  // Best is the highest price for bids and the lowest for asks.
  CHECK(levels.best().price == (B == Best::High ? 105 : 95));
  CHECK(levels.by_rank(0).price == levels.best().price);
  CHECK(levels.by_rank(2).price == (B == Best::High ? 95 : 105));

  levels.erase(100);
  CHECK(levels.size() == 2 && levels.find(100) == nullptr);
  CHECK(levels.find(105)->head == 2 && levels.find(95)->head == 3);

  levels.pop_best();
  CHECK(levels.size() == 1);
  CHECK(levels.best().price == (B == Best::High ? 95 : 105));
  levels.pop_best();
  CHECK(levels.empty());
}

template <Best B>
void negative_and_extreme_prices() {
  PriceLevels<B> levels(4);
  const Price lo = std::numeric_limits<Price>::min(), hi = std::numeric_limits<Price>::max();
  levels.get_or_insert(0).head = 1;
  levels.get_or_insert(lo).head = 2;
  levels.get_or_insert(hi).head = 3;
  levels.get_or_insert(-5).head = 4;
  CHECK(levels.size() == 4);
  CHECK(levels.best().price == (B == Best::High ? hi : lo));
  CHECK(levels.at(lo).head == 2 && levels.at(hi).head == 3 && levels.at(-5).head == 4 && levels.at(0).head == 1);
  levels.erase(lo);
  levels.erase(hi);
  CHECK(levels.size() == 2 && levels.at(0).head == 1 && levels.at(-5).head == 4);
}

// Inserts and erases at random prices drawn from a small range, so levels
// are created and removed in the middle of the array again and again.
template <Best B>
void churn_matches_the_reference(std::uint64_t seed, std::size_t capacity, Price range, int ops) {
  PriceLevels<B> levels(capacity);
  Reference<B> ref;
  std::uint64_t s = seed;

  for (int op = 0; op < ops; ++op) {
    const Price p = static_cast<Price>(mix(s) % static_cast<std::uint64_t>(range)) - range / 2;
    switch (mix(s) % 4) {
      case 0:
        if (ref.count(p) != 0 || ref.size() < capacity) {
          Level& l = levels.get_or_insert(p);
          if (ref.count(p) == 0) {
            l.head = head_for(p);
            l.tail = tail_for(p);
            ref[p] = Level{p, head_for(p), tail_for(p)};
          }
          CHECK(l.price == p);
        }
        break;
      case 1:
        if (ref.count(p) != 0) {
          levels.erase(p);
          ref.erase(p);
        }
        break;
      case 2:
        if (!ref.empty()) {
          levels.pop_best();
          ref.erase(ref.begin());
        }
        break;
      default: {
        const Level* found = levels.find(p);
        CHECK((found != nullptr) == (ref.count(p) != 0));
        if (found != nullptr) CHECK(found->head == ref[p].head && found->tail == ref[p].tail);
      }
    }
    CHECK(levels.size() == ref.size());
    if (op % 16 == 0) check_same(levels, ref);
  }
  check_same(levels, ref);
}

// Filled to the limit and then emptied from the touch, the way a sweep does.
template <Best B>
void fill_then_sweep() {
  const std::size_t n = 64;
  PriceLevels<B> levels(n);
  Reference<B> ref;
  std::uint64_t s = 99;
  while (ref.size() < n) {
    const Price p = static_cast<Price>(mix(s) % 1000);
    if (ref.count(p) == 0) {
      Level& l = levels.get_or_insert(p);
      l.head = head_for(p);
      ref[p] = Level{p, head_for(p), kNilIndex};
    }
  }
  check_same(levels, ref);
  while (!ref.empty()) {
    CHECK(levels.best().price == ref.begin()->first);
    levels.pop_best();
    ref.erase(ref.begin());
    check_same(levels, ref);
  }
}

template <Best B>
void no_operation_allocates() {
  PriceLevels<B> levels(128);
  const bench::AllocSnapshot before = bench::alloc_snapshot();
  for (Price p = 0; p < 128; ++p) levels.get_or_insert((p * 37) % 128);
  for (Price p = 0; p < 128; p += 3) levels.erase((p * 37) % 128);
  for (Price p = 0; p < 40; ++p) levels.get_or_insert(500 + p);
  CHECK(levels.find(500) != nullptr);
  levels.pop_best();
  const bench::AllocSnapshot after = bench::alloc_snapshot();
  CHECK(after.calls == before.calls);
}

template <Best B>
void run_all() {
  better_is_strict_and_directional<B>();
  insert_find_erase_basics<B>();
  negative_and_extreme_prices<B>();
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    churn_matches_the_reference<B>(seed, 16, 40, 4000);
    churn_matches_the_reference<B>(seed + 100, 300, 500, 6000);
  }
  churn_matches_the_reference<B>(7, 1, 6, 1000);
  fill_then_sweep<B>();
  no_operation_allocates<B>();
}

}  // namespace

int main() {
  run_all<Best::High>();
  run_all<Best::Low>();
  std::puts("price levels ok");
}
