#pragma once

// order_id -> Handle lookup, ADR-005. Open addressing with linear probing
// over one array, sized once at construction and never grown, so no
// operation allocates.
//
// The table has at least twice as many slots as the most entries it will be
// asked to hold. That bound is what lets insert() skip any "table full"
// path: with at most half the slots used, a probe always reaches an empty
// one, and probe runs stay short.
//
// A slot is empty when its handle is the invalid handle, so every order id
// is a legal key, zero and the maximum included.
//
// Lookup only. Nothing walks the table, so the order entries sit in cannot
// reach the output (S7).
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "pool.hpp"
#include "types.hpp"

namespace engine {

class IdMap {
 public:
  explicit IdMap(Index max_entries) {
    std::size_t slots = 2;
    while (slots < 2 * static_cast<std::size_t>(max_entries)) slots *= 2;
    table_.resize(slots);
    mask_ = slots - 1;
  }

  const Handle* find(OrderId id) const {
    for (std::size_t i = home(id);; i = (i + 1) & mask_) {
      const Entry& e = table_[i];
      if (!e.handle.valid()) return nullptr;
      if (e.id == id) return &e.handle;
    }
  }

  bool contains(OrderId id) const { return find(id) != nullptr; }

  // The caller guarantees id is absent and that fewer than max_entries are
  // stored. Book gets both for free: it checks for a duplicate before it
  // allocates, and it only inserts after the pool handed out a slot.
  void insert(OrderId id, Handle handle) {
    assert(handle.valid());
    assert(size_ < table_.size() / 2);
    std::size_t i = home(id);
    while (table_[i].handle.valid()) i = (i + 1) & mask_;
    table_[i] = {id, handle};
    ++size_;
  }

  // Removes id and reports whether it was there. Deleting leaves no marker
  // behind: the entries after the hole that would otherwise become
  // unreachable are shifted back into it, so lookups never have to step
  // over dead slots and probe runs don't grow with churn.
  bool erase(OrderId id) {
    std::size_t hole = home(id);
    for (;; hole = (hole + 1) & mask_) {
      if (!table_[hole].handle.valid()) return false;
      if (table_[hole].id == id) break;
    }

    for (std::size_t i = (hole + 1) & mask_; table_[i].handle.valid(); i = (i + 1) & mask_) {
      // The entry at i may move into the hole only if the hole lies on its
      // probe path, that is, no farther from its home slot than i is.
      if (((i - home(table_[i].id)) & mask_) >= ((i - hole) & mask_)) {
        table_[hole] = table_[i];
        hole = i;
      }
    }
    table_[hole].handle = Handle{};
    --size_;
    return true;
  }

  std::size_t size() const { return size_; }
  std::size_t slots() const { return table_.size(); }

 private:
  struct Entry {
    OrderId id = 0;
    Handle handle;
  };

  // The finalizer of splitmix64. Order ids tend to be sequential, which
  // linear probing turns into one long run unless the bits are mixed. The
  // constants are fixed, so the layout is the same on every run (S7).
  std::size_t home(OrderId id) const {
    std::uint64_t z = id;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return static_cast<std::size_t>(z) & mask_;
  }

  std::vector<Entry> table_;
  std::size_t mask_ = 0;
  std::size_t size_ = 0;
};

}  // namespace engine
