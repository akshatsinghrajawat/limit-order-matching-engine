# ADR-003: Price level container

## Decision

Each side of the book keeps its levels in one array sorted from worst to
best price, so the best level is the last element (`PriceLevels`,
`include/engine/price_levels.hpp`). The array is allocated once, for the
pool capacity plus one levels, and never grows. This is candidate (b) of
the three this ADR listed; it replaced `std::map` (a).

Candidate (c), a page table, would remove the shifting but needs a price
band in the spec; it is deferred until a measurement shows the shifts
matter (see Status).

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

Pinned core on the benchmark machine (i5-1135G7, core 3, `performance`
governor, turbo off, GCC 15.2), one session on 2026-11-04, one run per
build, W1 seed 1 with 1,000,000 measured commands. "O2" is the build with
`std::map` levels and the flat id map; "O3" is this container. The
`bench/results/ubuntu-i5-1135G7-{o2,o3}-*.csv` files hold the full runs.

| | O2 (`std::map`) | O3 (sorted array) |
| --- | ---: | ---: |
| throughput, ops/s | 5,687,100 | 5,455,611 |
| limit p50 / p99 / p99.9, ns | 201.7 / 555.1 / 2,762.1 | 132.7 / 433.6 / 2,722.8 |
| cancel p50 / p99 / p99.9, ns | 338.5 / 746.9 / 3,041.9 | 304.2 / 572.9 / 2,743.5 |

The array lowers p50 and p99 for both kinds (limit p50 -34%, limit p99
-22%, cancel p50 -10%, cancel p99 -23%) and leaves p99.9 unchanged. Batched
throughput is 4.1% lower. The per-operation passes serialize the pipeline
with `lfence` and the throughput pass does not, so the two can disagree; the
cause of this one has not been isolated.

An earlier version of this section reported an unpinned, shared-sandbox A/B
in which the array looked about ten times worse on cancel p99. The pinned
run did not reproduce it, and that result is withdrawn: a shared VM cannot
separate this effect from its own noise.

## What this does not establish

- One run per build. The documented run-to-run spread on this setup is about
  1.5% at p50 and p99 and 8% at p99.9, so the p50 and p99 gains are outside
  it. The throughput spread on baseline day was 0.07%, but a re-run of the
  baseline commit in this session was disturbed, so the 4% throughput
  difference is indicative and not established.
- Only W1, which is deep (about 1,000 levels per side). The shifts above
  cost less than the pointer chasing they replace on this book, and the
  cost grows with depth, so a much deeper book may behave differently. No
  other book shape was run.
- The p99.9 of about 2.7 us did not come from this container: it appears at
  O2 (see ADR-005) and is unchanged here.
- The page table is not built, so nothing here says it is faster.

## Status

(a) was accepted for M1. (b) was built and measured at M5: it removes the
allocations and, on the pinned run, lowers median and p99 latency against
`std::map` on W1 while costing 4% of batched throughput. It is accepted as
the container for now. (c) avoids the shifting entirely by indexing levels
by price offset, and that needs a bounded price range: the book would take a
price band when it is built and reject orders outside it, the way an
exchange price collar does. That changes what the engine accepts, so it is a
spec decision (S1) and gets its own ADR, to be written if a measurement
shows the shifts matter.
