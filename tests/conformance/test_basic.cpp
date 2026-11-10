// Conformance tests 1-19 plus 8b/8c/12b/14b (docs/spec.md test list), and
// 12c/12d for the ADR-009 decisions and 14c/14d for the width of the FOK
// pre-check's sum. Plain CHECKs, no framework -- consistent with the project's hand-rolled-tools
// approach and the fact there's no oracle yet to diff against (that's M2).
// This closes out the conformance list from the spec; M2's oracle and
// differential fuzzer are next.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies, and
// so the Book calls inside a check are not compiled out with it.
//
// Account convention (S3): every Buy order in this file uses account
// kBuyAcct and every Sell order uses kSellAcct, so ordinary crossing tests
// never accidentally trip self-trade prevention. Test 17 is the sole
// deliberate exception -- it assigns a matching account across sides to
// produce a real self-match.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>

#include "engine/book.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace engine;

static constexpr std::uint32_t kBuyAcct = 1;
static constexpr std::uint32_t kSellAcct = 2;

static void test1_single_limit_rests() {
  Book book(16);
  auto r = book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  CHECK(r.accepted);
  CHECK(r.trades.empty());
  CHECK(r.resting_qty == 10);
  CHECK(book.has_bid());
  CHECK(book.best_bid() == 100);
}

static void test2_best_bid_ask_tracking() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  book.add_limit_order(2, Side::Buy, 105, 10, kBuyAcct);  // better bid
  book.add_limit_order(3, Side::Sell, 110, 10, kSellAcct);
  book.add_limit_order(4, Side::Sell, 108, 10, kSellAcct);  // better ask
  CHECK(book.best_bid() == 105);
  CHECK(book.best_ask() == 108);
}

static void test3_crossing_fills_at_makers_price() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, kSellAcct);  // maker at 100
  auto r = book.add_limit_order(2, Side::Buy, 105, 10, kBuyAcct);  // taker willing to pay 105
  CHECK(r.trades.size() == 1);
  CHECK(r.trades[0].price == 100);  // maker's price, not the taker's
  CHECK(r.resting_qty == 0);
  CHECK(!book.has_ask());
}

static void test4_multilevel_sweep_price_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 102, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);  // better price, must fill first
  book.add_limit_order(3, Side::Sell, 101, 5, kSellAcct);
  auto r = book.add_limit_order(4, Side::Buy, 102, 15, kBuyAcct);
  CHECK(r.trades.size() == 3);
  CHECK(r.trades[0].price == 100);
  CHECK(r.trades[1].price == 101);
  CHECK(r.trades[2].price == 102);
  CHECK(r.resting_qty == 0);
}

static void test5_partial_fill_retains_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, kSellAcct);
  auto r = book.add_limit_order(2, Side::Buy, 100, 4, kBuyAcct);  // partial taker
  CHECK(r.trades.size() == 1 && r.trades[0].qty == 4);
  CHECK(book.has_ask());
  CHECK(book.best_ask() == 100);
  // maker 1's residual (6) must still be at the head -- verified by having
  // it fill first against the next taker, ahead of a newer same-price order.
  book.add_limit_order(3, Side::Sell, 100, 10, kSellAcct);
  auto r2 = book.add_limit_order(4, Side::Buy, 100, 6, kBuyAcct);
  CHECK(r2.trades.size() == 1);
  CHECK(r2.trades[0].maker_order_id == 1);  // order 1's residual, not order 3
}

static void test6_fifo_within_level() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(3, Side::Sell, 100, 5, kSellAcct);
  auto r = book.add_limit_order(4, Side::Buy, 100, 15, kBuyAcct);
  CHECK(r.trades.size() == 3);
  CHECK(r.trades[0].maker_order_id == 1);
  CHECK(r.trades[1].maker_order_id == 2);
  CHECK(r.trades[2].maker_order_id == 3);
}

static void test7_cancel_o1_and_level_removal() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  CHECK(book.cancel_order(1));
  CHECK(!book.has_bid());  // level must be removed once empty, not left dangling
}

static void test8_cancel_already_cancelled_is_clean() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  CHECK(book.cancel_order(1));
  CHECK(!book.cancel_order(1));  // second cancel: clean rejection, no crash
  CHECK(!book.cancel_order(999));  // never existed: same clean rejection
}

static void test8b_modify_after_cancel_is_clean() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 10, kBuyAcct);
  book.add_limit_order(2, Side::Buy, 99, 5, kBuyAcct);
  CHECK(book.cancel_order(1));
  CHECK(!book.modify_order(1, 100, 5));  // slot was freed via cancel_order
  // the other resting order must be completely unaffected by the rejection
  CHECK(book.order_at(book.head_at(Side::Buy, 99)).order_id == 2);
  CHECK(book.order_at(book.head_at(Side::Buy, 99)).qty == 5);
}

static void test8c_modify_after_fill_is_clean() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Buy, 100, 5, kBuyAcct);  // fully fills order 1 --
      // its slot is freed via cross()'s pool_.free(), a different call site
      // than cancel_order's. Pairs with 8b: if only one of these two tests
      // fails, that localizes which free path has the bug.
  CHECK(!book.modify_order(1, 100, 10));
}

static void test9_modify_qty_reduce_keeps_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);
  CHECK(book.modify_order(1, 100, 6));  // qty-reduce, same price
  CHECK(book.order_at(book.head_at(Side::Sell, 100)).order_id == 1);
  CHECK(book.order_at(book.head_at(Side::Sell, 100)).qty == 6);
  // prove priority was actually kept, not just the qty field: a taker for
  // exactly the reduced qty must drain order 1, not order 2.
  auto r = book.add_limit_order(3, Side::Buy, 100, 6, kBuyAcct);
  CHECK(r.trades.size() == 1);
  CHECK(r.trades[0].maker_order_id == 1);
  CHECK(book.order_at(book.head_at(Side::Sell, 100)).order_id == 2);
}

static void test10_modify_qty_increase_loses_priority() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);
  CHECK(book.modify_order(1, 100, 10));  // qty-increase, same price
  CHECK(book.order_at(book.head_at(Side::Sell, 100)).order_id == 2);
  auto r = book.add_limit_order(3, Side::Buy, 100, 5, kBuyAcct);
  CHECK(r.trades.size() == 1);
  CHECK(r.trades[0].maker_order_id == 2);  // order 2 now fills first
}

static void test11_modify_price_change_moves_level() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 10, kSellAcct);
  CHECK(book.modify_order(1, 105, 10));
  CHECK(book.best_ask() == 105);  // the 100 level must be gone, not just
                                    // shadowed -- else best_ask would be 100
  CHECK(book.order_at(book.head_at(Side::Sell, 105)).order_id == 1);
}

static void test12_modify_price_change_joins_existing_level_at_tail() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 105, 5, kSellAcct);
  CHECK(book.modify_order(1, 105, 5));  // moves into 2's level
  CHECK(book.order_at(book.head_at(Side::Sell, 105)).order_id == 2);
  auto r = book.add_limit_order(3, Side::Buy, 105, 10, kBuyAcct);
  CHECK(r.trades.size() == 2);
  CHECK(r.trades[0].maker_order_id == 2);  // pre-existing priority holds
  CHECK(r.trades[1].maker_order_id == 1);  // order 1 landed at the tail
}

static void test12b_modify_relink_within_same_level_preserves_fifo() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(3, Side::Sell, 100, 5, kSellAcct);
  // qty-increase at an unchanged price forces the unlink+append branch
  // with old_level == new_level -- the exact case the spec warns must not
  // be a swap-while-linked, or it corrupts 1's and 3's links.
  CHECK(book.modify_order(2, 100, 6));
  auto r = book.add_limit_order(4, Side::Buy, 100, 16, kBuyAcct);
  CHECK(r.trades.size() == 3);
  CHECK(r.trades[0].maker_order_id == 1);
  CHECK(r.trades[1].maker_order_id == 3);  // 3 now ahead of the moved 2
  CHECK(r.trades[2].maker_order_id == 2);
  CHECK(r.trades[2].qty == 6);
}

static void test13_market_order_sweeps_and_cancels_remainder() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 101, 5, kSellAcct);
  auto r = book.add_market_order(3, Side::Buy, 12, kBuyAcct);  // asks for
      // more than either level, or both combined, can supply
  CHECK(r.accepted);
  CHECK(r.trades.size() == 2);
  CHECK(r.trades[0].price == 100);  // best price first -- no price bound
  CHECK(r.trades[1].price == 101);  // doesn't change that priority still
  CHECK(r.trades[0].qty == 5 && r.trades[1].qty == 5);
  CHECK(r.resting_qty == 0);        // unfilled 2 is cancelled, not rested
  CHECK(!book.has_ask());           // book swept clean, no residual level
}

static void test14_ioc_partial_fill_cancels_remainder() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  auto r = book.add_limit_order(2, Side::Buy, 100, 10, kBuyAcct,
                                 TimeInForce::IOC);
  CHECK(r.trades.size() == 1 && r.trades[0].qty == 5);
  CHECK(r.resting_qty == 0);   // remaining 5 cancelled -- IOC never rests
  CHECK(!book.has_bid());      // and never touches the bid side at all
}

static void test14b_fok_pre_check_respects_self_trade_boundary() {
  Book book(16);
  static constexpr std::uint32_t kSelfAcct = 77;
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);   // available
  book.add_limit_order(2, Side::Sell, 100, 5, kSelfAcct);   // blocks the walk
  book.add_limit_order(3, Side::Sell, 100, 5, kSellAcct);   // unreachable --
      // a naive "sum qty at price" pre-check would see 15 and pass a
      // 10-lot FOK; the real available qty stops at order 2, so it's 5.
  auto r = book.add_limit_order(4, Side::Buy, 100, 10, kSelfAcct,
                                 TimeInForce::FOK);
  CHECK(r.accepted);
  CHECK(r.trades.empty());     // rejected before touching the book
  CHECK(book.has_ask());
  CHECK(book.best_ask() == 100);
  // all three resting orders completely untouched by the rejected attempt
  CHECK(book.order_at(book.head_at(Side::Sell, 100)).order_id == 1);
  CHECK(book.order_at(book.head_at(Side::Sell, 100)).qty == 5);
}

// The pre-check adds up the quantity it can reach, and a sum of 32-bit
// quantities does not fit in 32 bits. Each case here has a total that is
// exactly right in 64 bits and wrong after wrapping.
static void test14c_fok_pre_check_sum_is_not_32_bit() {
  static constexpr Qty kMax = std::numeric_limits<Qty>::max();
  static constexpr Qty kHalf = Qty{1} << 31;

  {  // one lot short of what the order asks for: rejected, book untouched
    Book book(16);
    book.add_limit_order(1, Side::Sell, 100, kMax - 1, kSellAcct);
    auto r = book.add_limit_order(2, Side::Buy, 100, kMax, kBuyAcct, TimeInForce::FOK);
    CHECK(r.accepted);
    CHECK(r.trades.empty());
    CHECK(r.resting_qty == 0);
    CHECK(book.snapshot().size() == 1);
    CHECK(book.order_at(book.head_at(Side::Sell, 100)).qty == kMax - 1);
    CHECK(book.invariant_violation() == nullptr);
  }
  {  // exactly the largest quantity there is
    Book book(16);
    book.add_limit_order(1, Side::Sell, 100, kMax, kSellAcct);
    auto r = book.add_limit_order(2, Side::Buy, 100, kMax, kBuyAcct, TimeInForce::FOK);
    CHECK(r.trades.size() == 1 && r.trades[0].qty == kMax);
    CHECK(!book.has_ask());
    CHECK(book.invariant_violation() == nullptr);
  }
  {  // two makers whose quantities add up to 2^32, which wraps to 0
    Book book(16);
    book.add_limit_order(1, Side::Sell, 100, kHalf, kSellAcct);
    book.add_limit_order(2, Side::Sell, 100, kHalf, kSellAcct);
    auto r = book.add_limit_order(3, Side::Buy, 100, 1, kBuyAcct, TimeInForce::FOK);
    CHECK(r.trades.size() == 1 && r.trades[0].maker_order_id == 1 && r.trades[0].qty == 1);
    CHECK(book.snapshot().size() == 2);
    CHECK(book.order_at(book.head_at(Side::Sell, 100)).qty == kHalf - 1);
    CHECK(book.invariant_violation() == nullptr);
  }
  {  // 2^32 + 5 available across two levels, which wraps to 5, against 7
    Book book(16);
    book.add_limit_order(1, Side::Sell, 100, kMax, kSellAcct);
    book.add_limit_order(2, Side::Sell, 101, 6, kSellAcct);
    auto r = book.add_limit_order(3, Side::Buy, 101, 7, kBuyAcct, TimeInForce::FOK);
    CHECK(r.trades.size() == 1 && r.trades[0].maker_order_id == 1 && r.trades[0].qty == 7);
    CHECK(book.snapshot().size() == 2);
    CHECK(book.invariant_violation() == nullptr);
  }
  {  // three levels of 2^31 and an order for 2^32 - 1: filled across two
    Book book(16);
    book.add_limit_order(1, Side::Sell, 100, kHalf, kSellAcct);
    book.add_limit_order(2, Side::Sell, 101, kHalf, kSellAcct);
    book.add_limit_order(3, Side::Sell, 102, kHalf, kSellAcct);
    auto r = book.add_limit_order(4, Side::Buy, 102, kMax, kBuyAcct, TimeInForce::FOK);
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0].maker_order_id == 1 && r.trades[0].qty == kHalf);
    CHECK(r.trades[1].maker_order_id == 2 && r.trades[1].qty == kHalf - 1);
    CHECK(book.snapshot().size() == 2);
    CHECK(book.order_at(book.head_at(Side::Sell, 101)).qty == 1);
    CHECK(book.order_at(book.head_at(Side::Sell, 102)).qty == kHalf);
    CHECK(book.invariant_violation() == nullptr);
  }
  {  // the same on the other side of the book
    Book book(16);
    book.add_limit_order(1, Side::Buy, 100, kHalf, kBuyAcct);
    book.add_limit_order(2, Side::Buy, 100, kHalf, kBuyAcct);
    auto r = book.add_limit_order(3, Side::Sell, 100, 1, kSellAcct, TimeInForce::FOK);
    CHECK(r.trades.size() == 1 && r.trades[0].maker_order_id == 1);
    CHECK(book.invariant_violation() == nullptr);
  }
}

// The self-trade boundary still decides what counts when the quantities are
// large: liquidity behind the first order of the taker's own account is not
// available, however much of it there is.
static void test14d_fok_pre_check_self_trade_boundary_with_wide_quantities() {
  static constexpr Qty kMax = std::numeric_limits<Qty>::max();
  static constexpr Qty kHalf = Qty{1} << 31;
  static constexpr std::uint32_t kSelfAcct = 77;

  {  // exactly the order's quantity sits ahead of the self order: filled
    Book book(16);
    book.add_limit_order(1, Side::Sell, 100, kHalf, kSellAcct);
    book.add_limit_order(2, Side::Sell, 100, kHalf - 1, kSellAcct);
    book.add_limit_order(3, Side::Sell, 100, 5, kSelfAcct);
    book.add_limit_order(4, Side::Sell, 100, kHalf, kSellAcct);
    auto r = book.add_limit_order(5, Side::Buy, 100, kMax, kSelfAcct, TimeInForce::FOK);
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0].qty == kHalf && r.trades[1].qty == kHalf - 1);
    CHECK(!r.stp_rejected);
    CHECK(book.snapshot().size() == 2);
    CHECK(book.order_at(book.head_at(Side::Sell, 100)).order_id == 3);
    CHECK(book.invariant_violation() == nullptr);
  }
  {  // one lot less ahead of it: rejected, nothing touched
    Book book(16);
    book.add_limit_order(1, Side::Sell, 100, kHalf, kSellAcct);
    book.add_limit_order(2, Side::Sell, 100, kHalf - 2, kSellAcct);
    book.add_limit_order(3, Side::Sell, 100, 5, kSelfAcct);
    book.add_limit_order(4, Side::Sell, 100, kHalf, kSellAcct);
    auto r = book.add_limit_order(5, Side::Buy, 100, kMax, kSelfAcct, TimeInForce::FOK);
    CHECK(r.accepted);
    CHECK(r.trades.empty());
    CHECK(book.snapshot().size() == 4);
    CHECK(book.order_at(book.head_at(Side::Sell, 100)).qty == kHalf);
    CHECK(book.invariant_violation() == nullptr);
  }
}

static void test15_fok_success_fills_atomically() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  book.add_limit_order(2, Side::Sell, 101, 5, kSellAcct);
  auto r = book.add_limit_order(3, Side::Buy, 101, 10, kBuyAcct,
                                 TimeInForce::FOK);
  CHECK(r.accepted);
  CHECK(r.trades.size() == 2);
  CHECK(r.trades[0].maker_order_id == 1);  // price-time priority, as usual
  CHECK(r.trades[1].maker_order_id == 2);
  CHECK(r.resting_qty == 0);
  CHECK(!book.has_ask());
}

static void test16_fok_failure_leaves_book_untouched() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 5, kSellAcct);
  auto r = book.add_limit_order(2, Side::Buy, 100, 10, kBuyAcct,
                                 TimeInForce::FOK);  // only 5 available
  CHECK(r.accepted);
  CHECK(r.trades.empty());  // all-or-nothing: nothing, since not all
  CHECK(r.resting_qty == 0);
  CHECK(book.has_ask());
  CHECK(book.best_ask() == 100);
  CHECK(book.order_at(book.head_at(Side::Sell, 100)).qty == 5);  // untouched
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

  CHECK(r.accepted);
  CHECK(r.trades.size() == 1);          // only the non-self fill happened
  CHECK(r.trades[0].maker_order_id == 1);
  CHECK(r.trades[0].qty == 5);
  CHECK(r.stp_rejected);
  CHECK(r.resting_qty == 0);            // remaining 5 rejected, not rested

  // order 2 (the self-match) must be completely untouched: still resting,
  // full original qty, still at the head of its level.
  CHECK(book.has_ask());
  CHECK(book.best_ask() == 100);
  CHECK(book.order_at(book.head_at(Side::Sell, 100)).order_id == 2);
  CHECK(book.order_at(book.head_at(Side::Sell, 100)).qty == 5);
}

static void test18_invalid_input_rejected_at_ingress() {
  Book book(16);
  auto r_zero_limit = book.add_limit_order(1, Side::Buy, 100, 0, kBuyAcct);
  CHECK(!r_zero_limit.accepted);
  CHECK(!book.has_bid());

  auto r_zero_market = book.add_market_order(2, Side::Buy, 0, kBuyAcct);
  CHECK(!r_zero_market.accepted);

  book.add_limit_order(3, Side::Buy, 100, 5, kBuyAcct);
  auto r_dup = book.add_limit_order(3, Side::Sell, 100, 5, kSellAcct);
      // reused order_id -- must be rejected at the id_map_ lookup, before
      // it ever reaches the pool or has a chance to cross (S1)
  CHECK(!r_dup.accepted);
  CHECK(r_dup.trades.empty());
  // order 3 is completely untouched by the rejected duplicate -- still
  // the original Buy order, not silently overwritten
  CHECK(book.order_at(book.head_at(Side::Buy, 100)).side == Side::Buy);
  CHECK(book.order_at(book.head_at(Side::Buy, 100)).qty == 5);

  CHECK(!book.modify_order(3, 100, 0));  // qty=0 modify also rejected
  CHECK(book.order_at(book.head_at(Side::Buy, 100)).qty == 5);  // untouched
}

static void test19_pool_exhaustion_is_clean_rejection() {
  Book book(2);  // tiny pool -- exactly 2 slots, deliberately tight
  auto r1 = book.add_limit_order(1, Side::Buy, 100, 5, kBuyAcct);
  auto r2 = book.add_limit_order(2, Side::Buy, 99, 5, kBuyAcct);
  CHECK(r1.accepted && r1.resting_qty == 5 && !r1.pool_exhausted);
  CHECK(r2.accepted && r2.resting_qty == 5 && !r2.pool_exhausted);

  // pool is now full (2/2). A third order with nothing to cross against
  // must be rejected cleanly on the rest() path, not corrupt memory via
  // an unvalidated Handle -- this is the case pool.hpp documented as
  // required and nothing previously exercised.
  auto r3 = book.add_limit_order(3, Side::Buy, 98, 5, kBuyAcct);
  CHECK(r3.accepted);           // well-formed order, ingress accepted it
  CHECK(r3.trades.empty());     // nothing to cross against
  CHECK(r3.pool_exhausted);
  CHECK(r3.resting_qty == 0);   // never entered the book

  // the two pre-existing orders are completely unaffected
  CHECK(book.best_bid() == 100);
  CHECK(book.order_at(book.head_at(Side::Buy, 100)).order_id == 1);
  CHECK(book.order_at(book.head_at(Side::Buy, 99)).order_id == 2);

  // order 3's price level (98) must not have been left behind as an
  // empty dangling level -- freeing a slot must still work normally
  // afterward, proving the pool and id_map_ are still in a sane state
  CHECK(book.cancel_order(1));
  CHECK(book.best_bid() == 99);
  auto r4 = book.add_limit_order(4, Side::Buy, 97, 5, kBuyAcct);
  CHECK(r4.accepted && r4.resting_qty == 5 && !r4.pool_exhausted);
}

// ADR-009: a modify that would trade is refused and changes nothing, so the
// book is never left crossed. Touching the opposite best counts.
static void test12c_modify_that_would_cross_is_rejected() {
  Book book(16);
  book.add_limit_order(1, Side::Buy, 100, 5, kBuyAcct);
  book.add_limit_order(3, Side::Buy, 100, 5, kBuyAcct);
  book.add_limit_order(2, Side::Sell, 105, 5, kSellAcct);

  CHECK(!book.modify_order(1, 106, 5));
  CHECK(!book.modify_order(1, 105, 5));
  CHECK(!book.modify_order(2, 100, 5));
  CHECK(book.best_bid() == 100 && book.best_ask() == 105);
  auto snap = book.snapshot();
  CHECK(snap.size() == 3);
  CHECK(snap[0].order_id == 1 && snap[0].seq_no == 0);  // still first in line
  CHECK(snap[1].order_id == 3 && snap[2].order_id == 2 && snap[2].price == 105);
  CHECK(book.invariant_violation() == nullptr);

  CHECK(book.modify_order(1, 104, 5));
  CHECK(book.modify_order(2, 106, 5));
  CHECK(book.best_bid() == 104 && book.best_ask() == 106);
  CHECK(book.invariant_violation() == nullptr);
}

// ADR-009: every command takes the next seq_no, accepted or not, and a trade
// carries the seq_no of the command that caused it.
static void test12d_every_command_takes_a_seq_no() {
  Book book(16);
  book.add_limit_order(1, Side::Sell, 100, 1, kSellAcct);  // 0
  book.add_limit_order(2, Side::Sell, 101, 1, kSellAcct);  // 1
  auto r = book.add_limit_order(3, Side::Buy, 101, 2, kBuyAcct);  // 2
  CHECK(r.trades.size() == 2);
  CHECK(r.trades[0].seq_no == 2 && r.trades[1].seq_no == 2);

  CHECK(!book.cancel_order(99));                                          // 3
  CHECK(!book.modify_order(99, 100, 1));                                  // 4
  CHECK(!book.add_limit_order(4, Side::Buy, 100, 0, kBuyAcct).accepted);  // 5
  book.add_limit_order(5, Side::Buy, 90, 1, kBuyAcct);                    // 6
  CHECK(!book.add_limit_order(5, Side::Buy, 91, 1, kBuyAcct).accepted);   // 7
  book.add_limit_order(6, Side::Buy, 90, 1, kBuyAcct);                    // 8
  r = book.add_limit_order(7, Side::Buy, 200, 5, kBuyAcct, TimeInForce::FOK);  // 9, nothing to take
  CHECK(r.accepted && r.trades.empty());
  book.add_limit_order(8, Side::Buy, 89, 1, kBuyAcct);                    // 10

  auto snap = book.snapshot();
  CHECK(snap.size() == 3);
  CHECK(snap[0].order_id == 5 && snap[0].seq_no == 6);
  CHECK(snap[1].order_id == 6 && snap[1].seq_no == 8);
  CHECK(snap[2].order_id == 8 && snap[2].seq_no == 10);

  r = book.add_market_order(9, Side::Sell, 1, kSellAcct);                 // 11
  CHECK(r.trades.size() == 1 && r.trades[0].seq_no == 11);
  book.add_limit_order(10, Side::Buy, 88, 1, kBuyAcct);                   // 12
  CHECK(book.snapshot().back().seq_no == 12);
}

// Not a numbered spec test: snapshot() is the observation hook M2's
// differential harness diffs through, so it gets its own check here before
// anything is built on top of it.
static void test_snapshot_reports_priority_order() {
  Book book(16);
  CHECK(book.snapshot().empty());

  book.add_limit_order(1, Side::Buy, 100, 5, kBuyAcct);
  book.add_limit_order(2, Side::Buy, 100, 7, kBuyAcct);
  book.add_limit_order(3, Side::Buy, 99, 4, kBuyAcct);
  book.add_limit_order(4, Side::Sell, 103, 6, kSellAcct);
  book.add_limit_order(5, Side::Sell, 101, 8, kSellAcct);

  auto snap = book.snapshot();
  CHECK(snap.size() == 5);
  CHECK(snap[0].order_id == 1 && snap[1].order_id == 2 && snap[2].order_id == 3);
  CHECK(snap[3].order_id == 5 && snap[4].order_id == 4);
  CHECK(snap[0].price == 100 && snap[2].price == 99);
  CHECK(snap[3].price == 101 && snap[4].price == 103);
  CHECK(snap[1].qty == 7 && snap[1].seq_no == 1 && snap[1].account == kBuyAcct);

  // qty increase sends order 1 to the tail of its level; snapshot must
  // reflect the new FIFO order and the fresh seq_no, not insertion order
  CHECK(book.modify_order(1, 100, 9));
  snap = book.snapshot();
  CHECK(snap[0].order_id == 2 && snap[1].order_id == 1);
  CHECK(snap[1].qty == 9 && snap[1].seq_no == 5);

  auto r = book.add_limit_order(6, Side::Buy, 101, 3, kBuyAcct);
  CHECK(r.trades.size() == 1);
  snap = book.snapshot();
  CHECK(snap[3].order_id == 5 && snap[3].qty == 5);
  CHECK(snap.size() == 5);
}

// Corrupts the book through order_at()'s const_cast to prove each check can
// actually fire. A checker that has never been seen failing proves nothing
// once the fuzzer starts relying on it.
static void test_invariants_hold_and_detect_corruption() {
  Book book(8);
  CHECK(book.invariant_violation() == nullptr);

  book.add_limit_order(1, Side::Buy, 100, 5, kBuyAcct);
  book.add_limit_order(2, Side::Buy, 100, 7, kBuyAcct);
  book.add_limit_order(3, Side::Buy, 99, 4, kBuyAcct);
  book.add_limit_order(4, Side::Sell, 103, 6, kSellAcct);
  book.modify_order(1, 100, 9);
  book.add_limit_order(5, Side::Buy, 103, 2, kBuyAcct);
  book.cancel_order(3);
  CHECK(book.invariant_violation() == nullptr);

  Index head = book.head_at(Side::Buy, 100);
  Index second = book.order_at(head).next_idx;
  Order& h = const_cast<Order&>(book.order_at(head));
  Order& s = const_cast<Order&>(book.order_at(second));

  Index saved_prev = s.prev_idx;
  s.prev_idx = kNilIndex;
  CHECK(book.invariant_violation() != nullptr);
  s.prev_idx = saved_prev;

  Qty saved_qty = h.qty;
  h.qty = 0;
  CHECK(book.invariant_violation() != nullptr);
  h.qty = saved_qty;

  SeqNo saved_seq = s.seq_no;
  s.seq_no = h.seq_no;
  CHECK(book.invariant_violation() != nullptr);
  s.seq_no = saved_seq;

  OrderId saved_id = s.order_id;
  s.order_id = 999;
  CHECK(book.invariant_violation() != nullptr);
  s.order_id = saved_id;

  CHECK(book.invariant_violation() == nullptr);
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
  test12c_modify_that_would_cross_is_rejected();
  test12d_every_command_takes_a_seq_no();
  test13_market_order_sweeps_and_cancels_remainder();
  test14_ioc_partial_fill_cancels_remainder();
  test14b_fok_pre_check_respects_self_trade_boundary();
  test14c_fok_pre_check_sum_is_not_32_bit();
  test14d_fok_pre_check_self_trade_boundary_with_wide_quantities();
  test15_fok_success_fills_atomically();
  test16_fok_failure_leaves_book_untouched();
  test17_self_trade_prevention();
  test18_invalid_input_rejected_at_ingress();
  test19_pool_exhaustion_is_clean_rejection();
  test_snapshot_reports_priority_order();
  test_invariants_hold_and_detect_corruption();
  std::printf("conformance tests 1-19 (+8b, 8c, 12b-12d, 14b-14d): all passed\n");
  return 0;
}
