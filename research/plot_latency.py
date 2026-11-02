#!/usr/bin/env python3
"""Plot per-message latency distributions from itchbook histogram CSVs.

Usage: uv run --with matplotlib research/plot_latency.py OUT.png LABEL=FILE.csv [LABEL=FILE.csv ...]

Draws the HdrHistogram-style "percentile spectrum": latency (log y) against 1/(1-percentile)
(log x), the standard way to show tails from p50 to p99.99+.
"""
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

def load(path):
    ns_per_tick = 1.0
    vals, counts = [], []
    with open(path) as f:
        for line in f:
            if line.startswith("#"):
                if "ns_per_tick=" in line:
                    ns_per_tick = float(line.split("ns_per_tick=")[1])
                continue
            if line.startswith("value"):
                continue
            v, c = line.strip().split(",")
            vals.append(int(v) * ns_per_tick)
            counts.append(int(c))
    total = sum(counts)
    xs, ys, acc = [], [], 0
    for v, c in zip(vals, counts):
        acc += c
        q = acc / total
        if q < 1.0:
            xs.append(1.0 / (1.0 - q))
            ys.append(max(v, 1.0))
    return xs, ys

out = sys.argv[1]
fig, ax = plt.subplots(figsize=(8, 4.8))
for spec in sys.argv[2:]:
    label, path = spec.split("=", 1)
    xs, ys = load(path)
    ax.step(xs, ys, where="post", label=label)
ax.set_xscale("log")
ax.set_yscale("log")
ticks = [2, 10, 100, 1000, 10000, 100000]
ax.set_xticks(ticks)
ax.set_xticklabels(["p50", "p90", "p99", "p99.9", "p99.99", "p99.999"])
ax.set_xlabel("percentile")
ax.set_ylabel("latency (ns, log)")
ax.set_title("matchbox: per-input engine latency (5M realistic inputs)")
ax.grid(True, which="both", alpha=0.3)
ax.legend()
fig.tight_layout()
fig.savefig(out, dpi=130)
print("wrote", out)
