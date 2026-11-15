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
#include <new>
#include <optional>
#include <stdexcept>
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

// Ends a va_list on every path out of line(), including a throw.
struct VaEnd {
  va_list& list;
  ~VaEnd() { va_end(list); }
};

// Every line the log writes today fits the buffer. One that does not is
// formatted again into a string of the size vsnprintf reported, from a copy
// of the arguments, so a longer field can never be cut off or read past the
// buffer. A negative result is a format that cannot be encoded: it throws
// instead of appending a line that is not what was asked for.
inline void line(std::string& out, const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_list again;
  va_start(args, fmt);
  va_copy(again, args);
  const VaEnd end_args{args};
  const VaEnd end_again{again};
  const int n = std::vsnprintf(buf, sizeof buf, fmt, args);
  if (n < 0) throw std::runtime_error("replay: output formatting failed");
  if (static_cast<std::size_t>(n) < sizeof buf) {
    out.append(buf, static_cast<std::size_t>(n));
  } else {
    std::string big(static_cast<std::size_t>(n) + 1, '\0');
    std::vsnprintf(big.data(), big.size(), fmt, again);
    out.append(big.data(), static_cast<std::size_t>(n));
  }
  out.push_back('\n');
}

inline unsigned long long u(std::uint64_t v) { return v; }
inline long long s(std::int64_t v) { return v; }

// The side as the log prints it. A value that is neither side is printed as
// such, not as a sell.
inline std::string side_label(Side side) {
  if (side == Side::Buy) return "buy";
  if (side == Side::Sell) return "sell";
  return "invalid(" + std::to_string(static_cast<unsigned>(side)) + ")";
}

inline std::string tif_label(TimeInForce tif) {
  if (tif == TimeInForce::GTC) return "gtc";
  if (tif == TimeInForce::IOC) return "ioc";
  if (tif == TimeInForce::FOK) return "fok";
  return "invalid(" + std::to_string(static_cast<unsigned>(tif)) + ")";
}

}  // namespace detail

// One line for the command and its outcome, then one for each trade it made.
// `position` is the command's place in the journal, counting from 0.
inline void append_events(std::string& log, std::size_t position, const Command& c, const Outcome& o,
                          const std::vector<Trade>& trades) {
  using detail::line;
  using detail::s;
  using detail::side_label;
  using detail::tif_label;
  using detail::u;
  const unsigned long long pos = position;
  switch (c.kind) {
    case CommandKind::Limit:
      line(log, "#%llu limit id=%llu side=%s price=%lld qty=%u account=%u tif=%s ok=%d rest=%u stp=%d full=%d", pos,
           u(c.id), side_label(c.side).c_str(), s(c.price), c.qty, c.account,
           tif_label(c.tif).c_str(), o.ok, o.resting_qty, o.stp_rejected, o.pool_exhausted);
      break;
    case CommandKind::Market:
      line(log, "#%llu market id=%llu side=%s qty=%u account=%u ok=%d rest=%u stp=%d full=%d", pos, u(c.id),
           side_label(c.side).c_str(), c.qty, c.account, o.ok, o.resting_qty, o.stp_rejected,
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

// Why replay refused to start or stopped, when the reason is not damage to
// the journal. journal::Status keeps describing the file.
enum class Limit : std::uint8_t {
  None,
  PoolCapacity,  // the header asks for more pool slots than ReplayLimits allows
  OutOfMemory,   // building the book or reserving the trade buffer failed
  JournalBytes,  // the journal is longer than ReplayLimits allows
  Commands,      // the journal holds more commands than ReplayLimits allows
  EventLogBytes, // the next command's lines would take the log past ReplayLimits
};

// The header's pool capacity is read from the file, and a Book of that size
// is allocated up front, so it is bounded before anything is built. A
// capacity over the bound is refused, never clamped: a smaller book would
// replay to a different state (ADR-009, point 3). The other three bound what
// the replay holds: the journal, the commands applied and the log written.
// The defaults are far above any journal the engine writes in a day; they
// are there so a hostile or runaway file ends in a Limit, not in the
// allocator.
struct ReplayLimits {
  Index max_pool_capacity = 1u << 20;
  std::uint64_t max_journal_bytes = 4ULL << 30;
  std::uint64_t max_commands = 200'000'000ULL;
  std::uint64_t max_event_log_bytes = 4ULL << 30;
};

struct Replayed {
  std::string log;
  std::string book;                   // the book after the last command that replayed
  std::size_t commands = 0;           // how many commands replayed
  std::size_t logged = 0;             // how many of them have their lines in log
  journal::Status stop = journal::Status::End;  // why reading stopped
  std::size_t valid_bytes = 0;        // where the good part of the journal ends
  Limit limit = Limit::None;          // set when a limit, not the journal, ended the replay
};

// Replays every command up to the first bad record or the first limit.
// Returns false when the header cannot be read or a limit is hit before the
// book is built, in which case nothing was replayed: out.stop says why for a
// bad header, out.limit for a limit. A damaged tail is not a failure:
// out.stop reports it and everything before it is replayed.
//
// A limit reached while replaying is not a failure either. out.limit names
// it, out.stop is Ok, and what was replayed is a prefix of what an unlimited
// run produces, byte for byte. The command limit stops before the command
// that would exceed it. The log limit stops after applying a command whose
// lines do not fit: commands, valid_bytes and book include that command,
// and logged and log do not, so logged is commands - 1.
//
// Throws std::bad_alloc if the log or the staging buffer cannot grow, and
// std::runtime_error if a log line cannot be formatted. A journal with a
// larger log than the bound is stopped before it grows that far, not caught
// after.
inline bool replay(const std::uint8_t* data, std::size_t size, Replayed& out,
                   const ReplayLimits& limits = ReplayLimits{}) {
  out = Replayed{};
  journal::Reader reader(data, size);
  out.stop = reader.open();
  if (out.stop != journal::Status::Ok) return false;

  if (size > limits.max_journal_bytes) {
    out.limit = Limit::JournalBytes;
    return false;
  }
  if (reader.pool_capacity() > limits.max_pool_capacity) {
    out.limit = Limit::PoolCapacity;
    return false;
  }

  std::optional<Book> book;
  Book::AddResult scratch;
  try {
    book.emplace(reader.pool_capacity());
    scratch.trades.reserve(reader.pool_capacity());
  } catch (const std::bad_alloc&) {
    out.limit = Limit::OutOfMemory;
    return false;
  }

  // One command's lines are built apart from the log and appended whole, so
  // the log never ends part way through a command.
  std::string staged;
  Command c;
  std::size_t good = reader.valid_bytes();
  while ((out.stop = reader.next(c)) == journal::Status::Ok) {
    if (out.commands >= limits.max_commands) {
      out.limit = Limit::Commands;
      break;
    }
    const Outcome o = apply(*book, c, scratch);
    ++out.commands;
    good = reader.valid_bytes();
    staged.clear();
    append_events(staged, out.commands - 1, c, o, scratch.trades);
    if (staged.size() > limits.max_event_log_bytes - out.log.size()) {
      out.limit = Limit::EventLogBytes;
      break;
    }
    out.log.append(staged);
    ++out.logged;
  }
  out.valid_bytes = out.limit == Limit::None ? reader.valid_bytes() : good;
  out.book = serialize_book(*book);
  return true;
}

}  // namespace engine::replay
