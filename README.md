# limit-order-matching-engine

[![CI](https://github.com/akshatsinghrajawat/limit-order-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/akshatsinghrajawat/limit-order-matching-engine/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue)
![License: MIT](https://img.shields.io/badge/license-MIT-green)

A single-threaded limit order matching engine in C++17, built around three properties: **specified** behaviour (every rule is written down before it is coded), **cross-checked** correctness (an independent oracle, exhaustive search and mutation testing), and **reproducible** output (a journal that replays to byte-identical results). After warm-up the hot path makes zero heap allocations, and every performance claim comes with a versioned benchmark methodology.

Where something is slow, unmeasured or unfinished, this README says so. Section 12 is a log of the bugs and wrong decisions found along the way, including a performance conclusion of mine that a pinned-core measurement overturned.

**Contents**
[Overview](#1-overview) · [Status](#2-status) · [Quick start](#3-quick-start) · [Worked example](#4-worked-example) · [Semantics](#5-semantics) · [Architecture](#6-architecture) · [Design decisions](#7-design-decisions-and-the-alternatives-that-lost) · [Verification](#8-verification) · [Determinism and replay](#9-determinism-and-replay) · [Performance](#10-performance) · [What is not done](#11-what-is-not-done) · [Engineering log](#12-engineering-log-what-went-wrong) · [Reproducing](#13-reproducing-everything) · [Repository layout](#14-repository-layout)

---

## 1. Overview

| | |
| --- | --- |
| Order types | Limit (GTC, IOC, FOK), market, cancel, modify |
| Order protection | Self-trade prevention per account |
| Priority | Strict price-time: best price first, FIFO within a price |
| Fill price | Always the resting order's price |
| Numeric model | `int64` price ticks (tick size 1), `uint32` lots. No floating point anywhere in engine state |
| Time | No wall-clock time in engine state. Ordering comes from a sequence number the core assigns itself |
| Memory | Fixed-size pool allocated once at construction. No `new`, `delete` or `shared_ptr` in the engine |
| Allocation on the hot path | 0 per command on workload W1 after warm-up, enforced by a test |
| Determinism | Output is a pure function of the command sequence. A golden journal and golden event log are compared byte for byte in CI |
| Invalid input | A command whose kind, side or time in force does not exist is refused, takes no sequence number and changes nothing, in the book, the journal writer and replay ([ADR-011](docs/adr/ADR-011-command-validation.md)) |
| Failure | The six `Book` entry points that change the book do not throw, so a command is never left half applied. If one ever failed, the process ends and replaying the journal recovers the state ([ADR-012](docs/adr/ADR-012-failure-and-recovery.md)) |
| Verification | Conformance tests, differential fuzzing against an independent oracle, exhaustive state-space search, mutation testing, ASan and UBSan on every test build |
| Dependencies | Standard library and CMake only |

**What this is not:** a trading system. It is one instrument, one tick size, in-process, with no network layer. The goal is a small core whose behaviour can be fully specified and fully checked, and then made fast without breaking either.

## 2. Status

M0 to M4 are done except the crash-recovery test. The first three allocation-removal steps (O1 to O3) are done and measured on a pinned core: throughput is up about 15% and the hot path allocates nothing, but the tail above p99.9 got about four times worse at O2 and has not been explained yet (see [section 10](#10-performance)).

After that came an 18-commit hardening series (commit subjects start with `M5:`): invalid commands refused, a fail-stop contract, bounded replay, capacity checks that hold in release builds, retirement of pool slots whose generation runs out, invariant checks with tests of their own, and an early exit in the FOK pre-check. Its cost was measured on a pinned core against the commit before the series: W1 throughput is unchanged, the median latency of a limit order is about 3 ns (2.6%) higher, and the other W1 figures are within the run-to-run spread ([section 10](#10-performance)).

| Milestone | Scope | State |
| --- | --- | --- |
| M0 | Spec (S1 to S8) and ADRs | done |
| M1 | Order pool, intrusive level lists, matcher, conformance tests | done |
| M2 | Reference oracle, differential fuzz, exhaustive search, mutation check | done |
| M3 | Benchmark harness, harness self-validation, W1 baseline | done |
| M4 | Journal format, replay, golden-file CI | done |
| M4 | Crash-recovery test (SIGKILL at random journal offsets) | not started |
| M5 | O1 to O3: remove every hot-path allocation | done and measured (pinned core) |
| M5 | Hardening series: input validation, fail-stop, bounded replay, capacity checks, slot retirement, invariant tests, FOK early exit | done and measured (pinned core): limit p50 +2.6%, deep FOK p50 about 100x lower |
| Next | Find the cause of the p99.9 increase that arrived with O2 | not started |
| Later | Page-table price levels (needs a price band in the spec) | not started, motivation weakened by the O3 measurements |
| Later | SPSC ring buffers around the core, cache-line layout, prefetching, ingress batching | not started |

## 3. Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

- Header-only engine in `include/engine/`. Requires a C++17 compiler (developed on GCC 13 to 15, CI on Ubuntu 26.04).
- Sanitizers are **on by default** (`-fsanitize=address,undefined`). Determinism and memory safety are part of the spec (S7), not an extra. UBSan is fatal (`-fno-sanitize-recover=all`), so an undefined-behaviour report fails the test instead of printing and carrying on; a test that triggers one on purpose checks that. Pass `-DENABLE_SANITIZERS=OFF` for benchmark builds only.
- `ctest` runs 22 test programs in under a minute on a laptop. The long runs live in CI: 100 seeds x 1,000,000 commands of differential fuzz, the full-depth exhaustive search, and the mutation check.

## 4. Worked example

```cpp
#include "engine/book.hpp"
using namespace engine;

Book book(1024);        // pool capacity: the most orders that can rest at once
Book::AddResult r;      // reuse one result so the hot path does not allocate

book.add_limit_order(r, /*id=*/1, Side::Sell, /*price=*/101, /*qty=*/10, /*account=*/7);
book.add_limit_order(r, 2, Side::Sell, 101, 5, 8);
book.add_limit_order(r, 3, Side::Sell, 102, 20, 9);
book.add_limit_order(r, 4, Side::Buy, 99, 30, 10);

book.add_limit_order(r, 5, Side::Buy, 102, 18, 11);                       // sweeps
book.add_limit_order(r, 6, Side::Buy, 102, 100, 12, TimeInForce::FOK);    // all or nothing
book.add_limit_order(r, 7, Side::Buy, 102, 100, 12, TimeInForce::IOC);    // take what is there
book.add_limit_order(r, 8, Side::Sell, 100, 5, 13);                       // rests above the 99 bid
book.modify_order(4, 100, 30);                                             // would trade: rejected
book.modify_order(4, 98, 10);                                              // reposition: applied
book.cancel_order(4);
book.cancel_order(4);                                                      // already gone: false

Book other(16);                                                            // self-trade prevention
other.add_limit_order(r, 1, Side::Sell, 150, 5, /*account=*/20);
other.add_limit_order(r, 2, Side::Buy, 150, 5, /*account=*/20);           // same account: rejected
```

Output of the full program (compiled and run under ASan and UBSan against the code in this repository):

```
sell 10 @101 (id 1)            accepted=1 rested=10 stp=0 trades=0
sell  5 @101 (id 2)            accepted=1 rested=5 stp=0 trades=0
sell 20 @102 (id 3)            accepted=1 rested=20 stp=0 trades=0
buy  30 @99  (id 4)            accepted=1 rested=30 stp=0 trades=0
buy  18 @102 (id 5)            accepted=1 rested=0 stp=0 trades=3
    trade #0  maker=1 taker=5  10 @ 101  seq=4
    trade #1  maker=2 taker=5  5 @ 101  seq=4
    trade #2  maker=3 taker=5  3 @ 102  seq=4
FOK buy 100 @102 (id 6)        accepted=1 rested=0 stp=0 trades=0
IOC buy 100 @102 (id 7)        accepted=1 rested=0 stp=0 trades=1
    trade #3  maker=3 taker=7  17 @ 102  seq=6
sell  5 @100 (id 8)            accepted=1 rested=5 stp=0 trades=0
modify id 4 to 100 (would trade)  -> rejected
modify id 4 to 98, qty 10          -> applied
cancel id 4                        -> done
cancel id 4 again                  -> unknown id

sell 5 @150 (id 1, acct 20)    accepted=1 rested=5 stp=0 trades=0
buy  5 @150 (id 2, acct 20)    accepted=1 rested=0 stp=1 trades=0
```

What to notice:

- Order 5 buys at 102 but pays **101** for the first 15 lots: fills happen at the maker's price. Order 1 fills before order 2 at the same price (FIFO).
- All three trades carry `seq=4`, the sequence number of the command that caused them. A command's sequence number is its position in the input (ADR-009).
- Order 6 (FOK) finds 17 lots and needs 100, so it is rejected and the book is untouched. Order 7 (IOC) takes the same 17 lots and drops the rest.
- Moving order 4's bid to 100 would cross the resting ask at 100, so the modify is rejected and the book is never left crossed. Moving it to 98 with a smaller quantity is a legal reposition.
- In the last two lines the same account is on both sides. The incoming buy is rejected (`stp=1`, no trades) and the resting sell is left alone.

## 5. Semantics

All rules live in [`docs/spec.md`](docs/spec.md) (S1 to S8). Anything the first draft left open was settled in [ADR-009](docs/adr/ADR-009-spec-gaps.md) **before** a golden file was generated, because golden files turn whatever the engine happens to do into a permanent test.

### Commands

| Command | Effect |
| --- | --- |
| `add_limit_order(id, side, price, qty, account, tif)` | Crosses against the opposite side while the price allows, then rests the residual if `tif` is GTC. A side or `tif` that is not one of the enum values is refused (below) |
| `add_market_order(id, side, qty, account)` | No price bound. Fills until the quantity is exhausted or the book is empty. The remainder is cancelled |
| `modify_order(id, new_price, new_qty)` | Repositions a resting order. Never trades |
| `cancel_order(id)` | Removes a resting order. Returns false for an unknown, filled or already-cancelled id |

### Invalid commands ([ADR-011](docs/adr/ADR-011-command-validation.md))

A command is valid when its kind is limit, market, cancel or modify, and, for a limit or market order, its side is buy or sell, and, for a limit order, its time in force is GTC, IOC or FOK. An out-of-range command is refused at ingress:

- `Book` returns without taking a sequence number, so the next valid command gets the number it would have had. Nothing rests, nothing trades, the book is unchanged.
- `journal::Writer::append` returns `Invalid` and writes nothing, and the writer stays usable. `Reader` rejects the same values as `BadKind` or `BadField`, so what the writer produces and what the reader accepts agree.
- A record holding such a value is never applied by replay: `Reader` stops before it and reports `BadKind` or `BadField`. `replay::apply` on a command built in memory does nothing, and the event-log formatter prints the value as `side=invalid(2)` or `tif=invalid(3)` instead of as sell or fok.

Zero quantity and a duplicate id are different: they are valid commands that fail, they take a sequence number, and they are rejected as before (ADR-009).

### Matching rules (S5, S6)

- **Price-time priority.** The best opposite level is matched first. Within a level, the order that arrived first fills first. "Arrived" means accepted by the engine, not received by a queue in front of it.
- **Fill price.** Every trade executes at the resting order's price, never the incoming order's.
- **Partial fills.** A partly filled resting order stays at the front of its level with its original priority.
- **GTC / IOC / FOK.** GTC rests any residual. IOC cancels it. FOK first checks how much is available at its limit, and if that is less than the order's quantity it is rejected whole, without touching the book. The available quantity is summed in 64 bits, so two resting orders of 2^31 lots do not wrap to zero, and the walk stops as soon as the order's quantity is covered.
- **FOK and self-trade prevention agree.** The FOK pre-check walks the book the same way the crossing loop does, including stopping at a self-match. A pre-check that ignored this would say yes, then cross, then stop partway and leave a partial fill behind. Conformance test 14b exists for exactly that case.
- **Pool full.** If a limit order's residual cannot rest because the pool is full, only the residual is dropped and flagged (`pool_exhausted`). Trades it already made stand.

### Modify (S2)

- A modify never frees the order's pool slot, so its handle and id stay stable for its whole life. Only a cancel frees a slot.
- Reducing quantity at the same price keeps time priority and updates in place.
- Increasing quantity or changing price moves the order to the back of its level (or a new level) with a new sequence number.
- A modify that would cross the book is rejected (ADR-009). A caller who wants the trade cancels and resubmits.
- A same-level reposition is unlink then append as one operation. Swapping in place while still linked corrupts FIFO order for every order behind it (conformance test 12b).

### Self-trade prevention (S3)

When an incoming order reaches a resting order with the same account, the incoming quantity is rejected and the resting order is untouched. Trades made before the self-match stand. This is the simplest deterministic policy and needs no relinking on the resting side. Real venues offer several policies (cancel oldest, cancel both, decrement); only this one is implemented.

### Sequencing and atomicity (S6, S8)

- The core assigns `seq_no` itself, one per command, in the order commands are processed. There is exactly one source of ordering truth, and producers never supply it.
- Every command takes a sequence number whether or not it changes the book, so a command's number is its position in the input. Trades carry the number of the command that made them. A resting order carries the number of the command that rested or last repositioned it.
- Each command runs to completion before the next starts. This is atomic by construction (one thread), not by a lock, and a command cannot stop part way because the entry points do not throw (next section). Any future change to the dequeue loop (for example ingress batching) must state in its ADR how it preserves this.

### Failure and recovery (S8, [ADR-012](docs/adr/ADR-012-failure-and-recovery.md))

- The six `Book` entry points that change the book (`add_limit_order` and `add_market_order` in two forms each, `modify_order`, `cancel_order`) are `noexcept`. If something did throw, the command would be half applied, and nothing in the book could say which half. The process ends in `std::terminate` instead.
- The pool, id map and level arrays never grow. The one thing a command can grow is `AddResult::trades`, and a sweep makes at most one trade per resting order, so reserving the pool capacity once at startup means no command allocates. `replay::replay` and the reference loop in `tests/support/journal_loop.hpp` both do this.
- The loop appends a command to the journal and applies it only if the append succeeded. Recovery is replay: after a stop, replaying the journal gives the state an uninterrupted run reaches.
- What this does not cover: the by-value overloads still allocate and are not `noexcept`, nothing is `fsync`ed, and a journal cannot be reopened for appending (section 11).

### Determinism rules (S7)

- Output is a pure function of the command sequence.
- Hash maps are used for lookup only and are never iterated where output depends on the order.
- No floating point and no wall-clock time in engine state. Timestamps may exist only in the journal wrapper.
- Every test build runs under ASan and UBSan.

## 6. Architecture

### Current shape and planned shape

```mermaid
flowchart TB
    subgraph today["Today: in-process, one thread"]
        direction LR
        cmd["Command<br/>limit, market, modify, cancel"] --> book["Book"]
        book --> ids["IdMap<br/>order_id to Handle"]
        book --> lv["PriceLevels x2<br/>sorted arrays of Level<br/>best level last"]
        lv --> pool["FixedPool of Order<br/>48 bytes each<br/>FIFO links live inside the orders"]
        ids --> pool
        book --> res["AddResult<br/>trades and flags<br/>owned by the caller"]
    end
    subgraph planned["Planned, not built"]
        direction LR
        prod["Producer"] -.-> in["SPSC ring"] -.-> core["Matching core<br/>single thread, owns the book"] -.-> out["SPSC ring"] -.-> sink["Sink"]
    end
    today ~~~ planned
```

The planned pipeline is deliberately last, so concurrency is added to a core that is already proven correct.

### Data structures

| Piece | What it is | Why |
| --- | --- | --- |
| `FixedPool<Order>` | One vector allocated at construction, never resized. Free slots form an intrusive list threaded through the slots themselves | No allocator calls. Direct indexed access makes cancel and modify O(1) |
| `Handle` | 64 bits: pool index in the low 32, generation in the high 32. The generation increments when a slot is freed | A stale handle fails validation instead of silently aliasing whatever order now occupies the slot. Tests 8b and 8c force this case. A slot whose generation would reach the maximum is retired and never reused, so a handle cannot come back to life after 2^32 frees ([ADR-001](docs/adr/ADR-001-order-storage.md)) |
| `Order` | 48 bytes (measured), hot fields first: price, qty, next, prev, side. Cold fields after: id, account, seq_no | Under one 64-byte cache line, so a separate hot/cold array split would only add indirection |
| `Level` | `price`, `head`, `tail` (pool indices) | An order's links live in the order itself, so a level is three words |
| FIFO per level | Intrusive doubly linked list through pool slots | Append, cancel and fill-from-front are O(1) with no node allocation |
| `IdMap` | Flat open-addressed table of 16-byte entries. Power-of-two size at least twice the pool capacity, linear probing, splitmix64 mixing, backward-shift erase, never grows. An insert past its capacity is refused in release builds too, not only asserted | Replaced `std::unordered_map`, which allocated a node per insert. No tombstones, so cancel-heavy workloads do not lengthen probe runs |
| `PriceLevels` | One array per side sorted worst to best, so the best level is the last element. Allocated once for pool capacity + 1 levels. A new level is refused when the array is full, in release builds too | Matching touches the best level and sweeps remove from that end, so both are O(1) with no shifting. **Interim container, see below** |

### The matching loop

For an incoming limit order the core does this, in order:

1. Reject a side or time in force that does not exist, before anything else, with no sequence number. Otherwise take the next `seq_no`. Reject a zero quantity or an id that already rests.
2. If FOK, run the read-only pre-check. Reject whole if the available quantity is short. The walk stops once the quantity is covered.
3. Walk the opposite side from its best level. At each level, walk the FIFO from the head. For each maker: stop on a self-match, otherwise fill `min(incoming, maker)` at the maker's price and record the trade. A fully filled maker is unlinked, removed from the id map and freed. A drained level is popped from the array end.
4. If the order is GTC with a residual and no self-match stopped it, allocate a slot, append it to its level (creating the level if needed) and register its id.

```mermaid
flowchart TD
    V{"Side and tif<br/>valid?"} -- no --> R0["Refuse: no seq_no,<br/>book untouched"]
    V -- yes --> A["Take next seq_no"]
    A --> B{"qty = 0 or<br/>id already resting?"}
    B -- yes --> R["Reject, book untouched"]
    B -- no --> C{"FOK?"}
    C -- yes --> D["Read-only pre-check<br/>same walk as the sweep"]
    D --> E{"Enough quantity<br/>before any self-match?"}
    E -- no --> R2["Reject whole order,<br/>book untouched"]
    E -- yes --> F
    C -- no --> F["Sweep the opposite side<br/>from its best level"]
    F --> G{"Quantity left and<br/>next maker marketable?"}
    G -- no --> K
    G -- yes --> H{"Maker has the<br/>same account?"}
    H -- yes --> S["Stop: reject the rest of the<br/>incoming quantity, never rest it"]
    H -- no --> I["Fill min(incoming, maker)<br/>at the maker's price, record trade"]
    I --> J{"Maker<br/>exhausted?"}
    J -- yes --> J2["Unlink, erase id,<br/>free slot, pop level if empty"]
    J -- no --> G
    J2 --> G
    K{"GTC order with<br/>residual left?"}
    K -- yes --> L["Allocate a slot, append to its level,<br/>register id<br/>(pool full: drop residual, flag it)"]
    K -- no --> M["Done: an IOC or market<br/>residual is cancelled"]
```

### Complexity

`L` is the number of price levels on a side.

| Operation | Cost |
| --- | --- |
| Id lookup | O(1) expected |
| Cancel | O(1) unlink, plus an O(L) array shift if the level empties away from the touch |
| Rest at an existing level | O(log L) to find the level, O(1) to append |
| Rest at a new level | O(log L) search plus O(L) memmove |
| Match against the touch | O(1) per fill, because the best level is the last array element |
| Modify | O(1) for a reduce, otherwise cancel plus rest cost |
| FOK pre-check | O(orders it walks): it stops when the quantity is covered, at the first level that does not cross, or at a self-match. It reads the same orders the sweep that follows will fill, plus at most one more |

The O(L) shifts are the known worst case of the price-level container. On W1 they were not the bottleneck (section 10), so the page table that would remove them is deferred until a measurement says they matter.

### Memory behaviour

The pool, id table and level arrays are sized from one number, the pool capacity, when the `Book` is built. Nothing grows afterwards. Level arrays hold capacity + 1 levels per side because the resting step creates a level before it learns the pool is full. A full pool is a clean rejection, not a crash, and so is a full id table or a full level array: each refuses the insert and the order is not rested. Because all three are sized from the same number, the last two cannot be reached through `Book`; they are tested on the containers directly (section 11). The id table costs 16 bytes per entry at twice the pool capacity, so a pool of 65,536 orders gets a 2 MiB table.

## 7. Design decisions and the alternatives that lost

Each decision has an ADR with the numbers behind it. The right column is what was rejected and why.

| Decision | Alternative rejected | Why |
| --- | --- | --- |
| Generation-counted handles ([ADR-001](docs/adr/ADR-001-order-storage.md)) | Raw indices | A stale index silently aliases a reused slot, so a late cancel corrupts someone else's order |
| Fixed pool, no heap ([ADR-001](docs/adr/ADR-001-order-storage.md)) | `shared_ptr<Order>` | Allocates per order. Incompatible with the zero-allocation requirement |
| Single `Order` struct, hot fields first ([ADR-002](docs/adr/ADR-002-hot-cold-split.md)) | Struct-of-arrays | At 48 bytes the whole order fits one cache line, so splitting adds an indirection and no locality |
| Intrusive list per level ([ADR-004](docs/adr/ADR-004-fifo-intrusive-list.md)) | `std::list` | Node allocation per insert |
| Flat open-addressed id map ([ADR-005](docs/adr/ADR-005-order-id-lookup.md)) | `std::unordered_map` | One heap node per insert was 42% of all allocations |
| Backward-shift erase ([ADR-005](docs/adr/ADR-005-order-id-lookup.md)) | Tombstones | W1 is 30% cancels, so tombstones would pile up and lengthen every probe |
| Table at most half full ([ADR-005](docs/adr/ADR-005-order-id-lookup.md)) | Denser table | About 1.5 probes per hit and 2.5 per miss at the limit. Memory is the price |
| Caller-owned `AddResult` ([ADR-008](docs/adr/ADR-008-allocation-sources.md)) | Return a fresh result | The trades vector was 31% of allocations. The by-value forms still exist and still allocate |
| Journal is replay authority ([ADR-006](docs/adr/ADR-006-event-log-journal.md)) | Replay from the event vector | Two records of "what happened" can diverge after a batching change. The journal is what ingress actually fed the core |
| Per-record CRC-32 ([ADR-010](docs/adr/ADR-010-journal-format.md)) | One checksum per file | A torn final record should not invalidate every record before it |
| Fixed record size per kind ([ADR-010](docs/adr/ADR-010-journal-format.md)) | Length field | A length field is one more thing a flipped bit can corrupt |
| No `fsync` per command ([ADR-010](docs/adr/ADR-010-journal-format.md)) | `fsync` each record | A flush survives process death. `fsync` costs a disk round trip per command and only buys power-loss safety, which is not claimed |
| Sorted-array levels ([ADR-003](docs/adr/ADR-003-price-level-container.md)) | `std::map` (kept as the baseline) | `std::map` was the last allocation source. The array removes it, and on the pinned-core W1 run it also lowered median and p99 latency. Its worst case, a deep book with a level erased far from the touch, shifts kilobytes (section 10) |
| Reject a crossing modify ([ADR-009](docs/adr/ADR-009-spec-gaps.md)) | Let modify trade | Keeps the book never-crossed and keeps modify a pure reposition |
| Refuse a command with an invalid side or time in force, before it takes a sequence number ([ADR-011](docs/adr/ADR-011-command-validation.md)) | Treat any other value as a sell or as IOC | A value outside the enum is not a command, so it must not change the numbering of the commands after it, in the book or in a replay |
| End the process if a command cannot complete ([ADR-012](docs/adr/ADR-012-failure-and-recovery.md)) | Catch the exception and undo the half-applied command | Undo needs a record on every command, in the path the benchmark measures, to cover an event the startup reserve already removes. The journal already holds what a restart needs |
| Retire a pool slot at the maximum generation ([ADR-001](docs/adr/ADR-001-order-storage.md)) | Let the generation wrap | A wrapped generation makes a very old handle valid again. A retired slot is one slot lost per 2^32 frees of that slot |
| Bound what replay allocates ([ADR-010](docs/adr/ADR-010-journal-format.md)) | Trust the header and the file | The header names the pool size and the file length decides the log length, so a damaged or hostile journal could exhaust memory before replay reported anything |

## 8. Verification

The engine and its checker share only the type aliases, so a bug has to be written twice to survive.

```mermaid
flowchart TB
    subgraph diff["1. Two implementations must agree"]
        direction LR
        spec["docs/spec.md<br/>S1 to S8, ADR-009"] --> eng["Engine"]
        spec --> orc["Oracle<br/>flat linear scan,<br/>shares only the types"]
        gen["Seeded fuzz and<br/>exhaustive enumeration"] --> eng
        gen --> orc
        eng --> cmp{"Same results,<br/>same book?"}
        orc --> cmp
    end
    subgraph guard["2. The tests must be able to fail"]
        direction LR
        mut["163 injected<br/>single-line faults"] --> suite["Test suite: conformance, differential,<br/>golden bytes, zero allocation, ASan and UBSan"] --> kill["Each fault must<br/>break some test"]
    end
    diff --> guard
```

| Layer | What it checks | Where |
| --- | --- | --- |
| Conformance tests (1 to 19, with variants 8b, 8c, 12b, 12c, 12d, 14b, 14c, 14d, 15b, 18b, 18c) | Resting, crossing, sweeps, partial fills, FIFO, cancel, modify, IOC, FOK (including 64-bit sums and the early exit), market, self-trade prevention, invalid input, invalid side and time in force, stale handles, pool exhaustion | `tests/conformance/` |
| Reference oracle | A flat, linear-scan model of the spec with no pool, handles or lists | `tests/support/oracle.hpp` |
| Differential fuzz (test 20) | Seeded random streams through engine and oracle in lockstep, comparing every result and periodically the full book. CI: 100 seeds x 1,000,000 commands in an optimized sanitized build. Zero divergences. The generator must actually produce rejected crossing modifies | `tests/fuzz/fuzz_diff.cpp` |
| Exhaustive search (test 21) | Every command sequence to a fixed depth over a small alphabet, three passes, about 137 million prefixes at full depth | `tests/fuzz/exhaustive_diff.cpp` |
| Invariant checker | After commands: list links agree in both directions, tails are right, no cycles, no empty levels, levels strictly ordered, each order sits at its own price and side, no zero-quantity resting order, sequence numbers increase within a level, the id map matches the pool exactly, pool slots in use equal linked orders (retired slots excluded), and the book is not crossed | `include/engine/invariants.hpp`, called through `Book::invariant_violation()` |
| Invariant checker tests | Each reachable violation message is produced from a deliberately corrupted pool, level array or id map and compared by exact text, so a check that is deleted or inverted fails a test. The cycle message is unreachable behind the earlier link check and is documented as such | `tests/unit/test_invariants.cpp` |
| Unit tests | `FixedPool` generation transitions and retirement, `IdMap` and `PriceLevels` capacity refusals (compiled with `NDEBUG` so the release behaviour is what is tested), journal encode, decode and writer status, `load` bounds | `tests/unit/` |
| Fail-stop (`unit_fail_stop`) | `noexcept` on the six entry points, zero allocation and zero throws under a failing allocator for worst-case sweeps and a self-trade mid-sweep, replay equal to an uninterrupted loop for every stopping point up to 120 commands, and a forked death test for the terminate path | `tests/unit/test_fail_stop.cpp` |
| Mutation testing | 163 hand-written single-line faults in nine engine headers. Each is applied to a copy, rebuilt, and the suite must fail. All 163 killed. A survivor fails CI, and so does a pattern that no longer matches its header | `tools/mutation_check.sh` |
| Determinism (tests 22, 23) | Replay is identical twice and equal to the live run; golden journal, event log and book compared byte for byte | `tests/replay/` |
| Zero allocation (test 25) | No `operator new` call across W1 (three seeds) and a stress stream covering IOC, FOK, market, modify, self-trade, duplicate ids and a full pool of 8 | `tests/bench/test_zero_alloc.cpp` |
| Harness validation (tests 26, 27) | Timer noise floor and measurement overhead | `bench/`, `tests/bench/` |
| Sanitizers (test 24) | ASan and UBSan on every test build, locally and in CI. UBSan reports are fatal, and `ubsan_aborts` checks that a deliberate signed overflow does abort | `CMakeLists.txt`, `.github/workflows/ci.yml`, `tests/unit/test_ubsan_death.cpp` |

**What mutation testing showed.** The conformance tests alone missed bugs that the differential tests caught. In the first mutation run, 5 of 19 mutants were killed only by the oracle comparison: strict crossing on the sell side, an equal-quantity modify losing priority, a modify not advancing the sequence number, a filled order left in the id map, and a failed rest leaking an empty level. Most of the replay mutants are killed only by the golden files. That is the reason those layers exist.

**What this does not establish.**
- The engine and the oracle were written from the same spec by the same author. Differential testing catches implementation slips, not a misreading both share. That is why the open spec points were recorded as ADR-009 instead of being left implicit.
- The mutants are a hand-picked list, not a systematic operator set. All killed means these faults are caught, not that no fault survives.
- A few mutants are equivalent in output, so only a test that reads memory past the point the code should have stopped can kill them. The FOK early-exit mutants are the example: the answer is the same with or without the early exit, so `test15b` corrupts a level that is reachable only by walking past the boundary and relies on ASan to catch the read.
- Fuzzing and exhaustive search cover the spec as written. They say nothing about behaviour a real venue has and this spec omits (auctions, other STP policies, multiple instruments).

## 9. Determinism and replay

### Journal format ([ADR-010](docs/adr/ADR-010-journal-format.md))

A 20-byte header, then one record per command, nothing in between. All integers little-endian and written byte by byte, so the file is identical on every machine and compiler.

| Header bytes | Field |
| ---: | --- |
| 0 to 7 | magic `LOMEJRNL` |
| 8 to 11 | format version (1) |
| 12 to 15 | pool capacity the book was built with |
| 16 to 19 | CRC-32 of bytes 0 to 15 |

| Kind | Command | Payload | Record size |
| ---: | --- | --- | ---: |
| 1 | limit | id u64, side u8, tif u8, price i64, qty u32, account u32 | 31 bytes |
| 2 | market | id u64, side u8, qty u32, account u32 | 22 |
| 3 | cancel | id u64 | 13 |
| 4 | modify | id u64, price i64, qty u32 | 25 |

Each record ends with a CRC-32 of its kind byte and payload. There is no length field and no stored sequence number, because the sequence number is the record's position and storing it would store one fact twice. The header carries the pool capacity because a full pool changes what a command does, so a replay into a differently sized book could diverge with every record intact.

The reader stops at the first bad record and reports why (`Truncated`, `BadChecksum`, `BadKind`, `BadField`) together with the length of the valid prefix. The writer flushes each record and a caller appends before it runs the command.

### Replay and golden tests

```mermaid
flowchart TB
    subgraph live["Live run"]
        direction LR
        c1["Command"] --> w["Journal writer<br/>append and flush first"] -->|"append succeeded"| b1["Book executes it"] --> e1["Events"]
    end
    subgraph rep["Replay"]
        direction LR
        j["Journal file<br/>header and CRC-checked records"] --> r["Reader<br/>stops at the first bad record"] --> b2["Fresh Book<br/>capacity read from the header"] --> e2["Events and book dump"]
    end
    w --> j
    gold["tests/golden/ files"] --> g{"Byte-identical?"}
    e1 --> g
    e2 --> g
```

- Replay builds a book from the header's capacity, applies records in order and stops at the first bad one, so a torn or corrupted journal replays exactly to its valid prefix.
- Replay is bounded (`ReplayLimits`, [ADR-010](docs/adr/ADR-010-journal-format.md)). A journal over any of four bounds (pool capacity 2^20, journal size 4 GiB, 200,000,000 commands, event log 4 GiB by default) ends with a `Limit` and is never clamped: a smaller book would replay to a different state. A limit that is hit part way leaves a prefix of the unlimited run, byte for byte, and a command's log lines are appended whole or not at all. `journal::load` with a bound refuses a larger file while reading it. If the log or its staging buffer cannot grow, replay can still throw `std::bad_alloc`, and a log line that cannot be formatted throws `std::runtime_error`.
- **Test 22** replays twice and requires identical bytes, requires replay to equal the live run, and exercises every truncation point and every single-bit flip of a journal.
- **Test 23** compares a checked-in golden journal, event log and final book (`tests/golden/`, a fixed seed and 400 commands) byte for byte. Any change in engine output breaks the build and has to be regenerated deliberately with `test_golden --update`, so a behaviour change cannot pass as a refactor. Separately, the journal unit test checks the writer against bytes produced by an independent Python encoder (`tools/journal_golden.py`), so the layout is what the ADR says and not merely what the writer and reader agree on.

### Known gaps in the journal

No `fsync` (survives process death, not power loss). No detection of deleted or reordered records, since each record is checked alone. Replay holds the whole journal in memory, up to the size limit. Reopening a journal and appending after recovery is not built. The SIGKILL crash-recovery test is the next M4 item.

## 10. Performance

### How to read these numbers

The full methodology is [`docs/benchmark-methodology.md`](docs/benchmark-methodology.md). In short:

- Per-operation latency uses `rdtscp` plus `lfence`. Throughput is timed over a whole batch so it excludes timestamp cost. The two come from separate passes over the same command stream, which must produce the same number of trades or the run aborts.
- The first 100,000 operations are warm-up. A percentile is reported only with at least 100 samples in its bucket. Limit-order and cancel latency are reported separately because their tails differ.
- Every result file starts with an environment block: CPU, pinned core, governor, turbo state, RAM, kernel, compiler and flags, timestamp-counter type and the binary's hash.
- The harness must pass its own checks first. The noise-floor test times an empty region and bounds the p99 spread, and the overhead test quantifies the cost of the timer itself.
- Only pinned-core Linux runs are published. `tools/run_baseline.sh` refuses to run unless the governor is `performance` and turbo is off, and it never overwrites an existing result.

**Workload W1** (versioned, never edited in place): seed 1, 1,000,000 measured operations after 100,000 warm-up, 10,000-tick price span, about 1,000 levels per side, Zipf-distributed prices, about 30% cancels, 50/50 sides, sizes 1 to 100. A fixed cancel probability did not hold the book at any stable depth (at 40% it grew to about 200,000 orders, at 45% it collapsed), so the cancel probability scales with book size. W1 contains limit orders and cancels only.

### Baseline: the first correct version, before any optimization

`std::map` price levels and `std::unordered_map` id lookup. Intel Core i5-1135G7 laptop, core 3 pinned, `performance` governor, turbo off (2.4 GHz), Linux 7.0.0-38, GCC 15.2, `-O3 -march=native -fno-plt`. This is the committed baseline ([`bench/results/`](bench/results)); the runner of that commit builds a fresh result per command.

| Metric | Value |
| --- | ---: |
| Throughput | 4,653,363 ops/s |
| Limit order p50 / p99 / p99.9 | 206.7 / 484.0 / 776.3 ns |
| Cancel order p50 / p99 / p99.9 | 322.8 / 530.7 / 813.9 ns |

A second full run on that day agreed on throughput to 0.07% and on p50 and p99 within 1.5%, so smaller differences are not evidence. The p99.99 column did not reproduce between runs and is not quoted. The timer's own overhead (about 21 ns) is not subtracted.

**A re-run of the baseline commit on 2026-11-04 was disturbed and is not used.** Same machine and settings, kernel now 7.0.0-42, it gave 3,962,733 ops/s with p99.9 around 20 us (against 776 ns on baseline day) while the runs after it were clean, which points to interference during that run, not to the engine. The files are kept in `bench/results/` (`*-m3-repeat-*`) because deleting an unflattering run would be worse than explaining it. Because of this, **the comparisons below are all within one session on 2026-11-04, never against the baseline-day numbers.**

### Optimization campaign: allocations

Allocation counts do not depend on timing, so they are exact and need no tuned machine. They are counted with a replaced global `operator new` over W1 seed 1, 1,000,000 measured commands.

| Step | Change | Allocations | Per command |
| --- | --- | ---: | ---: |
| Baseline | | 1,205,713 | 1.21 |
| O1 | Reuse one caller-owned `AddResult` for trades | 831,515 | 0.83 |
| O2 | Flat open-addressed id map | 326,276 | 0.33 |
| O3 | Preallocated sorted-array price levels | **0** | 0 |

Where the baseline's allocations came from, measured by wrapping each call site (ADR-008):

| Source | Allocations | Share |
| --- | ---: | ---: |
| `trades.push_back` in the crossing loop | 374,198 | 31% |
| id map insert | 505,239 | 42% |
| price level node | 326,276 | 27% |
| everything else, cancels included | 0 | 0% |

![Allocations per million commands and batched throughput across the campaign](docs/img/campaign.svg)

The three rows add up to the total exactly. The counts for O1, O2 and O3 are identical on GCC 13.3 and on the benchmark machine's GCC 15.2. Only `operator new` is counted, so a direct `malloc` would not show up.

### Optimization campaign: latency

One run per version, all in the same session on 2026-11-04: core 3 pinned, `performance` governor, turbo off, GCC 15.2, W1 seed 1, 1,000,000 measured operations. Each version was built from its own commit. "main" is the current head, which adds the ADR-009 and journal changes on top of O3.

| Version | Throughput (ops/s) | Limit p50 / p99 / p99.9 (ns) | Cancel p50 / p99 / p99.9 (ns) |
| --- | ---: | ---: | ---: |
| O1: reused result, `std::map` + `std::unordered_map` | 4,747,865 | 205.4 / 437.7 / 706.0 | 321.2 / 515.0 / 667.2 |
| O2: + flat id map | 5,687,100 | 201.7 / 555.1 / 2,762.1 | 338.5 / 746.9 / 3,041.9 |
| O3: + sorted-array levels | 5,455,611 | 132.7 / 433.6 / 2,722.8 | 304.2 / 572.9 / 2,743.5 |
| main | 5,440,715 | 132.7 / 432.8 / 2,702.5 | 303.4 / 572.9 / 2,719.1 |

![Latency by build on W1: p50, p99 and p99.9 for limit orders and cancels, log scale](docs/img/latency.svg)

What the table says, from O1 to main:

- **Throughput +14.6%.** Median limit latency **-35%** (205.4 to 132.7 ns), median cancel -5.5%, limit p99 unchanged.
- **Cancel p99 +11%** and **p99.9 about 4x worse** for both order kinds (706 to 2,702 ns limit, 667 to 2,719 ns cancel). The p99.9 jump arrives at O2 and O3 does not change it.
- **O2 to O3:** limit p50 -34%, limit p99 -22%, cancel p50 -10%, cancel p99 -23%, p99.9 unchanged, but batched throughput **-4.1%** (5.69M to 5.46M).
- **main equals O3** within the spread, so the ADR-009 and journal changes cost nothing measurable on W1.

How much to trust it:

- It is a single run per version. The documented run-to-run spread on this setup is about 1.5% at p50 and p99 and about 8% at p99.9, so the p99.9 jump and the O3 p50 and p99 gains are far outside it. The documented throughput spread on baseline day was only 0.07%, but the control re-run in this session was disturbed (see above), so treat single-run differences of a few percent, including the 4% throughput gap between O2 and O3 and the cancel p50 differences, as indicative and not established.
- The p99.9 level of about 2.7 us shows up in three separate builds (2,762 / 2,723 / 2,703 ns), which argues against a one-off disturbance. A repeat session is the way to confirm.
- The latency passes time each command with `rdtscp` plus `lfence`, which serializes the pipeline, while the throughput pass runs the batch unserialized. They can legitimately disagree (O3 better at p50 and p99 yet slower in batch). I have not isolated why.
- p99.99 and max are not interpreted.

**Cause of the p99.9 increase: not established.** Hypothesis, untested: the benchmark book has a pool of 65,536 orders, so the O2 id table is 131,072 entries of 16 bytes, 2 MiB, hit at effectively random addresses on every command, while only about 3,000 orders are live. The `std::unordered_map` it replaced kept those few thousand nodes close together. If that is right, a smaller table, or a hash that keeps nearby ids nearby, would bring the tail back. Other candidates (probe-run length under backward-shift erase, TLB misses) are equally unmeasured. This is the next item.

### Cost of the hardening series

Measured on the same machine as the rest of this section, on core 3 with governor `performance` and turbo off. `power-profiles-daemon` and `auto-cpufreq` were stopped for the run, no other process above 5% CPU was on the core or its hyperthread sibling, and every results file records the governor, the turbo state and the pinned core. Kernel 7.0.0-43, GCC 15.2.0. The flags are `-march=native -fno-plt` on top of Release's `-O3 -DNDEBUG`, with no duplicate `-O3`, so **these numbers are not comparable with the tables above** (the methodology document explains the flag change). Both columns were built and run in this one session.

"Before" is 55617ba, the commit the series started from. "After" is 33bb4b6, Commits 01 to 17. W1 seed 1, 1,000,000 measured operations, three runs per version, interleaved (before, after, before, after, ...). Cells are the mean of three, with the range in brackets.

| W1 | Before (55617ba) | After (33bb4b6) | Change |
| --- | ---: | ---: | ---: |
| Throughput, ops/s | 5,468,602 (5,458,554 to 5,482,718) | 5,469,965 (5,463,704 to 5,473,326) | +0.02% |
| Limit p50, ns | 131.6 (131.0 to 131.9) | 135.0 (134.3 to 136.0) | **+2.6%** |
| Limit p99, ns | 429.2 (427.8 to 431.1) | 433.1 (432.4 to 434.0) | +0.9% |
| Limit p99.9, ns | 2,608.7 (2,557.9 to 2,651.3) | 2,485.0 (2,440.5 to 2,553.8) | -4.7% |
| Cancel p50, ns | 302.6 (301.3 to 303.4) | 300.8 (300.1 to 301.3) | -0.6% |
| Cancel p99, ns | 570.3 (568.4 to 572.1) | 569.1 (568.0 to 569.6) | -0.2% |
| Cancel p99.9, ns | 2,573.3 (2,462.8 to 2,631.0) | 2,463.6 (2,412.8 to 2,530.2) | -4.3% |

- **Throughput did not change detectably.** The difference between the means, 0.02%, is smaller than the spread between runs of the same version (0.44% before, 0.18% after).
- **The limit-order median is slower by about 3.4 ns, roughly 8 cycles at 2.4 GHz.** That is outside the 1.5% p50 spread documented for this setup, and the three runs of each version do not overlap, so it is treated as real. Limit p99 (+0.9%) and both cancel figures are inside the 1.5% spread.
- **The p99.9 figures are lower by 4 to 5%.** The documented spread at p99.9 is about 8%, so no improvement is claimed.
- **Cause not isolated.** W1 sends no FOK and no invalid commands, so what a limit order gained is the side and time-in-force check before the sequence number, the capacity check on an id insert, the level insert returning a pointer, and `noexcept`. The series was measured as a whole, not commit by commit.

W2 (fill-or-kill, [methodology](docs/benchmark-methodology.md)), 200,000 rounds per scenario, three runs per version. "Before" is the "after" tree with `book.hpp` taken from the commit before the early exit, so the two differ only by that change.

| W2 | Before | After | Change |
| --- | ---: | ---: | ---: |
| Deep (4,000 orders in reach) p50, ns | 8,897 (8,895 to 8,899) | 86 (81 to 94) | about 100x lower |
| Deep p99, ns | 10,844 (10,793 to 10,896) | 126 (120 to 131) | about 85x lower |
| Deep p99.9, ns | 13,082 (12,735 to 13,337) | 197 (182 to 213) | about 65x lower |
| Shallow (4 orders in reach) p50, ns | 83.5 (81.8 to 85.2) | 85.9 (82.3 to 93.0) | no change established |
| Shallow p99, ns | 170 (167 to 174) | 147 (108 to 167) | no change established (one low run) |

- Before the early exit the deep FOK cost about 2.2 ns for each of the 4,000 orders it walked. After it, the deep and shallow medians are the same, 86 ns.
- **This is the best case for the early exit.** In W2 the first order in reach covers the whole FOK, so the walk stops at once. A FOK that needs most of the book still walks most of it.
- The "after" deep medians rose across the three runs (81, 84, 94 ns), so quote them to about 10%, not to the nanosecond.
- In all six noise files the noise-floor p99 spread was 0.0 ns. All overhead files report `one_percent_rule: exceeded`, as the earlier ones do: the 21 ns timer cost is more than 1% of a 130 ns operation, which is why latency is compared between versions and not read as absolute.

Result files: `bench/results/ubuntu-i5-1135G7-hardening-{base,head}-r{1,2,3}-{noise,overhead,w1}.csv` and `ubuntu-i5-1135G7-hardening-fok-{before,after}-r{1,2,3}-w2.csv`.

### The price-level container

Before the pinned run, an unpinned A/B in a shared sandbox suggested the sorted array was much slower than `std::map` (cancel p99 about ten times worse). **The pinned run on the benchmark machine did not reproduce that.** Cancel p99 went from 746.9 ns (O2, `std::map` levels) to 572.9 ns (O3). The shared sandbox result was an artifact of the environment and has been withdrawn.

The mechanism behind the original worry is real and measured: a level created or erased away from the touch moves every level behind it.

| Operation | Count per 1M commands | Mean levels moved | Mean bytes moved | Most |
| --- | ---: | ---: | ---: | ---: |
| Insert a level | 326,276 | 304.6 | 4.8 KB | 1,167 |
| Erase a level | 159,155 | 622.5 | 9.7 KB | 1,165 |

About 3 KB per command on average, and on this machine it does not dominate: the copies are sequential, hardware-prefetched memory moves, cheaper than the pointer-chasing `std::map` they replaced. The cost scales with the number of levels, so a book much deeper than W1's roughly 1,000 per side would move more. W1 is the only workload run, so no claim is made about other book shapes.

The page table (index levels by price offset, no shifting) is therefore deferred. It also needs a spec change, a price band taken at construction with out-of-band orders rejected like an exchange price collar, which gets its own ADR if a measurement ever justifies it.

## 11. What is not done

- **The p99.9 regression that arrived with O2.** About 4x worse than O1 on W1, cause unknown (hypothesis in section 10).
- **Repeat runs.** The latency table is one run per version in one session. A second session, with all power managers stopped, would tighten it.
- **Other workloads.** W1 (limit orders and cancels, one account per order) and W2 (fill-or-kill on shallow and deep books) have pinned-core results. No modify, IOC, market or self-trade traffic is benchmarked, so the cost of the hardening series on those paths is unknown. `modify_order` gained a capacity check in the series.
- **Cause of the 2.6% limit-order p50 increase.** The series was measured as a whole against 55617ba, not commit by commit, so the cause is not isolated (section 10).
- **The FOK early exit is measured at its best case only.** In W2 the first order in reach covers the order, so the walk stops at once. A FOK that needs most of the book has not been measured.
- **Crash recovery.** The SIGKILL-at-random-offsets test, `fsync`, record-chain integrity and resume-append are not built.
- **Unreachable guards.** `Book::rest` refuses when the id map or level array is full, and `modify_order` ends the process if a level cannot be created. Both arrays are sized so this cannot happen through `Book`, so those branches have no test; the containers' refusals are tested on their own.
- **By-value overloads.** They still allocate and are not `noexcept`, so the fail-stop contract covers the caller-owned `AddResult` forms only.
- **Concurrency.** The SPSC ring pipeline is planned and deliberately last.
- **Cache-line layout, branch reduction, prefetching, ingress batching.** Not started. Failed attempts will be documented in the same table as successful ones.
- **Page-table price levels.** Deferred, see section 10.
- **Scope limits.** Single instrument, single tick size, no network layer, no odd-lot book, no auctions, and self-trade prevention is the only protection policy.

## 12. Engineering log: what went wrong

Real bugs and wrong decisions, kept because each one changed the process.

| What happened | How it was found | What changed |
| --- | --- | --- |
| A use-after-free: a full pool returned an invalid handle and its index went straight into an unchecked write past the pool | ASan, in M1 | `rest()` checks the handle. Tests 18 and 19 cover pool exhaustion |
| ADR-002 listed `timestamp` as an `Order` field. S7 forbids wall-clock state in the core | Writing the actual struct | Field removed. ADR corrected and the mistake noted in it |
| The spec left four behaviours open (sequence numbers on trades, whether a modify may cross, pool exhaustion, duplicate ids). The oracle had been built to follow the engine on them, which is not the same as deciding them | Review before generating golden files | ADR-009 settles all four |
| A fixed cancel probability gave an unstable book in the workload generator | Long generator runs | Cancel probability scales with book size |
| Power management turned turbo on mid-benchmark. The clock rose from 2.4 to about 3.8 GHz and the timer cost fell from 22 to 12 ns. The noise-floor test passed with a spread of 8.3 ns | The harness's own overhead check | Baseline script refuses unless governor and turbo are right, and the methodology records the case and why the 10 ns limit is too loose |
| The zero-allocation claim was specified in M0 but could not pass until the allocation work was done | Building test 25 | Test 25 fails on the O2 engine and passes only on O3, so it is a real check and not a rubber stamp |
| I concluded from an unpinned, shared-sandbox A/B that sorted-array levels were about 10x worse on cancel tails, and the repo's ADR-003 and a commit message recorded that as a regression | The pinned-core run on the benchmark machine: cancel p99 improved 23% over O2 | The conclusion was withdrawn and the README rewritten from the pinned data. Lesson: a shared-VM A/B can mislead in either direction, so no latency claim is made without a pinned run |
| The re-run of the baseline commit came out 15% slower than the committed baseline with a 20 us p99.9 | Same-session control run | Marked as disturbed and excluded. All comparisons are within one session, never against the baseline-day numbers |
| p99.9 became about 4x worse at O2 while throughput improved | The pinned latency table | Open. Recorded in section 11 with an untested hypothesis |
| The FOK pre-check summed resting quantities in 32 bits, so two resting orders of 2^31 lots wrapped to zero and a fillable order was rejected | Run against the old engine, the boundary-biased quantity profile in the fuzzer diverged from the oracle at seed 1 (engine 0 trades, oracle 1); tests 14c and 14d isolate the wrap | 64-bit sum. The fuzzer gained a quantity profile biased to the boundaries (powers of two, `UINT32_MAX`), and a mutant that narrows the sum back |
| A command with an unknown kind was written as a 13-byte record under a good checksum. The reader stopped there, so every command accepted after it was lost on replay | On the old writer, a command with kind 9 placed after the first record of a four-command journal was appended successfully, and replay stopped with `BadKind` after one command, losing the three valid commands after it | `Writer::append` returns `Invalid` and writes nothing. ADR-011 defines which commands are valid |
| An out-of-range side rested as a sell and took a sequence number, and a time in force above 2 behaved as IOC | Tests 18b and 18c fail on the old code. A probe showed side 2 resting on the ask side with a sequence number taken, and a time in force of 3 acting as IOC | Refused before the sequence number is taken, in the book, the writer and replay |
| An allocation failure inside a command would have left it half applied: trades already made, the rest not | Failing the allocation of the trades vector let `std::bad_alloc` escape `add_limit_order`, and the next trade id was 1 instead of 0 | `noexcept` entry points, trades reserved at startup, ADR-012 |
| Replay built a `Book` of whatever capacity the journal header named, and the log grew with the file | Feeding `replay::replay` a header with a valid CRC and capacity `UINT32_MAX` requested a 192 GiB allocation (`std::bad_alloc`, or an ASan out-of-memory abort) | `ReplayLimits`, refused before anything is built, with `load` bounded while reading |
| After exactly 2^32 frees of one slot its generation wrapped and a stale handle validated again | A probe on the old pool, then pool tests with a small maximum generation | Slots retire at the maximum generation |
| The event-log line writer read past its 256-byte buffer when a field made a line longer | A test with a 400-character field, which ASan reported as a stack-buffer-overflow read | The line is formatted again at its full length, and an unencodable format throws |
| Three invariant checks could be removed from `Book` and all 20 tests still passed | Deleting them to see | The checks moved to `invariants.hpp`, and each reachable message has a test that corrupts the structure and compares the text |
| The id map and level arrays enforced their capacity only with `assert`. In a release build the id map accepted entries until its table was full and then looped forever on the next insert, and the level array wrote past its end | Compiled with `NDEBUG` and driven past capacity: the id map hung on the insert after the table filled, and ASan reported a heap-buffer-overflow write in the level array | Both refuse in release builds. Unreachable through `Book`, tested on the containers (section 11) |

## 13. Reproducing everything

```bash
# Tests (sanitized)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
ctest --test-dir build --output-on-failure

# Long differential fuzz, as in CI (optimized, sanitized)
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release
cmake --build build-rel --parallel --target fuzz_diff exhaustive_diff
./build-rel/fuzz_diff 100 1000000 1 32
./build-rel/exhaustive_diff

# Boundary-biased quantity profile, as in the ctest differential_fuzz_boundary
./build-rel/fuzz_diff 10 20000 1 8 boundary

# Mutation check (slow: about 45 seconds per mutant, about two hours for all 163)
bash tools/mutation_check.sh

# Allocation counts (any machine)
cmake -S . -B build-allocs -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF
cmake --build build-allocs --target bench_allocs
./build-allocs/bench_allocs <label> 1 1000000

# FOK workload W2 (shallow and deep books; quote it only from a pinned core)
cmake --build build-allocs --target bench_fok
./build-allocs/bench_fok <label> 200000 <cpu> > bench/results/<name>.csv

# Latency baseline (Linux, pinned core, governor and turbo set; read the methodology first)
tools/run_baseline.sh <cpu> <label>

# Redraw the charts from the committed result files (standard library only)
python3 tools/make_charts.py

# Regenerate the golden files after a deliberate behaviour change
./build/test_golden --update
```

## 14. Repository layout

```
include/engine/     header-only engine: book, pool, id_map, price_levels, level,
                    invariants, journal, command, replay, types
tests/conformance/  hand-written spec tests
tests/oracle/       oracle self-checks
tests/fuzz/         differential fuzz and exhaustive search
tests/unit/         id map, price levels, pool, invariants, journal, fail-stop, UBSan death test
tests/replay/       determinism tests (22, 23)
tests/golden/       checked-in golden journal, event log and book
tests/bench/        tests for the benchmark harness, zero-allocation test (25)
tests/support/      oracle, differential-test helpers and the reference journal loop (kept out of include/)
bench/              workload generator, timing, runners, allocation counter, results/
tools/              mutation check, baseline runner, journal golden-bytes encoder, chart generator
docs/               spec, benchmark methodology, ADR-001 to ADR-012, img/ (generated charts)
```

Documentation: [`docs/spec.md`](docs/spec.md) (S1 to S8), [`docs/adr/`](docs/adr) (ADR-001 to ADR-012), [`docs/benchmark-methodology.md`](docs/benchmark-methodology.md).


## License

MIT, see [`LICENSE`](LICENSE).

Author: Akshat Singh Rajawat
