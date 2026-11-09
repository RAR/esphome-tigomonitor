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
expect "polls counted, not listed" "$(bus 18140 "(d['stats']['polls'] > 20, [e for e in d['exchanges'] if e['cmd'] in ('0148', '0149')])")" "(True, [])"
expect "one row per exchange, named" "$(bus 18140 "sorted({(e['name'], e['sub_name']) for e in d['exchanges']})")" \
  "[('Tunnel', 'DiscState'), ('Tunnel', 'EncapString'), ('VERSION', '')]"
expect "each answered, timed" "$(bus 18140 "sorted({(e['sub_name'] or e['name'], e['result'], e['rt_ms'] is not None and e['rt_ms'] < 200) for e in d['exchanges'][:-1]})")" \
  "[('DiscState', 'answered', True), ('EncapString', 'delivered', True), ('VERSION', 'answered', True)]"
expect "the window decoded" "$(bus 18140 "sorted({e['summary'] for e in d['exchanges'] if e['sub_name'] == 'DiscState' and e['result'] == 'answered'})")" \
  "['Open, 900 s left · 2 of the 2 in the table have joined']"
expect "a query's text and node" "$(bus 18140 "[(e['summary'], e['node']) for e in d['exchanges'] if e['sub_name'] == 'EncapString'][:1]")" "[('^00Info', '0002')]"
expect "VERSION's reply" "$(bus 18140 "sorted({e['summary'] for e in d['exchanges'] if e['name'] == 'VERSION' and e['result'] == 'answered'})")" "['Mgate G8.59 Jan  1 2026']"
expect "payloads in hex" "$(bus 18140 "sorted({(e['request_hex'][:12], e['reply_hex'][:12]) for e in d['exchanges'] if e['sub_name'] == 'EncapString' and e['reply_hex']})")" "[('00 00 00 06 ', '00 0D 00 07 ')]"
expect "the strip" "$(bus 18140 "(d['stats']['window_s'], d['stats']['polls_empty'] > 20, d['stats']['count']['discovery'] > 0, d['stats']['rt_median_ms'] is not None, d['stats']['discovery'])")" \
  "(300, True, True, True, 'Open, 900 s left · 2 of the 2 in the table have joined')"
versions() { api "$1" /api/versions | python3 -c "import json,sys; d=json.load(sys.stdin); print([(k, e['addr'], e['model'], e['version'], e['since_unix_ms'] > 0) for k in ('taps', 'optimizers') for e in d[k]])"; }
expect "versions heard" "$(versions 18140)" \
  "[('taps', '1201', 'Mgate', 'G8.59 Jan  1 2026', True), ('optimizers', '0003', 'Mnode', 'K8.0120 (2D)', True)]"
smarts() { api "$1" /api/versions | python3 -c "import json,sys; d=json.load(sys.stdin); print([(o['addr'], o['settings'], o['stored'], o['heard_unix_ms'] > 0) for o in d['settings']])"; }
expect "an optimizer's settings, from its answer" "$(smarts 18140)" "[('0003', '0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 00 01', True, True)]"
mppt() { api "$1" /api/versions | python3 -c "import json,sys; d=json.load(sys.stdin); print([(o['addr'], o['params'], o['heard_unix_ms'] > 0) for o in d['mppt']])"; }
expect "its MPPT parameters, from its answer" "$(mppt 18140)" "[('0003', '12 40 08 12', True)]"
expect "listening: nothing ours, no other master" "$(bus 18140 "any(e['ours'] or e['other_master'] for e in d['exchanges'])")" False
expect "steady state: no alarm" "$(bus 18140 "d['alarm']")" "{'active': False, 'count': 0, 'first_unix_ms': 0, 'recent': []}"
expect "steady state: healthy" "$(health 18140)" "ok []"
expect "MQTT: alarm sensor announced" "$(mosquitto_sub -p 18834 -v --retained-only -W 1 -t 'homeassistant/binary_sensor/#' 2>/dev/null | grep -c '_tap_reconfigured' || true)" 1
kill $daemon; wait $daemon 2>/dev/null || true
kill $cca; wait $cca 2>/dev/null || true

# --- run 2: the CCA's UI reconfigures the TAP ---
python3 linux/tools/cca_on_bus.py "$out/bus" --reconfigure-after 2 --config-read &
cca=$!
sleep 0.5
build/linux/tigomonitord --state-dir "$out/state" --web-dir components/tigo_server/web 2> "$out/daemon2.log" &
daemon=$!
sleep 6
expect "alarm on, both named" "$(bus 18140 "(d['alarm']['active'], d['alarm']['count'], [a['what'] for a in d['alarm']['recent']])")" \
  "(True, 2, [\"GatewayCfg write (the TAP's channel, network ID and key)\", 'SET_CHANNEL'])"
expect "the log marks them" "$(bus 18140 "[e['reconfig'] for e in d['exchanges'] if e['reconfig']]")" \
  "[\"GatewayCfg write (the TAP's channel, network ID and key)\", 'SET_CHANNEL']"
expect "the write's block isn't shown" "$(bus 18140 "[(e['request_hex'][:12], e['request_hex'][14:], e['result']) for e in d['exchanges'] if e['sub_name'] == 'GatewayCfg write']")" \
  "[('00 00 00 0D ', \" 01 00 + 36 bytes hidden (the TAP's network config and key)\", 'answered')]"
expect "health names it" "$(api 18140 /api/health | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['status'], any('another device reconfigured the TAP (SET_CHANNEL, 2 frames' in p for p in d['problems']))")" "problem True"
radios() { api "$1" /api/versions | python3 -c "import json,sys; d=json.load(sys.stdin); print([(r['tap'], r['channel'], r['slt'], r['slm'], r['heard_unix_ms'] > 0) for r in d['radios']])"; }
expect "the TAP's radio, from its read" "$(radios 18140)" "[('1201', 18, 90, 60, True)]"
ids() { api "$1" /api/versions | python3 -c "import json,sys; d=json.load(sys.stdin); print([(i['tap'], i['mac'], i['heard_unix_ms'] > 0) for i in d['ids']])"; }
expect "the TAP's ID, from its answer to GET_MAC" "$(ids 18140)" "[('1201', '04C05B0000001234', True)]"
expect "the read's block isn't shown" "$(bus 18140 "any('EE EE' in (e['reply_hex'] or '') for e in d['exchanges'])")" False
expect "logged as an error" "$(grep -c 'another device sent the TAP a reconfiguration: GatewayCfg write' "$out/daemon2.log" || true)" 1
expect "MQTT: alarm on" "$(mosquitto_sub -p 18834 -C 1 -W 10 -t tigomonitor/test/system/state 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["tap_reconfigured"])')" True
kill $daemon; wait $daemon 2>/dev/null || true
expect "versions kept" "$(python3 -c "import json; d=json.load(open('$out/state/versions.json')); print(len(d['taps']), len(d['optimizers']))")" "1 1"
expect "radio kept" "$(python3 -c "import json; d=json.load(open('$out/state/versions.json')); print([(r['tap'], r['channel']) for r in d['radios']])")" "[('1201', 18)]"
expect "settings kept" "$(python3 -c "import json; d=json.load(open('$out/state/versions.json')); print([(o['addr'], o['stored']) for o in d['settings']])")" "[('0003', True)]"
expect "no key on disk" "$(grep -ci 'eeee\|238, 238' "$out/state/versions.json" || true)" 0
expect "kept by name only" "$(python3 -c "import json; d=json.load(open('$out/state/bus-alarm.json')); print(d['count'], sorted(d['recent'][0]))")" "2 ['unix_ms', 'what']"

# --- run 3: a restart keeps it; clearing ends it ---
build/linux/tigomonitord --state-dir "$out/state" --web-dir components/tigo_server/web 2> "$out/daemon3.log" &
daemon=$!
sleep 3
expect "still on after a restart" "$(bus 18140 "(d['alarm']['active'], d['alarm']['count'])")" "(True, 2)"
expect "radio back after a restart" "$(radios 18140)" "[('1201', 18, 90, 60, True)]"
expect "the TAP's ID back after a restart" "$(ids 18140)" "[('1201', '04C05B0000001234', True)]"
expect "MPPT parameters back after a restart" "$(mppt 18140)" "[('0003', '12 40 08 12', True)]"
expect "settings back after a restart" "$(smarts 18140)" "[('0003', '0FFF 0000 0FFF 0008 00C8 00C8 000A 0154 0154 S 00 01', True, True)]"
expect "said at startup" "$(grep -c 'alarm is still on from before the restart' "$out/daemon3.log" || true)" 1
expect "cleared" "$(api 18140 /api/bus/alarm/clear post)" '{"cleared":true}'
expect "off after clearing" "$(bus 18140 "d['alarm']['active']")" False
expect "healthy again" "$(health 18140)" "ok []"
kill $daemon; wait $daemon 2>/dev/null || true
expect "cleared on disk" "$(python3 -c "import json; print(json.load(open('$out/state/bus-alarm.json'))['count'])")" 0
kill $cca; wait $cca 2>/dev/null || true

[ $fail -eq 0 ] && echo "bus log e2e passed" || { echo "bus log e2e FAILED"; exit 1; }
