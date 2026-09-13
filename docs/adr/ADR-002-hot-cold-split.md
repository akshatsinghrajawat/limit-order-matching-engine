# ADR-002: Order field layout — hot/cold split

## Decision (partial — layout mechanism deferred to implementation)

`Order` fields split by access frequency:

- **Hot** (touched on every match-loop iteration): `price`, `qty`,
  `next_idx`, `prev_idx`, `side`, `level_handle`.
- **Cold** (touched only on cancel or audit): `order_id`, `account`,
  `seq_no`.

`timestamp` does **not** appear on `Order`, correcting this ADR's original
draft. S7 forbids wall-clock state anywhere in the core — timestamps live
only in the journal wrapper. This surfaced at implementation time, not
design time, which is itself worth noting: the spec review didn't catch it
because nothing forced anyone to write the actual struct out until M1.

`level_idx` (which level an order currently rests in) is also deliberately
absent for now. ADR-003's price-level container isn't built yet, and its
final shape decides how an order should reference its level — adding a
field for it now would be a guess this struct has to unlearn later.

Whether this is one aligned struct with hot fields grouped first, or two
separate arrays (SoA), is decided at implementation time by sketching the
actual access pattern of the match loop — not decided here in the abstract.
This ADR records the *classification* of fields as hot/cold; the *layout
mechanism* gets its rationale filled in once the match loop exists and can
be measured.

## Rationale

Cache-line locality matters most on fields touched inside the crossing
loop. Cold fields (id, account, timestamp) are read rarely enough that
their layout has no measurable effect on the hot path, so there is no
reason to let them share a cache line with fields the matcher touches on
every iteration.

## Status

Accepted at M1: single struct, hot fields declared first, no manual padding
yet — `sizeof(Order)` is 48 bytes (measured, not assumed), still under one
64-byte cache line, so splitting into two arrays (SoA) has no measurable
upside at this size and would only add indirection. Revisit if `Order`
grows past a cache line, or if profiling ever shows the cold fields being
pulled into cache alongside hot ones under real load.
