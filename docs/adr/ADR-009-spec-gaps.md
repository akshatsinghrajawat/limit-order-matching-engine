# ADR-009: Four points the spec left open

## Decision

1. **Every command takes the next `seq_no`.** Adds, market orders, modifies
   and cancels each take one, starting at 0, whether or not the command
   changes the book. A resting order carries the number of the command that
   rested it or last repositioned it. Every trade a command makes carries
   that command's number. So a command's `seq_no` is its position in the
   input.
2. **A modify that would trade is rejected.** A buy moved to the best ask
   or above, or a sell moved to the best bid or below, returns `false` and
   leaves the order where it was. The book is never crossed, and
   `invariant_violation()` now checks that.
3. **Pool exhaustion drops only the residual.** If a limit order's leftover
   quantity cannot rest because the pool is full, the leftover is dropped
   and `pool_exhausted` is set. The trades it already made stand.
4. **Duplicate ids are rejected only against resting orders.** An id that
   has filled or been cancelled can be submitted again.

Points 3 and 4 are what the engine already did. They are written down here
because the oracle was built to follow the engine on them, which is not the
same as having decided them.

## Rationale

**Why the golden files forced this now.** M4's journal and golden event log
fix whatever the engine does into bytes that CI compares forever. Any of
these four left as an accident of the first implementation would be fixed
into those files without anyone having chosen it.

**1. `seq_no` per command.** Before, a number was used up only when an order
rested or a modify moved one, and a trade carried the next unused number.
That made a trade share its `seq_no` with whichever order rested next, and
it made the number depend on which commands happened to change the book. S6
asks for one source of ordering truth assigned by the core on dequeue. A
per-command number is that, and it is also the journal position, which is
what replay needs to check against. The price is gaps in the numbers of
resting orders. Nothing reads a gap: the book only compares `seq_no` within
a level, where the order is still strictly increasing. At 10 million
commands a second a 64-bit counter lasts tens of thousands of years.

**2. Rejecting a crossing modify.** The engine let a modify move an order
through the other side and left the book crossed: a bid above an ask, the
two never trading with each other. The matching code assumes the best bid
is below the best ask: `cross()` stops at the first level that does not
cross, so a crossed pair is invisible to it. Two other ways were weighed:
letting modify match like a new order, which makes modify return trades and
needs its own self-trade and pool handling, and silently clamping the price,
which changes what the client asked for. Rejection is the smallest rule that
keeps the invariant. The cost is that a caller who wants to trade cancels and
resubmits, and loses priority, which a marketable modify would have lost
anyway.

**3. Residual only.** The pool can be full at the moment the leftover wants
to rest, and fills free slots as they go, so whether the leftover fits is not
known until matching has finished. Rejecting the whole order up front would
need a count that matching can change. Undoing the trades is not available:
a command that has run cannot be unwound (S8) and its events are out. So the
trades stand, and the flag says what was dropped.

**4. Resting ids only.** Rejecting every id ever seen needs a set that grows
without bound, which is an allocation per new id and what test 25 forbids.
The rule that costs nothing is the one the id map already enforces.

## What this does not establish

- `modify_order` returns `false` for an unknown id, a zero quantity and a
  crossing price alike. The caller cannot tell which. `AddResult` has flags
  for its cases and modify has none.
- Point 4 has a consequence: after an id fills and is reused, a late modify
  or cancel meant for the old order hits the new one. The engine does not
  defend against that; the gateway is expected to issue unique ids.
- Point 3 is the weakest of the four. Dropping the residual silently from
  the client's side of a trade stream is a policy, not a law, and a venue
  might prefer to reject before matching.
- The engine and the oracle have the same author. The oracle was changed to
  match these decisions, so agreement between them checks that both were
  changed the same way, not that the decisions are right.

## Status

Accepted. `spec.md` (S1, S2, S5, S6) states each rule. Conformance tests 12c
and 12d, the oracle's own checks and the differential tests cover them; the
fuzzer and the exhaustive pass now also have to reach a rejected crossing
modify, or they fail as vacuous.
