# Performance log

Raw output: `bench/results/20261102-003223/` (5 runs per in-process configuration, 3 per end-to-end rate).

## 1. Measuring the engine, not the harness
The first benchmark sink hashed every event with byte-wise FNV-1a (72 bytes/event, ~2.2
events/input). With hashing the engine measures **221 ns/input** (`hashcheck_fast.json`). With
a counting sink it runs at **81 ns/input**, so hashing cost more than matching itself. Hashing is now opt-in (`--hash`) and
used only for determinism checks (`hashcheck_*.json`: identical hashes for both engines over 5M
inputs).

## 2. Data structures vs reference
| | inputs/s | ns/input | allocations after warm-up |
|---|---|---|---|
| reference (std::map + std::list + unordered_map) | 2.08M | 482 | 3,856,696 |
| optimised engine | **12.36M** | **81** | **0** |

The gap is allocation (one list node per order plus map nodes per level) and pointer chasing.
The optimised engine keeps orders in an index-linked pool and levels in a sorted array with the
touch at `back()`.

## 3. Allocation-free after warm-up
`tests/test_alloc.cpp` first failed with exactly **1** allocation over 1M inputs: a pool crossing
its reserved capacity in the measured half. With capacities sized for the flow it is 0, which
the benchmark confirms (`allocs_after_warmup: 0`). Pools still grow if needed (slow path);
production sizing comes from the expected open-order count.

## 4. Cost of durability
| journal policy | inputs/s | ns/input | p50 / p99 per input |
|---|---|---|---|
| none | 12.36M | 81 | 125 / 541 ns |
| buffered write-ahead (page cache, flushed per burst) | 5.75M | 174 | |
| fsync every 64 inputs (group commit) | 1.76M | 569 | |
| fsync every input | 0.04M | 24,680 | 24.1 / 38.4 µs |

On this laptop's NVMe SSD an `fsync` costs about 24 µs, so per-input durability caps a single
sequencer at ~40k inputs/s. Group commit recovers most of the throughput while bounding loss to N
inputs. macOS `fsync` does not flush the drive's own cache (`F_FULLFSYNC` does, at a much higher
cost), so these numbers understate true power-loss durability on macOS.

## 5. End to end over loopback TCP
Order → ack, open loop, 8 connections, with a write-ahead journal without fsync:

| target req/s | p50 | p99 | p99.9 |
|---|---|---|---|
| 10k | 43 µs | 139 µs | 10.6 ms |
| 50k | 58 µs | 208 µs | 4.3 ms |
| 100k | 86 µs | 330 µs | 0.66 ms |
| 200k | 201 µs | 6.1 ms | 11.4 ms |

The machine was shared with an unrelated heavy workload during this run (load average 3-7,
recorded in `machine.txt`), so these are pessimistic. The millisecond tails at *low* rates are characteristic of busy-polling
threads being descheduled by macOS under contention: with fewer events, a preemption is more
likely to land on a measured request. The engine itself accounts for under 1 µs of these
latencies; the rest is the kernel TCP stack and syscalls on both ends.

## 6. Not done
- Hold acks until the journal record is durable (group-commit ack release).
- Hardware CRC32C (`crc32cx` on arm64, SSE4.2 on x86): the table-driven CRC is part of the
  journaled per-input cost.
- Pin core and gateway threads on Linux (`isolcpus`, `pthread_setaffinity_np`) and measure there.
