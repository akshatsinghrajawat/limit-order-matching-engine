// Conformance tests 1-8 (docs/spec.md test list). Plain asserts, no
// framework -- consistent with the project's hand-rolled-tools approach
// and the fact there's no oracle yet to diff against (that's M2). This
// file grows into the real conformance/ suite once tests 9-19 have
// something to test against (modify, STP, IOC/FOK all land after M1).
#include <cassert>
#include <cstdio>

#include "engine/book.hpp"

using namespace engine;

static void test1_single_limit_rests() {
  Book book(16);
  auto r = book.add_limit_order(1, Side::Buy, 100, 10, /*account=*/1);
  assert(r.accepted);
  assert(r.trades.empty());
  assert(r.resting_qty == 10);
  assert(book.has_bid());
  assert(book.best_bid() == 100);
}

static void test2_best_bid_ask_tracking() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, 1);
  book.add_limit_order(2, Side::Buy, 105, 10, 1);  // better bid
  book.add_limit_order(3, Side::Sell, 110, 10, 1);
  book.add_limit_order(4, Side::Sell, 108, 10, 1);  // better ask
  assert(book.best_bid() == 105);
  assert(book.best_ask() == 108);
}

static void test3_crossing_fills_at_makers_price() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, 1);  // maker at 100
  auto r = book.add_limit_order(2, Side::Buy, 105, 10, 1);  // taker willing to pay 105
  assert(r.trades.size() == 1);
  assert(r.trades[0].price == 100);  // maker's price, not the taker's
  assert(r.resting_qty == 0);
  assert(!book.has_ask());
}

static void test4_multilevel_sweep_price_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 102, 5, 1);
  book.add_limit_order(2, Side::Sell, 100, 5, 1);  // better price, must fill first
  book.add_limit_order(3, Side::Sell, 101, 5, 1);
  auto r = book.add_limit_order(4, Side::Buy, 102, 15, 1);
  assert(r.trades.size() == 3);
  assert(r.trades[0].price == 100);
  assert(r.trades[1].price == 101);
  assert(r.trades[2].price == 102);
  assert(r.resting_qty == 0);
}

static void test5_partial_fill_retains_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, 1);
  auto r = book.add_limit_order(2, Side::Buy, 100, 4, 1);  // partial taker
  assert(r.trades.size() == 1 && r.trades[0].qty == 4);
  assert(book.has_ask());
  assert(book.best_ask() == 100);
  // maker 1's residual (6) must still be at the head -- verified by having
  // it fill first against the next taker, ahead of a newer same-price order.
  book.add_limit_order(3, Side::Sell, 100, 10, 1);
  auto r2 = book.add_limit_order(4, Side::Buy, 100, 6, 1);
  assert(r2.trades.size() == 1);
  assert(r2.trades[0].maker_order_id == 1);  // order 1's residual, not order 3
}

static void test6_fifo_within_level() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, 1);
  book.add_limit_order(2, Side::Sell, 100, 5, 1);
  book.add_limit_order(3, Side::Sell, 100, 5, 1);
  auto r = book.add_limit_order(4, Side::Buy, 100, 15, 1);
  assert(r.trades.size() == 3);
  assert(r.trades[0].maker_order_id == 1);
  assert(r.trades[1].maker_order_id == 2);
  assert(r.trades[2].maker_order_id == 3);
}

static void test7_cancel_o1_and_level_removal() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, 1);
  assert(book.cancel_order(1));
  assert(!book.has_bid());  // level must be removed once empty, not left dangling
}

static void test8_cancel_already_cancelled_is_clean() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, 1);
  assert(book.cancel_order(1));
  assert(!book.cancel_order(1));  // second cancel: clean rejection, no crash
  assert(!book.cancel_order(999));  // never existed: same clean rejection
}

int main() {
  test1_single_limit_rests();
  test2_best_bid_ask_tracking();
  test3_crossing_fills_at_makers_price();
  test4_multilevel_sweep_price_priority();
  test5_partial_fill_retains_priority();
  test6_fifo_within_level();
  test7_cancel_o1_and_level_removal();
  test8_cancel_already_cancelled_is_clean();
  std::printf("conformance tests 1-8: all passed\n");
  return 0;
}
