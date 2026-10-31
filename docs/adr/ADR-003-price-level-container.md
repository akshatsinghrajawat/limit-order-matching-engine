# ADR-003: Price level container

## Decision

Each side of the book keeps its levels in one array sorted from worst to
best price, so the best level is the last element (`PriceLevels`,
`include/engine/price_levels.hpp`). The array is allocated once, for the
pool capacity plus one levels, and never grows. This is candidate (b) of
the three this ADR listed; it replaced `std::map` (a).

It is an interim container. The planned replacement is candidate (c), a
page table, which needs a price band in the spec (see Status).

## Rationale

**Why it exists.** `std::map` allocates a node for every new price level.
After the id map stopped allocating (ADR-005), those nodes were all that
was left: 326,276 allocations per 1,000,000 W1 commands. With the array,
W1 makes none (`bench_allocs`), which is what the zero-allocation test
(test 25) needs.

**Why the best level is last.** Matching works at the best level, and a
sweep removes levels from that end. Reading the last element and popping it
shift nothing. W1 makes 167,173 such pops per million commands.

**What it costs.** A level created or emptied away from the touch moves
every element behind it. On W1 (seed 1, 1,000,000 measured commands, about
1,000 levels per side) that is:

| operation | count | levels moved, mean | bytes moved, mean | most |
| --------- | ----: | -----------------: | ----------------: | ---: |
| insert    | 326,276 | 304.6 | 4.8 KB | 1,167 |
| erase     | 159,155 | 622.5 | 9.7 KB | 1,165 |

About 3 KB moved per command on average. A cancel that empties a level in
the middle of a deep book is the expensive case.

## Measured

Unpinned sandbox, three runs alternating the `std::map` build (after O2)
with this one, same seed and workload. The absolute numbers say nothing
about your machine and the runs were noisy; the gap is far larger than the
noise.

| | `std::map` | sorted array |
| --- | ---: | ---: |
| throughput, ops/s | 4.5M to 6.9M | 1.1M to 1.8M |
| cancel p99 | 0.68 to 0.81 µs | 8.1 to 9.8 µs |
| cancel p99.9 | 1.3 to 4.4 µs | 34 to 38 µs |
| limit p50 | 176 to 217 ns | 190 to 239 ns |

Allocation is gone and the tail on cancels is roughly ten times worse. The
median limit order is about the same, so the cost sits in the shifts, not
in the search.

## What this does not establish

- Only W1, which is deep. A book with tens of levels moves far less per
  operation, and the array may well win there. How deep real books are is
  the question this number can't answer.
- The numbers above are from a shared sandbox. The pinned-core figures
  (`tools/run_baseline.sh`, against the committed baseline) are still to be
  taken, and these should be replaced by them.
- The page table is not built, so nothing here says it is faster.

## Status

(a) was accepted for M1. (b) was built and measured at M5: it removes the
allocations and regresses W1 tail latency by about an order of magnitude on
cancels, which is why it is recorded here as a measured candidate and not
as the answer. (c) avoids the shifting entirely by indexing levels by price
offset, and that needs a bounded price range: the book would take a price
band when it is built and reject orders outside it, the way an exchange
price collar does. That changes what the engine accepts, so it is a spec
decision (S1) and gets its own ADR. The array stays until then because the
zero-allocation test needs a container that does not allocate.
