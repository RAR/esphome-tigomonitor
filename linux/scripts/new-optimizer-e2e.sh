#!/bin/sh
# A new optimizer on the bus (a replacement, or one added): its barcode turns
# up in a Frame 27 that the node table hasn't seen. Two synthetic buses
# through the daemon, 12 panels then 13 (the same seed: the first 12 are the
# same barcodes). A fresh install's own first day is not news; after it, the
# 13th is: logged once, saved, and counted for Home Assistant until placed.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/new-optimizer-e2e
rm -rf "$out" && mkdir -p "$out/state"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
stat() { python3 -c "import json,sys; d=json.load(open('$1')); print(eval(sys.argv[1]))" "$2"; }
python3 linux/tools/synth_bus.py "$out/twelve.bin" --panels 12 --rounds 3 >/dev/null
python3 linux/tools/synth_bus.py "$out/thirteen.bin" --panels 13 --rounds 3 >/dev/null
printf '{"device_id": "test", "publish_interval_s": 1, "mqtt": {"host": "127.0.0.1", "port": 18833}}\n' > "$out/state/settings.json"
mosquitto -p 18833 -d
trap 'pkill mosquitto || true' EXIT
sleep 0.5
replay() { build/linux/tigomonitord --state-dir "$out/state" --port 0 --replay "$1" --speed 0 2>> "$out/daemon.log"; }

# A fresh install: the panels it first hears are its baseline, not news.
replay "$out/twelve.bin"
expect "baseline saved" "$(stat "$out/state/state.json" "(len(d['seen_barcodes']), d['seen_since'] > 0)")" "(12, True)"
expect "nothing new on a fresh install" "$(stat "$out/state/state.json" "d.get('first_heard', {})")" "{}"
# Within its first day, a 13th is still part of the baseline.
replay "$out/thirteen.bin"
expect "a 13th on the first day: baseline" "$(stat "$out/state/state.json" "(len(d['seen_barcodes']), d.get('first_heard', {}))")" "(13, {})"

# A day later, one more is news.
python3 - "$out/state/state.json" <<'PY'
import json, sys, time
p = sys.argv[1]
d = json.load(open(p))
d['seen_barcodes'] = d['seen_barcodes'][:12]
d['seen_since'] = int(time.time()) - 2 * 86400
json.dump(d, open(p, 'w'))
PY
mosquitto_sub -p 18833 -t tigomonitor/test/system/state -W 20 > "$out/system.txt" 2>/dev/null &
sub=$!
sleep 0.3
replay "$out/thirteen.bin"
sleep 0.5
kill $sub 2>/dev/null || true
expect "the new one is saved with when it was first heard" "$(stat "$out/state/state.json" "[(k[-8:], v > 0) for k, v in d['first_heard'].items()]")" \
  "[('$(python3 -c "import json; print(json.load(open('$out/state/state.json'))['seen_barcodes'][-1][-8:])")', True)]"
expect "logged once" "$(grep -c 'new optimizer heard' "$out/daemon.log")" 1
expect "Home Assistant counts it" "$(tail -1 "$out/system.txt" | python3 -c 'import json,sys; print(json.load(sys.stdin)["new_optimizers"])')" 1
expect "a New optimizers sensor" "$(mosquitto_sub -p 18833 -v --retained-only -W 2 -t 'homeassistant/sensor/tigomonitor_test_new_optimizers/config' 2>/dev/null | wc -l)" 1
# Heard again: not news twice.
replay "$out/thirteen.bin"
expect "not logged again" "$(grep -c 'new optimizer heard' "$out/daemon.log")" 1

[ $fail = 0 ] && echo "new optimizer e2e passed" || { echo "new optimizer e2e FAILED; see $out/"; exit 1; }
