# ADR-001: Order storage — FixedPool + generation-counter handles

## Decision

Orders live in a pre-allocated `std::vector<Order>`, sized to a fixed
maximum (e.g. 1,000,000) and never resized after warmup. Freed slots are
tracked with an intrusive singly-linked free list threaded through the free
slots themselves — no separate free-list container.

A `Handle` is a 64-bit value: a 32-bit pool index packed with a 32-bit
generation counter (`handle = index | (gen << 32)`). On free, the slot's
generation increments. Every lookup through a handle validates that the
caller's generation matches the slot's current generation before returning
data.

No `shared_ptr`, no `new`/`delete` anywhere in the engine core, ever.

## Rationale

O(1) cancel and modify need direct indexed access (`pool[handle]`), which
rules out node-based containers. The generation counter exists specifically
to catch **use-after-free of a stale handle**: without it, a cancelled
order's slot can be silently reused by a new order, and a late modify/cancel
against the old handle would corrupt the new order's state instead of
failing loudly. This is not a theoretical concern — it's exactly the
failure mode conformance tests 8b and 8c exist to force.

## Alternatives considered

- **Raw index handles, no generation**: rejected — no way to detect
  use-after-free; a stale handle silently aliases whatever order now
  occupies that slot.
- **`shared_ptr<Order>`**: rejected — allocates on the heap per order,
  incompatible with S7 (no allocation in the hot path) and with the
  no-alloc test (test 25).

## Status

Accepted at M0 (design stage) — implementation and unit tests target M1.
