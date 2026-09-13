<!-- Compiled from decisions finalized during design review. Re-word as needed. -->

# Benchmark methodology

No number gets published without the environment and method that produced
it. This document is versioned alongside `bench/results/`; every results
CSV names which version of this doc it was produced under.

## Environment block (required in every result file)

CPU model, core pinned (`taskset` / `pthread_setaffinity_np`), governor set
to `performance`, turbo state, RAM, kernel version, compiler + flags
(`-O3 -march=native -fno-plt`), binary hash.

## Timing

`rdtscp` + `lfence` for per-operation timing. If per-op measurement
overhead exceeds 1% of the operation cost being measured, switch to batched
timing (N ops per `rdtscp` pair) instead — decided by measurement (tests
26–27), not assumed up front. Every published number states which mode
produced it.

Warmup: at least 100,000 operations before any measured run. Report N such
that the p99.9 latency bucket has at least 100 samples.

## Workloads (versioned: W1, W2, ...)

- **W1 (baseline)**: 10,000-tick price span, average book depth ~1,000
  levels/side, 30% cancels, 50/50 buy/sell split, Zipf `s=1.0` price
  concentration, order sizes 1–100 lots.
- Later workloads add: heavier cancel ratios (40%), bursty Poisson-arrival
  batches, deeper books. Each gets its own version number; no workload is
  silently modified in place.

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
