// Conformance tests 1-12 plus 8b/8c/12b/17 (docs/spec.md test list). Plain
// asserts, no framework -- consistent with the project's hand-rolled-tools
// approach and the fact there's no oracle yet to diff against (that's M2).
// This file grows into the real conformance/ suite once tests 13-16/18/19
// have something to test against (market/IOC/FOK, invalid input, order-id
// wraparound all land after STP).
//
// Account convention (S3): every Buy order in this file uses account
// kBuyAcct and every Sell order uses kSellAcct, so ordinary crossing tests
// never accidentally trip self-trade prevention. Test 17 is the sole
// deliberate exception -- it assigns a matching account across sides to
// produce a real self-match.
#include <cassert>
#include <cstdio>

#include "engine/book.hpp"

using namespace engine;

static constexpr std::uint32_t kBuyAcct = 1;
static constexpr std::uint32_t kSellAcct = 2;

static void test1_single_limit_rests() {
  Book book(16);
  auto r = book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  assert(r.accepted);
  assert(r.trades.empty());
  assert(r.resting_qty == 10);
  assert(book.has_bid());
  assert(book.best_bid() == 100);
}

static void test2_best_bid_ask_tracking() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  book.add_limit_order(2, Side::Buy, 105, 10, kBuyAcct);  // better bid
  book.add_limit_order(3, Side::Sell, 110, 10, kSellAcct);
  book.add_limit_order(4, Side::Sell, 108, 10, kSellAcct);  // better ask
  assert(book.best_bid() == 105);
  assert(book.best_ask() == 108);
}

static void test3_crossing_fills_at_makers_price() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, kSellAcct);  // maker at 100
  auto r = book.add_limit_order(2, Side::Buy, 105, 10, kBuyAcct);  // taker willing to pay 105
  assert(r.trades.size() == 1);
  assert(r.trades[0].price == 100);  // maker's price, not the taker's
  assert(r.resting_qty == 0);
  assert(!book.has_ask());
}

static void test4_multilevel_sweep_price_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 102, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);  // better price, must fill first
  book.add_limit_order(3, Side::Sell, 101, 5, kSellAcct);
  auto r = book.add_limit_order(4, Side::Buy, 102, 15, kBuyAcct);
  assert(r.trades.size() == 3);
  assert(r.trades[0].price == 100);
  assert(r.trades[1].price == 101);
  assert(r.trades[2].price == 102);
  assert(r.resting_qty == 0);
}

static void test5_partial_fill_retains_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, kSellAcct);
  auto r = book.add_limit_order(2, Side::Buy, 100, 4, kBuyAcct);  // partial taker
  assert(r.trades.size() == 1 && r.trades[0].qty == 4);
  assert(book.has_ask());
  assert(book.best_ask() == 100);
  // maker 1's residual (6) must still be at the head -- verified by having
  // it fill first against the next taker, ahead of a newer same-price order.
  book.add_limit_order(3, Side::Sell, 100, 10, kSellAcct);
  auto r2 = book.add_limit_order(4, Side::Buy, 100, 6, kBuyAcct);
  assert(r2.trades.size() == 1);
  assert(r2.trades[0].maker_order_id == 1);  // order 1's residual, not order 3
}

static void test6_fifo_within_level() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(3, Side::Sell, 100, 5, kSellAcct);
  auto r = book.add_limit_order(4, Side::Buy, 100, 15, kBuyAcct);
  assert(r.trades.size() == 3);
  assert(r.trades[0].maker_order_id == 1);
  assert(r.trades[1].maker_order_id == 2);
  assert(r.trades[2].maker_order_id == 3);
}

static void test7_cancel_o1_and_level_removal() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  assert(book.cancel_order(1));
  assert(!book.has_bid());  // level must be removed once empty, not left dangling
}

static void test8_cancel_already_cancelled_is_clean() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  assert(book.cancel_order(1));
  assert(!book.cancel_order(1));  // second cancel: clean rejection, no crash
  assert(!book.cancel_order(999));  // never existed: same clean rejection
}

static void test8b_modify_after_cancel_is_clean() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  book.add_limit_order(2, Side::Buy, 99, 5, kBuyAcct);
  assert(book.cancel_order(1));
  assert(!book.modify_order(1, 100, 5));  // slot was freed via cancel_order
  // the other resting order must be completely unaffected by the rejection
  assert(book.order_at(book.head_at(Side::Buy, 99)).order_id == 2);
  assert(book.order_at(book.head_at(Side::Buy, 99)).qty == 5);
}

static void test8c_modify_after_fill_is_clean() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Buy, 100, 5, kBuyAcct);  // fully fills order 1 --
      // its slot is freed via cross()'s pool_.free(), a different call site
      // than cancel_order's. Pairs with 8b: if only one of these two tests
      // fails, that localizes which free path has the bug.
  assert(!book.modify_order(1, 100, 10));
}

static void test9_modify_qty_reduce_keeps_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);
  assert(book.modify_order(1, 100, 6));  // qty-reduce, same price
  assert(book.order_at(book.head_at(Side::Sell, 100)).order_id == 1);
  assert(book.order_at(book.head_at(Side::Sell, 100)).qty == 6);
  // prove priority was actually kept, not just the qty field: a taker for
  // exactly the reduced qty must drain order 1, not order 2.
  auto r = book.add_limit_order(3, Side::Buy, 100, 6, kBuyAcct);
  assert(r.trades.size() == 1);
  assert(r.trades[0].maker_order_id == 1);
  assert(book.order_at(book.head_at(Side::Sell, 100)).order_id == 2);
}

static void test10_modify_qty_increase_loses_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);
  assert(book.modify_order(1, 100, 10));  // qty-increase, same price
  assert(book.order_at(book.head_at(Side::Sell, 100)).order_id == 2);
  auto r = book.add_limit_order(3, Side::Buy, 100, 5, kBuyAcct);
  assert(r.trades.size() == 1);
  assert(r.trades[0].maker_order_id == 2);  // order 2 now fills first
}

static void test11_modify_price_change_moves_level() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, kSellAcct);
  assert(book.modify_order(1, 105, 10));
  assert(book.best_ask() == 105);  // the 100 level must be gone, not just
                                    // shadowed -- else best_ask would be 100
  assert(book.order_at(book.head_at(Side::Sell, 105)).order_id == 1);
}

static void test12_modify_price_change_joins_existing_level_at_tail() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 105, 5, kSellAcct);
  assert(book.modify_order(1, 105, 5));  // moves into 2's level
  assert(book.order_at(book.head_at(Side::Sell, 105)).order_id == 2);
  auto r = book.add_limit_order(3, Side::Buy, 105, 10, kBuyAcct);
  assert(r.trades.size() == 2);
  assert(r.trades[0].maker_order_id == 2);  // pre-existing priority holds
  assert(r.trades[1].maker_order_id == 1);  // order 1 landed at the tail
}

static void test12b_modify_relink_within_same_level_preserves_fifo() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(3, Side::Sell, 100, 5, kSellAcct);
  // qty-increase at an unchanged price forces the unlink+append branch
  // with old_level == new_level -- the exact case the spec warns must not
  // be a swap-while-linked, or it corrupts 1's and 3's links.
  assert(book.modify_order(2, 100, 6));
  auto r = book.add_limit_order(4, Side::Buy, 100, 16, kBuyAcct);
  assert(r.trades.size() == 3);
  assert(r.trades[0].maker_order_id == 1);
  assert(r.trades[1].maker_order_id == 3);  // 3 now ahead of the moved 2
  assert(r.trades[2].maker_order_id == 2);
  assert(r.trades[2].qty == 6);
}

static void test17_self_trade_prevention() {
  Book book(16);
  static constexpr std::uint32_t kSelfAcct = 99;  // deliberately shared
      // across sides -- the sole test in this file that does this.
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);  // different
      // account, must fill first -- price-time priority still applies
      // ahead of the self-match check.
  book.add_limit_order(2, Side::Sell, 100, 5, kSelfAcct);  // same account
      // as the incoming taker below; must block it.
  auto r = book.add_limit_order(3, Side::Buy, 100, 10, kSelfAcct);

  assert(r.accepted);
  assert(r.trades.size() == 1);          // only the non-self fill happened
  assert(r.trades[0].maker_order_id == 1);
  assert(r.trades[0].qty == 5);
  assert(r.stp_rejected);
  assert(r.resting_qty == 0);            // remaining 5 rejected, not rested

  // order 2 (the self-match) must be completely untouched: still resting,
  // full original qty, still at the head of its level.
  assert(book.has_ask());
  assert(book.best_ask() == 100);
  assert(book.order_at(book.head_at(Side::Sell, 100)).order_id == 2);
  assert(book.order_at(book.head_at(Side::Sell, 100)).qty == 5);
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
  test8b_modify_after_cancel_is_clean();
  test8c_modify_after_fill_is_clean();
  test9_modify_qty_reduce_keeps_priority();
  test10_modify_qty_increase_loses_priority();
  test11_modify_price_change_moves_level();
  test12_modify_price_change_joins_existing_level_at_tail();
  test12b_modify_relink_within_same_level_preserves_fifo();
  test17_self_trade_prevention();
  std::printf("conformance tests 1-12 (+8b, 8c, 12b, 17): all passed\n");
  return 0;
}
