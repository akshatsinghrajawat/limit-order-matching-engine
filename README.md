# limit-order-matching-engine

Single-threaded C++ limit order matching engine. Strict price-time
priority, integer-tick pricing, deterministic replay via an append-only
journal. Correctness verified by differential fuzz testing against a
reference oracle. No floating point or wall-clock state in the core.
Benchmarks staged and versioned per milestone.

## Status

**M1 in progress.** `Order`/`Handle`/`FixedPool`/intrusive level list, plus
a naive `Book` matcher on top of ADR-003's `std::map` baseline, are
implemented and passing conformance tests 1–8 (rest, best bid/ask,
crossing-at-maker's-price, multi-level sweep, partial-fill priority, FIFO,
cancel, cancel-of-cancelled) under `-fsanitize=address,undefined` via
`ctest`. **Not implemented yet**: modify (S2, tests 9–12), self-trade
prevention (S3, test 17), market/IOC/FOK (S5). No oracle, no fuzzing, no
journal yet — this is a hand-checked correctness pass, not the formal
M2 verification.

## Roadmap

```
M0  spec + architecture                       — done
M1  Order/Handle/FixedPool/level list          — done
    naive matcher, conformance tests 1-8       — done
    modify (S2), STP (S3), tests 9-12, 17      — not started
    market/IOC/FOK (S5)                        — not started
M2  reference oracle + differential fuzz
    (1M ops, 100+ seeds) + exhaustive test
M3  validated benchmark harness + W1 baseline
M4  journal + golden-replay CI + remaining
    conformance + crash-recovery test          — committed scope ends here
M5  optimization campaign O1-O3 (no-alloc
    group), before/after numbers               — stretch
M6+ campaign O4-O9, deep-dive writeup          — stretch, unscheduled
```

M0–M4 is the committed, shippable scope. M5 onward is picked up only after
M4 is complete and reviewed — not pre-committed to a timeline.

## What this is not (yet)

- Not multi-instrument — single instrument, single tick size.
- Not networked — no wire protocol, in-process only.
- Not concurrent — the matching core is single-threaded by design, full
  stop. SPSC ring buffers sit around it as ingress/egress plumbing, added
  later and deliberately deferred so the core is proven correct first.
- No performance numbers published yet — none exist. They'll appear here,
  with full methodology, starting at M3.

## Design docs

- [`docs/spec.md`](docs/spec.md) — resolved semantics (S1–S8)
- [`docs/adr/`](docs/adr) — architecture decision records
- [`docs/benchmark-methodology.md`](docs/benchmark-methodology.md) —
  how any future number here should be read

## License

MIT — see [`LICENSE`](LICENSE).
