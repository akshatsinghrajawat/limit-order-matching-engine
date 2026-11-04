#pragma once

// A seeded stream of journalable commands, from the differential tests'
// generator, and the bytes of a journal holding them.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "diff.hpp"
#include "engine/command.hpp"
#include "engine/journal.hpp"

namespace stream {

inline engine::Command to_command(const diff::Op& op) {
  switch (op.kind) {
    case diff::Kind::Limit: return engine::limit_command(op.id, op.side, op.price, op.qty, op.account, op.tif);
    case diff::Kind::Market: return engine::market_command(op.id, op.side, op.qty, op.account);
    case diff::Kind::Cancel: return engine::cancel_command(op.id);
    case diff::Kind::Modify: return engine::modify_command(op.id, op.price, op.qty);
  }
  return {};
}

inline std::vector<engine::Command> make(std::uint64_t seed, std::size_t n) {
  diff::Generator gen(seed);
  std::vector<engine::Command> out;
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) out.push_back(to_command(gen.next()));
  return out;
}

inline std::vector<std::uint8_t> journal_bytes(engine::Index capacity, const std::vector<engine::Command>& cmds) {
  std::vector<std::uint8_t> out(engine::journal::kHeaderSize);
  engine::journal::encode_header(capacity, out.data());
  for (const engine::Command& c : cmds) {
    std::uint8_t rec[engine::journal::kMaxRecordSize];
    const std::size_t n = engine::journal::encode(c, rec);
    out.insert(out.end(), rec, rec + n);
  }
  return out;
}

}  // namespace stream
