#pragma once

// One side of the book: its price levels in a single array sorted worst to
// best, so the best level is the last element. ADR-003.
//
// Matching spends nearly all its time at the best level, and a sweep removes
// levels from the best end. Keeping the best at the back makes both of those
// an access to the last element and a pop, with no shifting. Only a level
// created or emptied away from the touch shifts the elements behind it.
//
// The array is allocated once, for as many levels as the side can ever hold,
// so nothing here allocates afterwards.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <vector>

#include "level.hpp"
#include "types.hpp"

namespace engine {

// Which end of the price axis is best. Bids want the highest price, asks the
// lowest.
enum class Best { High, Low };

template <Best B>
class PriceLevels {
 public:
  // max_levels is the most distinct prices the side may hold at once. Book
  // passes the pool capacity plus one: every level holds an order, and rest()
  // makes its level before it finds out the pool is full.
  explicit PriceLevels(std::size_t max_levels) : levels_(max_levels) {}

  // True if a is a better price than b for this side.
  static bool better(Price a, Price b) { return B == Best::High ? a > b : a < b; }

  bool empty() const { return count_ == 0; }
  std::size_t size() const { return count_; }

  Level& best() {
    assert(count_ > 0);
    return levels_[count_ - 1];
  }
  const Level& best() const {
    assert(count_ > 0);
    return levels_[count_ - 1];
  }

  // Removes the best level. O(1).
  void pop_best() {
    assert(count_ > 0);
    --count_;
  }

  // rank 0 is the best level, rank 1 the next best, and so on.
  const Level& by_rank(std::size_t rank) const {
    assert(rank < count_);
    return levels_[count_ - 1 - rank];
  }

  Level* find(Price price) {
    Level* at = lower(price);
    return at != end() && at->price == price ? at : nullptr;
  }
  const Level* find(Price price) const {
    const Level* at = lower(price);
    return at != end() && at->price == price ? at : nullptr;
  }

  // For a price the caller knows is present. Every resting order's level
  // exists, so a miss here is a bookkeeping bug.
  Level& at(Price price) {
    Level* found = find(price);
    assert(found != nullptr);
    return *found;
  }
  const Level& at(Price price) const {
    const Level* found = find(price);
    assert(found != nullptr);
    return *found;
  }

  // Returns the level at price, creating an empty one if there is none.
  // The reference is good until the next insert or erase: both can move
  // elements.
  Level& get_or_insert(Price price) {
    Level* at = lower(price);
    if (at != end() && at->price == price) return *at;

    assert(count_ < levels_.size());
    std::copy_backward(at, end(), end() + 1);
    ++count_;
    *at = Level{price, kNilIndex, kNilIndex};
    return *at;
  }

  // The level at price must exist and should be empty.
  void erase(Price price) {
    Level* at = find(price);
    assert(at != nullptr);
    std::copy(at + 1, end(), at);
    --count_;
  }

 private:
  Level* begin() { return levels_.data(); }
  Level* end() { return levels_.data() + count_; }
  const Level* begin() const { return levels_.data(); }
  const Level* end() const { return levels_.data() + count_; }

  // First element that is not worse than price, which is where price either
  // sits or has to be inserted to keep the order. The array runs worst to
  // best, so "worse" is better() with the arguments swapped.
  Level* lower(Price price) {
    return std::lower_bound(begin(), end(), price,
                            [](const Level& l, Price p) { return better(p, l.price); });
  }
  const Level* lower(Price price) const {
    return std::lower_bound(begin(), end(), price,
                            [](const Level& l, Price p) { return better(p, l.price); });
  }

  std::vector<Level> levels_;
  std::size_t count_ = 0;
};

}  // namespace engine
