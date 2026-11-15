# ADR-007: Differential testing against a reference oracle

## Decision

`tests/support/oracle.hpp` is a second implementation of the spec, written
to be obviously correct rather than fast: one flat vector of resting
orders, every question answered by a linear scan. It shares only the type
aliases in `engine/types.hpp` with the engine. Three tests run the same
commands through both and compare every result:

- Test 20, `fuzz_diff`: seeded random command sequences. After each
  command it compares the returned trades and flags, and every `stride`
  commands it compares the full book snapshot and runs
  `Book::invariant_violation()`.
- Test 21, `exhaustive_diff`: every command sequence up to a fixed depth
  over a small alphabet, in three passes (structure, full command mix,
  pool capacity).
- `tools/mutation_check.sh`: applies one small bug at a time to
  `book.hpp` and requires the suite to fail on each.

## Rationale

The conformance tests check cases someone thought of. The oracle checks
cases nobody did, and being a flat scan it has no pool, handles or lists to
share a bug with the engine. A divergence means one of the two misreads the
spec, and the spec decides which.

The snapshot diff only follows `next_idx`, so it cannot see a wrong
`prev_idx`, a stale level tail, a leaked pool slot or a dangling `id_map_`
entry. `invariant_violation()` walks those directly, which is why the
fuzzer runs both.

The mutation check measures the tests, not the engine. In the first run,
5 of 19 mutants were caught only by the differential tests and not by
`test_basic`: strict crossing on the sell side, equal-qty modify losing
priority, modify not advancing `seq_no`, a filled order left in `id_map_`,
and a failed `rest()` leaking an empty level.

## What this does not establish

The engine and the oracle have the same author, so a misreading of the
spec made in both would go unnoticed. The two points the spec first left
open, what `Trade::seq_no` holds and whether `modify_order` can cross the
book, are settled in ADR-009 and the oracle follows the spec text. That
settles which behavior is intended, not that it is right.

The mutants are a hand-picked list, not a systematic operator set. All
killed means these 19 bugs are caught, not that no bug survives. Builds
are Debug because `test_basic.cpp` uses `assert()`.

## Status

Accepted at M2.

## Addendum: the mutant list after the hardening series

The list this ADR describes had 19 mutants in `book.hpp`. It was 84 across
`book.hpp`, `id_map.hpp`, `price_levels.hpp`, `journal.hpp` and `replay.hpp`
before the hardening series, and is 163 across nine headers now:

| header             | mutants |
| ------------------ | ------: |
| `book.hpp`         |      37 |
| `id_map.hpp`       |      13 |
| `pool.hpp`         |       5 |
| `invariants.hpp`   |      14 |
| `price_levels.hpp` |      14 |
| `journal.hpp`      |      28 |
| `types.hpp`        |       4 |
| `command.hpp`      |       8 |
| `replay.hpp`       |      40 |

What changed in how the check works:

- A mutant is a literal replacement of the Nth occurrence of a string in one
  header. A pattern that no longer occurs, because an edit near it changed
  the text, is reported as `PATTERN NOT FOUND` and the check exits 2, so a
  mutant cannot be lost silently. After editing a header, run the check
  with the test run stubbed out to see that every pattern still resolves.
- `test_basic.cpp` uses `CHECK` and no longer depends on `assert()`. The
  builds are still Debug, so that the `assert()` calls in the engine
  headers are enabled.
- Some mutants cannot change any output. Removing the FOK pre-check's early
  exit returns the same total, only after walking further. Those are killed
  by a test that places corrupt memory where only the longer walk reads it,
  and relies on ASan. A mutant that survives is either a gap in the tests or
  one of these, and has to be looked at either way.
- A full run takes about 45 seconds per mutant on the machine it was timed
  on, about two hours for 163, so the CI job's limit is 240 minutes.
