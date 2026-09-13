#pragma once

#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "types.hpp"

namespace engine {

// ADR-001. A handle survives across a free/reuse cycle only if its
// generation still matches the slot's current generation — a stale handle
// from before a free() fails validate() instead of silently aliasing
// whatever order now occupies that index.
class Handle {
 public:
  Handle() = default;

  bool valid() const { return value_ != kNil(); }
  Index index() const { return static_cast<Index>(value_ & 0xFFFFFFFFu); }
  std::uint32_t generation() const {
    return static_cast<std::uint32_t>(value_ >> 32);
  }

  friend bool operator==(Handle a, Handle b) { return a.value_ == b.value_; }
  friend bool operator!=(Handle a, Handle b) { return !(a == b); }

 private:
  template <typename T>
  friend class FixedPool;  // only a pool may mint a handle into itself
  explicit Handle(std::uint64_t v) : value_(v) {}
  static constexpr std::uint64_t kNil() {
    return std::numeric_limits<std::uint64_t>::max();
  }

  std::uint64_t value_ = kNil();
};

// FixedPool<T>: pre-allocated, never resized after construction. No new/
// delete, ever — ADR-001. T occupies a union with the free-list link so a
// free slot costs nothing beyond what the slot already reserved; this only
// works because T's lifetime is never actually run (no ctor/dtor calls),
// which is why the pool is restricted to trivial T.
template <typename T>
class FixedPool {
  static_assert(std::is_trivially_copyable_v<T> &&
                    std::is_trivially_destructible_v<T>,
                "FixedPool never constructs or destroys T — it treats slot "
                "memory as raw storage, so T must be safe to overwrite and "
                "discard without running a destructor.");

 public:
  explicit FixedPool(Index capacity) : slots_(capacity), generation_(capacity, 0) {
    for (Index i = 0; i + 1 < capacity; ++i) slots_[i].next_free = i + 1;
    if (capacity > 0) slots_[capacity - 1].next_free = kNilIndex;
    free_head_ = capacity > 0 ? 0 : kNilIndex;
  }

  // Returns an invalid handle if the pool is exhausted. Callers must treat
  // that as a clean rejection — see conformance test for max-orders-reached
  // — not as a condition to retry or grow into.
  Handle allocate(const T& value) {
    if (free_head_ == kNilIndex) return Handle{};
    Index idx = free_head_;
    free_head_ = slots_[idx].next_free;
    slots_[idx].value = value;
    return make_handle(idx, generation_[idx]);
  }

  // Freeing an already-invalid handle is a no-op, not undefined behavior —
  // this is what lets cancel-of-already-cancelled (test 8) resolve as a
  // clean rejection at the caller instead of needing pool-level bookkeeping
  // to distinguish "never existed" from "already gone."
  void free(Handle h) {
    if (!validate(h)) return;
    Index idx = h.index();
    ++generation_[idx];
    slots_[idx].next_free = free_head_;
    free_head_ = idx;
  }

  T* get(Handle h) {
    return validate(h) ? &slots_[h.index()].value : nullptr;
  }
  const T* get(Handle h) const {
    return validate(h) ? &slots_[h.index()].value : nullptr;
  }

  bool validate(Handle h) const {
    return h.valid() && h.index() < slots_.size() &&
           h.generation() == generation_[h.index()];
  }

  // Direct access by raw index, skipping the generation check. Only for
  // code that already holds a validated handle for this call and is
  // walking an intrusive list through next_idx/prev_idx — those indices
  // only ever point at currently-occupied slots by construction (S8: a
  // slot can't be freed mid-traversal), so re-validating on every hop
  // would be pure overhead on the hot path.
  T& unchecked(Index idx) { return slots_[idx].value; }
  const T& unchecked(Index idx) const { return slots_[idx].value; }

  Index capacity() const { return static_cast<Index>(slots_.size()); }

 private:
  static Handle make_handle(Index idx, std::uint32_t gen) {
    return Handle{(static_cast<std::uint64_t>(gen) << 32) | idx};
  }

  union Slot {
    Slot() : next_free(kNilIndex) {}
    T value;
    Index next_free;
  };

  std::vector<Slot> slots_;
  std::vector<std::uint32_t> generation_;
  Index free_head_ = kNilIndex;
};

}  // namespace engine
