// IdMap against std::unordered_map as the reference, plus the properties the
// reference can't show: the sizing rule, that no operation allocates after
// construction, and that backward-shift deletion keeps every survivor
// reachable.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <unordered_map>
#include <vector>

#include "alloc_counter.hpp"
#include "engine/id_map.hpp"
#include "engine/pool.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using engine::Handle;
using engine::IdMap;
using engine::Index;
using engine::Order;
using engine::OrderId;

namespace {

std::uint64_t mix(std::uint64_t& state) {
  std::uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

// Distinct handles, so a lookup that returns another key's entry is caught.
// Only a pool can mint one.
struct Handles {
  explicit Handles(Index n) : pool(n) {
    for (Index i = 0; i < n; ++i) all.push_back(pool.allocate(Order{}));
  }
  engine::FixedPool<Order> pool;
  std::vector<Handle> all;
};

}  // namespace

// Runs first: a table that is too small makes a lookup of an absent key
// probe forever, and this is the check that fails before anything can.
static void table_has_at_least_twice_the_slots() {
  const std::size_t expect[][2] = {{0, 2}, {1, 2}, {2, 4}, {3, 8}, {4, 8}, {5, 16}, {64, 128}, {65536, 131072}};
  for (const auto& e : expect) {
    IdMap m(static_cast<Index>(e[0]));
    CHECK(m.slots() == e[1]);
    CHECK(m.slots() >= 2 * e[0]);
    CHECK((m.slots() & (m.slots() - 1)) == 0);
  }
}

static void empty_map() {
  IdMap m(8);
  CHECK(m.size() == 0);
  CHECK(m.find(0) == nullptr);
  CHECK(m.find(12345) == nullptr);
  CHECK(!m.contains(7));
  CHECK(!m.erase(7));
  CHECK(m.size() == 0);
}

static void insert_find_erase_basics() {
  Handles h(4);
  IdMap m(4);
  m.insert(10, h.all[0]);
  m.insert(20, h.all[1]);
  CHECK(m.size() == 2);
  CHECK(m.contains(10) && m.contains(20) && !m.contains(30));
  CHECK(*m.find(10) == h.all[0]);
  CHECK(*m.find(20) == h.all[1]);

  CHECK(m.erase(10));
  CHECK(!m.contains(10) && m.contains(20));
  CHECK(!m.erase(10));
  CHECK(m.size() == 1);

  // A removed id can be inserted again with a different handle.
  m.insert(10, h.all[2]);
  CHECK(*m.find(10) == h.all[2]);
}

static void every_id_is_a_legal_key() {
  Handles h(3);
  IdMap m(3);
  const OrderId zero = 0, top = std::numeric_limits<OrderId>::max();
  m.insert(zero, h.all[0]);
  m.insert(top, h.all[1]);
  CHECK(*m.find(zero) == h.all[0]);
  CHECK(*m.find(top) == h.all[1]);
  CHECK(m.erase(zero));
  CHECK(!m.contains(zero) && m.contains(top));
}

// Many ids in few slots, so probe runs form, wrap past the end of the table
// and get cut by deletions. The reference holds the same entries in a
// structure that can't have that problem.
static void churn_matches_the_reference(std::uint64_t seed, Index max_entries, std::size_t domain, int ops) {
  Handles h(static_cast<Index>(domain));
  std::vector<OrderId> keys(domain);
  std::uint64_t s = seed;
  for (std::size_t i = 0; i < domain; ++i) keys[i] = mix(s);
  keys[0] = 0;
  keys[1] = std::numeric_limits<OrderId>::max();

  IdMap m(max_entries);
  std::unordered_map<OrderId, Handle> ref;

  for (int op = 0; op < ops; ++op) {
    const std::size_t k = mix(s) % domain;
    const OrderId id = keys[k];
    const bool present = ref.count(id) != 0;

    switch (mix(s) % 3) {
      case 0:
        if (!present && ref.size() < max_entries) {
          m.insert(id, h.all[k]);
          ref[id] = h.all[k];
        }
        break;
      case 1:
        CHECK(m.erase(id) == present);
        ref.erase(id);
        break;
      default: {
        const Handle* found = m.find(id);
        CHECK((found != nullptr) == present);
        if (present) CHECK(*found == ref[id]);
        CHECK(m.contains(id) == present);
      }
    }
    CHECK(m.size() == ref.size());

    if (op % 64 == 0) {
      for (const auto& [rid, rh] : ref) {
        const Handle* found = m.find(rid);
        CHECK(found != nullptr && *found == rh);
      }
    }
  }
  for (const auto& [rid, rh] : ref) CHECK(m.erase(rid));
  CHECK(m.size() == 0);
}

// The table at its limit, drained in a scrambled order. After every removal
// each remaining entry has to still be found.
static void drain_a_full_table(std::uint64_t seed) {
  const Index n = 64;
  Handles h(n);
  IdMap m(n);
  std::uint64_t s = seed;
  std::vector<OrderId> ids;
  for (Index i = 0; i < n; ++i) {
    OrderId id = mix(s);
    m.insert(id, h.all[i]);
    ids.push_back(id);
  }
  CHECK(m.size() == n);

  std::vector<bool> gone(n, false);
  for (Index round = 0; round < n; ++round) {
    Index pick = static_cast<Index>(mix(s) % n);
    while (gone[pick]) pick = (pick + 1) % n;
    CHECK(m.erase(ids[pick]));
    gone[pick] = true;
    for (Index i = 0; i < n; ++i) {
      const Handle* found = m.find(ids[i]);
      CHECK((found != nullptr) == !gone[i]);
      if (found != nullptr) CHECK(*found == h.all[i]);
    }
  }
  CHECK(m.size() == 0);
}

static void sequential_ids_do_not_pile_up() {
  // Client order ids are usually a counter. Without mixing, linear probing
  // would turn that into one run; here every insert has to land and every
  // lookup has to find it, which holds for any layout, so this one checks
  // the other half: a drained table is empty again, not full of leftovers.
  const Index n = 1024;
  Handles h(n);
  IdMap m(n);
  for (Index i = 0; i < n; ++i) m.insert(i + 1, h.all[i]);
  for (Index i = 0; i < n; ++i) CHECK(*m.find(i + 1) == h.all[i]);
  for (Index i = 0; i < n; ++i) CHECK(m.erase(i + 1));
  CHECK(m.size() == 0);
  for (Index i = 0; i < n; ++i) CHECK(m.find(i + 1) == nullptr);
}

static void no_operation_allocates() {
  Handles h(256);
  IdMap m(256);
  const bench::AllocSnapshot before = bench::alloc_snapshot();
  for (Index i = 0; i < 256; ++i) m.insert(1000 + i, h.all[i]);
  for (Index i = 0; i < 256; ++i) CHECK(m.contains(1000 + i));
  CHECK(!m.contains(5));
  for (Index i = 0; i < 256; i += 2) CHECK(m.erase(1000 + i));
  for (Index i = 0; i < 128; ++i) m.insert(5000 + i, h.all[i]);
  const bench::AllocSnapshot after = bench::alloc_snapshot();
  CHECK(after.calls == before.calls);
}

int main() {
  table_has_at_least_twice_the_slots();
  empty_map();
  insert_find_erase_basics();
  every_id_is_a_legal_key();

  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    churn_matches_the_reference(seed, 8, 24, 4000);
    churn_matches_the_reference(seed + 100, 64, 200, 6000);
  }
  churn_matches_the_reference(7, 1, 4, 2000);
  churn_matches_the_reference(8, 0, 3, 500);

  for (std::uint64_t seed = 1; seed <= 20; ++seed) drain_a_full_table(seed);
  sequential_ids_do_not_pile_up();
  no_operation_allocates();

  std::puts("id map ok");
}
