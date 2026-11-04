# ADR-006: Event log and journal

## Decision

The event log is an append-only vector of POD event structs, reserved up
front (no reallocation churn in the hot path). Replay reads the **journal**
— the command log written by ingress — not the in-memory event vector.

## Rationale

There must be exactly one source of ordering and replay truth in the
system (S6). If replay were driven from the event vector instead of the
journal, there would be two representations of "what happened" that could
silently diverge after a future change — for example, a batching
optimization (O6, outbound event batching) that changes *when* events are
appended to the vector without changing the underlying command order. The
journal is the ground truth precisely because it is what ingress actually
received, in the order the core actually processed it (S8); the event
vector is a derived output, not a source of record.

## Status

Accepted at M0. Journal writer/reader and golden-replay CI (tests 22–23)
target M4.

## Replay (M4)

`include/engine/replay.hpp` replays a journal into a fresh book, built with
the pool capacity the journal's header records (ADR-010), and writes the
engine's observable output as text. It reads nothing but the journal.

**The log.** One line for each command, in journal order, with its outcome,
then one indented line for each trade it made:

```
#5 limit id=4 side=buy price=105 qty=1 account=1 tif=gtc ok=1 rest=0 stp=0 full=0
  trade seq=5 id=0 maker=3 taker=4 price=100 qty=1
```

`#5` is the command's position in the journal. The trade's `seq=5` is the
engine's own number for it, and the two being equal is ADR-009's rule that a
command's `seq_no` is its position; test 22 checks it on every trade. The
book is written the same way, bids best first and then asks, oldest first
within a price.

**Why text.** The log is what S7 says must be identical on every run, and a
diff of two text files shows where they part. A binary log would be
smaller and faster and would need a tool to read.

**A damaged journal.** Replay stops at the first record that is not good and
reports why. Everything before it is replayed, and the book after those
commands is exactly the book the original run had at that point. This is
what makes a journal torn by a kill usable.

**Tests.**
- Test 22 (`replay_deterministic`): the same journal replays to the same bytes
  twice and to the same bytes as running the commands live; cutting the
  journal at every length, and flipping every bit of it, replays exactly the
  whole records before the damage; the pool capacity in the header decides
  what the replay does.
- Test 23 (`golden_replay`): `tests/golden/journal.hex` replays to
  `events.txt` and `book.txt`. These are committed output. A change to
  matching, to the log or to the journal format makes the test fail with the
  first line that differs, and `test_golden --update` rewrites the files so
  the change appears as a diff in the commit. `.gitattributes` marks them
  `-text` so no checkout rewrites their line endings.

**What this does not establish.** The golden files record what the engine
did when they were generated, not what it should do. They catch change, not
error: the differential tests against the oracle are what check the engine is
right. And live runs and replays share `apply()`, so a wrong `apply()` agrees
with itself; only the golden files and the mutants for `replay.hpp` see it.
