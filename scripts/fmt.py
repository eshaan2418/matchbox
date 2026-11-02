#!/usr/bin/env python3
"""Pretty-print demo JSON lines: fmt.py load|server|replay < json"""
import json, sys
d = json.load(sys.stdin)
kind = sys.argv[1]
if kind == "load":
    l = d["order_to_ack_us"]
    print(f'   sent {d["sent"]:,}  acks {d["acks"]:,}  fills {d["fills"]:,}  unacked {d["unacked_at_end"]}')
    print(f'   order->ack over loopback TCP: p50 {l["p50"]:.1f} us  p99 {l["p99"]:.1f} us  p99.9 {l["p99.9"]:.1f} us')
elif kind == "server":
    print(f'inputs {d["inputs"]:,}  events {d["events"]:,}  hash {d["event_hash"]}')
else:
    print(f'inputs {d["records"]:,}  events {d["events"]:,}  hash {d["event_hash"]}  ({d["inputs_per_sec"]/1e6:.1f}M inputs/s)')
