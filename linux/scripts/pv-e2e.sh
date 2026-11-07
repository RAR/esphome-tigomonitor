#!/bin/sh
# PV off and on (docs/superpowers/specs/2026-11-03-pv-off-design.md): the
# 0x0B00 00/01 the master sends, against a mock TAP that keeps a PV state
# and echoes the byte, as the roof's does. Checks the API's guards, that
# off is confirmed and saved, and that nothing sends 01 while it is held:
# not a restart, a TAP power cut, a bring-up or a settings save.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/pv-e2e
rm -rf "$out" && mkdir -p "$out"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
# req METHOD PORT PATH [JSON] [USER:PASS]: prints "<code> <body>"
req() {
  python3 - "$@" <<'PY'
import base64, sys, urllib.error, urllib.request as u
method, port, path = sys.argv[1:4]
body = sys.argv[4].encode() if len(sys.argv) > 4 and sys.argv[4] else None
h = {'Content-Type': 'application/json'}
if len(sys.argv) > 5:
    h['Authorization'] = 'Basic ' + base64.b64encode(sys.argv[5].encode()).decode()
try:
    r = u.urlopen(u.Request(f'http://127.0.0.1:{port}{path}', body, h, method=method), timeout=5)
    print(r.status, r.read().decode())
except urllib.error.HTTPError as e:
    print(e.code, e.read().decode())
PY
}
code() { req "$@" | cut -d' ' -f1; }
body() { req "$@" | cut -d' ' -f2-; }
pv() { body GET "$1" /api/master '' admin:pw | python3 -c "import json,sys; p=json.load(sys.stdin)['pv']; print(eval(sys.argv[1]))" "$2"; }
stat() { python3 -c "import json,sys; d=json.load(open('$1')); print(eval(sys.argv[1]))" "$2"; }
waitfor() {  # waitfor TRIES PYEXPR-ON-STATS STATSFILE
  for _ in $(seq "$1"); do [ -f "$3" ] && [ "$(stat "$3" "$2")" = True ] && return 0; sleep 0.25; done
  return 0
}
tap=0 daemon=0
trap 'kill $daemon $tap 2>/dev/null || true' EXIT
start() {  # start NAME PORT EXTRA-SETTINGS [MOCK FLAGS...]
  name=$1 port=$2 extra=$3
  shift 3
  mkdir -p "$out/$name"
  python3 linux/tools/mock_tap.py "$out/$name.tty" --window 600 --nodes 44 --stats "$out/$name.json" "$@" &
  tap=$!
  sleep 0.5
  printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw"%s}\n' \
    "$PWD/$out/$name.tty" "$port" "$extra" > "$out/$name/settings.json"
  restart "$name"
}
restart() {
  build/linux/tigomonitord --state-dir "$out/$1" --web-dir components/tigo_server/web 2>> "$out/$1.log" &
  daemon=$!
  for _ in $(seq 40); do [ "$(code GET "$port" /api/health)" != 000 ] 2>/dev/null && break; sleep 0.25; done
  sleep 1
}
stop() { kill $daemon; wait $daemon 2>/dev/null || true; }
stop_all() { stop; kill $tap; wait $tap 2>/dev/null || true; }

# --- run 1: off and on from the API, with housekeeping on ---
start one 18201 ', "master_housekeeping": true'
expect "no password, no switch" "$(code POST 18201 /api/master/pv '{"on": false}')" 401
expect "a bad body is refused" "$(code POST 18201 /api/master/pv '{"on": "no"}' admin:pw)" 400
expect "off" "$(req POST 18201 /api/master/pv '{"on": false}' admin:pw)" '200 {"on":false}'
waitfor 40 "d['pv'] == 'off'" "$out/one.json"
expect "the TAP got 00" "$(stat "$out/one.json" "(d['pv'], d['session'][-1])")" "('off', '00')"
sleep 1
expect "/api/master says off, confirmed, set here" "$(pv 18201 "(p['on'], p['confirmed'], p['unconfirmed'], p['source'], p['last_sent'], p['last_reply'])")" \
  "(False, True, False, 'setting', '00', '00')"
expect "saved, for this machine only" "$(stat "$out/one/pv.json" "d['off']") $(python3 -c "import os; print(oct(os.stat('$out/one/pv.json').st_mode)[-3:])")" "True 600"
expect "logged" "$(grep -c 'PV output: off' "$out/one.log")" 1
# A restart while held: 00 first, and no 01 after it.
before="$(stat "$out/one.json" "len(d['session'])")"
stop
restart one
waitfor 60 "len(d['session']) > $before" "$out/one.json"
sleep 2
expect "restarted: 00 and no 01" "$(stat "$out/one.json" "sorted(set(d['session'][$before:]))")" "['00']"
expect "and says off" "$(pv 18201 "p['on']")" False
# A settings save rebuilds the bus master: still 00, no 01.
before="$(stat "$out/one.json" "len(d['session'])")"
req POST 18201 /api/settings '{"master_queries": true}' admin:pw >/dev/null
waitfor 80 "len(d['session']) > $before" "$out/one.json"
sleep 2
expect "settings saved: 00 and no 01" "$(stat "$out/one.json" "sorted(set(d['session'][$before:]))")" "['00']"
expect "on" "$(code POST 18201 /api/master/pv '{"on": true}' admin:pw)" 200
waitfor 40 "d['pv'] == 'on'" "$out/one.json"
expect "the TAP got 01" "$(stat "$out/one.json" "(d['pv'], d['session'][-1])")" "('on', '01')"
stop_all

# --- run 2: a TAP power cut while off is held, housekeeping off ---
start two 18202 '' --power-cut-every 25 --power-off 4
code POST 18202 /api/master/pv '{"on": false}' admin:pw >/dev/null
waitfor 160 "len(d['cut_marks']) >= 1 and len(d['session']) > d['cut_marks'][0]" "$out/two.json"
sleep 2
expect "after the cut: 00 again, no 01" "$(stat "$out/two.json" "(len(d['cut_marks']) >= 1, sorted(set(d['session'][d['cut_marks'][0]:])))")" "(True, ['00'])"
stop_all

# --- run 3: a cold bring-up (the TAP at the factory address) with off held ---
mkdir -p "$out/three"
printf '{"off": true, "heard": false, "since_unix": 1}\n' > "$out/three/pv.json"
start three 18203 ', "master_housekeeping": true' --factory
waitfor 120 "len(d['session']) >= 1 and len(d['housekeeping']) >= 1" "$out/three.json"
sleep 2
expect "bring-up: 00 only" "$(stat "$out/three.json" "sorted(set(d['session']))")" "['00']"
stop_all

# --- run 4: a TAP that answers the wrong byte ---
start four 18204 '' --pv-wrong-reply
code POST 18204 /api/master/pv '{"on": false}' admin:pw >/dev/null
sleep 32
expect "sent 6 times, then flagged" "$(stat "$out/four.json" "d['session'].count('00')") $(pv 18204 "(p['confirmed'], p['unconfirmed'])")" "6 (False, True)"
expect "a health problem" "$(body GET 18204 /api/health | python3 -c 'import json,sys; print(any("PV output" in p for p in json.load(sys.stdin)["problems"]))')" True
# Restarted mid-retry: still off, 00 first.
before="$(stat "$out/four.json" "len(d['session'])")"
stop
restart four
waitfor 60 "len(d['session']) > $before" "$out/four.json"
sleep 1
expect "restarted mid-retry: 00, no 01" "$(stat "$out/four.json" "sorted(set(d['session'][$before:]))")" "['00']"
stop_all

# --- run 5: an unreadable pv.json says nothing about PV ---
# Neither 00 nor 01, the housekeeping's included: the TAP keeps what it was
# last told. Holding off would make a disk fault stop the roof.
mkdir -p "$out/five"
printf '{' > "$out/five/pv.json"
start five 18205 ', "master_housekeeping": true'
waitfor 60 "d['polls'] >= 20" "$out/five.json"
sleep 2
expect "unreadable: no 0x0B00" "$(stat "$out/five.json" "d['session']")" "[]"
expect "and says so" "$(body GET 18205 /api/health | python3 -c 'import json,sys; print(any("pv.json" in p for p in json.load(sys.stdin)["problems"]))')" True
stop_all

# --- run 6: no web password, and the listen role ---
start six 18206 ''
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18206}\n' "$PWD/$out/six.tty" > "$out/six/settings.json"
stop
restart six
expect "no web password: 403" "$(req POST 18206 /api/master/pv '{"on": false}')" '403 {"error":"set a web password first"}'
stop
printf '{"serial_port": "%s", "web_port": 18206, "web_password": "pw"}\n' "$PWD/$out/six.tty" > "$out/six/settings.json"
restart six
expect "listening: 409" "$(code POST 18206 /api/master/pv '{"on": false}' admin:pw)" 409
stop_all

# --- run 7: a pv.json the daemon can't open counts as unreadable, not missing ---
# Root's file, mode 600, and the daemon running as nobody: nothing said about
# PV, and the health problem (a missing file would be PV on, and say 01).
mkdir -p "$out/seven"
printf '{"off": false, "heard": false, "since_unix": 1}\n' > "$out/seven/pv.json"
chmod 600 "$out/seven/pv.json"
python3 linux/tools/mock_tap.py "$out/seven.tty" --window 600 --nodes 44 --stats "$out/seven.json" &
tap=$!
sleep 0.5
chmod 666 "$(readlink -f "$out/seven.tty")"
printf '{"bus_role": "master", "master_startup": true, "master_housekeeping": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18207, "web_password": "pw"}\n' \
  "$PWD/$out/seven.tty" > "$out/seven/settings.json"
chown 65534:65534 "$out/seven" "$out/seven/settings.json"
setpriv --reuid=65534 --regid=65534 --clear-groups build/linux/tigomonitord --state-dir "$out/seven" \
  --web-dir components/tigo_server/web 2>> "$out/seven.log" &
daemon=$!
port=18207
waitfor 60 "d['polls'] >= 20" "$out/seven.json"
sleep 2
expect "can't open pv.json: no 0x0B00" "$(stat "$out/seven.json" "d['session']")" "[]"
expect "and says so" "$(body GET 18207 /api/health | python3 -c 'import json,sys; print(any("pv.json" in p for p in json.load(sys.stdin)["problems"]))')" True
stop_all

# --- run 8: the discovery switch (not PV, but the same kind of owner's switch) ---
# A 2 s window renewed to 2 s, so renewals come every few seconds. Let it
# close: reads go on, renewals stop, and nothing else is sent in their place;
# renew again: they resume. Saved: a restart keeps it, and so does a
# settings save, which builds a new bus master.
start eight 18208 '' --window 2 --renew-to 2
waitfor 60 "len(d['renewals']) >= 2" "$out/eight.json"
expect "renewing to start with" "$(body GET 18208 /api/master '' admin:pw | python3 -c 'import json,sys; print(json.load(sys.stdin)["discovery_renewing"])')" True
expect "no password, no switch" "$(code POST 18208 /api/master/discovery '{"renew": false}')" 401
expect "a bad body is refused" "$(code POST 18208 /api/master/discovery '{"renew": 0}' admin:pw)" 400
expect "let it close" "$(req POST 18208 /api/master/discovery '{"renew": false}' admin:pw)" '200 {"renew":false}'
sleep 1
n="$(stat "$out/eight.json" "len(d['renewals'])")"
c="$(stat "$out/eight.json" "d['checks']")"
o="$(stat "$out/eight.json" "len(d['other'])")"
sleep 25
expect "no renewals while it's left to close" "$(stat "$out/eight.json" "len(d['renewals'])")" "$n"
expect "but it's still read" "$(stat "$out/eight.json" "d['checks'] > $c")" True
expect "and nothing new is sent" "$(stat "$out/eight.json" "len(d['other'])")" "$o"
expect "/api/master says so" "$(body GET 18208 /api/master '' admin:pw | python3 -c 'import json,sys; m=json.load(sys.stdin); print(m["discovery_renewing"], m["discovery"]["remaining_s"], m["fault"])')" "False 0 none"
expect "logged" "$(grep -c 'discovery window left to close' "$out/eight.log")" 1
expect "renew again" "$(req POST 18208 /api/master/discovery '{"renew": true}' admin:pw)" '200 {"renew":true}'
waitfor 120 "len(d['renewals']) > $n" "$out/eight.json"
expect "renewals resume" "$(stat "$out/eight.json" "len(d['renewals']) > $n")" True
req POST 18208 /api/master/discovery '{"renew": false}' admin:pw > /dev/null
expect "saved" "$(python3 -c "import json; print(json.load(open('$out/eight/discovery.json')))")" "{'renew': False}"
stop
restart eight
expect "a restart keeps it" "$(body GET 18208 /api/master '' admin:pw | python3 -c 'import json,sys; print(json.load(sys.stdin)["discovery_renewing"])')" False
n="$(stat "$out/eight.json" "len(d['renewals'])")"
sleep 8
expect "and renews nothing" "$(stat "$out/eight.json" "len(d['renewals'])")" "$n"
expect "a settings save keeps it" "$(code POST 18208 /api/settings '{"master_housekeeping": false}' admin:pw)" 200
sleep 2
expect "and the new master says so" "$(body GET 18208 /api/master '' admin:pw | python3 -c 'import json,sys; print(json.load(sys.stdin)["discovery_renewing"])')" False
expect "renew again, saved" "$(req POST 18208 /api/master/discovery '{"renew": true}' admin:pw)" '200 {"renew":true}'
stop
restart eight
expect "and a restart keeps that too" "$(body GET 18208 /api/master '' admin:pw | python3 -c 'import json,sys; print(json.load(sys.stdin)["discovery_renewing"])')" True
stop_all

[ $fail = 0 ] && echo "pv e2e passed" || { echo "pv e2e FAILED; see $out/"; exit 1; }
