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

- No latency number. The sandbox this was built in is too noisy to separate
  a map swap from run-to-run variation. The effect on latency is to be
  measured on the benchmark machine with `tools/run_baseline.sh` against the
  committed baseline.
- The allocation count was taken with GCC 13.3. For O1 the same count came
  out identical on the benchmark machine's GCC 15.2; this one has not been
  run there yet.
- The size rule assumes the caller passes the pool capacity, which `Book`
  does. An `IdMap` built with a smaller bound than the entries it is given
  would fill, and a lookup of an absent key would then probe forever.
  `insert` asserts the bound in builds that keep assertions.
- The hash is no defense against ids chosen to collide, which lengthen probe
  runs. The engine takes ids from a trusted gateway.

## Status

Accepted. O1 (ADR-008) and this change together remove every allocation
except the price-level nodes. ADR-003's replacement is what is left before
the zero-allocation test (test 25) can pass.
