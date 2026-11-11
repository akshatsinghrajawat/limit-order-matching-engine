// The journal's encoder, reader and writer.
//
// The golden bytes come from tools/journal_golden.py, which encodes the same
// commands without any engine code, so they check the layout rather than
// just that the encoder and reader agree with each other. The truncation and
// bit-flip tests run over every position of the file.
//
// CHECK instead of assert so a Release build (-DNDEBUG) still verifies.
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#if __has_include(<sys/resource.h>)
#include <sys/resource.h>
#endif

#include "engine/journal.hpp"

#define CHECK(c)                                                                 \
  do {                                                                           \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
      std::abort();                                                              \
    }                                                                            \
  } while (0)

using namespace engine;
using journal::AppendStatus;
using journal::Status;

namespace {

constexpr Index kCapacity = 65536;

// Same commands, same order as tools/journal_golden.py.
std::vector<Command> golden_commands() {
  return {
      limit_command(1, Side::Buy, 100, 10, 1),
      limit_command(2, Side::Sell, 101, 5, 2, TimeInForce::IOC),
      limit_command(3, Side::Buy, -5, std::numeric_limits<Qty>::max(), 7, TimeInForce::FOK),
      market_command(4, Side::Sell, 3, 9),
      cancel_command(std::numeric_limits<OrderId>::max()),
      modify_command(1, std::numeric_limits<Price>::min(), 1),
  };
}

const std::uint8_t kGolden[] = {
    0x4c, 0x4f, 0x4d, 0x45, 0x4a, 0x52, 0x4e, 0x4c, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x3e, 0xbe, 0x97, 0x3a, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x64, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x0a, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc8,
    0x55, 0xe7, 0x57, 0x01, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x01, 0x65, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00,
    0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x41, 0xe9, 0xb9, 0xa8, 0x01, 0x03,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xfb, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x07, 0x00, 0x00,
    0x00, 0x5f, 0xed, 0xc8, 0x16, 0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x2b,
    0xdf, 0x3b, 0xf5, 0x03, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0x1e, 0x28, 0xe2, 0x9b, 0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00,
    0x00, 0x81, 0x2d, 0xc0, 0x3f,
};

std::vector<std::uint8_t> encode_file(Index capacity, const std::vector<Command>& cmds) {
  std::vector<std::uint8_t> out(journal::kHeaderSize);
  journal::encode_header(capacity, out.data());
  for (const Command& c : cmds) {
    std::uint8_t rec[journal::kMaxRecordSize];
    const std::size_t n = journal::encode(c, rec);
    out.insert(out.end(), rec, rec + n);
  }
  return out;
}

// Offsets at which a record ends, header included, so a position can be
// classified as on a boundary or inside a record.
std::vector<std::size_t> boundaries(const std::vector<Command>& cmds) {
  std::vector<std::size_t> b{journal::kHeaderSize};
  for (const Command& c : cmds) {
    std::uint8_t rec[journal::kMaxRecordSize];
    b.push_back(b.back() + journal::encode(c, rec));
  }
  return b;
}

// Recomputes the checksum after the bytes of a record (or the header) were
// edited on purpose, so the edit is the only thing wrong with it.
void reseal(std::vector<std::uint8_t>& b, std::size_t offset, std::size_t body) {
  const std::uint32_t crc = journal::crc32(b.data() + offset, body);
  for (int i = 0; i < 4; ++i) b[offset + body + i] = static_cast<std::uint8_t>(crc >> (8 * i));
}

// Reads until the first status that is not Ok.
Status read_all(const std::uint8_t* data, std::size_t size, std::vector<Command>& out,
                std::size_t& valid) {
  out.clear();
  journal::Reader r(data, size);
  Status s = r.open();
  valid = r.valid_bytes();
  if (s != Status::Ok) return s;
  Command c;
  while ((s = r.next(c)) == Status::Ok) {
    out.push_back(c);
    CHECK(out.size() <= 1000);  // a reader that stops advancing must fail, not run away
  }
  valid = r.valid_bytes();
  return s;
}

}  // namespace

static void layout_matches_the_independent_encoder() {
  const std::vector<std::uint8_t> got = encode_file(kCapacity, golden_commands());
  CHECK(got.size() == sizeof kGolden);
  for (std::size_t i = 0; i < got.size(); ++i) CHECK(got[i] == kGolden[i]);
}

static void crc32_known_answer() {
  // The standard check value for CRC-32 over "123456789".
  const std::uint8_t digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(journal::crc32(digits, sizeof digits) == 0xCBF43926u);
  CHECK(journal::crc32(digits, 0) == 0);
}

static void golden_file_reads_back() {
  std::vector<Command> got;
  std::size_t valid = 0;
  CHECK(read_all(kGolden, sizeof kGolden, got, valid) == Status::End);
  CHECK(got == golden_commands());
  CHECK(valid == sizeof kGolden);

  journal::Reader r(kGolden, sizeof kGolden);
  CHECK(r.open() == Status::Ok);
  CHECK(r.pool_capacity() == kCapacity);
}

static void unused_fields_read_back_as_defaults() {
  const std::vector<Command> cmds = {cancel_command(5), modify_command(5, 7, 8), market_command(6, Side::Sell, 2, 3)};
  const auto bytes = encode_file(0, cmds);
  std::vector<Command> got;
  std::size_t valid;
  CHECK(read_all(bytes.data(), bytes.size(), got, valid) == Status::End);
  CHECK(got == cmds);
  CHECK(got[0].side == Side::Buy && got[0].qty == 0 && got[0].price == 0 && got[0].account == 0);
  CHECK(got[1].tif == TimeInForce::GTC && got[1].account == 0);
}

static void empty_journal_has_no_records() {
  const auto bytes = encode_file(8, {});
  CHECK(bytes.size() == journal::kHeaderSize);
  journal::Reader r(bytes.data(), bytes.size());
  CHECK(r.open() == Status::Ok);
  CHECK(r.pool_capacity() == 8);
  Command c;
  CHECK(r.next(c) == Status::End);
  CHECK(r.valid_bytes() == journal::kHeaderSize);
}

// Cut the file at every length. A cut on a boundary reads cleanly up to
// there; a cut inside a record reads the records before it and then reports
// Truncated, and either way valid_bytes() is the last boundary at or before
// the cut.
static void every_truncation() {
  const auto cmds = golden_commands();
  const auto bytes = encode_file(kCapacity, cmds);
  const auto bound = boundaries(cmds);

  for (std::size_t len = 0; len <= bytes.size(); ++len) {
    std::vector<Command> got;
    std::size_t valid = 0;
    const Status s = read_all(bytes.data(), len, got, valid);

    if (len < journal::kHeaderSize) {
      CHECK(s == Status::Truncated);
      CHECK(valid == 0 && got.empty());
      continue;
    }
    std::size_t whole = 0;
    while (whole + 1 < bound.size() && bound[whole + 1] <= len) ++whole;
    CHECK(valid == bound[whole]);
    CHECK(got.size() == whole);
    for (std::size_t i = 0; i < whole; ++i) CHECK(got[i] == cmds[i]);
    CHECK(s == (bound[whole] == len ? Status::End : Status::Truncated));
  }
}

// Flip every bit of the file, one at a time. The header must refuse to open.
// In a record, every record before it must still read back unchanged, and
// the damaged one must not come back as a command: it is an error, and
// valid_bytes() stops at the boundary before it.
static void every_bit_flip() {
  const auto cmds = golden_commands();
  const auto original = encode_file(kCapacity, cmds);
  const auto bound = boundaries(cmds);

  for (std::size_t byte = 0; byte < original.size(); ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto bytes = original;
      bytes[byte] ^= static_cast<std::uint8_t>(1u << bit);

      std::vector<Command> got;
      std::size_t valid = 0;
      const Status s = read_all(bytes.data(), bytes.size(), got, valid);

      if (byte < journal::kHeaderSize) {
        CHECK(s == Status::BadChecksum || s == Status::BadField);
        CHECK(got.empty());
        continue;
      }
      std::size_t damaged = 0;
      while (bound[damaged + 1] <= byte) ++damaged;
      CHECK(s != Status::Ok && s != Status::End);
      CHECK(got.size() == damaged);
      for (std::size_t i = 0; i < damaged; ++i) CHECK(got[i] == cmds[i]);
      CHECK(valid == bound[damaged]);
    }
  }
}

static void each_status_is_reachable() {
  const auto base = encode_file(kCapacity, {cancel_command(1)});
  Command c;

  {  // a kind byte that is not a command
    auto b = base;
    b[journal::kHeaderSize] = 0;
    journal::Reader r(b.data(), b.size());
    CHECK(r.open() == Status::Ok);
    CHECK(r.next(c) == Status::BadKind);
    b[journal::kHeaderSize] = 5;
    journal::Reader r2(b.data(), b.size());
    CHECK(r2.open() == Status::Ok);
    CHECK(r2.next(c) == Status::BadKind);
  }
  {  // a field that cannot be valid, under a checksum that matches
    auto b = encode_file(kCapacity, {limit_command(1, Side::Buy, 1, 1, 1)});
    const std::size_t body = b.size() - journal::kHeaderSize - 4;

    b[journal::kHeaderSize + 9] = 2;  // side
    reseal(b, journal::kHeaderSize, body);
    journal::Reader r(b.data(), b.size());
    CHECK(r.open() == Status::Ok);
    CHECK(r.next(c) == Status::BadField);
    CHECK(r.valid_bytes() == journal::kHeaderSize);

    b[journal::kHeaderSize + 9] = 0;
    b[journal::kHeaderSize + 10] = 3;  // time in force
    reseal(b, journal::kHeaderSize, body);
    journal::Reader r2(b.data(), b.size());
    CHECK(r2.open() == Status::Ok);
    CHECK(r2.next(c) == Status::BadField);
  }
  {  // the same for a market order, which has a side and no time in force
    auto b = encode_file(kCapacity, {market_command(1, Side::Buy, 1, 1)});
    b[journal::kHeaderSize + 9] = 2;
    reseal(b, journal::kHeaderSize, b.size() - journal::kHeaderSize - 4);
    journal::Reader r(b.data(), b.size());
    CHECK(r.open() == Status::Ok);
    CHECK(r.next(c) == Status::BadField);
  }
  {  // a header that is not this format, or not this version, under a good checksum
    auto b = base;
    b[0] = 'X';
    reseal(b, 0, 16);
    journal::Reader r(b.data(), b.size());
    CHECK(r.open() == Status::BadField);

    b = base;
    b[8] = 2;
    reseal(b, 0, 16);
    journal::Reader r2(b.data(), b.size());
    CHECK(r2.open() == Status::BadField);
  }
}

static void writer_produces_the_same_bytes() {
  const char* path = "journal_writer_test.bin";
  const auto cmds = golden_commands();
  {
    journal::Writer w;
    CHECK(w.open(path, kCapacity));
    for (const Command& c : cmds) CHECK(w.append(c) == AppendStatus::Ok);
  }
  std::vector<std::uint8_t> bytes;
  CHECK(journal::load(path, bytes));
  CHECK(bytes.size() == sizeof kGolden);
  for (std::size_t i = 0; i < bytes.size(); ++i) CHECK(bytes[i] == kGolden[i]);
  std::remove(path);
}

// Each append reaches the file before it returns, not when the writer is
// closed: this is what lets a killed process leave a replayable journal.
static void writer_flushes_every_record() {
  const char* path = "journal_flush_test.bin";
  journal::Writer w;
  CHECK(w.open(path, 4));
  std::vector<std::uint8_t> bytes;
  CHECK(journal::load(path, bytes) && bytes.size() == journal::kHeaderSize);

  std::size_t expect = journal::kHeaderSize;
  for (const Command& c : golden_commands()) {
    std::uint8_t rec[journal::kMaxRecordSize];
    expect += journal::encode(c, rec);
    CHECK(w.append(c) == AppendStatus::Ok);
    CHECK(journal::load(path, bytes) && bytes.size() == expect);
  }
  w.close();
  std::remove(path);
}

static void writer_and_load_report_failure() {
  journal::Writer w;
  CHECK(!w.open("no_such_directory/journal.bin", 4));
  CHECK(w.append(cancel_command(1)) == AppendStatus::Unusable);

  std::vector<std::uint8_t> bytes;
  CHECK(!journal::load("no_such_directory/journal.bin", bytes));
}

// A write the operating system refuses has to be reported, from the header
// or from a record. /dev/full accepts the open and fails every flush, which
// fails the header, so open returns false and every append after it is
// Unusable.
static void writer_reports_a_full_disk() {
  std::FILE* probe = std::fopen("/dev/full", "wb");
  if (probe == nullptr) return;  // not Linux: nothing to test against
  std::fclose(probe);

  journal::Writer w;
  CHECK(!w.open("/dev/full", 4));
  CHECK(w.append(cancel_command(1)) == AppendStatus::Unusable);
}

static Command with_kind(std::uint8_t kind) {
  Command c;
  c.kind = static_cast<CommandKind>(kind);
  c.id = 42;
  return c;
}

static void every_command_kind_is_valid_and_round_trips() {
  for (const Command& c : golden_commands()) CHECK(valid(c));
  CHECK(valid(CommandKind::Limit) && valid(CommandKind::Market));
  CHECK(valid(CommandKind::Cancel) && valid(CommandKind::Modify));

  std::vector<Command> back;
  std::size_t valid_bytes = 0;
  const auto bytes = encode_file(kCapacity, golden_commands());
  CHECK(read_all(bytes.data(), bytes.size(), back, valid_bytes) == Status::End);
  CHECK(back == golden_commands());
}

// Nothing is written for a command the reader would refuse, and the buffer
// the caller passed in is left as it was.
static void invalid_kinds_are_not_encoded() {
  for (const std::uint8_t kind : {0, 5, 9, 255}) {
    const Command c = with_kind(kind);
    CHECK(!valid(c) && !valid(c.kind));
    std::uint8_t rec[journal::kMaxRecordSize];
    std::memset(rec, 0xAA, sizeof rec);
    CHECK(journal::encode(c, rec) == 0);
    for (const std::uint8_t byte : rec) CHECK(byte == 0xAA);
  }
}

// The validity check and the reader are two statements of one rule. For every
// possible first byte of a record, the reader calls it a bad kind exactly
// when valid() says the kind does not exist.
static void kind_validity_agrees_with_the_reader() {
  int valid_kinds = 0;
  for (int b = 0; b < 256; ++b) {
    const auto kind = static_cast<CommandKind>(b);
    std::vector<std::uint8_t> bytes(journal::kHeaderSize);
    journal::encode_header(kCapacity, bytes.data());
    if (valid(kind)) {
      Command c;
      c.kind = kind;
      std::uint8_t rec[journal::kMaxRecordSize];
      const std::size_t n = journal::encode(c, rec);
      CHECK(n > 0);
      bytes.insert(bytes.end(), rec, rec + n);
      ++valid_kinds;
    } else {
      bytes.push_back(static_cast<std::uint8_t>(b));
      bytes.insert(bytes.end(), 12, 0);
    }
    journal::Reader r(bytes.data(), bytes.size());
    CHECK(r.open() == Status::Ok);
    Command out;
    const Status s = r.next(out);
    CHECK((s == Status::BadKind) == !valid(kind));
    if (valid(kind)) CHECK(s == Status::Ok && out.kind == kind);
  }
  CHECK(valid_kinds == 4);
}

// A refused command leaves no trace: the file is byte for byte what it would
// have been without it, and the writer carries on.
static void an_invalid_command_leaves_the_file_alone() {
  const char* with = "journal_invalid_with.bin";
  const char* without = "journal_invalid_without.bin";
  {
    journal::Writer a, b;
    CHECK(a.open(with, kCapacity) && b.open(without, kCapacity));
    CHECK(a.append(limit_command(1, Side::Buy, 100, 5, 1)) == AppendStatus::Ok);
    CHECK(b.append(limit_command(1, Side::Buy, 100, 5, 1)) == AppendStatus::Ok);
    for (const std::uint8_t kind : {0, 5, 9, 255}) {
      CHECK(a.append(with_kind(kind)) == AppendStatus::Invalid);
    }
    CHECK(a.append(cancel_command(1)) == AppendStatus::Ok);
    CHECK(b.append(cancel_command(1)) == AppendStatus::Ok);
  }
  std::vector<std::uint8_t> x, y;
  CHECK(journal::load(with, x) && journal::load(without, y));
  CHECK(x == y);
  std::remove(with);
  std::remove(without);
}

// A writer that is not open cannot write anything, but whether a command is
// acceptable does not depend on that, so a bad one is still called bad.
static void a_writer_that_is_not_open_is_unusable_but_reports_invalid() {
  journal::Writer w;
  CHECK(w.append(cancel_command(1)) == AppendStatus::Unusable);
  CHECK(w.append(with_kind(9)) == AppendStatus::Invalid);
}

// open succeeds, then the file size limit is lowered to leave room for the
// header and a part of the first record: the write fails after the open
// worked, which /dev/full cannot do.
static void a_failed_write_is_an_io_error_and_then_the_writer_is_unusable() {
#if defined(RLIMIT_FSIZE) && defined(SIGXFSZ)
  const char* path = "journal_fsize_test.bin";
  auto old_handler = std::signal(SIGXFSZ, SIG_IGN);
  rlimit old_limit{};
  CHECK(getrlimit(RLIMIT_FSIZE, &old_limit) == 0);
  rlimit limit = old_limit;
  limit.rlim_cur = journal::kHeaderSize + 10;
  if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
    std::signal(SIGXFSZ, old_handler);
    return;
  }

  journal::Writer w;
  const bool opened = w.open(path, kCapacity);
  const AppendStatus first = w.append(cancel_command(1));
  const AppendStatus second = w.append(cancel_command(2));
  const AppendStatus bad = w.append(with_kind(9));
  w.close();

  setrlimit(RLIMIT_FSIZE, &old_limit);
  std::signal(SIGXFSZ, old_handler);
  std::remove(path);

  CHECK(opened);
  CHECK(first == AppendStatus::IoError);
  CHECK(second == AppendStatus::Unusable);
  CHECK(bad == AppendStatus::Invalid);
#endif
}

int main() {
  layout_matches_the_independent_encoder();
  crc32_known_answer();
  golden_file_reads_back();
  unused_fields_read_back_as_defaults();
  empty_journal_has_no_records();
  every_truncation();
  every_bit_flip();
  each_status_is_reachable();
  writer_produces_the_same_bytes();
  writer_flushes_every_record();
  writer_and_load_report_failure();
  writer_reports_a_full_disk();
  every_command_kind_is_valid_and_round_trips();
  invalid_kinds_are_not_encoded();
  kind_validity_agrees_with_the_reader();
  an_invalid_command_leaves_the_file_alone();
  a_writer_that_is_not_open_is_unusable_but_reports_invalid();
  a_failed_write_is_an_io_error_and_then_the_writer_is_unusable();
  std::puts("journal ok");
}
