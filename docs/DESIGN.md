# matchbox design

## 1. Process structure

```
 clients ──TCP──▶ gateway thread ──SPSC(Request)──▶ core thread ──SPSC(Event)──▶ gateway thread ──TCP──▶ clients
                  kqueue/epoll                      sequencer: seq = ++n          routes by account
                  framing, logon                    journal.append(input)  (write-ahead)
                  per-session buffers               engine.process(input)
                                                    event hash
```

- **Gateway** (one thread): non-blocking sockets on kqueue (macOS) or epoll (Linux). It handles
  framing, logon, decoding and per-session output buffers, and routes each engine event to the
  session of the event's account. A maker's fill is therefore delivered to the maker, not only to
  the taker that caused it.
- **Core** (one thread): the only place sequence numbers are assigned. Each request gets
  `seq = ++n` (a new order's id is its seq), is appended to the journal **before** it is
  processed (write-ahead), and is then processed by the engine. The journal order is the
  processing order.
- **Rings**: two bounded SPSC rings (acquire/release, 128-byte padded indices). Back-pressure in
  both directions. The gateway keeps draining the event ring while it waits for space in the
  request ring, so the two threads cannot deadlock on full rings.
- Both threads busy-poll in `--spin 1` mode (lowest latency, two cores). `--spin 0` sleeps when
  idle.

## 2. Engine data structures

| structure | representation | why |
|---|---|---|
| orders | pool of 80-byte records, 32-bit index links | no allocation per order; indices survive pool growth |
| price level | `{price, leaves, visible, count, head, tail}` in a pool | aggregates make FOK checks and snapshots O(levels) |
| side | `vector<{price, level}>` sorted worst→best, touch at `back()` | activity is at the touch: `push_back`/`pop_back`, short scans |
| id → order | open-addressing map (Fibonacci hash, backward-shift delete) | O(1) cancel/replace without pointer-chasing |
| stop books | two sorted vectors (buy: stop ascending; sell: stop descending, ties by id) | the next candidate is always `front()`; trigger checks are O(1) |
| events | `Sink` template parameter | emission inlines; no virtual call per event |

After warm-up the engine performs **zero heap allocations** (`tests/test_alloc.cpp`, and the
benchmark's `allocs_after_warmup`).

## 3. Matching loop

For a taker with quantity left, while the opposite `back()` level is marketable, take the level's
head order:
1. **STP**: if the accounts match, apply the taker's STP mode (cancel newest, oldest or both).
2. Otherwise **fill** `min(taker leaves, maker visible)` at the maker's price, emit taker and maker
   fills, and update both orders and the level aggregates.
3. An exhausted maker is removed, or, if it is an iceberg with reserve, refilled and **re-linked
   at the back** of the same level.

After the loop the order rests (GTC limit) or is cancelled (IOC/market). FOK runs a read-only
pre-check over the level aggregates, or per order when STP applies, before any state changes.
Post-only checks marketability before acceptance. Stops are evaluated after each input; each step
selects the front of the buy or sell stop book, and the cascade continues until neither front
qualifies.

## 4. Two independent implementations

`ReferenceEngine` uses `std::map<Price, std::list<Order>>`, an `unordered_map` index and a linear
scan over stops. It is written to be read against `docs/RULES.md`, not to be fast. The optimised
engine must produce byte-identical event streams:
- the differential tests (1.4M random inputs across seven flow profiles) compare every event and
  the full book state;
- `fuzz_engine` feeds arbitrary bytes (any field values) to both engines and aborts on any
  difference;
- the benchmark prints both engines' event hashes on 5M inputs, and they are identical.

Two implementations can share a misreading of the rules, so the property tests check invariants
that do not depend on either: fill pairing, no trade-through, makers trading at their own price,
quantity conservation, an uncrossed book, and level aggregates equal to their queues.

## 5. Journal and replay

Each record is `u32 len | u32 CRC32C | Input (72 B)`, buffered and flushed when a burst ends.
Durability is a policy:

| policy | guarantee | cost |
|---|---|---|
| no fsync (default) | survives process crash (page cache), not power loss | none measurable in-process |
| `--fsync-every N` | group commit: at most N acknowledged inputs lost on power loss | one fsync per N inputs |
| `--fsync-every 1` | nothing acknowledged is lost | one fsync per input (see benchmarks) |

The journal is written before the engine processes the input. A production exchange would also
**hold the ack until the record is durable**. Here acks are sent as soon as the engine emits them,
which is correct only under the no-fsync policy. Group commit with ack release is listed in the
roadmap.

Replay reads records in order, stops cleanly at a torn tail or a CRC mismatch (tested), and feeds
the inputs to a fresh engine. Because the engine is a pure function of the input sequence, the
replayed event stream is identical. `matchbox serve` prints its live event hash, and
`matchbox replay` prints the same hash.

## 6. Wire protocol

Fixed-layout little-endian frames: `u16 len | u8 type | payload`.
- Client → server: Logon (account), NewOrder (45 B), Cancel (16 B), Replace (32 B).
- Server → client: LogonAck, Event (the 72-byte engine event).

Malformed frames (bad length, unknown type, orders before logon) close the session and are
counted. Enum fields are passed through unchanged, and the engine's validation rejects
out-of-range values, so the gateway holds no business logic.

## 7. What is deliberately missing
- Multiple instruments per core: one engine instance per symbol behind a router, sharded across
  cores.
- Market-data publication: an ITCH-like outbound feed of book changes.
- Pre-trade risk checks in the gateway (position limits, fat-finger price bands, rate limits).
- Session-level sequence numbers and resend on reconnect (FIX-style).
