#!/bin/sh
# Giving a spare TAP its radio settings (tap_config.h), against the mock
# TAP: refused without master_bench and for a MAC that isn't the TAP's; a
# dry run reads the TAP and writes nothing; the real one writes, saves with
# the stored read and checks; a reset to that TAP alone only when asked.
# The key never appears in the API's answers or the log.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/provision-e2e
rm -rf "$out" && mkdir -p "$out"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
req() {  # req METHOD PORT PATH [JSON]: prints "<code> <body>"
  python3 - "$@" <<'PY'
import base64, sys, urllib.error, urllib.request as u
method, port, path = sys.argv[1:4]
body = sys.argv[4].encode() if len(sys.argv) > 4 and sys.argv[4] else None
h = {'Content-Type': 'application/json', 'Authorization': 'Basic ' + base64.b64encode(b'admin:pw').decode()}
try:
    r = u.urlopen(u.Request(f'http://127.0.0.1:{port}{path}', body, h, method=method), timeout=5)
    print(r.status, r.read().decode())
except urllib.error.HTTPError as e:
    print(e.code, e.read().decode())
PY
}
code() { req "$@" | cut -d' ' -f1; }
body() { req "$@" | cut -d' ' -f2-; }
edit() { body GET "$1" /api/master | python3 -c "import json,sys; e=json.load(sys.stdin)['table']['edit']; print(eval(sys.argv[1]))" "$2"; }
stat() { python3 -c "import json,sys; d=json.load(open('$1')); print(eval(sys.argv[1]))" "$2"; }
finished() {
  for _ in $(seq 80); do [ "$(edit "$1" "e['finished']")" = True ] && break; sleep 0.25; done
  sleep 1.5
}
tap=0 daemon=0
trap 'kill $daemon $tap 2>/dev/null || true' EXIT
start() {  # start NAME PORT BENCH
  name=$1 port=$2 bench=$3
  mkdir -p "$out/$name"
  # shellcheck disable=SC2086
  python3 linux/tools/mock_tap.py "$out/$name.tty" --window 600 --nodes 2 --panels 2 --stats "$out/$name.json" ${4:-} &
  tap=$!
  sleep 0.5
  printf '{"bus_role": "master", "master_bench": %s, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw"}\n' \
    "$bench" "$PWD/$out/$name.tty" "$port" > "$out/$name/settings.json"
  build/linux/tigomonitord --state-dir "$out/$name" --web-dir components/tigo_server/web 2>> "$out/$name.log" &
  daemon=$!
  for _ in $(seq 40); do [ "$(code GET "$port" /api/health)" != 000 ] 2>/dev/null && break; sleep 0.25; done
  sleep 3
}
stop_all() { kill $daemon $tap; wait $daemon $tap 2>/dev/null || true; }
MAC=04C05B3000A1B2C3

# --- bench off: refused, nothing sent ---
start off 18401 false
expect "refused without master_bench" "$(body POST 18401 /api/master/taps/provision "{\"mac\": \"$MAC\", \"channel\": 20}" | python3 -c "import json,sys; print('bench' in json.load(sys.stdin)['error'])")" True
stop_all
expect "and wrote nothing" "$(stat "$out/off.json" "len(d['cfg_writes'])")" 0

# --- bench on ---
start on 18402 true
expect "a channel the TAP refuses" "$(code POST 18402 /api/master/taps/provision "{\"mac\": \"$MAC\", \"channel\": 27}")" 400
# A MAC that isn't this TAP's: the job asks the TAP which it is and stops.
expect "another TAP's MAC starts" "$(code POST 18402 /api/master/taps/provision '{"mac": "04C05B0000FFFFFF", "channel": 20, "dry_run": false}')" 200
finished 18402
expect "and stops at asking which TAP it is" "$(edit 18402 "(e['ok'], e['message'])")" \
  "(False, 'the TAP at that address is $MAC, not 04C05B0000FFFFFF')"
expect "having written nothing" "$(stat "$out/on.json" "len(d['cfg_writes'])")" 0
# A dry run: the default.
r="$(body POST 18402 /api/master/taps/provision "{\"mac\": \"$MAC\", \"channel\": 20, \"pan\": \"5A5A\"}")"
expect "a dry run by default" "$(echo "$r" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['dry_run'], 'channel 20, PAN 5A5A' in d['given'], 'key hidden' in d['given'])")" "True True True"
finished 18402
expect "the dry run lists what it would send" "$(edit 18402 "(e['kind'], e['ok'], len(e['dry_frames']), e['dry_frames'][1])")" \
  "('provision', True, 3, '0D 00 01 (the save)')"
expect "and wrote nothing" "$(stat "$out/on.json" "len(d['cfg_writes'])")" 0
# The real one.
expect "the write starts" "$(code POST 18402 /api/master/taps/provision "{\"mac\": \"$MAC\", \"channel\": 20, \"pan\": \"5A5A\", \"dry_run\": false}")" 200
finished 18402
expect "it finished" "$(edit 18402 "(e['ok'], e['step'])")" "(True, 'done')"
expect "one write, on channel 20, PAN 5A5A" "$(stat "$out/on.json" "d['cfg_writes']")" "[{'channel': 20, 'pan': '5A5A'}]"
expect "saved by the stored read" "$(stat "$out/on.json" "d['cfg_saves'] >= 1")" True
expect "no reset unless asked" "$(stat "$out/on.json" "d['unicast_resets'] + d['resets']")" 0
# And with a reset, to that TAP alone.
expect "again, with a reset" "$(code POST 18402 /api/master/taps/provision "{\"mac\": \"$MAC\", \"channel\": 22, \"reset\": true, \"dry_run\": false}")" 200
finished 18402
sleep 3
expect "reset to it alone, no broadcast" "$(stat "$out/on.json" "(d['unicast_resets'], d['resets'])")" "(1, 0)"
expect "the PAN from its MAC" "$(stat "$out/on.json" "d['cfg_writes'][-1]")" "{'channel': 22, 'pan': 'B2C3'}"
expect "still polled after the reset" "$(body GET 18402 /api/master | python3 -c "import json,sys; print(json.load(sys.stdin)['taps'][0]['state'])")" running
# No key anywhere a person sees: the API's answer and the log show it hidden,
# and the write's 32-hex-digit key never appears.
expect "the log says key hidden" "$(grep -c 'key hidden' "$out/on.log" | tr -d ' ' | sed 's/^[1-9][0-9]*$/yes/')" yes
# Our own write is in the bus log, but it isn't another device's: no alarm.
expect "no reconfiguration alarm from our own write" "$(grep -c 'another device sent the TAP' "$out/on.log" || true)" 0
stop_all

# --- a TAP with the G8.59's 46-byte block: written in that form, its 10
# unexplained bytes left as the TAP had them ---
start wide 18403 true --cfg-size=46
expect "the write starts" "$(code POST 18403 /api/master/taps/provision "{\"mac\": \"$MAC\", \"channel\": 20, \"pan\": \"5A5A\", \"dry_run\": false}")" 200
finished 18403
expect "it finished" "$(edit 18403 "(e['ok'], e['step'])")" "(True, 'done')"
expect "one write, on channel 20, PAN 5A5A" "$(stat "$out/wide.json" "d['cfg_writes']")" "[{'channel': 20, 'pan': '5A5A'}]"
expect "the TAP's 10 bytes as they were" "$(stat "$out/wide.json" "d['cfg_extra'] == bytes(range(31, 41)).hex()")" True
stop_all

[ $fail = 0 ] && echo "provision e2e passed" || { echo "provision e2e FAILED; see $out/"; exit 1; }
