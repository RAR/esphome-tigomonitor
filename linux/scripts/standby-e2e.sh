#!/bin/sh
# Two masters on one bus, without a TAP: tigomonitord in the standby role
# against linux/tools/mock_tap.py, with a port for each daemon on the same
# bus. Checks the standby only listens while another master runs the bus,
# takes over when that one goes quiet, never takes the bus back, yields to
# any master it hears (a CCA included), and never re-addresses a running TAP.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/standby-e2e
rm -rf "$out" && mkdir -p "$out/a" "$out/b" "$out/c"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
api() {  # api PORT PATH
  python3 -c "import sys, urllib.request as u; print(u.urlopen(f'http://127.0.0.1:{sys.argv[1]}{sys.argv[2]}', timeout=5).read().decode())" "$@"
}
standby() {  # standby PORT: "<state> <takeovers> <yields>"
  api "$1" /api/master | python3 -c 'import json,sys; s=json.load(sys.stdin)["standby"]; print(s["state"], s["takeovers"], s["yields"])'
}
stat() { python3 -c "import json,sys; d=json.load(open('$1')); print(eval(sys.argv[1]))" "$2"; }
frames() { stat "$out/tap.json" "d['frames_by_link'][$1]"; }
wait_state() {  # wait_state PORT STATE SECONDS
  for _ in $(seq $(($3 * 4))); do [ "$(standby "$1" 2>/dev/null | cut -d' ' -f1)" = "$2" ] && return 0; sleep 0.25; done
  return 0
}
health() {
  python3 - "$1" <<'PY'
import json, sys, urllib.error, urllib.request as u
try:
    r = u.urlopen(f'http://127.0.0.1:{sys.argv[1]}/api/health', timeout=5); code, d = r.status, json.load(r)
except urllib.error.HTTPError as e:
    code, d = e.code, json.load(e)
print(code, d['status'], json.dumps(d['problems']))
PY
}
mosquitto -p 18832 -d
mqtt_state() { mosquitto_sub -p 18832 -C 1 -W 15 -t "tigomonitor/$1/master/state" 2>/dev/null || true; }
settings() {  # settings DIR LINK WEB_PORT TAKEOVER_S
  printf '{"bus_role": "standby", "standby_takeover_s": %s, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "device_id": "%s", "publish_interval_s": 5, "mqtt": {"host": "127.0.0.1", "port": 18832}}\n' \
    "$4" "$PWD/$out/$2" "$3" "$(basename "$1")" > "$1/settings.json"
}
start() {  # start DIR: the daemon's pid in $pid
  build/linux/tigomonitord --state-dir "$1" --web-dir components/tigo_server/web 2>> "$1/daemon.log" &
  pid=$!
}

# --- run 1: two standbys, a 2 s one (the primary) and a 6 s one ---
python3 linux/tools/mock_tap.py "$out/link-a" --extra-link "$out/link-b" --lease 600 --nodes 44 --stats "$out/tap.json" &
tap=$!
sleep 0.5
settings "$out/a" link-a 18110 2
settings "$out/b" link-b 18111 6
start "$out/a"; a=$pid
start "$out/b"; b=$pid
trap 'kill -CONT $a $b 2>/dev/null || true; kill $a $b $tap 2>/dev/null || true; pkill mosquitto || true' EXIT
sleep 1
expect "both listen first" "$(frames 0) $(frames 1)" "0 0"
wait_state 18110 active 10
sleep 3
expect "the 2 s one takes the bus" "$(standby 18110)" "active 1 0"
expect "the 6 s one hears it and stands by" "$(standby 18111)" "standing_by 0 0"
expect "nothing sent by the one standing by" "$(frames 1)" 0
expect "it hears the master" "$(api 18111 /api/master | python3 -c 'import json,sys; print(0 <= json.load(sys.stdin)["standby"]["master_heard_ms_ago"] < 1000)')" True
expect "and decodes the panels" "$(api 18111 /api/devices | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["devices"]))')" 6
expect "standing by is healthy" "$(health 18111)" '200 ok []'
expect "MQTT: standing by is no problem" "$(mqtt_state b | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["status"], d["problem"])')" "standing_by False"
expect "MQTT: the active one" "$(mqtt_state a | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["status"], d["problem"])')" "active False"

# The primary stops: the 6 s one takes over within its time, and the lease
# and polls are answered.
kill $a; wait $a 2>/dev/null || true
t0=$(date +%s)
wait_state 18111 active 12
expect "the standby takes over when the master stops" "$(standby 18111)" "active 1 0"
expect "within its takeover time and a bit" "$([ $(( $(date +%s) - t0 )) -le 9 ] && echo yes || echo no)" yes
sleep 3
expect "its polls are answered" "$(api 18111 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["polls"] > 0, d["fault"], d["lease_read"])')" "True none True"

# The old primary comes back: it stands by, and stays standing by.
before=$(frames 0)
start "$out/a"; a=$pid
sleep 8
expect "the old master comes back standing by" "$(standby 18110)" "standing_by 0 0"
expect "and sends nothing" "$(( $(frames 0) - before ))" 0
expect "the bus stays with the other one" "$(standby 18111)" "active 1 0"

# The active one hangs: the other takes over; when it wakes, it hears the
# new master and yields, sending at most the one request it was in.
kill -STOP $b
wait_state 18110 active 8
expect "a hung master is replaced" "$(standby 18110)" "active 1 0"
kill -CONT $b
wait_state 18111 standing_by 5
sleep 1
expect "woken, it yields" "$(standby 18111)" "standing_by 1 1"
before=$(frames 1)
sleep 4
expect "and sends nothing after" "$(( $(frames 1) - before ))" 0
expect "never re-addressed or reset the TAP" "$(stat "$out/tap.json" "(d['addressed_to'], d['resets'], d['other'])")" "(None, 0, [])"
kill $a $b; wait $a $b 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 2: a CCA comes back beside a standby that runs the bus ---
# The mock plays the CCA: a request every 0.2 s from 8 s to 16 s. The
# standby yields to it, stands by while it runs, and takes over once it
# goes quiet.
python3 linux/tools/mock_tap.py "$out/link-c" --foreign-after 8 --foreign-every 0.2 --foreign-until 16 \
  --lease 600 --nodes 44 --stats "$out/tap.json" &
tap=$!
sleep 0.5
settings "$out/c" link-c 18112 2
start "$out/c"; c=$pid
trap 'kill $c $tap 2>/dev/null || true; pkill mosquitto || true' EXIT
wait_state 18112 active 6
expect "alone on the bus, it takes over" "$(standby 18112)" "active 1 0"
sleep 6.5
expect "it yields to the CCA" "$(standby 18112)" "standing_by 1 1"
before=$(frames 0)
sleep 3
expect "and stays quiet while the CCA runs" "$(( $(frames 0) - before ))" 0
wait_state 18112 active 8
expect "the CCA goes quiet: it takes over again" "$(standby 18112)" "active 2 1"
expect "no fault from any of it" "$(api 18112 /api/master | python3 -c 'import json,sys; print(json.load(sys.stdin)["fault"])')" none
kill $c; wait $c 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

[ $fail = 0 ] && echo "standby e2e passed" || { echo "standby e2e FAILED; see $out/"; exit 1; }
