# ADR-011: Which commands are valid

## Decision

1. **A command is valid when the journal can store it and the reader reads
   it back unchanged.** The values are the ones `journal::Reader` accepts:

   | field         | valid values                          |
   | ------------- | ------------------------------------- |
   | kind          | Limit 1, Market 2, Cancel 3, Modify 4 |
   | side          | Buy 0, Sell 1                         |
   | time in force | GTC 0, IOC 1, FOK 2                   |

   A limit order is checked on side and time in force, a market order on
   side, a cancel and a modify on neither: their records have no such
   bytes, so whatever those fields hold is never written and never read.
   Quantity, price, id and account have no range check here. A zero
   quantity, a duplicate id and a modify of an unknown id are valid
   commands that the book rejects (ADR-009).
2. **An invalid command is not a command.** It is not journaled, takes no
   `seq_no` and changes nothing. ADR-009 stands: every command takes the
   next number, and this says what counts as a command.
3. **The book refuses it before taking a number.** `add_limit_order` and
   `add_market_order` check the side, and a limit order its time in force,
   ahead of `next_seq_++`. On an invalid value they return the reset result
   (`accepted` false, no trades, nothing rested) and the sequence does not
   move.
4. **The writer refuses it before writing a byte.** `Writer::append`
   returns an `AppendStatus` instead of a `bool`:

   | status     | meaning                                                       | writer afterwards |
   | ---------- | ------------------------------------------------------------- | ----------------- |
   | `Ok`       | written and flushed to the operating system                   | usable            |
   | `Invalid`  | not a valid command; nothing written                          | unchanged         |
   | `IoError`  | write or flush failed; what reached the file is unknown       | unusable          |
   | `Unusable` | writer not open, or an earlier append failed; nothing written | unusable          |

   They are checked in that order: `Invalid`, then `Unusable`, then the
   write. A command that is not valid gets `Invalid` even from a writer
   that is closed or has already failed.
5. **Replay applies the same rule.** `replay::apply` refuses an invalid
   command the same way, leaving the book untouched and consuming nothing.
   A journal written through the writer never holds one, so this covers
   commands built in memory and applied directly.
6. **The reader is the authority.** `valid()` must agree with the reader's
   range checks. If they ever differ, `valid()` is the one that is wrong.

## Rationale

**Why validity follows the reader.** The journal is the replay authority
(ADR-006), and the reader stops at the first record it cannot read: a record
has no length field to resynchronise on (ADR-010). A record the writer
accepts and the reader refuses therefore costs every command after it.
Before this decision the writer accepted them. Kind 9 went out as a 13-byte
record, and side 2 or time in force 3 as a full record with a good checksum,
and `append` returned true. With one such command second in a journal of
four, every append succeeded and replay recovered one command; the two valid
commands acknowledged after the bad one were gone, and `stop` reported
`BadField` (`BadKind` for the kind).

**Why the book checks as well.** The writer covers commands that pass
through the journal. The book is also called directly, by the tests, the
differential harness and anything that does not journal. Before this
decision an out-of-range side took the sell branch and traded as a sell,
and time in force 3 failed the GTC test and behaved as IOC: accepted,
traded, never rested. Neither was a rule anyone chose.

**Why before `next_seq_++`.** ADR-009 makes `seq_no` a command's position
in the input, and ADR-010 makes that its position in the file. A command
that never reaches the file has no position. If the book took a number for
it, the live run would be one ahead of its own replay from then on: every
later trade and resting order would carry a different `seq_no` live than in
the replay, and S7's byte-identical log would fail with no damaged byte
anywhere. Rejections the journal can hold are different. A zero quantity or
a duplicate id is in the file, replay rejects it again, and both runs count
it.

**Why a status and not a `bool`.** `false` had two causes with opposite
remedies. A bad command is the sender's fault: refuse it and carry on. A
failed write leaves the end of the file unknown, so running the command
would act on something the journal may not hold (ADR-010 has the caller
append first and run only if the append succeeded), and the caller has to
stop running commands. With one `false` the
caller must treat both as the worse case, and then a single malformed
message stops the engine, or treat both as the milder one and carry on past
a failed disk.

**Why `Invalid` leaves the writer usable and comes first.** Nothing was
written, so the file is intact and the next valid command can be appended.
Marking the writer failed would let one malformed command end journaling
for every command after it. Putting `Invalid` ahead of the writer's state
makes "is this command acceptable" depend on the command alone: a bad
command is reported as bad whether or not the disk is healthy, and `IoError`
and `Unusable` always mean the journal, never the input.

**Alternatives.**

- *Clamp to a valid value.* Treating side 2 as a sell is what the book did by
  accident. It changes what the sender asked for and hides the error from
  them.
- *Let the reader accept any byte.* Every consumer would then have to say
  what side 2 means, and the only answer on offer is the accidental one.
- *Check only in the book.* The journal is written before the command runs,
  so the record is already in the file when the book refuses.
- *Leave it to the caller to check first.* The writer is where the record is
  made, so it is where one is refused. A caller that forgets would write a
  journal replay cannot read. `valid()` is public so a caller can still
  refuse earlier.

## What this does not establish

- The ranges say nothing about price, quantity, id or account. A negative
  price or a zero quantity is valid here. A zero quantity stays a book
  rejection that takes a number.
- An invalid command leaves no trace: not in the journal, not in the event
  log, not in the book. Only the caller's return value says it was refused,
  and `AddResult` has no flag for why, like the other rejections. A gateway
  that wants a record of refused input has to keep it itself.
- `IoError` and `Unusable` say the journal cannot be trusted. What the
  process does about that is a separate decision.
- Adding a value to `Side` or `TimeInForce` means changing the reader,
  `valid()` and the table above together, and the journal version
  (ADR-010), because an old reader refuses the new value.

## Status

Accepted. `spec.md` S6 points here. This record was written before the code
that enforces it.
