# ADR-005: order_id → handle lookup

## Decision

`IdMap` (`include/engine/id_map.hpp`): one flat array of `{order_id,
Handle}` entries, open addressing with linear probing, sized once when the
`Book` is built and never grown. This replaced the `std::unordered_map` the
engine started with (O2 of the campaign).

- **Size.** The smallest power of two that is at least twice the pool
  capacity. The book can't hold more orders than the pool has slots, so the
  table is never more than half full and an insert can't fail.
- **Empty slot.** A slot whose handle is the invalid handle. Every order id,
  zero and the maximum included, is a legal key.
- **Hash.** The finalizer of splitmix64 over the id, masked to the table
  size. Client ids tend to be a counter, and linear probing turns a counter
  into one long run unless the bits are mixed. The constants are fixed, so
  the layout is the same on every run (S7).
- **Erase.** Backward shift: after removing an entry, later entries in the
  same probe run that can legally sit in the hole are moved into it. No
  tombstones, so a lookup never steps over dead slots and probe runs don't
  lengthen as orders come and go.
- The map is for lookup only. Nothing iterates it, so its layout can't reach
  the output (S7).

## Rationale

**Ingress allocates on neither path.** The duplicate check (S1) is a pure
lookup and was allocation-free under `std::unordered_map` too. What
allocated was registering an accepted order: one heap node per insert, plus
the occasional rehash. Counting calls to the global `operator new` on W1
(seed 1, 1,000,000 measured commands, `bench_allocs`), that was 505,239 of
the 831,515 allocations left after O1. After this change the run makes
326,276, all of them `std::map` level nodes (ADR-003), and no command makes
more than one.

**Why half full.** At the limit, linear probing needs about 1.5 probes for a
hit and 2.5 for a miss on average. A fuller table saves memory and costs
probes on every command, and this lookup is on the path of every command.
The memory is not small: an entry is 16 bytes, so a pool of 65,536 orders
gets a 131,072-slot table of 2 MiB. If that ever matters, the sizing loop in
the constructor is the one place to change.

**Why backward shift and not tombstones.** A cancel-heavy workload (W1 is
30% cancels) leaves a tombstone for every cancel, and a lookup has to step
over each one until a rebuild clears them. Backward shift moves a few
entries at erase time and leaves a table that looks as if the removed order
had never been inserted.

## What this does not establish

- **Latency effect, measured and mixed.** Pinned core on the benchmark
  machine, one session on 2026-11-04, W1 seed 1, 1,000,000 commands, one run
  per build (O1 = `std::unordered_map`, O2 = this map):

  | | O1 | O2 |
  | --- | ---: | ---: |
  | throughput, ops/s | 4,747,865 | 5,687,100 |
  | limit p50 / p99 / p99.9, ns | 205.4 / 437.7 / 706.0 | 201.7 / 555.1 / 2,762.1 |
  | cancel p50 / p99 / p99.9, ns | 321.2 / 515.0 / 667.2 | 338.5 / 746.9 / 3,041.9 |

  Throughput is 20% higher, the median is about the same, and the tail got
  worse: cancel p99 +45% and p99.9 about four times higher for both kinds.
  The same 2.7 us p99.9 shows in the O3 and main builds, so it is
  reproducible within the session. **The cause is not established.** The
  benchmark pool is 65,536 orders, so the table is 131,072 entries of 16
  bytes, 2 MiB, probed at effectively random addresses while only about
  3,000 orders are live; the node map it replaced kept those few thousand
  entries close together. That is a hypothesis, not a finding. A smaller
  table, a different hash, or counting probe lengths would test it.
- The allocation count (326,276 on W1) is identical on GCC 13.3 and on the
  benchmark machine's GCC 15.2.
- The size rule assumes the caller passes the pool capacity, which `Book`
  does. An `IdMap` built with a smaller bound than the entries it is given
  would fill, and a lookup of an absent key would then probe forever.
  `insert` asserts the bound in builds that keep assertions.
- The hash is no defense against ids chosen to collide, which lengthen probe
  runs. The engine takes ids from a trusted gateway.

## Status

Accepted. O1 (ADR-008) and this change together removed every allocation
except the price-level nodes, and ADR-003's array removed those, which is
what lets the zero-allocation test (test 25) pass.
