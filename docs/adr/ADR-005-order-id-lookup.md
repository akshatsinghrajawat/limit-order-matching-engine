# ADR-005: order_id → handle lookup

## Decision

Start with `std::unordered_map<uint64_t, Handle>`. This is a measured
campaign item (O2) — replace with a hand-rolled flat open-addressed map
(power-of-2 sized, linear probing) once the no-alloc pass requires it. This
map is used for lookup only; it is never iterated for output (per S7).

## Rationale

**Ingress allocates on accept, not on lookup.** The dedup-check-and-reject
path (S1) — checking whether an incoming `order_id` already exists — is a
pure lookup and is allocation-free under *any* map implementation,
including `std::unordered_map`. It is only the accept-and-register path —
inserting a genuinely new `order_id` into the map — that allocates a node
under `std::unordered_map`. Naming the exact operation that carries the
cost ("insertion allocates") is the useful statement here; "ingress isn't
hot-path-clean yet" is not — it doesn't tell the next reader where to look.

This is also why O1 (a no-alloc pass on the order pool alone) cannot make
the no-alloc test (test 25) pass by itself: `std::map` (ADR-003) and
`std::unordered_map` (this ADR) both still allocate per insert on the
accept path regardless of what O1 does. O1, O2, and O3 are a jointly
necessary group, not three independent wins — see the campaign README
framing.

## Status

Baseline accepted for M1. Flat open-addressed replacement measured at M5
(O1–O3 group), with a single combined before/after number for the group.
