# ADR-004: FIFO within a price level — intrusive doubly-linked list

## Decision

Each `Level` owns `head_idx` / `tail_idx` (uint32, pool indices). Each
`Order` carries `next_idx` / `prev_idx`. Sentinel value: `NIL_IDX =
0xFFFFFFFF`.

- Append (new order arrives): O(1), append at tail.
- Cancel: O(1) unlink from wherever the order sits in the list.
- Fill-from-front: O(1), pop from head.
- **Modify that changes level or loses priority (S2)**: unlink from the
  current position, then append at the tail of the target level. If the
  target level is the *same* level (e.g. certain price-change cases), this
  must still be a genuine unlink-then-append — never a swap performed while
  the order is still linked into the list. Doing the swap in place risks
  leaving `next_idx`/`prev_idx` pointing at stale neighbors mid-operation,
  which corrupts FIFO order for every order behind the moved one.

## Rationale

No node allocation on insert or removal — the "nodes" are pre-existing
pool slots, not heap objects. This is what makes cancel and modify O(1)
without touching the allocator, which is required for both S7 (no
allocation in the hot path) and the modify-preserves-priority semantics in
S2. The same-level edge case is specifically why conformance test 12b
exists: it is the one case where "just move the order" is easy to get
subtly wrong.

## Status

Accepted at M0. Implementation and unit tests (including 12b) target M1.
