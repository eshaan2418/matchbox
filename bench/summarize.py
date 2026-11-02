#!/usr/bin/env python3
"""Summarise matchbox benchmark JSON (medians across runs)."""
import glob, json, os, re, statistics, sys
from collections import defaultdict
d = sys.argv[1]
g = defaultdict(list)
for f in sorted(glob.glob(os.path.join(d, "*.run*.json"))):
    g[re.sub(r"\.run\d+\.json$", "", os.path.basename(f))].append(json.load(open(f)))
med = statistics.median
print(f"# matchbox benchmark summary ({os.path.basename(d)})\n")
print("## In-process engine\n")
print("| config | runs | inputs/s (median) | min-max | ns/input | allocs after warm-up | fsyncs |")
print("|---|---|---|---|---|---|---|")
for k, rs in g.items():
    if not k.startswith("engine"): continue
    r = [x["inputs_per_sec"] for x in rs]
    print(f'| {k} | {len(rs)} | {med(r)/1e6:.2f}M | {min(r)/1e6:.2f}-{max(r)/1e6:.2f}M | {1e9/med(r):.0f} | {max(x["allocs_after_warmup"] for x in rs)} | {rs[0]["fsyncs"]} |')
lat = {k: v for k, v in g.items() if k.startswith("engine") and "latency_ns" in v[0]}
if lat:
    print("\n### Per-input latency (ns, median across runs)\n")
    print("| config | p50 | p90 | p99 | p99.9 | p99.99 | max |")
    print("|---|---|---|---|---|---|---|")
    for k, rs in lat.items():
        print(f"| {k} | " + " | ".join(f'{med([x["latency_ns"][q] for x in rs]):.0f}' for q in ["p50", "p90", "p99", "p99.9", "p99.99", "max"]) + " |")
e2e = sorted({k for k in g if k.startswith("e2e_r") and not k.endswith(("_server", "_replay"))},
             key=lambda s: int(re.search(r"r(\d+)", s).group(1)))
if e2e:
    print("\n## End to end over loopback TCP (order -> ack, µs, median across runs)\n")
    print("| target req/s | achieved new-order/s | p50 | p90 | p99 | p99.9 | p99.99 | max | unacked | replay hash == live |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for k in e2e:
        rs = g[k]
        ok = all(s["event_hash"] == r["event_hash"] for s, r in zip(g[k + "_server"], g[k + "_replay"]))
        q = lambda p: med([x["order_to_ack_us"][p] for x in rs])
        print(f'| {rs[0]["target_rate"]:.0f} | {med([x["achieved_rate"] for x in rs]):,.0f} | {q("p50"):.1f} | {q("p90"):.1f} | {q("p99"):.1f} | {q("p99.9"):.1f} | {q("p99.99"):.1f} | {q("max"):.0f} | {max(x["unacked_at_end"] for x in rs)} | {ok} |')
