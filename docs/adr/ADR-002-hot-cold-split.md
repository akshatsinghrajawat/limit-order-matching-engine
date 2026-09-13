# ADR-002: Order field layout — hot/cold split

## Decision (partial — layout mechanism deferred to implementation)

`Order` fields split by access frequency:

- **Hot** (touched on every match-loop iteration): `price`, `qty`,
  `next_idx`, `prev_idx`, `side`, `level_handle`.
- **Cold** (touched only on cancel or audit): `order_id`, `account`,
  `timestamp`.

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

Partially accepted at M0. Layout mechanism decision + rationale to be
filled in during M1 implementation, once measured.
