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
spec made in both would go unnoticed. Two points are not settled by the
spec and the oracle follows the engine on them: what `Trade::seq_no` holds,
and that `modify_order` never crosses the book, so a crossed book is a
legal state. Both are called out in `oracle.hpp` and should be settled in
`spec.md` before M4 fixes them into the golden files.

The mutants are a hand-picked list, not a systematic operator set. All
killed means these 19 bugs are caught, not that no bug survives. Builds
are Debug because `test_basic.cpp` uses `assert()`.

## Status

Accepted at M2.
