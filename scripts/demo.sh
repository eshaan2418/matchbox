#!/usr/bin/env bash
# Start the exchange with a journal, drive it with the load generator, stop it, replay the
# journal, and show the live and replayed event hashes match.
set -euo pipefail
B="${BUILD:-build}"; J="${TMPDIR:-/tmp}/matchbox-demo.journal"; S="${TMPDIR:-/tmp}/matchbox-demo-server.json"
echo "== start exchange with write-ahead journal"
"$B/matchbox" serve --port 9300 --journal "$J" --duration 7 > "$S" 2>/dev/null &
sleep 0.5
echo "== 8 sessions, 50k orders/s for 4 s (open-loop load generator)"
"$B/mbload" --port 9300 --connections 8 --rate 50000 --duration 4 --warmup 1 | python3 scripts/fmt.py load
wait
echo "== live server:    $(python3 scripts/fmt.py server < "$S")"
echo "== journal replay: $("$B/matchbox" replay "$J" | python3 scripts/fmt.py replay)"
