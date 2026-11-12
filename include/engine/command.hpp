#pragma once

// An input to the book, in the form the journal stores it. Which fields mean
// something depends on the kind; the others stay at their defaults, so two
// commands compare equal exactly when they would do the same thing.
#include <cstdint>

#include "types.hpp"

namespace engine {

enum class CommandKind : std::uint8_t { Limit = 1, Market = 2, Cancel = 3, Modify = 4 };

// Whether the journal has a record for this kind. journal::Reader accepts
// the same four values and no others (ADR-011).
inline bool valid(CommandKind k) {
  switch (k) {
    case CommandKind::Limit:
    case CommandKind::Market:
    case CommandKind::Cancel:
    case CommandKind::Modify:
      return true;
  }
  return false;
}

struct Command {
  CommandKind kind = CommandKind::Limit;
  OrderId id = 0;
  Side side = Side::Buy;                // Limit, Market
  TimeInForce tif = TimeInForce::GTC;   // Limit
  Price price = 0;                      // Limit, Modify
  Qty qty = 0;                          // Limit, Market, Modify
  std::uint32_t account = 0;            // Limit, Market

  friend bool operator==(const Command& a, const Command& b) {
    return a.kind == b.kind && a.id == b.id && a.side == b.side && a.tif == b.tif &&
           a.price == b.price && a.qty == b.qty && a.account == b.account;
  }
  friend bool operator!=(const Command& a, const Command& b) { return !(a == b); }
};

// Whether the journal can store the command and read it back unchanged
// (ADR-011). A limit or market order is checked on its side; a cancel and a
// modify have no side in their records, so whatever the field holds is never
// written and never read.
inline bool valid(const Command& c) {
  if (!valid(c.kind)) return false;
  if (c.kind == CommandKind::Limit && !valid(c.tif)) return false;
  if (c.kind == CommandKind::Limit || c.kind == CommandKind::Market) return valid(c.side);
  return true;
}

inline Command limit_command(OrderId id, Side side, Price price, Qty qty, std::uint32_t account,
                             TimeInForce tif = TimeInForce::GTC) {
  Command c;
  c.kind = CommandKind::Limit;
  c.id = id;
  c.side = side;
  c.tif = tif;
  c.price = price;
  c.qty = qty;
  c.account = account;
  return c;
}

inline Command market_command(OrderId id, Side side, Qty qty, std::uint32_t account) {
  Command c;
  c.kind = CommandKind::Market;
  c.id = id;
  c.side = side;
  c.qty = qty;
  c.account = account;
  return c;
}

inline Command cancel_command(OrderId id) {
  Command c;
  c.kind = CommandKind::Cancel;
  c.id = id;
  return c;
}

inline Command modify_command(OrderId id, Price price, Qty qty) {
  Command c;
  c.kind = CommandKind::Modify;
  c.id = id;
  c.price = price;
  c.qty = qty;
  return c;
}

}  // namespace engine
