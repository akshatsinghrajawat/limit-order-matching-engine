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

### Decision from test 27

An empty timed region costs about 25 ns on the machine used so far, which is
11 to 17% of an average W1 operation (five runs: 11.1, 14.7, 16.7, 15.6 and
13.5%). That is far over the 1% line, so:

- **Throughput is the batched figure.** One `rdtscp` pair around all the
  measured operations, which spreads the timer cost over all of them.
- **Latency percentiles stay per-operation.** A batch gives an average,
  not a distribution, so the percentiles can't be batched. They include the
  timer cost.
- **The timer cost is recorded, not subtracted.** Every results file carries
  `timer_overhead_p50_ns`, measured at the start of that run, and names both
  timing modes (`latency_timing`, `throughput_timing`). Read each latency as
  including about that many nanoseconds.

Nothing is subtracted because the data doesn't show it would be right. The
shared VM these runs came from moved the batched cost itself by about 40%
between repetitions (159 to 223 ns), which is more than the effect being
checked, so it can't say whether the mean per-operation sample minus the
empty-region cost recovers the batched cost. Run `bench_overhead` on the
machine that publishes numbers, and if that difference is small and steady
there, subtracting becomes defensible and this section should change.

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
measured counts, the two timing modes and the timer overhead, trade count
and throughput, then one CSV table with a row per kind. A build with sanitizers on prints a warning,
and its flags line shows it; those numbers aren't published.

## Producing a baseline

`tools/run_baseline.sh <cpu> <label>` does the whole sequence on one core:
a Release build with sanitizers off and `-O3 -march=native -fno-plt`, then
`bench_noise`, `bench_overhead` and `bench_w1`, in that order. If the noise
floor fails it stops, since nothing measured on that setup can be trusted.
It writes `<label>-noise.csv`, `<label>-overhead.csv` and `<label>-w1.csv`
to `bench/results/`, only if all three steps worked, and never over an
existing file.

It refuses to start unless that core's governor is `performance`, and it
refuses to keep the results unless the files themselves record governor
`performance` and turbo `disabled`. The environment block is read after the
measurements, so a setting that changed mid-run shows up there. Turbo off
is `echo 1 | sudo tee /sys/devices/system/cpu/intel_pstate/no_turbo`.

Stop power managers before running. On the Ubuntu laptop used for the
baseline, auto-cpufreq set turbo back on as soon as the benchmark put load
on the core: the clock went from 2.4 to about 3.8 GHz, the empty-timer cost
went from 22 to 12 ns partway through the first noise run, and throughput
moved by up to 11% between runs. With it stopped (`sudo snap stop
auto-cpufreq`), turbo off and the governor set, the core held 2.4 GHz under
load. `power-profiles-daemon` was also running and is the next suspect if a
setting still changes.

Pick a core nothing else is using, not core 0, and with its hyperthread
sibling idle if the CPU has one (`lscpu -e` lists the pairs). Close
everything else first; a browser is enough to move the p99.

The three files are committed together under the methodology version they
state. A W1 file without the noise and overhead files next to it from the
same run isn't a published result.

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

- **Test 26 — noise floor**: `bench_noise` times an empty region ten times
  (1,000,000 operations per run, after a 100,000-operation warmup) and
  compares the p99 of each run. The highest p99 minus the lowest must be at
  most 10 ns. Details below.
- **Test 27 — overhead check**: `bench_overhead` runs W1 five times and
  reports, for each run, the batched cost per operation, the mean of the
  per-operation samples, the p50 of an empty timed region, and the empty
  region's share of the batched cost. It quantifies and doesn't pass or
  fail. The decision it feeds is under "Timing".

### Noise floor

The empty region is timed exactly as `bench_w1` times a command, two
`rdtscp` reads around nothing, so the p99 spread measured here is the
spread of the timer and the machine with no engine code involved. A p99
difference between two configurations smaller than that spread isn't
claimed as a result.

The limit is in nanoseconds, not a fraction of the p99, because the no-op's
p99 is about 30 ns and a fraction of that says little about whether an
operation costing a few hundred ns can be measured. 10 ns is a judgment
call. Seven passes of ten runs on a shared cloud VM, core pinned, gave
spreads from 1.9 to 14.3 ns and one of them was over the limit. A machine
that fails at random isn't one to publish from. If the machine that
produces the published numbers stays well under 10 ns, tighten the limit
and bump the version.

`bench_noise` exits 1 over the limit. Run it on the same core and the same
kind of build (Release, sanitizers off) as the benchmark itself, before
trusting any number from that setup, and commit its results file next to the
numbers it vouches for. The unit test only checks the arithmetic and the
file layout; a loaded CI runner would fail a real noise check for reasons
unrelated to the code.

## Publishing

Every results CSV is committed alongside the methodology-doc version it
was produced under. Charts are generated with a small hand-written C++ SVG
generator (same approach as `optimal-search-strategies`) — no external
plotting dependency.
