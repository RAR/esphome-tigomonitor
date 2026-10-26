#!/bin/sh
# The bus master without a TAP: tigomonitord in the master role against a
# mock TAP on a pseudo-terminal (linux/tools/mock_tap.py). Checks it polls and
# decodes, checks the lease before ever renewing it, renews with the TAP's
# node count, lets the lease lapse while it's frozen, and stops transmitting
# when another master appears.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/master-e2e
rm -rf "$out" && mkdir -p "$out/state" "$out/state2"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
api() {  # api PORT PATH [JSON]
  python3 - "$@" <<'PY'
import sys, urllib.error, urllib.request as u
url = f'http://127.0.0.1:{sys.argv[1]}{sys.argv[2]}'
req = u.Request(url, sys.argv[3].encode(), {'Content-Type': 'application/json'}) if len(sys.argv) > 3 else url
try:
    r = u.urlopen(req, timeout=5)
    print(r.status if len(sys.argv) > 3 else r.read().decode())
except urllib.error.HTTPError as e:
    print(e.code)
PY
}
stat() { python3 -c "import json,sys; d=json.load(open('$1')); print(eval(sys.argv[1]))" "$2"; }

# --- run 1: polls, lease, renewal, and a frozen daemon ---
# The master reads the lease every 20 s, as the CCA does, so a 3 s lease is
# renewed at the 20 s check, and again at each check after.
python3 linux/tools/mock_tap.py "$out/tap" --lease 3 --renew-to 3 --nodes 45 --stats "$out/tap.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18092}\n' "$PWD/$out/tap" \
  > "$out/state/settings.json"
build/linux/tigomonitord --state-dir "$out/state" --web-dir components/tigo_server/web 2> "$out/daemon.log" &
daemon=$!
trap 'kill -CONT $daemon 2>/dev/null || true; kill $daemon $tap 2>/dev/null || true' EXIT
renewals() { stat "$out/tap.json" "len(d['renewals'])"; }
for _ in $(seq 60); do [ -f "$out/tap.json" ] && [ "$(renewals)" -ge 1 ] && break; sleep 0.5; done
expect "panels decoded" "$(api 18092 /api/devices | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["devices"]))')" 6
expect "first lease frame is a check" "$(stat "$out/tap.json" "d['first_lease_frame']")" check
expect "renewed with the TAP's node count" "$(stat "$out/tap.json" "[r['nodes'] for r in d['renewals']][:1]")" "[45]"
expect "cursor followed the TAP" "$(stat "$out/tap.json" "d['cursor_mismatches']")" 0
expect "nothing outside the allow-list" "$(stat "$out/tap.json" "d['other']")" "[]"
master="$(api 18092 /api/master)"
expect "/api/master active" "$(echo "$master" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["active"], d["fault"], d["renewals"] >= 1, d["lease"]["node_count"])')" "True none True 45"
# Frozen across the next renewal it would make (the 20 s check after this
# one): nothing renews the lease until the daemon runs again.
before="$(renewals)"
kill -STOP $daemon
sleep 25
sleep 1.5  # the mock's stats file lags by up to a second
during="$(renewals)"
kill -CONT $daemon
for _ in $(seq 20); do [ "$(renewals)" -gt "$during" ] && break; sleep 0.5; done
expect "no renewal while frozen" "$((during - before))" 0
expect "renews once running again" "$([ "$(renewals)" -gt "$during" ] && echo yes || echo no)" yes
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 2: a second master appears ---
python3 linux/tools/mock_tap.py "$out/tap2" --foreign-after 3 --stats "$out/tap2.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18093}\n' "$PWD/$out/tap2" \
  > "$out/state2/settings.json"
build/linux/tigomonitord --state-dir "$out/state2" --web-dir components/tigo_server/web 2> "$out/daemon2.log" &
daemon=$!
sleep 6
expect "second master stops it" "$(api 18093 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["active"], d["fault"])')" "False second_master"
expect "nothing sent after the second master" "$(stat "$out/tap2.json" "d['frames_after_foreign'] <= 1")" True
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 3: the port appears after the master starts (a late USB adapter) ---
mkdir -p "$out/state3"
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18094}\n' "$PWD/$out/tap3" \
  > "$out/state3/settings.json"
build/linux/tigomonitord --state-dir "$out/state3" --web-dir components/tigo_server/web 2> "$out/daemon3.log" &
daemon=$!
sleep 2
python3 linux/tools/mock_tap.py "$out/tap3" --stats "$out/tap3.json" &
tap=$!
sleep 8
expect "opens a port that appears later" "$(api 18094 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["active"], d["polls"] > 0)')" "True True"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 4: an adapter that echoes, lost replies, and a TAP with its own cursor ---
mkdir -p "$out/state4"
python3 linux/tools/mock_tap.py "$out/tap4" --echo --drop-every 7 --start-cursor 6FC4 --stats "$out/tap4.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18095}\n' "$PWD/$out/tap4" \
  > "$out/state4/settings.json"
build/linux/tigomonitord --state-dir "$out/state4" --web-dir components/tigo_server/web \
  --capture "$out/bus4.bin" 2> "$out/daemon4.log" &
daemon=$!
sleep 6
m4="$(api 18095 /api/master)"
expect "echo is not a second master" "$(echo "$m4" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["active"], d["fault"])')" "True none"
expect "took the TAP's cursor once" "$(echo "$m4" | python3 -c 'import json,sys; print(json.load(sys.stdin)["resyncs"])')" 1
expect "in step after that" "$(stat "$out/tap4.json" "d['cursor_mismatches']")" 1
expect "lost replies retried" "$(stat "$out/tap4.json" "d['retries'] >= d.get('dropped', 0) - 1 > 0")" True
expect "panels decoded" "$(api 18095 /api/devices | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["devices"]))')" 6
expect "capture holds what was sent" "$(python3 linux/tools/bus_frames.py "$out/bus4.bin" --requests | grep -c ' 0148 ' | awk '{print ($1 > 10)}')" 1
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 5: "auto" is resolved as listening does, not opened as a file ---
mkdir -p "$out/state5"
printf '{"bus_role": "master", "serial_port": "auto", "gateway_id": 4609, "web_port": 18096}\n' > "$out/state5/settings.json"
build/linux/tigomonitord --state-dir "$out/state5" --web-dir components/tigo_server/web 2> "$out/daemon5.log" &
daemon=$!
sleep 2
expect "auto is resolved, not opened" "$(api 18096 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["fault"], "no adapter found" in d["detail"])')" "no_port True"
kill $daemon; wait $daemon 2>/dev/null || true

# --- run 6: bench mode against a TAP with no mesh (the spare): version, refusals ---
mkdir -p "$out/state6"
python3 linux/tools/mock_tap.py "$out/tap6" --bare --stats "$out/tap6.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_bench": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18097}\n' \
  "$PWD/$out/tap6" > "$out/state6/settings.json"
build/linux/tigomonitord --state-dir "$out/state6" --web-dir components/tigo_server/web 2> "$out/daemon6.log" &
daemon=$!
sleep 3
expect "bench asks the version first" "$(stat "$out/tap6.json" "d['first_frame']")" 000A
expect "version logged" "$(grep -c 'TAP version: Mgate' "$out/daemon6.log")" 1
expect "refusal logged, once" "$(grep -c 'the TAP refused command 0B0F' "$out/daemon6.log")" 1
expect "refused polls named as such" "$(api 18097 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["fault"], "refuses polls" in d["detail"])')" "no_reply True"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 7: startup discovery (TAP boots at the factory address) ---
mkdir -p "$out/state7"
python3 linux/tools/mock_tap.py "$out/tap7" --factory --lease 600 --nodes 44 --stats "$out/tap7.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18098}\n' \
  "$PWD/$out/tap7" > "$out/state7/settings.json"
build/linux/tigomonitord --state-dir "$out/state7" --web-dir components/tigo_server/web 2> "$out/daemon7.log" &
daemon=$!
sleep 6
expect "TAP re-addressed from factory to 0x1201" "$(stat "$out/tap7.json" "d['addressed_to']")" 1201
m7="$(api 18098 /api/master)"
expect "running after discovery" "$(echo "$m7" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["active"], d["fault"], d["phase"])')" "True none running"
expect "polling the re-addressed TAP" "$(api 18098 /api/devices | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["devices"]) > 0)')" True
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 8: startup, but the TAP is already addressed -> no re-addressing writes ---
mkdir -p "$out/state8"
python3 linux/tools/mock_tap.py "$out/tap8" --lease 600 --nodes 44 --stats "$out/tap8.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18099}\n' \
  "$PWD/$out/tap8" > "$out/state8/settings.json"
build/linux/tigomonitord --state-dir "$out/state8" --web-dir components/tigo_server/web 2> "$out/daemon8.log" &
daemon=$!
sleep 5
expect "no re-addressing on an addressed TAP" "$(stat "$out/tap8.json" "d['addressed_to']")" None
expect "went straight to running" "$(api 18099 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["phase"], d["polls"] > 0)')" "running True"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

[ $fail = 0 ] && echo "master e2e passed" || { echo "master e2e FAILED; see $out/"; exit 1; }
