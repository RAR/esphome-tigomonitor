#!/bin/sh
# Two masters on one bus, without a TAP: tigomonitord in the standby role
# against linux/tools/mock_tap.py, with a port for each daemon on the same
# bus. Checks the standby only listens while another master runs the bus,
# takes over when that one goes quiet, never takes the bus back, yields to
# any master it hears (a CCA included), and never re-addresses a running TAP.
# Over MQTT, only the one running the bus publishes panels, and Home
# Assistant's panel entities follow whichever that is.
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
panel_states() {  # panel_states DEVICE SECONDS: panel readings it publishes in that time
  mosquitto_sub -p 18832 -t "tigomonitor/$1/panel/+/state" -W "$2" 2>/dev/null | wc -l
}
some() { [ "$1" -gt 0 ] && echo some || echo none; }
panel_owner() {  # the monitor whose topic Home Assistant reads a panel's power from
  mosquitto_sub -p 18832 -t 'homeassistant/sensor/+/config' -W 2 -v 2>/dev/null | grep -m1 '/tigo_[0-9A-F]*_power/config ' |
    sed 's/^[^ ]* //' | python3 -c 'import json,sys; print(json.load(sys.stdin)["state_topic"].split("/")[1])' || true
}
wait_owner() {  # wait_owner DEVICE SECONDS
  for _ in $(seq "$2"); do [ "$(panel_owner)" = "$1" ] && return 0; sleep 1; done
  return 0
}
settings() {  # settings DIR LINK WEB_PORT TAKEOVER_S
  printf '{"bus_role": "standby", "master_startup": false, "master_housekeeping": false, "master_queries": false, "master_optimizer_writes": false, "master_read_tap_config": false, "discovery_auto": false, "standby_takeover_s": %s, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "device_id": "%s", "publish_interval_s": 5, "mqtt": {"host": "127.0.0.1", "port": 18832}}\n' \
    "$4" "$PWD/$out/$2" "$3" "$(basename "$1")" > "$1/settings.json"
}
start() {  # start DIR: the daemon's pid in $pid
  build/linux/tigomonitord --state-dir "$1" --web-dir components/tigo_server/web 2>> "$1/daemon.log" &
  pid=$!
}

# --- run 1: two standbys, a 2 s one (the primary) and a 6 s one ---
python3 linux/tools/mock_tap.py "$out/link-a" --extra-link "$out/link-b" --window 600 --nodes 44 --stats "$out/tap.json" &
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
expect "MQTT: panels from the active one" "$(some "$(panel_states a 7)")" some
expect "MQTT: none from the one standing by" "$(some "$(panel_states b 7)")" none
wait_owner a 5
expect "MQTT: Home Assistant reads the active one" "$(panel_owner)" a

# The primary stops: the 6 s one takes over within its time, and the discovery window
# and polls are answered.
kill $a; wait $a 2>/dev/null || true
t0=$(date +%s)
wait_state 18111 active 12
expect "the standby takes over when the master stops" "$(standby 18111)" "active 1 0"
expect "within its takeover time and a bit" "$([ $(( $(date +%s) - t0 )) -le 9 ] && echo yes || echo no)" yes
sleep 3
expect "its polls are answered" "$(api 18111 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["polls"] > 0, d["fault"], d["discovery_read"])')" "True none True"
wait_owner b 12
expect "MQTT: Home Assistant follows the takeover" "$(panel_owner)" b
expect "MQTT: and the new master's panels" "$(some "$(panel_states b 7)")" some

# The old primary comes back: it stands by, and stays standing by. With
# master_bench on, too: its VERSION probe waits until it runs the bus.
python3 -c 'import json,sys; p=sys.argv[1]; d=json.load(open(p)); d["master_bench"]=True; json.dump(d, open(p, "w"))' "$out/a/settings.json"
before=$(frames 0)
start "$out/a"; a=$pid
sleep 8
expect "the old master comes back standing by" "$(standby 18110)" "standing_by 0 0"
expect "and sends nothing" "$(( $(frames 0) - before ))" 0
expect "the bus stays with the other one" "$(standby 18111)" "active 1 0"
expect "MQTT: no panels from the one back" "$(some "$(panel_states a 7)")" none
expect "MQTT: Home Assistant stays with the master" "$(panel_owner)" b
# Panel readings while standing by: published to its own topics, but not
# announced, so Home Assistant stays with the master. The save reconnects
# MQTT, which stalls the main loop for a moment: the standby must read what
# the port holds from meanwhile, and listen again, before it calls the bus
# quiet, or it takes over from a running master.
before=$(frames 0)
python3 -c 'import sys, urllib.request as u; u.urlopen(u.Request(sys.argv[1], b"{\"mqtt\": {\"standby_panels\": true}}", {"Content-Type": "application/json"}))' \
  http://127.0.0.1:18110/api/settings
expect "MQTT: the setting on, readings while standing by" "$(some "$(panel_states a 10)")" some
expect "a save doesn't make it take over" "$(standby 18110) $(( $(frames 0) - before ))" "standing_by 0 0 0"
expect "MQTT: still not announced" "$(panel_owner)" b
expect "the setting is saved" "$(api 18110 /api/settings | python3 -c 'import json,sys; print(json.load(sys.stdin)["mqtt"]["standby_panels"])')" True

# The active one hangs: the other takes over; when it wakes, it hears the
# new master and yields, sending at most the one request it was in.
kill -STOP $b
wait_state 18110 active 8
expect "a hung master is replaced" "$(standby 18110)" "active 1 0"
sleep 1
expect "the bench standby asked VERSION once it ran the bus, not before" \
  "$(sed -n '/(bench mode)/,$p' "$out/a/daemon.log" | grep -c 'TAP version: Mgate') $(grep -c 'no answer to VERSION' "$out/a/daemon.log")" "1 0"
kill -CONT $b
wait_state 18111 standing_by 5
sleep 1
expect "woken, it yields" "$(standby 18111)" "standing_by 1 1"
# Checked only now: the mock writes to both links from one thread, so while
# b's is frozen and full it blocks a's too, and a's main loop with it. A
# real bus can't do that.
wait_owner a 12
expect "MQTT: Home Assistant follows it back" "$(panel_owner)" a
before=$(frames 1)
sleep 4
expect "and sends nothing after" "$(( $(frames 1) - before ))" 0
expect "never re-addressed or reset the TAP" "$(stat "$out/tap.json" "(d['addressed_to'], d['resets'], d['other'])")" "(None, 0, [])"
# b ran the bus once already, then stood by: taking it again, it must
# announce the panels again over a's.
kill $a; wait $a 2>/dev/null || true
wait_state 18111 active 12
wait_owner b 12
expect "MQTT: Home Assistant follows a second takeover" "$(panel_owner)" b
kill $b; wait $b 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 2: a CCA comes back beside a standby that runs the bus ---
# The mock plays the CCA: a request every 0.2 s from 8 s to 16 s. The
# standby yields to it, stands by while it runs, and takes over once it
# goes quiet.
python3 linux/tools/mock_tap.py "$out/link-c" --foreign-after 8 --foreign-every 0.2 --foreign-until 16 \
  --window 600 --nodes 44 --stats "$out/tap.json" &
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

# --- run 3: PV off held across a takeover ---
# A runs the bus and B stands by, both with a web password. Off asked of A
# reaches the TAP; B hears it, saves it, and refuses to change it; when A
# dies, B takes over and its first word to the TAP is 00, with no 01 after.
mkdir -p "$out/pa" "$out/pb"
python3 linux/tools/mock_tap.py "$out/link-pa" --extra-link "$out/link-pb" --window 600 --nodes 44 --stats "$out/tap3.json" &
tap=$!
sleep 0.5
for d in pa:2:18113 pb:6:18114; do
  n=${d%%:*} rest=${d#*:}
  printf '{"bus_role": "standby", "master_startup": false, "master_housekeeping": false, "master_queries": false, "master_optimizer_writes": false, "master_read_tap_config": false, "discovery_auto": false, "standby_takeover_s": %s, "master_housekeeping": true, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw"}\n' \
    "${rest%%:*}" "$PWD/$out/link-$n" "${rest#*:}" > "$out/$n/settings.json"
done
start "$out/pa"; a=$pid
start "$out/pb"; b=$pid
trap 'kill $a $b $tap 2>/dev/null || true; pkill mosquitto || true' EXIT
pvreq() {  # pvreq PORT ON: the HTTP code
  python3 - "$1" "$2" <<'PY'
import base64, sys, urllib.error, urllib.request as u
h = {'Content-Type': 'application/json', 'Authorization': 'Basic ' + base64.b64encode(b'admin:pw').decode()}
body = ('{"on": %s}' % sys.argv[2]).encode()
try:
    print(u.urlopen(u.Request(f'http://127.0.0.1:{sys.argv[1]}/api/master/pv', body, h), timeout=5).status)
except urllib.error.HTTPError as e:
    print(e.code)
PY
}
authed() {  # authed PORT PATH
  python3 -c "import base64,sys,urllib.request as u; r=u.Request(f'http://127.0.0.1:{sys.argv[1]}{sys.argv[2]}', headers={'Authorization': 'Basic ' + base64.b64encode(b'admin:pw').decode()}); print(u.urlopen(r, timeout=5).read().decode())" "$@"
}
pvof() { authed "$1" /api/master | python3 -c "import json,sys; p=json.load(sys.stdin)['pv']; print(p['on'], p['source'])"; }
for _ in $(seq 40); do [ "$(authed 18113 /api/master 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["standby"]["state"])' 2>/dev/null)" = active ] && break; sleep 0.25; done
sleep 2
expect "off, asked of the one running the bus" "$(pvreq 18113 false)" 200
for _ in $(seq 40); do [ "$(pvof 18114 2>/dev/null)" = "False heard" ] && break; sleep 0.25; done
expect "the standby heard it" "$(pvof 18114)" "False heard"
expect "and saved it" "$(stat "$out/pb/pv.json" "(d['off'], d['heard'])")" "(True, True)"
expect "the standby won't change it" "$(pvreq 18114 true)" 409
before="$(stat "$out/tap3.json" "len(d['session'])")"
kill $a; wait $a 2>/dev/null || true
for _ in $(seq 60); do [ "$(authed 18114 /api/master 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["standby"]["state"])' 2>/dev/null)" = active ] && break; sleep 0.25; done
for _ in $(seq 40); do [ "$(stat "$out/tap3.json" "len(d['session']) > $before")" = True ] && break; sleep 0.25; done
sleep 10
expect "after the takeover: 00, and no 01" "$(stat "$out/tap3.json" "sorted(set(d['session'][$before:]))")" "['00']"
expect "on, asked of the new master" "$(pvreq 18114 true)" 200
for _ in $(seq 40); do [ "$(stat "$out/tap3.json" "d['pv']")" = on ] && break; sleep 0.25; done
expect "the TAP got 01" "$(stat "$out/tap3.json" "(d['pv'], d['session'][-1])")" "('on', '01')"
kill $b; wait $b 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 4: the PV switch in Home Assistant (mqtt.pv_switch) ---
# Off by default: no switch announced, and a command does nothing. On: the
# switch is announced, OFF reaches the TAP and the state follows. A standby
# standing by ignores it.
mkdir -p "$out/qa" "$out/qb"
python3 linux/tools/mock_tap.py "$out/link-qa" --extra-link "$out/link-qb" --window 600 --nodes 44 --stats "$out/tap4.json" &
tap=$!
sleep 0.5
qsettings() {  # qsettings NAME TAKEOVER PORT PV_SWITCH
  printf '{"bus_role": "standby", "master_startup": false, "master_housekeeping": false, "master_queries": false, "master_optimizer_writes": false, "master_read_tap_config": false, "discovery_auto": false, "standby_takeover_s": %s, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw", "device_id": "%s", "publish_interval_s": 2, "mqtt": {"host": "127.0.0.1", "port": 18832, "pv_switch": %s}}\n' \
    "$2" "$PWD/$out/link-$1" "$3" "$1" "$4" > "$out/$1/settings.json"
}
pvconfig() { mosquitto_sub -p 18832 -v --retained-only -W 2 -t "homeassistant/switch/tigomonitor_$1_pv_output/config" 2>/dev/null | wc -l; }
qsettings qa 2 18115 false
qsettings qb 6 18116 true
start "$out/qa"; a=$pid
start "$out/qb"; b=$pid
trap 'kill $a $b $tap 2>/dev/null || true; pkill mosquitto || true' EXIT
sleep 8
expect "pv_switch off: no switch announced" "$(pvconfig qa)" 0
mosquitto_pub -p 18832 -t tigomonitor/qa/pv/set -m OFF
sleep 3
expect "and OFF does nothing" "$(stat "$out/tap4.json" "d['pv']")" on
expect "the standby announces its switch" "$(pvconfig qb)" 1
mosquitto_pub -p 18832 -t tigomonitor/qb/pv/set -m OFF
sleep 3
expect "a standby standing by ignores OFF" "$(stat "$out/tap4.json" "d['pv']") $(grep -c 'PV switch: ignored' "$out/qb/daemon.log")" "on 1"
kill $a $b; wait $a $b 2>/dev/null || true
qsettings qa 2 18115 true
start "$out/qa"; a=$pid
b=0
sleep 12
expect "pv_switch on: announced" "$(pvconfig qa)" 1
mosquitto_pub -p 18832 -t tigomonitor/qa/pv/set -m OFF
for _ in $(seq 40); do [ "$(stat "$out/tap4.json" "d['pv']")" = off ] && break; sleep 0.25; done
expect "OFF reaches the TAP" "$(stat "$out/tap4.json" "(d['pv'], d['session'][-1])")" "('off', '00')"
expect "and the state says so" "$(mosquitto_sub -p 18832 -C 1 -W 10 -t tigomonitor/qa/master/state 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["pv"])')" OFF
mosquitto_pub -p 18832 -t tigomonitor/qa/pv/set -m ON
for _ in $(seq 40); do [ "$(stat "$out/tap4.json" "d['pv']")" = on ] && break; sleep 0.25; done
expect "ON turns it back on" "$(stat "$out/tap4.json" "d['pv']")" on
kill $a; wait $a 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 5: a standby that has heard no PV state yet says nothing about it ---
# A runs the bus and holds PV off, confirmed. B joins after that (a fresh
# install: no pv.json), so it hears no 0x0B00 before A dies: the next is an
# hour away. Taking over, B must not send its own default, 01.
mkdir -p "$out/ra" "$out/rb"
python3 linux/tools/mock_tap.py "$out/link-ra" --extra-link "$out/link-rb" --window 600 --nodes 44 --stats "$out/tap5.json" &
tap=$!
sleep 0.5
for d in ra:2:18117 rb:6:18118; do
  n=${d%%:*} rest=${d#*:}
  printf '{"bus_role": "standby", "master_startup": false, "master_housekeeping": false, "master_queries": false, "master_optimizer_writes": false, "master_read_tap_config": false, "discovery_auto": false, "standby_takeover_s": %s, "master_housekeeping": true, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw"}\n' \
    "${rest%%:*}" "$PWD/$out/link-$n" "${rest#*:}" > "$out/$n/settings.json"
done
start "$out/ra"; a=$pid
trap 'kill $a $b $tap 2>/dev/null || true; pkill mosquitto || true' EXIT
for _ in $(seq 40); do [ "$(authed 18117 /api/master 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["standby"]["state"])' 2>/dev/null)" = active ] && break; sleep 0.25; done
sleep 2
pvreq 18117 false >/dev/null
for _ in $(seq 40); do [ "$(stat "$out/tap5.json" "d['pv']")" = off ] && break; sleep 0.25; done
sleep 1
start "$out/rb"; b=$pid
sleep 4
expect "the late standby heard no PV state" "$(pvof 18118)" "True setting"
before="$(stat "$out/tap5.json" "len(d['session'])")"
kill $a; wait $a 2>/dev/null || true
for _ in $(seq 60); do [ "$(authed 18118 /api/master 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["standby"]["state"])' 2>/dev/null)" = active ] && break; sleep 0.25; done
sleep 8
expect "taking over, it sends no 01" "$(stat "$out/tap5.json" "('01' in d['session'][$before:], d['pv'])")" "(False, 'off')"
kill $b; wait $b 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

[ $fail = 0 ] && echo "standby e2e passed" || { echo "standby e2e FAILED; see $out/"; exit 1; }
