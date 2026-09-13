# ADR-003: Price level container

## Decision

Start with `std::map<int64, Level*>` for correctness. This is a
**measured campaign item** (O3), not a final decision — the initial
implementation optimizes for "obviously correct," not for speed.

Candidates to measure head-to-head at the O3 stage, in the campaign ADR:

- (a) `std::map<int64, Level*>` — the baseline.
- (b) Flat sorted array of price levels with `memmove` insert.
- (c) Two-level structure: a page table mapping price to a page via
  arithmetic, with a dense page array underneath.

## Rationale

`std::map` gives correct ordering and O(log n) operations for free, at the
cost of per-node allocation and pointer-chasing — acceptable for a
correctness-first baseline, unacceptable once the no-alloc invariant (S7,
test 25) is enforced. (b) trades insert cost for cache-friendly iteration.
(c) is expected to win for dense tick ranges specifically, at the cost of
wasted space at wide, sparse price ranges — that tradeoff gets documented
in the campaign ADR, not asserted here before it's measured.

## Status

Baseline (a) accepted for M1. Candidates (b) and (c) measured against it at
M5 (O1–O3 optimization group); winner and full before/after table recorded
here as an addendum once measured.
