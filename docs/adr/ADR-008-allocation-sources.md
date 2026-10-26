# ADR-008: Where the allocations come from, and the reusable AddResult (O1)

## Decision

`Book::add_limit_order` and `Book::add_market_order` have overloads that
take the `AddResult` to fill as their first argument. They clear it and
write into it, and `trades` keeps its capacity from one command to the
next. The existing forms build a fresh result, call the overload and return
it, so there is one implementation of the matching path. The benchmark
runner, the allocation profile and the differential harness keep one result
for a whole run.

## Rationale

W1, seed 1, 1,000,000 measured commands after 100,000 of warmup, counting
calls to the global `operator new` (`bench_allocs`). The engine before this
change made 1,205,713. Wrapping the three call sites that allocate, in a
scratch copy of `book.hpp` that was not committed, split them:

| source                                  | allocations | share |
| --------------------------------------- | ----------: | ----: |
| `trades.push_back` in `cross()`         |     374,198 |   31% |
| `id_map_[...] = h` in `rest()`          |     505,239 |   42% |
| `level_for_insert()` in `rest()`        |     326,276 |   27% |
| everything else                         |           0 |    0% |

The three add up to the total exactly. The `id_map_` figure is the node for
each new entry and any rehash that came with it. Cancels allocate nothing.

`trades` goes first because it is the one source that needs no new data
structure. The vector is the caller's to keep, so reusing it removes every
allocation it was making once its capacity covers the largest sweep. The
other two need a container replacement each (ADR-005 for `id_map_`, ADR-003
for the price levels), with measurements of the replacements against what
they replace.

After this change the same run makes 831,515, which is the second and third
rows above, so nothing else moved. No command makes more than two
allocations, where one used to make six.

## What this does not establish

- The count is for libstdc++ as shipped with GCC 13.3. A different standard
  library grows vectors and allocates map and hash nodes differently, so
  recount with `bench_allocs` before reusing the table.
- It removes the allocation only for a caller that reuses its result. The
  by-value forms still allocate on every command that trades, so nothing on
  a hot path should call them.
- A command that trades more than any before it grows the vector once. That
  happens during warmup. A caller that needs it never to happen can reserve
  `result.trades` up front.
- The baseline files in `bench/results/` were produced by a runner that
  called the by-value form. From this commit on the runner reuses a result,
  so a run after it differs from the baseline in the harness as well as in
  the engine.
- The engine still allocates on 1.6 of every 3 limit orders that rest. Test
  25, which asserts zero, needs ADR-003's and ADR-005's replacements first.
