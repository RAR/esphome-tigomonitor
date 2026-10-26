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
trap 'kill -CONT $daemon 2>/dev/null; kill $daemon $tap 2>/dev/null || true' EXIT
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

[ $fail = 0 ] && echo "master e2e passed" || { echo "master e2e FAILED; see $out/"; exit 1; }
