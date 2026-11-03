# ADR-010: Command journal format

## Decision

The journal is a file of the commands the core was given, in order: a
20-byte header, then one record per command, with nothing between them.
`include/engine/journal.hpp` writes and reads it.

**Header (20 bytes, all integers little-endian):**

| bytes | field |
| ----: | ----- |
| 0-7   | magic `LOMEJRNL` |
| 8-11  | format version, 1 |
| 12-15 | pool capacity the book was built with |
| 16-19 | CRC-32 of bytes 0-15 |

**Record:** a kind byte, a payload whose length is fixed by the kind, and a
CRC-32 of the kind byte and the payload.

| kind | command | payload | record |
| ---: | ------- | ------- | -----: |
| 1 | limit  | id u64, side u8, tif u8, price i64, qty u32, account u32 | 31 bytes |
| 2 | market | id u64, side u8, qty u32, account u32 | 22 |
| 3 | cancel | id u64 | 13 |
| 4 | modify | id u64, price i64, qty u32 | 25 |

There is no length field and no sequence number in a record. A command's
`seq_no` is its position in the file (ADR-009), so storing it would store the
same fact twice and give the two a way to disagree.

**Reading** stops at the first record that is not good and reports why:
`Truncated` (the file ends inside a record), `BadChecksum`, `BadKind` (the
first byte is not a command kind), or `BadField` (the checksum matches but a
side or time-in-force byte is out of range). `valid_bytes()` is where the good
part ends, so a caller can replay everything before it and treat the rest as a
damaged tail.

**Writing** appends a record and flushes it to the operating system before
returning. The caller is expected to append a command before running it and
to run it only if the append succeeded.

## Rationale

**Replay needs the pool capacity.** A full pool changes what a command does
(ADR-009, point 3), so a replay into a book of a different size can diverge
without any record being wrong. The header carries the size so a reader builds
the same book.

**Explicit little-endian, one byte at a time.** Copying the struct, or
`memcpy` of an integer, writes whatever the compiler and the machine lay out.
The golden file in the tests would then be a statement about one build.
Shifting bytes out makes the file the same everywhere, and
`tools/journal_golden.py` encodes the same commands without any engine code to
check that the layout is what this ADR says and not just what the encoder and
the reader agree on.

**A checksum per record, not per file.** The case that matters is a process
killed in the middle of a write. A file-level checksum would reject the whole
journal for one torn record at the end. A checksum on each record lets the
reader replay every complete record and stop exactly at the torn one.

**CRC-32, not a stronger hash.** The job is to notice accidental damage, a
torn write or a flipped bit, not to resist an attacker who can rewrite the
file and recompute the checksum. CRC-32 is guaranteed to catch any
single-bit error and any burst up to 32 bits, is a dozen lines, and is the one
every tool (`zlib`, `cksum`) can recompute.

**Fixed record sizes by kind.** A length field would be one more thing a
flipped bit can make wrong. With the kind deciding the length, a damaged kind
byte gives a wrong length, and the checksum over that span fails with
overwhelming probability. The test flips every bit of a journal and requires
that no damaged record ever reads back as a command.

**Flush every record, no fsync.** A flush hands the bytes to the kernel, which
is what survives `SIGKILL`: the process is gone and the page cache is not.
An `fsync` per command costs a disk round trip, orders of magnitude more than
the command, and buys survival of a power cut or a kernel crash, which this
project does not claim to handle.

## What this does not establish

- **Power loss.** Without `fsync` the tail can be lost, and on some
  filesystems a later block can reach the disk before an earlier one.
- **Deleted or reordered records.** Each record is checked alone. A whole
  record removed, or two swapped, still reads cleanly and replays a different
  history. A chain (each checksum covering the one before) would catch it and
  is not built.
- **Memory.** The reader takes the whole file in memory. A journal of 100
  million commands is about 3 GB.
- **Appending after recovery.** The writer always creates a new file.
  Reopening a journal, cutting it at `valid_bytes()` and carrying on is not
  built.
- **Other platforms.** The test that a full disk is reported uses
  `/dev/full` and is skipped where there is none.

## Status

Accepted. The replay driver and the golden files (tests 22 and 23) and the
kill-and-recover test come next and build on this format; ADR-006 will gain a
section when replay lands.
