// Checks the oracle against expectations worked out by hand from
// docs/spec.md, with no engine logic involved. The differential tests are
// only as good as the oracle, so it gets its own scenarios before anything
// is diffed against it. Expected values come from the spec, not from running
// the engine, except where a test comment says the spec is silent.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstdio>
#include <cstdlib>

#include "oracle.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace oracle;

constexpr std::uint32_t A = 1;
constexpr std::uint32_t B = 2;
constexpr TimeInForce GTC = TimeInForce::GTC;
constexpr TimeInForce IOC = TimeInForce::IOC;
constexpr TimeInForce FOK = TimeInForce::FOK;

static void crossing_fills_at_makers_price_and_leaves_residual() {
  Oracle o(16);
  CHECK(o.add_limit(1, Side::Sell, 100, 10, B, GTC).resting_qty == 10);
  Result r = o.add_limit(2, Side::Buy, 105, 4, A, GTC);
  CHECK(r.accepted && r.resting_qty == 0);
  CHECK(r.trades.size() == 1);
  CHECK(r.trades[0].maker_order_id == 1 && r.trades[0].taker_order_id == 2);
  CHECK(r.trades[0].price == 100 && r.trades[0].qty == 4);
  auto s = o.snapshot();
  CHECK(s.size() == 1 && s[0].order_id == 1 && s[0].qty == 6 && s[0].seq_no == 0);
}

static void sweep_respects_price_then_time() {
  Oracle o(16);
  o.add_limit(1, Side::Sell, 102, 5, B, GTC);
  o.add_limit(2, Side::Sell, 100, 5, B, GTC);
  o.add_limit(3, Side::Sell, 101, 5, B, GTC);
  o.add_limit(4, Side::Sell, 100, 5, B, GTC);
  Result r = o.add_limit(5, Side::Buy, 102, 17, A, GTC);
  CHECK(r.trades.size() == 4);
  const OrderId makers[] = {2, 4, 3, 1};
  const Price prices[] = {100, 100, 101, 102};
  const Qty qtys[] = {5, 5, 5, 2};
  for (int i = 0; i < 4; ++i) {
    CHECK(r.trades[i].maker_order_id == makers[i]);
    CHECK(r.trades[i].price == prices[i]);
    CHECK(r.trades[i].qty == qtys[i]);
    CHECK(r.trades[i].trade_id == static_cast<std::uint64_t>(i));
  }
  auto s = o.snapshot();
  CHECK(s.size() == 1 && s[0].order_id == 1 && s[0].qty == 3);
}

static void self_trade_rejects_the_incoming_remainder() {
  Oracle o(16);
  o.add_limit(1, Side::Sell, 100, 3, A, GTC);
  o.add_limit(2, Side::Sell, 100, 3, B, GTC);
  Result r = o.add_limit(3, Side::Buy, 100, 5, A, GTC);
  CHECK(r.accepted && r.stp_rejected && r.trades.empty() && r.resting_qty == 0);
  CHECK(o.snapshot().size() == 2);

  o.add_limit(4, Side::Sell, 99, 2, B, GTC);
  r = o.add_limit(5, Side::Buy, 100, 5, A, GTC);
  CHECK(r.trades.size() == 1 && r.trades[0].maker_order_id == 4 && r.trades[0].qty == 2);
  CHECK(r.stp_rejected && r.resting_qty == 0);
  CHECK(o.snapshot().size() == 2);
}

static void fok_counts_only_liquidity_before_a_self_match() {
  Oracle o(16);
  o.add_limit(1, Side::Sell, 100, 3, B, GTC);
  o.add_limit(2, Side::Sell, 100, 3, A, GTC);
  Result r = o.add_limit(3, Side::Buy, 101, 5, A, FOK);
  CHECK(r.accepted && r.trades.empty() && r.resting_qty == 0);
  CHECK(o.snapshot().size() == 2 && o.snapshot()[0].qty == 3);

  r = o.add_limit(4, Side::Buy, 101, 3, A, FOK);
  CHECK(r.trades.size() == 1 && r.trades[0].maker_order_id == 1);
  CHECK(o.snapshot().size() == 1 && o.snapshot()[0].order_id == 2);
}

static void fok_fills_across_levels_when_enough() {
  Oracle o(16);
  o.add_limit(1, Side::Sell, 100, 2, B, GTC);
  o.add_limit(2, Side::Sell, 101, 2, B, GTC);
  Result r = o.add_limit(3, Side::Buy, 101, 4, A, FOK);
  CHECK(r.trades.size() == 2 && r.trades[1].price == 101);
  CHECK(o.snapshot().empty());
}

static void ioc_and_market_never_rest() {
  Oracle o(16);
  o.add_limit(1, Side::Sell, 100, 3, B, GTC);
  Result r = o.add_limit(2, Side::Buy, 100, 5, A, IOC);
  CHECK(r.trades.size() == 1 && r.trades[0].qty == 3 && r.resting_qty == 0);
  CHECK(o.snapshot().empty());

  r = o.add_market(3, Side::Buy, 4, A);
  CHECK(r.accepted && r.trades.empty() && r.resting_qty == 0);

  o.add_limit(4, Side::Buy, 90, 1, A, GTC);
  r = o.add_market(5, Side::Sell, 2, B);
  CHECK(r.trades.size() == 1 && r.trades[0].price == 90 && r.trades[0].qty == 1);
  CHECK(o.snapshot().empty());
}

static void modify_priority_rules() {
  Oracle o(16);
  o.add_limit(1, Side::Buy, 100, 5, A, GTC);
  o.add_limit(2, Side::Buy, 100, 5, A, GTC);
  o.add_limit(3, Side::Buy, 100, 5, A, GTC);

  CHECK(o.modify(1, 100, 3));
  auto s = o.snapshot();
  CHECK(s[0].order_id == 1 && s[0].qty == 3 && s[0].seq_no == 0);

  CHECK(o.modify(1, 100, 4));
  s = o.snapshot();
  CHECK(s[0].order_id == 2 && s[1].order_id == 3 && s[2].order_id == 1);
  CHECK(s[2].seq_no == 3);

  CHECK(o.modify(2, 101, 5));
  s = o.snapshot();
  CHECK(s[0].order_id == 2 && s[0].price == 101 && s[0].seq_no == 4);
  CHECK(s[1].order_id == 3 && s[2].order_id == 1);

  CHECK(o.modify(3, 100, 5));
  s = o.snapshot();
  CHECK(s[1].order_id == 3 && s[2].order_id == 1);

  CHECK(o.modify(3, 100, 6));
  s = o.snapshot();
  CHECK(s[1].order_id == 1 && s[2].order_id == 3 && s[2].seq_no == 5);
}

static void rejections_leave_state_alone() {
  Oracle o(16);
  o.add_limit(1, Side::Buy, 100, 5, A, GTC);
  CHECK(!o.add_limit(2, Side::Buy, 100, 0, A, GTC).accepted);
  CHECK(!o.add_limit(1, Side::Sell, 90, 5, B, GTC).accepted);
  CHECK(o.snapshot().size() == 1 && o.snapshot()[0].qty == 5);
  CHECK(!o.cancel(99));
  CHECK(!o.modify(99, 100, 1));
  CHECK(!o.modify(1, 100, 0));
  CHECK(o.snapshot()[0].qty == 5);
  CHECK(o.cancel(1));
  CHECK(!o.cancel(1));
  CHECK(!o.modify(1, 100, 1));

  o.add_limit(10, Side::Sell, 100, 1, B, GTC);
  o.add_limit(11, Side::Buy, 100, 1, A, GTC);
  CHECK(o.add_limit(10, Side::Sell, 100, 1, B, GTC).accepted);
}

static void capacity_exhaustion_is_a_clean_rejection() {
  Oracle o(2);
  CHECK(o.add_limit(1, Side::Buy, 100, 5, A, GTC).resting_qty == 5);
  CHECK(o.add_limit(2, Side::Buy, 99, 5, A, GTC).resting_qty == 5);
  Result r = o.add_limit(3, Side::Buy, 98, 5, A, GTC);
  CHECK(r.accepted && r.pool_exhausted && r.resting_qty == 0);
  CHECK(o.snapshot().size() == 2);
  CHECK(o.cancel(1));
  r = o.add_limit(4, Side::Buy, 97, 5, A, GTC);
  CHECK(r.resting_qty == 5 && !r.pool_exhausted);
}

// The spec puts seq_no in the trade event and never says what it holds. Here
// it is the next unassigned sequence number at the moment of the fill; trades
// do not consume one, so it equals the seq_no the next resting order gets.
// This pins down what the engine does, it is not derived from the spec.
static void trade_ids_and_sequence_numbers() {
  Oracle o(16);
  o.add_limit(1, Side::Sell, 100, 1, B, GTC);
  Result r = o.add_limit(2, Side::Buy, 100, 1, A, IOC);
  CHECK(r.trades[0].trade_id == 0 && r.trades[0].seq_no == 1);
  o.add_limit(3, Side::Sell, 100, 1, B, GTC);
  CHECK(o.snapshot()[0].seq_no == 1);
  r = o.add_limit(4, Side::Buy, 100, 1, A, IOC);
  CHECK(r.trades[0].trade_id == 1 && r.trades[0].seq_no == 2);
}

// The spec says modify repositions an order and never says it can trade, so
// a price move through the opposite side leaves the book crossed, and the
// next incoming order trades against it at the resting price. Like seq_no
// above, this pins down the engine's behavior; the spec does not decide it.
static void modify_never_crosses_but_later_orders_do() {
  Oracle o(16);
  o.add_limit(1, Side::Buy, 100, 5, A, GTC);
  o.add_limit(2, Side::Sell, 105, 5, B, GTC);
  CHECK(o.modify(1, 106, 5));
  auto s = o.snapshot();
  CHECK(s.size() == 2 && s[0].price == 106 && s[1].price == 105);

  Result r = o.add_limit(3, Side::Sell, 104, 2, B, GTC);
  CHECK(r.trades.size() == 1 && r.trades[0].maker_order_id == 1);
  CHECK(r.trades[0].price == 106 && r.trades[0].qty == 2);
}

int main() {
  crossing_fills_at_makers_price_and_leaves_residual();
  sweep_respects_price_then_time();
  self_trade_rejects_the_incoming_remainder();
  fok_counts_only_liquidity_before_a_self_match();
  fok_fills_across_levels_when_enough();
  ioc_and_market_never_rest();
  modify_priority_rules();
  rejections_leave_state_alone();
  capacity_exhaustion_is_a_clean_rejection();
  trade_ids_and_sequence_numbers();
  modify_never_crosses_but_later_orders_do();
  std::printf("oracle self-check: all passed\n");
  return 0;
}
