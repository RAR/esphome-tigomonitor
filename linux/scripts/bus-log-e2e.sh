#!/bin/sh
# The bus log and the TAP-reconfiguration alarm, without hardware: tigomonitord
# listening on a pseudo-terminal where linux/tools/cca_on_bus.py plays a CCA.
# Checks the log names the frames and shows their payloads but for the
# GatewayCfg block, which holds the network key; that a GatewayCfg
# write and a SET_CHANNEL raise the alarm in /api/bus, /api/health and MQTT;
# that it survives a restart; that clearing it ends it; and that a CCA in its
# steady state raises nothing.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/bus-log-e2e
rm -rf "$out" && mkdir -p "$out/state" "$out/state2"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
api() {  # api PORT PATH [POST]
  python3 - "$@" <<'PY'
import sys, urllib.error, urllib.request as u
url = f'http://127.0.0.1:{sys.argv[1]}{sys.argv[2]}'
req = u.Request(url, b'{}', {'Content-Type': 'application/json'}) if len(sys.argv) > 3 else url
try:
    r = u.urlopen(req, timeout=5)
    print(r.read().decode())
except urllib.error.HTTPError as e:
    print(e.read().decode())
PY
}
bus() { api "$1" /api/bus | python3 -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$2"; }
health() { api "$1" /api/health | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['status'], [p for p in d['problems'] if p.startswith('bus:')])"; }
mosquitto -p 18834 -d
trap 'kill $daemon $cca 2>/dev/null || true; pkill mosquitto || true' EXIT

# --- run 1: a CCA in steady state: named frames, no alarm ---
python3 linux/tools/cca_on_bus.py "$out/bus" &
cca=$!
sleep 0.5
printf '{"bus_role": "listen", "serial_port": "%s", "web_port": 18140, "device_id": "test", "publish_interval_s": 2, "mqtt": {"host": "127.0.0.1", "port": 18834}}\n' \
  "$PWD/$out/bus" > "$out/state/settings.json"
build/linux/tigomonitord --state-dir "$out/state" --web-dir components/tigo_server/web 2> "$out/daemon.log" &
daemon=$!
sleep 6
expect "polls counted, not logged" "$(bus 18140 "(d['polls'] > 20, [e for e in d['events'] if e['cmd'] in ('0148', '0149')])")" "(True, [])"
expect "frames named" "$(bus 18140 "sorted({e['name'] for e in d['events']})")" \
  "['Tunnel reply · DiscState reply', 'Tunnel reply · EncapString reply', 'Tunnel · DiscState', 'Tunnel · EncapString', 'VERSION', 'VERSION reply']"
expect "payloads in hex" "$(bus 18140 "sorted({e['hex'][:12] for e in d['events'] if e['cmd'] == '0B10' and e['sub'] == 7})")" "['00 0D 00 07 ']"
expect "a query's text and node" "$(bus 18140 "[(e['text'], e['node']) for e in d['events'] if e['sub'] == 6][:1]")" "[('^00Info', '0002')]"
expect "VERSION's reply text" "$(bus 18140 "sorted({e['text'] for e in d['events'] if e['cmd'] == '000B'})")" "['Mgate G8.59 Jan  1 2026']"
expect "no other text" "$(bus 18140 "[e['name'] for e in d['events'] if e['text'] and e['sub'] != 6 and e['cmd'] != '000B']")" "[]"
expect "listening: nothing is ours" "$(bus 18140 "any(e['ours'] for e in d['events'])")" False
expect "steady state: no alarm" "$(bus 18140 "d['alarm']")" "{'active': False, 'count': 0, 'first_unix_ms': 0, 'recent': []}"
expect "steady state: healthy" "$(health 18140)" "ok []"
expect "MQTT: alarm sensor announced" "$(mosquitto_sub -p 18834 -v --retained-only -W 1 -t 'homeassistant/binary_sensor/#' 2>/dev/null | grep -c '_tap_reconfigured' || true)" 1
kill $daemon; wait $daemon 2>/dev/null || true
kill $cca; wait $cca 2>/dev/null || true

# --- run 2: the CCA's UI reconfigures the TAP ---
python3 linux/tools/cca_on_bus.py "$out/bus" --reconfigure-after 2 &
cca=$!
sleep 0.5
build/linux/tigomonitord --state-dir "$out/state" --web-dir components/tigo_server/web 2> "$out/daemon2.log" &
daemon=$!
sleep 6
expect "alarm on, both named" "$(bus 18140 "(d['alarm']['active'], d['alarm']['count'], [a['what'] for a in d['alarm']['recent']])")" \
  "(True, 2, [\"GatewayCfg write (the TAP's channel, network ID and key)\", 'SET_CHANNEL'])"
expect "the log marks them" "$(bus 18140 "[e['reconfig'] for e in d['events'] if e['reconfig']]")" \
  "[\"GatewayCfg write (the TAP's channel, network ID and key)\", 'SET_CHANNEL']"
expect "the write's block isn't shown" "$(bus 18140 "[(e['len'], e['hex'][:12], e['hex'][14:]) for e in d['events'] if e['sub'] == 13]")" \
  "[(43, '00 00 00 0D ', \" 01 00 + 36 bytes hidden (the TAP's network config and key)\")]"
expect "health names it" "$(api 18140 /api/health | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['status'], any('another device reconfigured the TAP (SET_CHANNEL, 2 frames' in p for p in d['problems']))")" "problem True"
expect "logged as an error" "$(grep -c 'another device sent the TAP a reconfiguration: GatewayCfg write' "$out/daemon2.log" || true)" 1
expect "MQTT: alarm on" "$(mosquitto_sub -p 18834 -C 1 -W 10 -t tigomonitor/test/system/state 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["tap_reconfigured"])')" True
kill $daemon; wait $daemon 2>/dev/null || true
expect "kept by name only" "$(python3 -c "import json; d=json.load(open('$out/state/bus-alarm.json')); print(d['count'], sorted(d['recent'][0]))")" "2 ['unix_ms', 'what']"

# --- run 3: a restart keeps it; clearing ends it ---
build/linux/tigomonitord --state-dir "$out/state" --web-dir components/tigo_server/web 2> "$out/daemon3.log" &
daemon=$!
sleep 3
expect "still on after a restart" "$(bus 18140 "(d['alarm']['active'], d['alarm']['count'])")" "(True, 2)"
expect "said at startup" "$(grep -c 'alarm is still on from before the restart' "$out/daemon3.log" || true)" 1
expect "cleared" "$(api 18140 /api/bus/alarm/clear post)" '{"cleared":true}'
expect "off after clearing" "$(bus 18140 "d['alarm']['active']")" False
expect "healthy again" "$(health 18140)" "ok []"
kill $daemon; wait $daemon 2>/dev/null || true
expect "cleared on disk" "$(python3 -c "import json; print(json.load(open('$out/state/bus-alarm.json'))['count'])")" 0
kill $cca; wait $cca 2>/dev/null || true

[ $fail -eq 0 ] && echo "bus log e2e passed" || { echo "bus log e2e FAILED"; exit 1; }
