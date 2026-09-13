# ADR-006: Event log and journal

## Decision

The event log is an append-only vector of POD event structs, reserved up
front (no reallocation churn in the hot path). Replay reads the **journal**
— the command log written by ingress — not the in-memory event vector.

## Rationale

There must be exactly one source of ordering and replay truth in the
system (S6). If replay were driven from the event vector instead of the
journal, there would be two representations of "what happened" that could
silently diverge after a future change — for example, a batching
optimization (O6, outbound event batching) that changes *when* events are
appended to the vector without changing the underlying command order. The
journal is the ground truth precisely because it is what ingress actually
received, in the order the core actually processed it (S8); the event
vector is a derived output, not a source of record.

## Status

Accepted at M0. Journal writer/reader and golden-replay CI (tests 22–23)
target M4.
