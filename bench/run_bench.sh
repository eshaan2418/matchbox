#!/usr/bin/env bash
# Benchmark suite: in-process engine (fast vs reference, latency, journal policies) and end-to-end
# loopback TCP (mbload). Run on an idle machine. Raw JSON in bench/results/<stamp>/.
set -euo pipefail
B="${BUILD:-build}"; RUNS="${RUNS:-5}"; N="${N:-5000000}"
OUT="bench/results/$(date +%Y%m%d-%H%M%S)"; mkdir -p "$OUT"
SCR="${TMPDIR:-/tmp}/matchbox-bench-$$"; mkdir -p "$SCR"
TO="gtimeout 900"; command -v gtimeout >/dev/null || TO="timeout 900"
{ date -u +%FT%TZ; git rev-parse --short HEAD 2>/dev/null || true
  (sysctl -n machdep.cpu.brand_string 2>/dev/null || lscpu | grep 'Model name'); c++ --version | head -1
  pmset -g therm 2>/dev/null | tr '\n' ' '; echo; uptime; } > "$OUT/machine.txt"
run() { local name="$1"; shift; for i in $(seq 1 "$RUNS"); do echo "[$name] $i/$RUNS" >&2
  $TO "$@" > "$OUT/$name.run$i.json"; done; }
run engine_fast            "$B/matchbox-bench" --inputs "$N"
run engine_ref             "$B/matchbox-bench" --inputs "$N" --impl ref
run engine_fast_latency    "$B/matchbox-bench" --inputs "$N" --latency --hist-out "$OUT/engine_fast_latency.hist.csv"
run engine_ref_latency     "$B/matchbox-bench" --inputs "$N" --impl ref --latency --hist-out "$OUT/engine_ref_latency.hist.csv"
run engine_fast_journal    "$B/matchbox-bench" --inputs "$N" --journal "$SCR/j.bin"
run engine_fast_fsync64    "$B/matchbox-bench" --inputs 1000000 --journal "$SCR/j.bin" --fsync-every 64
RUNS=3 run engine_fast_fsync1 "$B/matchbox-bench" --inputs 20000 --journal "$SCR/j.bin" --fsync-every 1 --latency
# End to end over loopback TCP.
for rate in 10000 50000 100000 200000; do
  for i in $(seq 1 3); do
    echo "[e2e $rate] $i/3" >&2
    "$B/matchbox" serve --port 9200 --journal "$SCR/e2e.bin" --duration 14 > "$OUT/e2e_r${rate}_server.run$i.json" 2>/dev/null &
    SP=$!; sleep 0.5
    $TO "$B/mbload" --port 9200 --connections 8 --rate "$rate" --duration 10 --warmup 2 --hist-out "$OUT/e2e_r${rate}.run$i.hist.csv" > "$OUT/e2e_r${rate}.run$i.json"
    wait $SP
    "$B/matchbox" replay "$SCR/e2e.bin" > "$OUT/e2e_r${rate}_replay.run$i.json"
  done
done
rm -rf "$SCR"
python3 bench/summarize.py "$OUT" | tee "$OUT/SUMMARY.md"
