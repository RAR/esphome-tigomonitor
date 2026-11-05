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

# A missing optimizer: the 13th stops reporting (a bus of 12 again) and the
# TAP's roster stops listing it. Its last reading 4 days ago: missing, and
# off the roster, counted for Home Assistant until it's heard again.
python3 - "$out/state/state.json" <<'PY'
import json, sys, time
p = sys.argv[1]
d = json.load(open(p))
gone = d['seen_barcodes'][-1]
for n in d['nodes']:
    if n['long_address'] == gone:
        n['last_heard_unix'] = int(time.time()) - 4 * 86400
        n['last_roster_unix'] = int(time.time()) - 4 * 86400
json.dump(d, open(p, 'w'))
PY
mosquitto_sub -p 18833 -t tigomonitor/test/system/state -W 20 > "$out/system2.txt" 2>/dev/null &
sub=$!
sleep 0.3
replay "$out/twelve.bin"
sleep 0.5
kill $sub 2>/dev/null || true
expect "a reading and a roster entry kept per node" "$(stat "$out/state/state.json" "sum(1 for n in d['nodes'] if n.get('last_heard_unix', 0) > 0 and n.get('last_roster_unix', 0) > 0)")" 13
expect "Home Assistant counts the missing one" "$(tail -1 "$out/system2.txt" | python3 -c 'import json,sys; print(json.load(sys.stdin)["missing_optimizers"])')" 1
expect "a Missing optimizers sensor" "$(mosquitto_sub -p 18833 -v --retained-only -W 2 -t 'homeassistant/sensor/tigomonitor_test_missing_optimizers/config' 2>/dev/null | wc -l)" 1
# Marked as known (the owner knows it isn't reporting): not counted as missing.
python3 - "$out/state/state.json" <<'PY'
import json, sys
p = sys.argv[1]
d = json.load(open(p))
gone = d['seen_barcodes'][-1]
for n in d['nodes']:
    if n['long_address'] == gone:
        n['known'] = True
json.dump(d, open(p, 'w'))
PY
mosquitto_sub -p 18833 -t tigomonitor/test/system/state -W 20 > "$out/system2k.txt" 2>/dev/null &
sub=$!
sleep 0.3
replay "$out/twelve.bin"
sleep 0.5
kill $sub 2>/dev/null || true
expect "known: not counted as missing" "$(tail -1 "$out/system2k.txt" | python3 -c 'import json,sys; print(json.load(sys.stdin)["missing_optimizers"])')" 0
expect "the mark is kept" "$(stat "$out/state/state.json" "sum(1 for n in d['nodes'] if n.get('known'))")" 1
# Heard again: not missing, and no longer marked known.
mosquitto_sub -p 18833 -t tigomonitor/test/system/state -W 20 > "$out/system3.txt" 2>/dev/null &
sub=$!
sleep 0.3
replay "$out/thirteen.bin"
sleep 0.5
kill $sub 2>/dev/null || true
expect "heard again: not missing" "$(tail -1 "$out/system3.txt" | python3 -c 'import json,sys; print(json.load(sys.stdin)["missing_optimizers"])')" 0
expect "heard again: the mark is cleared" "$(stat "$out/state/state.json" "sum(1 for n in d['nodes'] if n.get('known'))")" 0

# Setup answers: running the bus with queries on, the master keeps each
# optimizer's last !Mppt and !Info answers, and /api/nodes carries them.
python3 linux/tools/mock_tap.py "$out/tap.tty" --window 600 --nodes 44 --stats "$out/tap.json" &
tap=$!
sleep 0.5
mkdir -p "$out/m"
printf '{"bus_role": "master", "master_queries": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18208}\n' "$PWD/$out/tap.tty" > "$out/m/settings.json"
build/linux/tigomonitord --state-dir "$out/m" --web-dir components/tigo_server/web 2> "$out/m.log" &
daemon=$!
trap 'kill $daemon $tap 2>/dev/null || true; pkill mosquitto || true' EXIT
setup() { python3 -c "import json,urllib.request as u; d=json.load(u.urlopen('http://127.0.0.1:18208/api/nodes', timeout=5)); print(eval(__import__('sys').argv[1]))" "$1"; }
for _ in $(seq 120); do [ "$(setup "sum(1 for n in d['nodes'] if (n.get('setup') or {}).get('mppt'))" 2>/dev/null)" -ge 2 ] 2>/dev/null && break; sleep 0.5; done
expect "the master keeps the !Mppt answers" "$(setup "sorted({n['setup']['mppt'].split()[3] for n in d['nodes'] if (n.get('setup') or {}).get('mppt')})")" "['S']"
expect "and the !Info ones" "$(setup "all(n['setup']['info'].startswith('!Info ') for n in d['nodes'] if (n.get('setup') or {}).get('info'))")" True

# Marking one known from the Node Table: POST /api/nodes/known. The mock TAP
# goes first, so no reading clears the mark (a reading does: tested above).
kill $tap; wait $tap 2>/dev/null || true
post() {  # post JSON: prints "<code> <body>"
  python3 - "$1" <<'PY'
import sys, urllib.error, urllib.request as u
r = u.Request('http://127.0.0.1:18208/api/nodes/known', sys.argv[1].encode(), {'Content-Type': 'application/json'}, method='POST')
try:
    x = u.urlopen(r, timeout=5); print(x.status, x.read().decode())
except urllib.error.HTTPError as e:
    print(e.code, e.read().decode())
PY
}
bc="$(setup "d['nodes'][0]['long_address']")"
overview() { python3 -c "import json,urllib.request as u; print(json.load(u.urlopen('http://127.0.0.1:18208/api/overview', timeout=5))['max_devices'])"; }
before="$(overview)"
expect "unmarked to start with" "$(setup "sum(1 for n in d['nodes'] if n.get('known'))")" 0
expect "a bad body is refused" "$(post '{"barcode": "x"}' | cut -d' ' -f1)" 400
expect "an unknown barcode is refused" "$(post '{"barcode": "04C05B00000000FF", "known": true}' | cut -d' ' -f1)" 404
expect "mark it known" "$(post "{\"barcode\": \"$bc\", \"known\": true}")" '200 {"known":true}'
expect "the sidebar's total leaves it out" "$(overview)" "$((before - 1))"
expect "/api/nodes says so" "$(setup "[n['known'] for n in d['nodes'] if n['long_address'] == '$bc']")" "[True]"
layout() { python3 -c "import json,urllib.request as u; d=json.load(u.urlopen('http://127.0.0.1:18208/api/layout', timeout=5)); print(eval(__import__('sys').argv[1]))" "$1"; }
expect "and /api/layout" "$(layout "[p.get('known') for p in d['unassigned'] + [p for i in d['inverters'] for m in i['mppts'] for s in m['strings'] for p in s['panels']] if p['barcode'] == '$bc']")" "[True]"
expect "unmark it" "$(post "{\"barcode\": \"$bc\", \"known\": false}")" '200 {"known":false}'
expect "unmarked" "$(setup "sum(1 for n in d['nodes'] if n.get('known'))")" 0
kill $daemon; wait $daemon 2>/dev/null || true

[ $fail = 0 ] && echo "new optimizer e2e passed" || { echo "new optimizer e2e FAILED; see $out/"; exit 1; }
