# matchbox: deep-dive interview questions

### 1. Walk through a buy limit order that partially fills against an iceberg and rests.
The gateway decodes the NewOrder frame, stamps the session's account and pushes it to the core
ring. The core assigns `seq` (= order id), journals it, and calls `Engine::process`. Validation
passes, and the order is not post-only, so `Accepted` is emitted. `execute` allocates nothing new:
the order already sits in a pool slot. `match` loops while the best ask is at or below the limit.
The head of the best level is the iceberg, with visible 3 and reserve 7. The engine fills 3 at
the iceberg's price, emitting a taker fill and a maker fill. The iceberg's visible slice hits 0
with reserve left, so it is unlinked, refilled to `min(display, leaves)` and re-linked at the
**back** of the same level. The loop continues with the next order at that price (or the refilled
iceberg if it is alone). When the limit is no longer marketable, the remainder rests:
`level_for` finds or inserts the bid level near `back()`, the order is linked at the tail, and
`Rested` is emitted. The core pushes each event onto the outbound ring, and the gateway delivers
it to the account it belongs to.

### 2. Why is a new order's id its sequence number?
It makes ids unique and monotonic with no extra state. It ties every order to its journal record.
And because ids increase, Fibonacci hashing spreads them well in the open-addressing map. Clients
correlate their requests with acknowledgements through `client_tag`.

### 3. What exactly happens to queue priority on replace, and why?
Same price with a smaller or equal quantity: the order is reduced in place and **keeps** its time
priority. It only became less aggressive, so the queue behind it loses nothing. Iceberg reserve is
cut first, then the visible slice. A price change or a size increase is cancel-and-new: the order
**loses** priority and can trade immediately if it is now marketable. Without this rule a trader
could hold the front of the queue with a tiny order and grow it later. The rule is tested in
`ReplaceKeepsPriorityOnlyForSamePriceDecrease`.

### 4. How does FOK interact with icebergs and STP?
FOK runs a read-only simulation before touching state. An iceberg counts at its full leaves
(visible + reserve), because a taker sweeping a level keeps matching it as it refills. With STP
off, the engine sums level aggregates, O(levels). With STP on, it walks orders, and meeting an own
order makes the FOK fail. Cancelling resting orders as a side effect of a FOK that then fails
would surprise the user. A failed FOK has no side effects at all.

### 5. Why evaluate stops after an input instead of the instant a trade happens?
Determinism and simplicity. Mid-input triggering makes behaviour depend on exactly which fill
crossed the stop and interleaves the stop's own matching with the incoming order's. After-input
evaluation gives a clean rule: each step picks the front of the buy stop book (lowest stop) if it
qualifies, else the front of the sell book (highest stop). Each triggered stop runs to
completion, and its fills can trigger more stops (cascade). The stop books are sorted so the next
candidate is always `front()`, making the check O(1) per step.

### 6. How do you know the optimised engine is correct?
- **Same rules, two implementations.** The reference engine uses std::map/std::list and is
  written to be read against RULES.md. The optimised engine must emit byte-identical event
  streams over 1.4M random inputs across seven profiles (one account, many accounts, tight book,
  wide book, stop-heavy, realistic), under libFuzzer on arbitrary input bytes, and on the 5M-input
  benchmark flow (identical hashes).
- **Shared misunderstandings** are caught by the property tests, which assert invariants rather
  than comparing implementations: fills pair up, no trade-through, makers trade at their own
  price, quantities are conserved, the book is never crossed after an input, and aggregates equal
  their queues.
- **The tests found real bugs during development.** The property test's own fill-pairing logic
  was wrong for icebergs, because a maker fill can be followed by the same pair's next fill. A
  1-allocation failure exposed a pool crossing its reserved capacity.

### 7. Why one core thread? Isn't that a bottleneck?
At 81 ns per input, one core sustains ~12M inputs/s per instrument, far above any single book's
real message rate. Single-threaded matching is what makes the engine deterministic, and
determinism is non-negotiable: recovery, replication and audits all replay the sequenced log.
Scaling is by sharding instruments across cores (per-symbol engines), not by parallelising one
book.

### 8. What does the journal guarantee, and what doesn't it?
Every input is appended (CRC32C-framed) **before** it is processed, so the journal order is the
processing order. Under the default policy, records reach the page cache when a burst ends, which
survives a process crash but not power loss. `--fsync-every N` bounds power-loss exposure to N
inputs (group commit). The gap: acks are sent as soon as the engine emits them, so under fsync
policies a client can see an ack for an input that is not yet durable. A real exchange holds acks
(or the outbound ring) until the group commit completes. That is the first roadmap item.

### 9. How is a torn journal handled?
Each record is length + CRC32C + payload. Replay stops at the first record whose length is
impossible or whose CRC fails (`corrupt`), or that is incomplete (`truncated_tail`), and reports
the offset. Tests cut a record in half and flip a byte. On restart, the exchange truncates the
journal to the last good record and replays it.

### 10. Explain the gateway/core deadlock you avoided.
If the gateway blocks pushing a request into a full core ring while the core blocks pushing
events into a full gateway ring, both wait forever. The gateway therefore drains the event ring
while it waits for request space. The core never waits on the gateway except to push events, and
those are always being drained. On shutdown, the core stops pushing once `running` is false.

### 11. Why does the load generator measure from the scheduled send time?
If latency were measured from the actual send, a slow server would make a closed-loop generator
send less, hiding exactly the queueing delay that matters (coordinated omission). mbload sends on
a fixed schedule, catches up after stalls, and timestamps each request with its scheduled time,
so queueing delay shows up in the latency.

### 12. Your end-to-end p99.9 at 10k req/s is 10 ms but 0.66 ms at 100k. Why is it worse at low load?
Both threads busy-poll, and on a laptop shared with a heavy unrelated workload, macOS preempts
them. A preemption costs a whole scheduler quantum (milliseconds). At low rates there are fewer
requests in flight, so each preemption lands on a larger fraction of measured requests, and
spinning threads also look "idle" to the scheduler more often. The engine contributes under 1 µs.
On Linux, `isolcpus` plus thread pinning, or kernel bypass, removes this class of tail.

### 13. Why a sorted array of levels instead of a tree or a price-indexed array?
Activity concentrates at the touch, and the touch is at `back()`, so most level inserts and
removes are `push_back`/`pop_back`, and a linear scan from the back finds the level in a few
steps. A tree costs an allocation and dependent cache misses per level. A price-indexed array is
O(1) but needs bounds and re-centring. It is the natural choice for a single instrument with
known price bands, and would be a sensible next step for a production venue with collars.

### 14. Why are events emitted for both taker and maker?
Each fill has two counterparties, and each must be told on their own session. The gateway routes
by the event's account, so the maker's fill goes to the maker. It also keeps the event stream
self-describing for market-data and clearing consumers.

### 15. Self-trade prevention: why three modes?
Venues offer different modes because firms want different outcomes. Cancel newest keeps the
resting liquidity (a market maker's quotes). Cancel oldest lets the new intent through. Cancel
both is the most conservative. STP is the taker's choice in this engine, as on most venues.

### 16. How would you add market data?
Every book change is already an event: Rested adds, Fill and Cancelled reduce, Replaced modifies.
An ITCH-like publisher maps events to Add, Execute, Cancel, Delete and Replace messages, with the
sequence number as the feed sequence, on its own thread from a third ring. Because it derives from
the deterministic event stream, the feed is replayable too.

### 17. What breaks at 10× load?
In-process, nothing does: 12M inputs/s is far above a single symbol's rate. The network path
breaks first: one gateway thread making a syscall per read and write saturates at a few hundred
thousand messages per second on a laptop. Fixes are batched sends (`sendmmsg`/`writev`), multiple
gateway threads each owning sessions with an SPSC ring per gateway into the sequencer (the core
then merges N rings in a fixed round-robin, so the order stays deterministic), and kernel bypass.
Journaling with fsync breaks earliest (~40k inputs/s per-input fsync), which is why group commit
exists.

### 18. How would you take this to FPGA or kernel bypass?
- **Kernel bypass:** ef_vi or DPDK receive straight into the decoder on a pinned core, with the
  core thread on another pinned core, shared-memory rings and no syscalls on the hot path.
  Journaling to NVMe uses io_uring or SPDK with group commit.
- **FPGA:** put the gateway's parse and validate stage (and risk checks) in hardware and keep
  matching in software, or, as some venues do, match the top-of-book in hardware for the most
  liquid instruments, with the software engine as the reference and the differential harness
  validating the hardware.

### 19. Why no exceptions and no virtual calls?
The tools are built with `-fno-exceptions -fno-rtti`. The engine reports errors as `Rejected`
events, which are part of the deterministic output, and as counters. The event sink is a template
parameter, so emission inlines. The only allocation is pool growth (a slow path, absent after
warm-up).

### 20. What would you change for a real venue?
- Multiple instruments.
- Market-order price protection and collars.
- Volatility halts and auctions (open/close crosses).
- Drop-copy and clearing feeds.
- Session-level sequencing and resend (FIX), persisted per-session state.
- Pre-trade risk checks in the gateway.
- Hot-standby replication of the journal.
- Formal verification of the rules against a TLA+ or model-based spec, in addition to the
  reference engine.
