<!--
  This document compiles decisions made and finalized during design review,
  before any implementation existed. It is a first draft, not a final text —
  read it through and rewrite anything that doesn't sound like you before
  committing to it as the record of "why."
-->

# Spec: limit-order-matching-engine

Semantics are resolved here once. Any change after implementation begins
requires a documented ADR, not a silent edit.

## S1 — Order model

- Fields: `order_id` (uint64, **client-supplied**), `side` (enum), `price`
  (int64, integer ticks — never floating point; tick size fixed at 1 for this
  project, documented as a simplification), `qty` (uint32, integer lots,
  min 1), `seq_no` (uint64), handle implicit (index into the order pool).
- Duplicate `order_id` detection happens at ingress, via the `order_id ->
  handle` lookup, **before** any pool allocation. A duplicate is rejected at
  the lookup step; it never reaches the pool.
- Allocation boundary (see ADR-005): the dedup-check-and-reject path is a
  pure lookup and is allocation-free under any map implementation. Only
  accept-and-register (inserting a *new* order_id into the map) allocates.
  This is the exact boundary the no-alloc optimization pass targets — not
  "ingress isn't clean yet," but "insertion allocates, lookup does not."

## S2 — Modify semantics

Modify **never frees the pool slot**. The handle and `order_id` stay stable
for the order's entire lifetime — only a real Cancel frees a slot (and
increments its generation counter).

- **Quantity reduce**: update in place, keep existing time priority.
- **Quantity increase** or **price change**: unlink the order from its
  current level, then relink — either at the tail of the same level, or at
  the tail of a new level — and assign a new `seq_no`. This loses time
  priority, matching the conservative common exchange rule for quantity
  increases (an increase changes book position; a decrease does not).
- Edge case: a price-change modify that resolves to the *same* level must
  unlink-then-append as one operation. Do not swap-in-place while still
  linked — that corrupts FIFO order for every order behind it. (Covered by
  conformance test 12b.)

## S3 — Self-trade prevention (STP)

Policy: reject the incoming aggressive quantity when it would self-match;
the resting order is untouched. This is the simplest deterministic policy —
it preserves book state and is the easiest to explain and prove correct.
STP is applied per account tag.

Default test fixtures use **distinct synthetic account ids** per side/test.
STP is **not** the default behavior across the suite — only the dedicated
self-trade test (conformance test 17) deliberately assigns matching account
tags to produce a self-match. Any other test that accidentally shares an
account tag across crossing orders is a test bug, not expected behavior.

## S4 — Odd lots / residuals

All quantities are integer lots; there is no fractional matching. The
residual of a partial fill stays resting with its original time priority.
There is no separate "odd lot" book. Documented simplification.

## S5 — Trade print and crossing rules

An incoming marketable order matches against the best opposite price
level(s), respecting strict price-time priority, and fills **at the resting
order's price** (never the taker's).

- Partial fills are allowed; an order rests only if its residual quantity
  is greater than zero and it was a limit order.
- **Market order**: fills against the book until exhausted or the book is
  empty; any unfilled remainder is cancelled (not rejected retroactively —
  documented).
- **IOC**: same fill behavior as a marketable limit order; unfilled
  remainder is cancelled.
- **FOK**: pre-check total available quantity at the limit price. If
  insufficient, reject the entire order **without touching the book**. This
  must be atomic — no partial state is ever visible. This atomicity is
  guaranteed twice over: explicitly, by rejecting before any book mutation;
  and structurally, by S8 — no other command can be interleaved between the
  pre-check and the fill attempt, so nothing could have touched the book in
  between even if the check had allowed it.
- Execution events are emitted in match order:
  `{maker_order_id, taker_order_id, price, qty, trade_id, seq_no}`.

## S6 — Priority

Strict price-time priority. Within a price level: FIFO by arrival sequence.
"Arrival" means order of **acceptance by the engine**, not order of receipt
by the ingress ring.

`seq_no` is assigned by the matching core itself, on dequeue from the
ingress ring — never by a producer. There must be exactly one source of
ordering truth in the system. (The same principle is why the journal, not
the in-memory event vector, is the sole replay authority — see ADR-006.)

## S7 — Determinism invariants (hard, test-enforced)

- Output is a pure function of `(seed, input command sequence)`. The same
  inputs must produce a byte-identical execution/event log and a
  byte-identical final book serialization, every time.
- No unordered-container iteration anywhere output depends on it. Hash maps
  are permitted only for O(1) lookup by id — never iterated for output.
- No floating point anywhere in engine state.
- No wall-clock time in engine state. Timestamps are recorded only in the
  journal wrapper, never inside the core.
- No undefined behavior: the engine is compiled with
  `-fsanitize=address,undefined` in every test build, always.

## S8 — Command atomicity

Each dequeued command executes to completion — including every match, fill,
and event emission it triggers — before the next command is dequeued.
There is no interleaving point inside the core. This is atomic **by
construction** (the core is single-threaded, one command at a time), not by
any lock or check.

Any future change to the dequeue loop (e.g. O8, ingress batching or command
coalescing) must explicitly preserve this property, and its ADR must state
*how*. This is the enforcement mechanism for an otherwise-untestable
architectural invariant — a mandatory one-line justification in the ADR,
not a unit test.

Two questions raised at design time — "what happens on cancel of an order
currently being matched?" and "what about ingress during a FOK pre-check?"
— are both non-issues **because S8 holds**, not because they were handled
as special cases.

---

## Known simplifications (carried into the README's "what's not fast yet")

- Single instrument, single tick size (1), no network layer.
- No fractional/odd-lot matching.
- STP is the only order-protection policy implemented (real venues support
  several: cancel-newest, cancel-oldest, cancel-both, decrement-and-cancel —
  not in scope here).
