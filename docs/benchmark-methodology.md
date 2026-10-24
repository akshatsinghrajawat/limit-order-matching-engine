<!-- Compiled from decisions finalized during design review. Re-word as needed. -->

# Benchmark methodology

**Version 1.**

No number gets published without the environment and method that produced
it. This document is versioned alongside `bench/results/`; every results
CSV names which version of this doc it was produced under. The version
number goes up when the way numbers are produced changes, and the change
is described here.

## Environment block (required in every result file)

CPU model, core pinned (`taskset` / `sched_setaffinity`), governor set
to `performance`, turbo state, RAM, kernel version, compiler + flags
(`-O3 -march=native -fno-plt`), binary hash.

`bench/env.hpp` writes the block as `# key: value` lines at the top of a
results file: `cpu_model`, `pinned_cpu`, `governor`, `turbo`, `ram`,
`kernel`, `compiler`, `flags`, `tsc`, `binary_hash`. `pinned_cpu` is read
back from the kernel's affinity mask, not taken from what the program asked
for. A field that can't be read says `unavailable`. Governor, turbo, RAM,
kernel and the hash come from `/sys`, `/proc` and `uname`, so only Linux
fills them all in; published numbers come from Linux runs, and a Windows
build is for development.

## Timing

`rdtscp` + `lfence` for per-operation timing. If per-op measurement
overhead exceeds 1% of the operation cost being measured, switch to batched
timing (N ops per `rdtscp` pair) instead — decided by measurement (tests
26–27), not assumed up front. Every published number states which mode
produced it.

Ticks become nanoseconds through a rate measured against `steady_clock`
during a 20 ms spin at startup (`TscClock` in `bench/timing.hpp`). The `tsc`
line of the environment block records that rate and whether the CPU reports
an invariant counter.

Warmup: at least 100,000 operations before any measured run. Report N such
that the p99.9 latency bucket has at least 100 samples.

## Running it

`bench_w1 <label> [seed] [measured_ops] [cpu]` writes a results file to
stdout. It runs the same command stream twice, each time on a fresh book
after a 100,000-operation warmup. One pass is timed per operation and gives
the latency percentiles. The other is timed as a whole and gives throughput,
so throughput doesn't include the cost of the per-operation timestamps.
Throughput counts every operation, cancels included. The two passes must
produce the same number of trades, or the run aborts.

Latency is reported separately for limit orders and for cancels. Percentiles
are nearest-rank over the sorted samples. The runner refuses to run with
fewer than 100,000 samples of either kind, which is what the 100-sample
p99.9 bucket rule works out to; the 1,000,000 measured operations it
defaults to leave about 300,000 cancels. The p99.99 column needs ten times
that many samples before its bucket holds 100, so read it as indicative.

The results file starts with the environment block, then `# key: value`
lines for the methodology version, label, workload, seed, warmup and
measured counts, timing mode, trade count and throughput, then one CSV
table with a row per kind. A build with sanitizers on prints a warning,
and its flags line shows it; those numbers aren't published.

## Workloads (versioned: W1, W2, ...)

- **W1 (baseline)**: 10,000-tick price span, average book depth ~1,000
  levels/side, 30% cancels, 50/50 buy/sell split, Zipf `s=1.0` price
  concentration, order sizes 1–100 lots. Defined by `bench/workload.hpp`
  and pinned by `tests/bench/test_workload.cpp`. Limit orders and cancels
  only: no modify, market, IOC or FOK. One account per order, so
  self-trade prevention never fires.
- Later workloads add: heavier cancel ratios (40%), bursty Poisson-arrival
  batches, deeper books. Each gets its own version number; no workload is
  silently modified in place.

### How W1 holds its depth

A fixed cancel probability doesn't hold a book at any size. A cancel removes
one order whatever the book looks like, so the book grows forever or empties
out. On this engine (cancels aimed at live orders, `cross_shift = 3`), 40%
cancels grew to about 200,000 resting orders in 4M operations and 45%
collapsed to a few dozen.

So W1 scales the cancel probability by `resting / target_resting`. It is 30%
when the book holds the target of 3,000 resting orders, about 1,000 levels
per side. `cross_shift = 50` makes about half of all limit orders marketable,
and it is the value where the cancel share settles at 30%. Over 4M
operations: 30.4% cancels, 1,010 to 1,110 levels per side, about 56% of
limit orders trading, no drift. The generator runs its commands through the
engine as it goes, so every cancel hits a live order.

Two things to keep in mind when reading W1 results. The cancel share comes
out of the controller and isn't a setting, so changing `cross_shift` changes
it. And because the engine generates the stream, a change that alters the
engine's trades changes the workload; the golden hash in the test is there
to catch that.

## Metrics

Orders/sec throughput, plus p50 / p99 / p99.9 / p99.99 latency. Cancel
latency and modify latency are reported **separately** from insert/match
latency — they are different operations with different tails, and a single
blended number hides exactly the thing worth looking at.

## Harness validation (must pass before any number is trusted)

- **Test 26 — noise floor**: benchmark a no-op loop; p99 variance across 10
  runs must stay within a documented threshold.
- **Test 27 — overhead check**: quantify measurement overhead
  (batched-timing vs. per-op `rdtscp`) and either subtract it or document it
  alongside every number it affects.

## Publishing

Every results CSV is committed alongside the methodology-doc version it
was produced under. Charts are generated with a small hand-written C++ SVG
generator (same approach as `optimal-search-strategies`) — no external
plotting dependency.
