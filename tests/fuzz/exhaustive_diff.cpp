// Test 21: exhaustive differential test. Runs every operation sequence up to
// a fixed depth over a small alphabet on the engine and the oracle, comparing
// full state after each operation. The fuzzer samples a huge space; this one
// covers a small space completely, so a divergence reachable with these
// symbols within the depth cannot be missed. Anything outside the alphabet
// (larger quantities, more prices, other order ids as targets) is not covered.
//
//   exhaustive_diff [depth_structure=5] [depth_full=5] [depth_capacity=5]
//
// The defaults are the full run, about 1.4e8 sequence prefixes and about a
// minute optimized. Sanitizers slow it by well over an order of magnitude (a
// partial sanitized run extrapolates to about an hour), so the ctest entry
// passes smaller depths. The passes differ in alphabet and pool capacity so
// that pool exhaustion is reachable at every depth:
//   structure: GTC limits, cancel, modify; capacity 8
//   full:      adds IOC, FOK, market and a limit reusing order id 1, which is
//              a duplicate while id 1 rests and a plain reuse once it is gone;
//              capacity 3
//   capacity:  GTC limits only; capacity 2
// The account of the order at step i is i % 2, so self-trades come from the
// sequence itself instead of multiplying the alphabet.
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#include "diff.hpp"

namespace {

using diff::Kind;
using diff::Op;
using engine::Side;
using engine::TimeInForce;

struct Sym {
  Kind kind;
  Side side;
  engine::Price price;
  engine::Qty qty;
  TimeInForce tif;
  engine::OrderId target;
};

constexpr engine::Price kPrices[] = {10, 11, 12};

// reuse_id != 0 makes the order carry that id instead of a fresh one.
Sym limit(Side side, engine::Price px, engine::Qty qty, TimeInForce tif,
          engine::OrderId reuse_id = 0) {
  return {Kind::Limit, side, px, qty, tif, reuse_id};
}

Sym market(Side side, engine::Qty qty) {
  return {Kind::Market, side, 0, qty, TimeInForce::IOC, 0};
}

Sym cancel(engine::OrderId id) {
  return {Kind::Cancel, Side::Buy, 0, 0, TimeInForce::GTC, id};
}

Sym modify(engine::OrderId id, engine::Price px, engine::Qty qty) {
  return {Kind::Modify, Side::Buy, px, qty, TimeInForce::GTC, id};
}

void add_gtc(std::vector<Sym>& a) {
  for (auto side : {Side::Buy, Side::Sell})
    for (auto px : kPrices)
      for (engine::Qty q : {1u, 2u}) a.push_back(limit(side, px, q, TimeInForce::GTC));
}

void add_cancels(std::vector<Sym>& a, std::initializer_list<engine::OrderId> ids) {
  for (auto id : ids) a.push_back(cancel(id));
}

void add_modifies(std::vector<Sym>& a, std::initializer_list<engine::OrderId> ids) {
  for (auto id : ids)
    for (auto px : kPrices)
      for (engine::Qty q : {1u, 3u}) a.push_back(modify(id, px, q));
}

std::vector<Sym> structure_alphabet() {
  std::vector<Sym> a;
  add_gtc(a);
  add_cancels(a, {1, 2});
  add_modifies(a, {1});
  return a;
}

std::vector<Sym> full_alphabet() {
  std::vector<Sym> a;
  add_gtc(a);
  for (auto side : {Side::Buy, Side::Sell}) {
    for (auto px : kPrices) {
      a.push_back(limit(side, px, 2, TimeInForce::IOC));
      a.push_back(limit(side, px, 2, TimeInForce::FOK));
    }
    a.push_back(market(side, 2));
    a.push_back(limit(side, 11, 1, TimeInForce::GTC, 1));
  }
  add_cancels(a, {1, 2});
  add_modifies(a, {1, 2});
  return a;
}

std::vector<Sym> capacity_alphabet() {
  std::vector<Sym> a;
  add_gtc(a);
  return a;
}

Op make_op(const Sym& s, unsigned step) {
  Op op;
  op.kind = s.kind;
  op.side = s.side;
  op.price = s.price;
  op.qty = s.qty;
  op.tif = s.tif;
  bool creates = s.kind == Kind::Limit || s.kind == Kind::Market;
  op.id = (creates && s.target == 0) ? step + 1 : s.target;
  op.account = step % 2;
  return op;
}

struct Totals {
  std::uint64_t nodes = 0;
  diff::Stats seen;
};

void accumulate(const diff::Stats& before, const diff::Stats& after, diff::Stats& t) {
  t.trades += after.trades - before.trades;
  t.stp += after.stp - before.stp;
  t.fok_rejected += after.fok_rejected - before.fok_rejected;
  t.pool_exhausted += after.pool_exhausted - before.pool_exhausted;
  t.modify_ok += after.modify_ok - before.modify_ok;
  t.cancel_ok += after.cancel_ok - before.cancel_ok;
  t.crossed_states += after.crossed_states - before.crossed_states;
}

bool dfs(const diff::Pair& state, unsigned step, unsigned depth, const std::vector<Sym>& alphabet,
         std::vector<Op>& trail, Totals& totals, const char* pass) {
  if (step == depth) return true;
  for (const Sym& s : alphabet) {
    diff::Pair next = state;
    Op op = make_op(s, step);
    trail.push_back(op);
    std::string err = next.apply(op);
    if (err.empty()) err = next.check_state();
    if (!err.empty()) {
      std::fprintf(stderr, "DIVERGENCE in pass '%s' at step %u\n  %s\n", pass, step, err.c_str());
      for (std::size_t i = 0; i < trail.size(); ++i) {
        std::fprintf(stderr, "  op %zu: %s\n", i, diff::describe(trail[i]).c_str());
      }
      return false;
    }
    ++totals.nodes;
    accumulate(state.stats, next.stats, totals.seen);
    if (!dfs(next, step + 1, depth, alphabet, trail, totals, pass)) return false;
    trail.pop_back();
  }
  return true;
}

// Paths a pass must exercise, otherwise it proved nothing about them.
struct Required {
  bool stp = false;
  bool fok_rejection = false;
  bool modify = false;
};

bool run_pass(const char* name, const std::vector<Sym>& alphabet, unsigned depth,
              engine::Index capacity, Required need) {
  diff::Pair root(capacity);
  Totals totals;
  std::vector<Op> trail;
  if (!dfs(root, 0, depth, alphabet, trail, totals, name)) return false;

  const diff::Stats& s = totals.seen;
  std::printf("%-9s alphabet=%zu depth=%u capacity=%u nodes=%llu | trades=%llu stp=%llu "
              "fok_rejected=%llu pool_exhausted=%llu modify_ok=%llu cancel_ok=%llu crossed=%llu\n",
              name, alphabet.size(), depth, capacity, (unsigned long long)totals.nodes,
              (unsigned long long)s.trades, (unsigned long long)s.stp,
              (unsigned long long)s.fok_rejected, (unsigned long long)s.pool_exhausted,
              (unsigned long long)s.modify_ok, (unsigned long long)s.cancel_ok,
              (unsigned long long)s.crossed_states);

  bool need_exhaustion = depth > capacity;
  bool vacuous = depth >= 3 && (s.trades == 0 || (need.stp && s.stp == 0) ||
                                (need.fok_rejection && s.fok_rejected == 0) ||
                                (need.modify && s.modify_ok == 0) ||
                                (need_exhaustion && s.pool_exhausted == 0));
  if (vacuous) {
    std::fprintf(stderr, "pass '%s': a required path was never exercised\n", name);
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  unsigned depth_structure = argc > 1 ? static_cast<unsigned>(std::atoi(argv[1])) : 5;
  unsigned depth_full = argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 5;
  unsigned depth_capacity = argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 5;

  if (!run_pass("structure", structure_alphabet(), depth_structure, 8, {true, false, true})) {
    return 1;
  }
  if (!run_pass("full", full_alphabet(), depth_full, 3, {true, true, true})) return 1;
  if (!run_pass("capacity", capacity_alphabet(), depth_capacity, 2, {})) return 1;
  std::printf("exhaustive_diff ok\n");
  return 0;
}
