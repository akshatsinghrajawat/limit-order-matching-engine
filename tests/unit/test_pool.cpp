// FixedPool slot retirement: a slot freed at its last generation leaves
// service for good, so the generation counter never wraps to a value an old
// handle still carries. The pool is instantiated with small maximums here;
// the default maximum is 2^32 - 1 and cannot be reached in a test.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "engine/pool.hpp"
#include "engine/types.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace engine;

static_assert(sizeof(Handle) == 8, "a handle is an index and a generation in one word");

namespace {

std::uint64_t mix(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// One slot through its whole life. Generations 0 .. MaxGen - 1 each hand out
// a handle that is valid until the free, then the slot retires.
template <std::uint32_t MaxGen>
void a_slot_is_used_for_exactly_max_gen_generations() {
  FixedPool<Order, MaxGen> pool(1);
  std::vector<Handle> seen;
  for (std::uint32_t g = 0; g < MaxGen; ++g) {
    const Handle h = pool.allocate(Order{});
    CHECK(h.valid() && h.index() == 0 && h.generation() == g);
    CHECK(pool.get(h) != nullptr);
    CHECK(pool.free_slots() == 0 && pool.retired_slots() == 0);
    pool.free(h);
    CHECK(!pool.validate(h));
    seen.push_back(h);
    if (g + 1 < MaxGen) {
      CHECK(pool.free_slots() == 1 && pool.retired_slots() == 0);
    }
  }
  CHECK(pool.free_slots() == 0 && pool.retired_slots() == 1);
  CHECK(!pool.allocate(Order{}).valid());
  for (const Handle& h : seen) CHECK(!pool.validate(h) && pool.get(h) == nullptr);

  // The handle a caller rebuilds from the index carries the maximum and is
  // refused, even though it matches what the slot stores.
  const Handle sentinel = pool.handle_of(0);
  CHECK(sentinel.generation() == MaxGen);
  CHECK(!pool.validate(sentinel) && pool.get(sentinel) == nullptr);
}

// Freeing a handle that is stale or retired changes nothing: the slot is not
// retired a second time and the other slot is not disturbed.
template <std::uint32_t MaxGen>
void freeing_a_stale_or_retired_handle_changes_nothing() {
  FixedPool<Order, MaxGen> pool(2);
  const Handle other = pool.allocate(Order{});
  std::vector<Handle> cycled;
  for (std::uint32_t g = 0; g < MaxGen; ++g) {
    const Handle h = pool.allocate(Order{});
    CHECK(h.valid() && h.index() == 1);
    pool.free(h);
    cycled.push_back(h);
    pool.free(h);  // a second free of the same handle
  }
  CHECK(pool.retired_slots() == 1 && pool.free_slots() == 0);

  for (int again = 0; again < 2; ++again) {
    for (const Handle& h : cycled) pool.free(h);
    pool.free(pool.handle_of(1));
    CHECK(pool.retired_slots() == 1 && pool.free_slots() == 0);
  }
  CHECK(pool.validate(other) && pool.get(other) != nullptr);
}

// Every slot is handed out exactly MaxGen times and then the pool is empty
// for good: capacity * MaxGen allocations, never one more.
template <std::uint32_t MaxGen>
void a_retired_slot_is_never_handed_out_again() {
  const Index capacity = 5;
  FixedPool<Order, MaxGen> pool(capacity);
  std::size_t handed_out = 0;
  std::vector<std::uint32_t> uses(capacity, 0);
  for (;;) {
    std::vector<Handle> batch;
    for (Index i = 0; i < capacity; ++i) {
      const Handle h = pool.allocate(Order{});
      if (!h.valid()) break;
      CHECK(h.generation() == uses[h.index()]);
      ++uses[h.index()];
      batch.push_back(h);
    }
    if (batch.empty()) break;
    handed_out += batch.size();
    for (const Handle& h : batch) pool.free(h);
    CHECK(handed_out <= static_cast<std::size_t>(capacity) * MaxGen);
  }
  CHECK(handed_out == static_cast<std::size_t>(capacity) * MaxGen);
  CHECK(pool.retired_slots() == capacity && pool.free_slots() == 0);
}

// Random allocate and free, including frees of handles that are long stale,
// against a model of every slot's generation. At every step the pool's free
// and retired counts and its verdict on every handle ever issued match.
template <std::uint32_t MaxGen>
void random_use_matches_the_model(std::uint64_t seed) {
  const Index capacity = 8;
  FixedPool<Order, MaxGen> pool(capacity);
  struct Model {
    std::uint32_t gen = 0;
    bool live = false;
    bool retired = false;
  };
  std::vector<Model> model(capacity);
  std::vector<Handle> issued;
  std::vector<Handle> live;
  std::uint64_t s = seed;

  auto free_in_model = [&] {
    Index n = 0;
    for (const Model& m : model) n += (!m.live && !m.retired);
    return n;
  };

  for (int step = 0; step < 4000; ++step) {
    if (mix(s) % 2 == 0) {
      const Handle h = pool.allocate(Order{});
      CHECK(h.valid() == (free_in_model() > 0));
      if (h.valid()) {
        Model& m = model[h.index()];
        CHECK(!m.live && !m.retired && h.generation() == m.gen);
        m.live = true;
        issued.push_back(h);
        live.push_back(h);
      }
    } else if (!issued.empty()) {
      const Handle h = issued[mix(s) % issued.size()];
      Model& m = model[h.index()];
      const bool should_free = m.live && !m.retired && h.generation() == m.gen;
      pool.free(h);
      if (should_free) {
        m.live = false;
        if (m.gen + 1 < MaxGen) {
          ++m.gen;
        } else {
          m.gen = MaxGen;
          m.retired = true;
        }
      }
    }

    Index retired = 0;
    for (const Model& m : model) retired += m.retired;
    CHECK(pool.retired_slots() == retired);
    CHECK(pool.free_slots() == free_in_model());
    for (const Handle& h : issued) {
      const Model& m = model[h.index()];
      CHECK(pool.validate(h) == (m.live && h.generation() == m.gen));
    }
    for (Index i = 0; i < capacity; ++i) {
      // validate() knows generations, not occupancy: only a retired slot's
      // rebuilt handle is refused.
      const Handle rebuilt = pool.handle_of(i);
      CHECK(pool.validate(rebuilt) == !model[i].retired);
    }
    CHECK(pool.free_slots() + pool.retired_slots() + [&] {
      Index n = 0;
      for (const Model& m : model) n += m.live;
      return n;
    }() == capacity);
  }
}

// With the default maximum nothing retires in any run a test can afford.
void the_default_maximum_retires_nothing_in_ordinary_use() {
  FixedPool<Order> pool(4);
  for (int round = 0; round < 1000; ++round) {
    const Handle h = pool.allocate(Order{});
    CHECK(h.valid() && h.generation() == static_cast<std::uint32_t>(round));
    pool.free(h);
  }
  CHECK(pool.retired_slots() == 0 && pool.free_slots() == 4);
}

}  // namespace

int main() {
  a_slot_is_used_for_exactly_max_gen_generations<1>();
  a_slot_is_used_for_exactly_max_gen_generations<2>();
  a_slot_is_used_for_exactly_max_gen_generations<3>();
  freeing_a_stale_or_retired_handle_changes_nothing<1>();
  freeing_a_stale_or_retired_handle_changes_nothing<2>();
  freeing_a_stale_or_retired_handle_changes_nothing<3>();
  a_retired_slot_is_never_handed_out_again<1>();
  a_retired_slot_is_never_handed_out_again<2>();
  a_retired_slot_is_never_handed_out_again<3>();
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    random_use_matches_the_model<1>(seed);
    random_use_matches_the_model<2>(seed);
    random_use_matches_the_model<3>(seed);
  }
  the_default_maximum_retires_nothing_in_ordinary_use();
  std::puts("pool ok");
}
