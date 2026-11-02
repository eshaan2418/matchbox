# matchbox benchmark summary (20261102-003223)

## In-process engine

| config | runs | inputs/s (median) | min-max | ns/input | allocs after warm-up | fsyncs |
|---|---|---|---|---|---|---|
| engine_fast | 5 | 12.36M | 10.40-12.97M | 81 | 0 | 0 |
| engine_fast_fsync1 | 3 | 0.04M | 0.04-0.04M | 24680 | 0 | 20001 |
| engine_fast_fsync64 | 5 | 1.76M | 1.68-1.83M | 569 | 0 | 15626 |
| engine_fast_journal | 5 | 5.75M | 5.63-5.90M | 174 | 0 | 1 |
| engine_fast_latency | 5 | 7.04M | 6.92-7.19M | 142 | 0 | 0 |
| engine_ref | 5 | 2.08M | 1.88-2.32M | 482 | 3856696 | 0 |
| engine_ref_latency | 5 | 1.96M | 1.75-2.00M | 511 | 3856696 | 0 |

### Per-input latency (ns, median across runs)

| config | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|
| engine_fast_fsync1 | 24128 | 30128 | 38368 | 55648 | 97728 | 178022 |
| engine_fast_latency | 125 | 250 | 541 | 1333 | 5060 | 138529 |
| engine_ref_latency | 417 | 875 | 1958 | 4792 | 18032 | 24941653 |

## End to end over loopback TCP (order -> ack, µs, median across runs)

| target req/s | achieved new-order/s | p50 | p90 | p99 | p99.9 | p99.99 | max | unacked | replay hash == live |
|---|---|---|---|---|---|---|---|---|---|
| 10000 | 7,994 | 43.4 | 72.2 | 138.9 | 10641.4 | 18415.6 | 19330 | 0 | True |
| 50000 | 39,997 | 58.4 | 102.0 | 208.4 | 4284.4 | 8241.1 | 9076 | 0 | True |
| 100000 | 79,996 | 86.3 | 212.2 | 329.5 | 661.0 | 3389.4 | 3860 | 0 | True |
| 200000 | 159,930 | 200.7 | 363.3 | 6058.0 | 11395.1 | 12615.7 | 12974 | 0 | True |
