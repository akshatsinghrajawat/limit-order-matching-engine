#pragma once

// The command journal: every command the core is given, in the order it was
// given, in a file that can be replayed. Format and reasons are in ADR-010.
//
// Integers are written little-endian by shifting bytes out one at a time, not
// by copying structs, so the bytes do not depend on the compiler or the
// machine. Each record carries its own CRC-32, which is how a torn or
// damaged tail is told apart from a good one.
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "command.hpp"

namespace engine::journal {

inline constexpr char kMagic[8] = {'L', 'O', 'M', 'E', 'J', 'R', 'N', 'L'};
inline constexpr std::uint32_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::size_t kMaxRecordSize = 31;

namespace detail {

constexpr std::array<std::uint32_t, 256> make_crc_table() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    t[i] = c;
  }
  return t;
}

inline constexpr std::array<std::uint32_t, 256> kCrcTable = make_crc_table();

inline void put(std::uint8_t* out, std::uint64_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) out[i] = static_cast<std::uint8_t>(v >> (8 * i));
}

inline std::uint64_t get(const std::uint8_t* in, int bytes) {
  std::uint64_t v = 0;
  for (int i = 0; i < bytes; ++i) v |= static_cast<std::uint64_t>(in[i]) << (8 * i);
  return v;
}

// Bytes after the kind byte and before the checksum, or 0 for a kind that
// does not exist.
inline std::size_t payload_size(std::uint8_t kind) {
  switch (kind) {
    case static_cast<std::uint8_t>(CommandKind::Limit): return 8 + 1 + 1 + 8 + 4 + 4;
    case static_cast<std::uint8_t>(CommandKind::Market): return 8 + 1 + 4 + 4;
    case static_cast<std::uint8_t>(CommandKind::Cancel): return 8;
    case static_cast<std::uint8_t>(CommandKind::Modify): return 8 + 8 + 4;
    default: return 0;
  }
}

}  // namespace detail

// Standard CRC-32 (the one in zlib and Ethernet).
inline std::uint32_t crc32(const std::uint8_t* data, std::size_t n) {
  std::uint32_t c = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < n; ++i) c = detail::kCrcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// Writes the 20-byte header. pool_capacity is recorded because replay has to
// build a book of the same size: a full pool changes what the book does.
inline void encode_header(Index pool_capacity, std::uint8_t out[kHeaderSize]) {
  for (int i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(kMagic[i]);
  detail::put(out + 8, kVersion, 4);
  detail::put(out + 12, pool_capacity, 4);
  detail::put(out + 16, crc32(out, 16), 4);
}

// Writes one record and returns its length, at most kMaxRecordSize.
inline std::size_t encode(const Command& c, std::uint8_t* out) {
  std::uint8_t* p = out;
  *p++ = static_cast<std::uint8_t>(c.kind);
  detail::put(p, c.id, 8);
  p += 8;
  switch (c.kind) {
    case CommandKind::Limit:
      *p++ = static_cast<std::uint8_t>(c.side);
      *p++ = static_cast<std::uint8_t>(c.tif);
      detail::put(p, static_cast<std::uint64_t>(c.price), 8);
      p += 8;
      detail::put(p, c.qty, 4);
      p += 4;
      detail::put(p, c.account, 4);
      p += 4;
      break;
    case CommandKind::Market:
      *p++ = static_cast<std::uint8_t>(c.side);
      detail::put(p, c.qty, 4);
      p += 4;
      detail::put(p, c.account, 4);
      p += 4;
      break;
    case CommandKind::Cancel:
      break;
    case CommandKind::Modify:
      detail::put(p, static_cast<std::uint64_t>(c.price), 8);
      p += 8;
      detail::put(p, c.qty, 4);
      p += 4;
      break;
  }
  detail::put(p, crc32(out, static_cast<std::size_t>(p - out)), 4);
  return static_cast<std::size_t>(p - out) + 4;
}

enum class Status {
  Ok,           // a record was read
  End,          // the data ended exactly after a record
  Truncated,    // the data ended inside a header or a record
  BadChecksum,  // the bytes are all there and the checksum does not match
  BadKind,      // the record's first byte is not a command kind
  BadField,     // the checksum matches but a field holds a value that cannot be
};

// Reads a journal held in memory. The data must outlive the reader.
//
// Call open() first and read records only if it returns Ok. After next()
// returns anything but Ok, valid_bytes() is where the good part of the
// journal ends. Everything before it replays; everything from
// it on is a damaged or torn tail.
class Reader {
 public:
  Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  Status open() {
    if (size_ < kHeaderSize) return Status::Truncated;
    for (int i = 0; i < 8; ++i) {
      if (data_[i] != static_cast<std::uint8_t>(kMagic[i])) return Status::BadField;
    }
    if (crc32(data_, 16) != detail::get(data_ + 16, 4)) return Status::BadChecksum;
    if (detail::get(data_ + 8, 4) != kVersion) return Status::BadField;
    capacity_ = static_cast<Index>(detail::get(data_ + 12, 4));
    pos_ = kHeaderSize;
    return Status::Ok;
  }

  Index pool_capacity() const { return capacity_; }
  std::size_t valid_bytes() const { return pos_; }

  Status next(Command& out) {
    if (pos_ == size_) return Status::End;

    const std::uint8_t* rec = data_ + pos_;
    const std::size_t body = detail::payload_size(rec[0]) + 1;
    if (body == 1) return Status::BadKind;
    if (size_ - pos_ < body + 4) return Status::Truncated;
    if (crc32(rec, body) != detail::get(rec + body, 4)) return Status::BadChecksum;

    Command c;
    c.kind = static_cast<CommandKind>(rec[0]);
    c.id = detail::get(rec + 1, 8);
    const std::uint8_t* p = rec + 9;
    switch (c.kind) {
      case CommandKind::Limit:
        if (p[0] > 1 || p[1] > 2) return Status::BadField;
        c.side = static_cast<Side>(p[0]);
        c.tif = static_cast<TimeInForce>(p[1]);
        c.price = static_cast<Price>(detail::get(p + 2, 8));
        c.qty = static_cast<Qty>(detail::get(p + 10, 4));
        c.account = static_cast<std::uint32_t>(detail::get(p + 14, 4));
        break;
      case CommandKind::Market:
        if (p[0] > 1) return Status::BadField;
        c.side = static_cast<Side>(p[0]);
        c.qty = static_cast<Qty>(detail::get(p + 1, 4));
        c.account = static_cast<std::uint32_t>(detail::get(p + 5, 4));
        break;
      case CommandKind::Cancel:
        break;
      case CommandKind::Modify:
        c.price = static_cast<Price>(detail::get(p, 8));
        c.qty = static_cast<Qty>(detail::get(p + 8, 4));
        break;
    }
    out = c;
    pos_ += body + 4;
    return Status::Ok;
  }

 private:
  const std::uint8_t* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
  Index capacity_ = 0;
};

// Appends records to a file. Each append is flushed to the operating system
// before it returns, so a command that was acknowledged is in the file even
// if the process is killed straight afterwards. It is not fsynced: a power
// cut or a kernel crash can still lose the tail (ADR-010).
class Writer {
 public:
  Writer() = default;
  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  ~Writer() { close(); }

  bool open(const char* path, Index pool_capacity) {
    close();
    failed_ = false;
    file_ = std::fopen(path, "wb");
    if (file_ == nullptr) return false;
    std::uint8_t header[kHeaderSize];
    encode_header(pool_capacity, header);
    return write(header, kHeaderSize);
  }

  // False if the record may not be fully written. Once an append has failed
  // every later one fails too: what reached the file is unknown, and a record
  // written after a gap would be a valid-looking record in the wrong place.
  bool append(const Command& c) {
    std::uint8_t rec[kMaxRecordSize];
    return write(rec, encode(c, rec));
  }

  void close() {
    if (file_ != nullptr) std::fclose(file_);
    file_ = nullptr;
  }

 private:
  bool write(const std::uint8_t* bytes, std::size_t n) {
    if (file_ == nullptr || failed_) return false;
    if (std::fwrite(bytes, 1, n, file_) != n || std::fflush(file_) != 0) {
      failed_ = true;
      return false;
    }
    return true;
  }

  std::FILE* file_ = nullptr;
  bool failed_ = false;
};

// Reads a whole file. False if it cannot be opened or read.
inline bool load(const char* path, std::vector<std::uint8_t>& out) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) return false;
  out.clear();
  std::uint8_t buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
  const bool ok = std::ferror(f) == 0;
  std::fclose(f);
  return ok;
}

}  // namespace engine::journal
