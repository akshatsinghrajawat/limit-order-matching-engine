#pragma once

// Replays a journal into a fresh book and writes down everything that
// happened as text. The text is the engine's observable output: S7 says it
// must be the same bytes on every run, and tests 22 and 23 hold it to that.
//
// The journal is the only input (ADR-006). Replay does not look at any
// event log the original run produced; it makes its own.
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "book.hpp"
#include "command.hpp"
#include "journal.hpp"

namespace engine::replay {

// What a command did, as far as the log records it.
struct Outcome {
  bool ok = false;  // add or market: accepted. cancel or modify: took effect.
  Qty resting_qty = 0;
  bool stp_rejected = false;
  bool pool_exhausted = false;
};

// Runs one command on the book. The trades an add made are left in
// scratch.trades, which is empty after any other command. A command that is
// not valid does nothing: the book is untouched, it takes no sequence
// number, and the outcome is the default one (ADR-011).
inline Outcome apply(Book& book, const Command& c, Book::AddResult& scratch) {
  Outcome o;
  if (!valid(c)) {
    scratch.reset();
    return o;
  }
  switch (c.kind) {
    case CommandKind::Limit:
      book.add_limit_order(scratch, c.id, c.side, c.price, c.qty, c.account, c.tif);
      break;
    case CommandKind::Market:
      book.add_market_order(scratch, c.id, c.side, c.qty, c.account);
      break;
    case CommandKind::Cancel:
      scratch.reset();
      o.ok = book.cancel_order(c.id);
      return o;
    case CommandKind::Modify:
      scratch.reset();
      o.ok = book.modify_order(c.id, c.price, c.qty);
      return o;
  }
  o.ok = scratch.accepted;
  o.resting_qty = scratch.resting_qty;
  o.stp_rejected = scratch.stp_rejected;
  o.pool_exhausted = scratch.pool_exhausted;
  return o;
}

namespace detail {

inline void line(std::string& out, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

inline void line(std::string& out, const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  const int n = std::vsnprintf(buf, sizeof buf, fmt, args);
  va_end(args);
  out.append(buf, static_cast<std::size_t>(n));
  out.push_back('\n');
}

inline unsigned long long u(std::uint64_t v) { return v; }
inline long long s(std::int64_t v) { return v; }

}  // namespace detail

// One line for the command and its outcome, then one for each trade it made.
// `position` is the command's place in the journal, counting from 0.
inline void append_events(std::string& log, std::size_t position, const Command& c, const Outcome& o,
                          const std::vector<Trade>& trades) {
  using detail::line;
  using detail::s;
  using detail::u;
  const unsigned long long pos = position;
  switch (c.kind) {
    case CommandKind::Limit:
      line(log, "#%llu limit id=%llu side=%s price=%lld qty=%u account=%u tif=%s ok=%d rest=%u stp=%d full=%d", pos,
           u(c.id), c.side == Side::Buy ? "buy" : "sell", s(c.price), c.qty, c.account,
           c.tif == TimeInForce::GTC ? "gtc" : c.tif == TimeInForce::IOC ? "ioc" : "fok", o.ok,
           o.resting_qty, o.stp_rejected, o.pool_exhausted);
      break;
    case CommandKind::Market:
      line(log, "#%llu market id=%llu side=%s qty=%u account=%u ok=%d rest=%u stp=%d full=%d", pos, u(c.id),
           c.side == Side::Buy ? "buy" : "sell", c.qty, c.account, o.ok, o.resting_qty, o.stp_rejected,
           o.pool_exhausted);
      break;
    case CommandKind::Cancel:
      line(log, "#%llu cancel id=%llu ok=%d", pos, u(c.id), o.ok);
      break;
    case CommandKind::Modify:
      line(log, "#%llu modify id=%llu price=%lld qty=%u ok=%d", pos, u(c.id), s(c.price), c.qty, o.ok);
      break;
  }
  for (const Trade& t : trades) {
    line(log, "  trade seq=%llu id=%llu maker=%llu taker=%llu price=%lld qty=%u", u(t.seq_no), u(t.trade_id),
         u(t.maker_order_id), u(t.taker_order_id), s(t.price), t.qty);
  }
}

// Every resting order, bids best first and then asks best first, oldest
// first within a price: the order Book::snapshot() defines.
inline std::string serialize_book(const Book& book) {
  using detail::line;
  using detail::s;
  using detail::u;
  std::string out;
  const auto orders = book.snapshot();
  for (const auto& o : orders) {
    line(out, "%s price=%lld id=%llu qty=%u account=%u seq=%llu", o.side == Side::Buy ? "bid" : "ask", s(o.price),
         u(o.order_id), o.qty, o.account, u(o.seq_no));
  }
  line(out, "orders=%zu", orders.size());
  return out;
}

struct Replayed {
  std::string log;
  std::string book;                   // the book after the last command that replayed
  std::size_t commands = 0;           // how many commands replayed
  journal::Status stop = journal::Status::End;  // why reading stopped
  std::size_t valid_bytes = 0;        // where the good part of the journal ends
};

// Replays every command up to the first bad record. Returns false only when
// the header cannot be read, in which case nothing was replayed and
// out.stop says why. A damaged tail is not a failure: out.stop reports it
// and everything before it is replayed.
inline bool replay(const std::uint8_t* data, std::size_t size, Replayed& out) {
  out = Replayed{};
  journal::Reader reader(data, size);
  out.stop = reader.open();
  if (out.stop != journal::Status::Ok) return false;

  Book book(reader.pool_capacity());
  Book::AddResult scratch;
  Command c;
  while ((out.stop = reader.next(c)) == journal::Status::Ok) {
    const Outcome o = apply(book, c, scratch);
    append_events(out.log, out.commands, c, o, scratch.trades);
    ++out.commands;
  }
  out.valid_bytes = reader.valid_bytes();
  out.book = serialize_book(book);
  return true;
}

}  // namespace engine::replay
