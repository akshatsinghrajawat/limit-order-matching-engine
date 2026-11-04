// Test 23: the committed journal replays to the committed event log and
// book, byte for byte.
//
// The three files in tests/golden are the reference for what the engine
// does. A change to matching, to the log format or to the journal format
// shows up here as a diff, and the diff is the review: if the new output is
// what you meant, regenerate with
//
//     test_golden --update
//
// and commit the changed files with the code that changed them.
//
// The journal in the repository is replayed as it is; it is not regenerated
// from a seed, so a change to the random generator cannot move it.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "command_stream.hpp"
#include "engine/replay.hpp"

#ifndef GOLDEN_DIR
#error "GOLDEN_DIR must name tests/golden"
#endif

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace engine;

namespace {

// Chosen so the stream fills a pool of this size and reaches every path the
// log has a column for; main() checks that it did.
constexpr std::uint64_t kSeed = 7;
constexpr std::size_t kCommands = 400;
constexpr Index kCapacity = 16;

std::string path(const char* name) { return std::string(GOLDEN_DIR) + "/" + name; }

bool read_text(const char* name, std::string& out) {
  std::FILE* f = std::fopen(path(name).c_str(), "rb");
  if (f == nullptr) return false;
  out.clear();
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

bool write_text(const char* name, const std::string& text) {
  std::FILE* f = std::fopen(path(name).c_str(), "wb");
  if (f == nullptr) return false;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  return std::fclose(f) == 0 && ok;
}

// 16 bytes to a line, lowercase hex.
std::string to_hex(const std::vector<std::uint8_t>& bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    out.push_back(digits[bytes[i] >> 4]);
    out.push_back(digits[bytes[i] & 15]);
    if (i % 16 == 15 || i + 1 == bytes.size()) out.push_back('\n');
  }
  return out;
}

bool from_hex(const std::string& text, std::vector<std::uint8_t>& out) {
  out.clear();
  int high = -1;
  for (char ch : text) {
    if (ch == '\n') continue;
    int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1;
    if (v < 0) return false;
    if (high < 0) {
      high = v;
    } else {
      out.push_back(static_cast<std::uint8_t>(high * 16 + v));
      high = -1;
    }
  }
  return high < 0;
}

// Reports the first line where two texts differ, which is the part of a
// diff worth reading when a whole file changed.
void report_difference(const char* what, const std::string& want, const std::string& got) {
  std::size_t i = 0;
  while (i < want.size() && i < got.size() && want[i] == got[i]) ++i;
  std::size_t line_no = 1;
  for (std::size_t k = 0; k < i; ++k) line_no += want[k] == '\n';
  auto line_at = [&](const std::string& s) {
    std::size_t b = s.rfind('\n', i == 0 ? 0 : i - 1);
    b = (b == std::string::npos || i == 0) ? 0 : b + 1;
    std::size_t e = s.find('\n', i);
    return s.substr(b, e == std::string::npos ? std::string::npos : e - b);
  };
  std::fprintf(stderr, "golden %s differs at line %zu\n  committed: %s\n  replayed:  %s\n", what, line_no,
               line_at(want).c_str(), line_at(got).c_str());
  std::fprintf(stderr, "If the change is intended, run: test_golden --update\n");
}

bool same(const char* what, const std::string& want, const std::string& got) {
  if (want == got) return true;
  report_difference(what, want, got);
  return false;
}

struct Output {
  std::vector<std::uint8_t> journal;
  replay::Replayed replayed;
};

Output generate() {
  Output o;
  o.journal = stream::journal_bytes(kCapacity, stream::make(kSeed, kCommands));
  CHECK(replay::replay(o.journal.data(), o.journal.size(), o.replayed));
  return o;
}

// The stream is only worth keeping as a reference if it reaches these.
void check_coverage(const std::string& log) {
  for (const char* needle : {"limit id=", "market id=", "cancel id=", "modify id=", "tif=ioc", "tif=fok", "  trade seq=",
                              "stp=1", "full=1", "ok=0", "ok=1"}) {
    if (log.find(needle) == std::string::npos) {
      std::fprintf(stderr, "golden stream never reaches '%s'; pick another seed\n", needle);
      std::abort();
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--update") == 0) {
    const Output o = generate();
    check_coverage(o.replayed.log);
    CHECK(write_text("journal.hex", to_hex(o.journal)));
    CHECK(write_text("events.txt", o.replayed.log));
    CHECK(write_text("book.txt", o.replayed.book));
    std::printf("wrote %s: %zu commands, %zu journal bytes\n", GOLDEN_DIR, o.replayed.commands, o.journal.size());
    return 0;
  }

  std::string hex, events, book;
  CHECK(read_text("journal.hex", hex));
  CHECK(read_text("events.txt", events));
  CHECK(read_text("book.txt", book));

  std::vector<std::uint8_t> bytes;
  CHECK(from_hex(hex, bytes));

  replay::Replayed r;
  CHECK(replay::replay(bytes.data(), bytes.size(), r));
  CHECK(r.stop == journal::Status::End);
  CHECK(r.valid_bytes == bytes.size());
  check_coverage(events);

  bool ok = same("event log", events, r.log);
  ok = same("book", book, r.book) && ok;

  // The committed journal must also be what the encoder writes for the
  // commands in it, or the format moved without a version bump.
  journal::Reader reader(bytes.data(), bytes.size());
  CHECK(reader.open() == journal::Status::Ok);
  std::vector<Command> cmds;
  Command c;
  while (reader.next(c) == journal::Status::Ok) cmds.push_back(c);
  ok = same("journal bytes", hex, to_hex(stream::journal_bytes(reader.pool_capacity(), cmds))) && ok;

  if (!ok) return 1;
  std::puts("golden ok");
}
