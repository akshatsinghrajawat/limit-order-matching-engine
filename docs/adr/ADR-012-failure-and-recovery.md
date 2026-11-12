# ADR-012: Failure and recovery

## Decision

1. **The six entry points that change the book do not throw.**
   `add_limit_order` and `add_market_order` (each in its two forms),
   `modify_order` and `cancel_order` are `noexcept`. A command that stops
   partway has left earlier fills applied and later ones not, and nothing
   in the book can say which. If an exception did leave one of them, the
   process ends in `std::terminate`.
2. **Once the caller's result has room for the trades, they cannot fail.**
   The pool, the id map and both price-level arrays are sized at
   construction and never grow (ADR-001, ADR-003, ADR-005). The only
   storage a command can grow is `AddResult::trades`. A sweep makes at most
   one trade per resting order, and there are at most `pool capacity`
   resting orders, so `trades.reserve(pool capacity)` once at startup
   covers every command. `replay::replay` does this after constructing its
   book, and the loop in `tests/support/journal_loop.hpp` does it before the
   first command.
3. **A trade id is taken after its trade exists.** `cross` advances
   `next_trade_id_` on the line after `push_back`, so an id is never spent
   on a trade that was not stored.
4. **The journal is written first.** The loop appends the command and
   applies it only if `append` returned `Ok`. The book is never ahead of
   the journal, and a command the writer refused (ADR-011) is not applied
   either.
5. **Recovery is replay.** After a stop, replaying the journal (ADR-006)
   gives the state an uninterrupted run reaches after the last record in
   the file. A command that was written and not yet applied when the
   process ended is applied by the replay, in its place in the sequence,
   with the `seq_no` and trade ids it would have had.

## Rationale

**Why end the process.** The alternative is to undo a half-applied
command: put back the makers a sweep had already filled, the levels it had
already erased and the ids it had already freed. That needs an undo record
on every command, in the path the benchmark measures, to cover an event
that item 2 removes. The journal already holds everything a restart
needs, so stopping and replaying costs nothing on the normal path.

**Why a bound and not a check.** Reserving the worst case at startup turns
"can an allocation fail here" into a property of the sizes chosen, which a
test can state: with the result reserved, the entry points make no
allocation at all on the largest sweep the pool allows. A check at each
`push_back` would be a branch on the hot path with no recovery to go to.

**What this does not cover.**
- The by-value forms of `add_limit_order` and `add_market_order` build a
  new `AddResult`, so a sweep allocates for its trades. They exist for
  tests and examples. A caller that uses them accepts that a failed
  allocation ends the process.
- `Writer` flushes to the operating system and does not call `fsync`
  (ADR-010). A power loss can lose records the loop had applied. A process
  kill cannot.
- Nothing appends to a journal after recovery; a restarted process starts a
  new file (ADR-010).

## Tests

`unit_fail_stop` (`tests/unit/test_fail_stop.cpp`) replaces global
`operator new` so it can count and fail allocations.
- `static_assert(noexcept(...))` for each of the six entry points.
- With the result reserved, a limit and a market sweep of each side, the
  largest sweep the pool allows, a sweep stopped by a self-match, a
  fill-or-kill, an immediate-or-cancel, a modify and a cancel make no
  allocation, with allocation set to throw. Trade ids run consecutively
  from 0 across them.
- A control with an unreserved result shows the same injector does see the
  allocations that the reserve prevents.
- After each of the first 120 commands of a seeded stream, the journal the
  loop wrote replays to the same book and the same event log as the loop,
  including two trades that follow, which carry the next `seq_no` and
  trade ids.
- On POSIX, a forked child writes a command to the journal, then has the
  allocation for its first trade fail inside `apply`. It ends with
  `SIGABRT`, and the journal it left replays to the state of a run that
  was not interrupted.
