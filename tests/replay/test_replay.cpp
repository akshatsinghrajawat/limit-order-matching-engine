// Test 22: replaying a journal is deterministic, and a damaged journal
// replays exactly the commands before the damage.
//
// "Live" below means running the commands straight into a book and logging
// them with the same functions replay uses. It is the reference for what the
// log and the book should be after the first k commands.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "command_stream.hpp"
#include "engine/replay.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace engine;
using journal::Status;

namespace {

// Small enough that the pool fills and the full-pool path is in the log.
constexpr Index kCapacity = 16;

// The log and book after each prefix of the commands, run live.
struct Live {
  std::string log;
  std::vector<std::size_t> log_size;      // log_size[k]: bytes of log after k commands
  std::vector<std::string> book_after;    // book_after[k]: book after k commands
  std::size_t trades = 0;
};

Live run_live(const std::vector<Command>& cmds) {
  Live live;
  Book book(kCapacity);
  Book::AddResult scratch;
  live.log_size.push_back(0);
  live.book_after.push_back(replay::serialize_book(book));
  for (std::size_t i = 0; i < cmds.size(); ++i) {
    const replay::Outcome o = replay::apply(book, cmds[i], scratch);
    // ADR-009: a trade carries the seq_no of the command that made it, and a
    // command's seq_no is its place in the input.
    for (const Trade& t : scratch.trades) CHECK(t.seq_no == i);
    live.trades += scratch.trades.size();
    replay::append_events(live.log, i, cmds[i], o, scratch.trades);
    live.log_size.push_back(live.log.size());
    live.book_after.push_back(replay::serialize_book(book));
  }
  return live;
}

// Offsets at which each record ends, header included.
std::vector<std::size_t> record_ends(const std::vector<Command>& cmds) {
  std::vector<std::size_t> ends{journal::kHeaderSize};
  for (const Command& c : cmds) {
    std::uint8_t rec[journal::kMaxRecordSize];
    ends.push_back(ends.back() + journal::encode(c, rec));
  }
  return ends;
}

// The replay of a journal that holds only the first k commands intact must
// look like the live run stopped after k.
void check_prefix(const replay::Replayed& r, const Live& live, std::size_t k) {
  CHECK(r.commands == k);
  CHECK(r.log.size() == live.log_size[k]);
  CHECK(r.log == live.log.substr(0, live.log_size[k]));
  CHECK(r.book == live.book_after[k]);
}

}  // namespace

static void replay_twice_is_byte_identical() {
  const auto cmds = stream::make(1, 3000);
  const auto bytes = stream::journal_bytes(kCapacity, cmds);

  replay::Replayed a, b;
  CHECK(replay::replay(bytes.data(), bytes.size(), a));
  CHECK(replay::replay(bytes.data(), bytes.size(), b));
  CHECK(a.stop == Status::End && b.stop == Status::End);
  CHECK(a.commands == cmds.size());
  CHECK(a.valid_bytes == bytes.size());
  CHECK(a.log == b.log);
  CHECK(a.book == b.book);
}

static void replay_matches_the_live_run() {
  const auto cmds = stream::make(2, 3000);
  const auto bytes = stream::journal_bytes(kCapacity, cmds);
  const Live live = run_live(cmds);

  replay::Replayed r;
  CHECK(replay::replay(bytes.data(), bytes.size(), r));
  check_prefix(r, live, cmds.size());

  // The stream has to reach the paths the log is supposed to cover.
  CHECK(live.trades > 100);
  CHECK(live.log.find("stp=1") != std::string::npos);
  CHECK(live.log.find("full=1") != std::string::npos);
  CHECK(live.log.find("cancel id=") != std::string::npos);
  CHECK(live.log.find("modify id=") != std::string::npos);
  CHECK(live.log.find("ok=0") != std::string::npos);
}

// Cut the journal at every length from the header on. A cut inside a record
// leaves a torn tail: the records before it replay, and the book is exactly
// what the live run had after them.
static void every_truncation_replays_the_whole_records_before_it() {
  const auto cmds = stream::make(3, 150);
  const auto bytes = stream::journal_bytes(kCapacity, cmds);
  const auto ends = record_ends(cmds);
  const Live live = run_live(cmds);

  for (std::size_t len = 0; len <= bytes.size(); ++len) {
    replay::Replayed r;
    const bool opened = replay::replay(bytes.data(), len, r);
    if (len < journal::kHeaderSize) {
      CHECK(!opened && r.stop == Status::Truncated && r.commands == 0);
      continue;
    }
    CHECK(opened);
    std::size_t whole = 0;
    while (whole + 1 < ends.size() && ends[whole + 1] <= len) ++whole;
    check_prefix(r, live, whole);
    CHECK(r.valid_bytes == ends[whole]);
    CHECK(r.stop == (ends[whole] == len ? Status::End : Status::Truncated));
  }
}

// Flip every bit. The commands before the damaged record replay and the
// damaged one does not; a flip in the header replays nothing.
static void every_bit_flip_replays_the_records_before_it() {
  const auto cmds = stream::make(4, 40);
  const auto original = stream::journal_bytes(kCapacity, cmds);
  const auto ends = record_ends(cmds);
  const Live live = run_live(cmds);

  for (std::size_t byte = 0; byte < original.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto bytes = original;
      bytes[byte] ^= static_cast<std::uint8_t>(1u << bit);

      replay::Replayed r;
      const bool opened = replay::replay(bytes.data(), bytes.size(), r);
      if (byte < journal::kHeaderSize) {
        CHECK(!opened && r.commands == 0);
        continue;
      }
      CHECK(opened);
      std::size_t damaged = 0;
      while (ends[damaged + 1] <= byte) ++damaged;
      check_prefix(r, live, damaged);
      CHECK(r.valid_bytes == ends[damaged]);
      CHECK(r.stop != Status::Ok && r.stop != Status::End);
    }
  }
}

// The pool capacity in the header is what the replay builds its book with.
static void replay_uses_the_pool_capacity_in_the_header() {
  const auto cmds = stream::make(5, 500);
  const auto small = stream::journal_bytes(4, cmds);
  const auto large = stream::journal_bytes(4096, cmds);

  replay::Replayed a, b;
  CHECK(replay::replay(small.data(), small.size(), a));
  CHECK(replay::replay(large.data(), large.size(), b));
  CHECK(a.log.find("full=1") != std::string::npos);
  CHECK(b.log.find("full=1") == std::string::npos);
  CHECK(a.log != b.log);
}

int main() {
  replay_twice_is_byte_identical();
  replay_matches_the_live_run();
  every_truncation_replays_the_whole_records_before_it();
  every_bit_flip_replays_the_records_before_it();
  replay_uses_the_pool_capacity_in_the_header();
  std::puts("replay ok");
}
