#!/bin/sh
# The bus master without a TAP: tigomonitord in the master role against a
# mock TAP on a pseudo-terminal (linux/tools/mock_tap.py). Checks it polls and
# decodes, checks the discovery window before ever renewing it, renews with the TAP's
# node count, lets the discovery window lapse while it's frozen, and stops transmitting
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
# The master's status over MQTT: one state message, and the retained configs.
mqtt_state() { mosquitto_sub -p 18831 -C 1 -W 15 -t tigomonitor/test/master/state 2>/dev/null || true; }
mqtt_configs() { mosquitto_sub -p 18831 -v --retained-only -W 1 -t 'homeassistant/#' 2>/dev/null | grep -c "_bus_master" || true; }
field() { python3 -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$1"; }
mosquitto -p 18831 -d
health() {  # health PORT: "<HTTP code> <status> <problems>"
  python3 - "$1" <<'PY'
import json, sys, urllib.error, urllib.request as u
try:
    r = u.urlopen(f'http://127.0.0.1:{sys.argv[1]}/api/health', timeout=5); code, d = r.status, json.load(r)
except urllib.error.HTTPError as e:
    code, d = e.code, json.load(e)
print(code, d['status'], json.dumps(d['problems']))
PY
}
stat() { python3 -c "import json,sys; d=json.load(open('$1')); print(eval(sys.argv[1]))" "$2"; }

# --- run 1: polls, discovery window, renewal, and a frozen daemon ---
# The master reads the discovery window every 20 s, as the CCA does, so a
# 3 s window is renewed at the 20 s check, and again at each check after.
python3 linux/tools/mock_tap.py "$out/tap" --window 3 --renew-to 3 --nodes 45 --stats "$out/tap.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18092, "device_id": "test", "publish_interval_s": 5, "mqtt": {"host": "127.0.0.1", "port": 18831}}\n' "$PWD/$out/tap" \
  > "$out/state/settings.json"
build/linux/tigomonitord --state-dir "$out/state" --web-dir components/tigo_server/web 2> "$out/daemon.log" &
daemon=$!
trap 'kill -CONT $daemon 2>/dev/null || true; kill $daemon $tap 2>/dev/null || true; pkill mosquitto || true' EXIT
renewals() { stat "$out/tap.json" "len(d['renewals'])"; }
for _ in $(seq 60); do [ -f "$out/tap.json" ] && [ "$(renewals)" -ge 1 ] && break; sleep 0.5; done
expect "panels decoded" "$(api 18092 /api/devices | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["devices"]))')" 6
expect "first discovery frame is a check" "$(stat "$out/tap.json" "d['first_discovery_frame']")" check
expect "renewed with the TAP's node count" "$(stat "$out/tap.json" "[r['nodes'] for r in d['renewals']][:1]")" "[45]"
expect "cursor followed the TAP" "$(stat "$out/tap.json" "d['cursor_mismatches']")" 0
expect "nothing outside the allow-list" "$(stat "$out/tap.json" "d['other']")" "[]"
master="$(api 18092 /api/master)"
expect "/api/master active" "$(echo "$master" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["active"], d["fault"], d["renewals"] >= 1, d["discovery"]["node_count"], d["discovery"]["joined"], d["discovery"]["in_table"])')" "True none True 45 40 45"
expect "MQTT: master active" "$(mqtt_state | field '(d["status"], d["problem"], d["discovery_nodes"], d["renewals"] >= 1)')" "('active', False, 45, True)"
expect "MQTT: master entities announced" "$(mqtt_configs)" 8
expect "healthy" "$(health 18092)" '200 ok []'
# Frozen across the next renewal it would make (the 20 s check after this
# one): nothing renews the discovery window until the daemon runs again.
before="$(renewals)"
kill -STOP $daemon
sleep 25
sleep 1.5  # the mock's stats file lags by up to a second
during="$(renewals)"
kill -CONT $daemon
for _ in $(seq 20); do [ "$(renewals)" -gt "$during" ] && break; sleep 0.5; done
expect "no renewal while frozen" "$((during - before))" 0
expect "renews once running again" "$([ "$(renewals)" -gt "$during" ] && echo yes || echo no)" yes
expect "the freeze shows as the longest gap" "$(api 18092 /api/master | python3 -c 'import json,sys; g=json.load(sys.stdin)["gap_ms"]; print(g["max"] >= 25000, g["max_at_unix"] > 0, g["recent"] >= 25000)')" "True True True"
expect "windows run down and renewed are not losses" "$(api 18092 /api/master | python3 -c 'import json,sys; t=json.load(sys.stdin)["tap"]; print(t["lost"], t["back_without_discovery"])')" "0 0"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 2: a second master appears ---
python3 linux/tools/mock_tap.py "$out/tap2" --foreign-after 3 --stats "$out/tap2.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18093, "device_id": "test", "publish_interval_s": 5, "mqtt": {"host": "127.0.0.1", "port": 18831}}\n' "$PWD/$out/tap2" \
  > "$out/state2/settings.json"
build/linux/tigomonitord --state-dir "$out/state2" --web-dir components/tigo_server/web 2> "$out/daemon2.log" &
daemon=$!
sleep 6
expect "second master stops it" "$(api 18093 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["active"], d["fault"])')" "False second_master"
expect "nothing sent after the second master" "$(stat "$out/tap2.json" "d['frames_after_foreign'] <= 1")" True
expect "MQTT: the fault shows" "$(mqtt_state | field '(d["status"], d["problem"])')" "('second_master', True)"
expect "health names the fault" "$(health 18093 | cut -c1-39)" '503 problem ["bus master: second_master'
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 3: the port appears after the master starts (a late USB adapter) ---
mkdir -p "$out/state3"
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18094}\n' "$PWD/$out/tap3" \
  > "$out/state3/settings.json"
build/linux/tigomonitord --state-dir "$out/state3" --web-dir components/tigo_server/web 2> "$out/daemon3.log" &
daemon=$!
sleep 2
# Every second reply lost: the master spends most of its time waiting on
# the TAP, where the freezes below then mostly land.
python3 linux/tools/mock_tap.py "$out/tap3" --drop-every 2 --stats "$out/tap3.json" &
tap=$!
sleep 8
expect "opens a port that appears later" "$(api 18094 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["active"], d["polls"] > 0)')" "True True"
# Freezes land wherever the daemon happens to be, mostly waiting on the
# TAP's reply: each must still raise the longest pause past its length.
missed=""
for secs in 2 3 4 5; do
  kill -STOP $daemon; sleep $secs; kill -CONT $daemon; sleep 1
  max="$(api 18094 /api/master | python3 -c 'import json,sys; print(json.load(sys.stdin)["gap_ms"]["max"])')"
  [ "$max" -ge $((secs * 1000 - 100)) ] || missed="$missed ${secs}s(max $max)"
done
expect "every freeze counted, mid-request too" "${missed:-none}" none
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

# --- run 6b: startup mode against a TAP that refuses polls: one warning, no re-probe loop ---
# A refusal (0x0006) is the TAP answering. It isn't lost, so startup mode must
# not go looking for it again, and the master backs off to a request a second.
mkdir -p "$out/state6b"
python3 linux/tools/mock_tap.py "$out/tap6b" --bare --stats "$out/tap6b.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18125}\n' \
  "$PWD/$out/tap6b" > "$out/state6b/settings.json"
build/linux/tigomonitord --state-dir "$out/state6b" --web-dir components/tigo_server/web 2> "$out/daemon6b.log" &
daemon=$!
sleep 6
before=$(stat "$out/tap6b.json" "d['refused']")
sleep 10
after=$(stat "$out/tap6b.json" "d['refused']")
expect "refusing TAP probed once" "$(stat "$out/tap6b.json" "d['probes']")" 1
expect "no looking for it again" "$(grep -c 'looking for it again' "$out/daemon6b.log" || true)" 0
expect "refused polls warned once" "$(grep -c 'refuses polls' "$out/daemon6b.log" || true)" 1
expect "refusing TAP not counted lost" "$(api 18125 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["tap"]["lost"], d["fault"], "refuses polls" in d["detail"])')" "0 no_reply True"
expect "backed off to about one a second" "$(python3 -c "print($after - $before <= 15)")" True
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 7: startup discovery (TAP boots at the factory address) ---
mkdir -p "$out/state7"
python3 linux/tools/mock_tap.py "$out/tap7" --factory --window 600 --nodes 44 --stats "$out/tap7.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18098}\n' \
  "$PWD/$out/tap7" > "$out/state7/settings.json"
build/linux/tigomonitord --state-dir "$out/state7" --web-dir components/tigo_server/web 2> "$out/daemon7.log" &
daemon=$!
for _ in $(seq 40); do [ "$(api 18098 /api/master | python3 -c 'import json,sys; print(json.load(sys.stdin)["phase"])')" = running ] && break; sleep 0.5; done
sleep 1
expect "TAP re-addressed from factory to 0x1201" "$(stat "$out/tap7.json" "d['addressed_to']")" 1201
m7="$(api 18098 /api/master)"
expect "running after discovery" "$(echo "$m7" | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["active"], d["fault"], d["phase"])')" "True none running"
expect "polling the re-addressed TAP" "$(api 18098 /api/devices | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["devices"]) > 0)')" True
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 8: startup, but the TAP is already addressed -> no re-addressing writes ---
mkdir -p "$out/state8"
python3 linux/tools/mock_tap.py "$out/tap8" --window 600 --nodes 44 --stats "$out/tap8.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18099}\n' \
  "$PWD/$out/tap8" > "$out/state8/settings.json"
build/linux/tigomonitord --state-dir "$out/state8" --web-dir components/tigo_server/web 2> "$out/daemon8.log" &
daemon=$!
for _ in $(seq 40); do [ "$(api 18099 /api/master | python3 -c 'import json,sys; print(json.load(sys.stdin)["phase"])')" = running ] && break; sleep 0.5; done
sleep 1
expect "no re-addressing on an addressed TAP" "$(stat "$out/tap8.json" "d['addressed_to']")" None
expect "went straight to running" "$(api 18099 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["phase"], d["polls"] > 0)')" "running True"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 9: startup switched on while running, then the TAP power-cycled ---
# Turning on "Bring up the TAP" alone takes effect without a restart; and a
# TAP that comes back at the factory address is found and re-addressed.
mkdir -p "$out/state9"
python3 linux/tools/mock_tap.py "$out/tap9" --window 600 --nodes 44 --stats "$out/tap9.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18100}\n' \
  "$PWD/$out/tap9" > "$out/state9/settings.json"
build/linux/tigomonitord --state-dir "$out/state9" --web-dir components/tigo_server/web 2> "$out/daemon9.log" &
daemon=$!
sleep 3
expect "startup switched on" "$(api 18100 /api/settings '{"master_startup": true}')" 200
sleep 3
expect "probed without a restart" "$(grep -c 'the TAP is already at 1201' "$out/daemon9.log")" 1
kill $tap; wait $tap 2>/dev/null || true
python3 linux/tools/mock_tap.py "$out/tap9" --factory --window 600 --nodes 44 --stats "$out/tap9b.json" &
tap=$!
for _ in $(seq 40); do [ -f "$out/tap9b.json" ] && [ "$(stat "$out/tap9b.json" "d['addressed_to']")" = 1201 ] && break; sleep 0.5; done
expect "power-cycled TAP re-addressed" "$(stat "$out/tap9b.json" "d['addressed_to']")" 1201
for _ in $(seq 20); do [ "$(api 18100 /api/master | python3 -c 'import json,sys; print(json.load(sys.stdin)["phase"])')" = running ] && break; sleep 0.5; done
expect "polling it again" "$(api 18100 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["phase"], d["fault"])')" "running none"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 10: a second TAP on the bus, left at the factory address ---
# The scan after bring-up finds it and says so; it is never re-addressed or
# polled, and the first TAP runs as normal. Once with the first TAP already
# addressed, once with both at the factory address after a power cut.
for mode in warm cold; do
  mkdir -p "$out/state10$mode"
  flag=""; [ $mode = cold ] && flag="--factory"
  python3 linux/tools/mock_tap.py "$out/tap10$mode" $flag --second-tap --window 600 --nodes 44 --stats "$out/tap10$mode.json" &
  tap=$!
  sleep 0.5
  printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18101}\n' \
    "$PWD/$out/tap10$mode" > "$out/state10$mode/settings.json"
  build/linux/tigomonitord --state-dir "$out/state10$mode" --web-dir components/tigo_server/web 2> "$out/daemon10$mode.log" &
  daemon=$!
  for _ in $(seq 40); do [ "$(api 18101 /api/master | python3 -c 'import json,sys; print(json.load(sys.stdin)["phase"])' 2>/dev/null)" = running ] && break; sleep 0.5; done
  sleep 2
  expect "$mode: second TAP flagged" "$(api 18101 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["fault"], d["extra_taps"], "04c05b3000d4e5f6" in d["detail"])')" "extra_tap ['04c05b3000d4e5f6'] True"
  expect "$mode: first TAP polled" "$(api 18101 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["phase"], d["polls"] > 0)')" "running True"
  expect "$mode: second TAP never re-addressed" "$(stat "$out/tap10$mode.json" "d['second_written']")" 0
  expect "$mode: logged" "$(grep -c 'another TAP answered at the factory address' "$out/daemon10$mode.log")" 1
  kill $daemon; wait $daemon 2>/dev/null || true
  kill $tap; wait $tap 2>/dev/null || true
done

# --- run 11: housekeeping learned while listening ---
# A replayed bus with the CCA's 0x41s on it: the requests are learned, one
# per stream and target, and the TAP's replies and other frames are not.
# The tail is invented; the roof's isn't committed.
mkdir -p "$out/state11"
python3 - "$out/bus11.bin" <<'PY'
import sys
sys.path.insert(0, 'linux/tools')
from mock_tap import encode
TAIL = bytes(range(0x11, 0x22))
def h41(seq, tuid, stream):
    return encode(0x1201, 0x0B0F, bytes([0, 0, 0, 0x41, seq, tuid >> 8, tuid & 0xFF, 0, 0, 0, stream, 0]) + TAIL)
out = b''
for seq, (tuid, stream) in enumerate([(0x0001, 1), (0x0027, 2), (0x0009, 4), (0x0001, 1), (0x0028, 1)], 1):
    out += h41(seq, tuid, stream) + encode(0x9201, 0x0B10, bytes([0, 0x0D, 0, 0x41, seq]))
out += encode(0x9201, 0x0B0F, bytes([0, 0, 0, 0x41, 9, 0, 0x30, 0, 0, 0, 1, 0]) + TAIL)  # not a request
out += encode(0x1201, 0x0B0F, bytes([0, 0, 0, 0x41, 9, 0, 0x31, 0, 0, 0, 3, 0]) + TAIL)  # no such stream
open(sys.argv[1], 'wb').write(out)
PY
build/linux/tigomonitord --state-dir "$out/state11" --port 0 --replay "$out/bus11.bin" --speed 0 2> "$out/daemon11.log"
expect "learned one per stream and target" "$(stat "$out/state11/cca-0x41.json" "[(e['stream'], e['tuid']) for e in d['entries']]")" "[(1, 1), (2, 39), (4, 9), (1, 40)]"
expect "kept on this machine only" "$(python3 -c "import os; print(oct(os.stat('$out/state11/cca-0x41.json').st_mode)[-3:])")" 600

# --- run 12: housekeeping sent, from what was learned ---
# Warm: the TAP already at its id, so 0x0B00 01 straight away, then each
# stream's 0x41 a few seconds apart, 02 then 01 (at 0x0001) then 04: the
# learned content, to optimizers the mock's polls reported. Cold: after a
# re-address, 0x0B00 00 then 01.
for mode in warm cold; do
  mkdir -p "$out/state12$mode"
  cp "$out/state11/cca-0x41.json" "$out/state12$mode/"
  flag=""; [ $mode = cold ] && flag="--factory"
  python3 linux/tools/mock_tap.py "$out/tap12$mode" $flag --window 600 --nodes 44 --stats "$out/tap12$mode.json" &
  tap=$!
  sleep 0.5
  printf '{"bus_role": "master", "master_startup": true, "master_housekeeping": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18102}\n' \
    "$PWD/$out/tap12$mode" > "$out/state12$mode/settings.json"
  build/linux/tigomonitord --state-dir "$out/state12$mode" --web-dir components/tigo_server/web 2> "$out/daemon12$mode.log" &
  daemon=$!
  for _ in $(seq 60); do [ -f "$out/tap12$mode.json" ] && [ "$(stat "$out/tap12$mode.json" "len(d['housekeeping'])")" -ge 3 ] && break; sleep 0.5; done
  want_session="['01']"; [ $mode = cold ] && want_session="['00', '01']"
  expect "$mode: 0x0B00 at the start" "$(stat "$out/tap12$mode.json" "d['session']")" "$want_session"
  expect "$mode: one 0x41 per stream, 02 01 04" "$(stat "$out/tap12$mode.json" "[h['args'][10:12] for h in d['housekeeping'][:3]]")" "['02', '01', '04']"
  expect "$mode: stream 01 starts at 0x0001" "$(stat "$out/tap12$mode.json" "[h['args'][:4] for h in d['housekeeping'][:3]][1]")" 0001
  expect "$mode: learned content, to panels heard" "$(python3 -c "
import json, urllib.request as u
content = {e['stream']: e['args'][4:] for e in json.load(open('$out/state11/cca-0x41.json'))['entries']}
panels = {d['addr'].lower() for d in json.load(u.urlopen('http://127.0.0.1:18102/api/devices'))['devices']}
hk = json.load(open('$out/tap12$mode.json'))['housekeeping']
print(len(panels) > 0, all(h['args'][4:] == content[int(h['args'][10:12], 16)] and
      (h['args'][:4] == '0001' or h['args'][:4] in panels) for h in hk))")" "True True"
  expect "$mode: still polling, nothing else sent" "$(stat "$out/tap12$mode.json" "(d['polls'] > 0, d['other'])")" "(True, [])"
  expect "$mode: /api/master counts" "$(api 18102 /api/master | python3 -c 'import json,sys; h=json.load(sys.stdin)["housekeeping"]; print(h["on"], h["learned_41"], h["sent_41"] >= 3, h["sent_session"])')" "True 4 True $([ $mode = cold ] && echo 2 || echo 1)"
  kill $daemon; wait $daemon 2>/dev/null || true
  kill $tap; wait $tap 2>/dev/null || true
done

# --- run 13: housekeeping on with nothing learned sends the built-in 0x41 content ---
mkdir -p "$out/state13"
python3 linux/tools/mock_tap.py "$out/tap13" --window 600 --nodes 44 --stats "$out/tap13.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_housekeeping": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18103}\n' \
  "$PWD/$out/tap13" > "$out/state13/settings.json"
build/linux/tigomonitord --state-dir "$out/state13" --web-dir components/tigo_server/web 2> "$out/daemon13.log" &
daemon=$!
for _ in $(seq 60); do [ -f "$out/tap13.json" ] && [ "$(stat "$out/tap13.json" "len(d['housekeeping'])")" -ge 3 ] && break; sleep 0.5; done
expect "nothing learned: 0x0B00 01, then the built-in content per stream" "$(stat "$out/tap13.json" "(d['session'], [h['args'][4:] for h in d['housekeeping'][:3]], d['polls'] > 0)")" \
  "(['01'], ['0000000200aa0000000004050fe808a00a188ff605c7', '0000000100aa0000000004050fe808a00a188ff605c7', '0000000400aa0000000004050fe808a00a188ff605c7'], True)"
expect "says it uses the built-in content" "$(grep -c 'none heard: using the built-in content' "$out/daemon13.log")" 1
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 14: a cold bring-up with everything on, in the CCA's order ---
# The TAP at the factory address: re-addressed, hopped to 1202, RESET, quiet,
# back at 1201; then VERSION, 0x0E02, 0B00 00, 0D 0000, 0B00 01, 0D 0001, the
# 0x22 pair and two walks of the node table; then the 0x06 queries, with the
# CCA's two writes where the mock's answers call for them.
mkdir -p "$out/state14"
python3 linux/tools/mock_tap.py "$out/tap14" --factory --window 600 --nodes 44 --stats "$out/tap14.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "master_housekeeping": true, "master_queries": true, "master_optimizer_writes": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18104}\n' \
  "$PWD/$out/tap14" > "$out/state14/settings.json"
build/linux/tigomonitord --state-dir "$out/state14" --web-dir components/tigo_server/web 2> "$out/daemon14.log" &
daemon=$!
for _ in $(seq 240); do
  [ -f "$out/tap14.json" ] && [ "$(stat "$out/tap14.json" "sum(q['text'].startswith(('#00w255', '^00Smrt_')) for q in d['queries'])")" -ge 2 ] && break
  sleep 0.5
done
sleep 2
log() { stat "$out/tap14.json" "$1"; }
expect "the hop: to 1202, read there, RESET" "$(log "[x['f'][:9] for x in d['log']][[x['f'] for x in d['log']].index('0000/0010') - 2:][:3]")" "['1201/003C', '1202/003A', '0000/0010']"
expect "the hop's SET_ID_BY_MAC names 1202" "$(log "[x['f'] for x in d['log'] if x['f'].startswith('1201/003C')][0][-4:]")" 1202
expect "quiet for 4.5 s after the RESET, nothing sent into it" "$(log "(lambda i: (d['log'][i + 1]['t'] - d['log'][i]['t'] >= 4.4, d['quiet_broken']))([x['f'] for x in d['log']].index('0000/0010'))")" "(True, 0)"
expect "back at 1201, then the CCA's bring-up order" "$(log "(lambda i: [x['f'] for x in d['log'][i + 1:i + 11]])([x['f'] for x in d['log']].index('0000/0010'))")" \
  "['1201/003A', '1201/000A', '1201/0E02', '1201/0B00 00', '1201/0B0F 0d 0000', '1201/0B00 01', '1201/0B0F 0d 0001', '1201/0B0F 22 0000', '1201/0B0F 26 0000', '1201/0B0F 26 0008']"
expect "the second 0x22 a second after the first" "$(log "(lambda b: (b[1] - b[0] >= 0.9))([x['t'] for x in d['log'] if x['f'].startswith('1201/0B0F 22')][:2])")" True
expect "two walks, each page from the last reply's end" "$(log "d['walk'][:4]")" "['0000', '0008', '0000', '0008']"
expect "all five reads to every panel" "$(log "sorted({(q['node'], q['text']) for q in d['queries'] if q['text'].startswith('^00') and '_' not in q['text']}) == sorted((f'{n:04X}', t) for n in range(2, 8) for t in ('^00Version', '^00Tests', '^00Info', '^00Smrt', '^00Mppt'))")" True
expect "the writes: w255 to the panel whose Info asked, Smrt_ S to the one in mode N" "$(log "sorted((q['node'], q['text']) for q in d['queries'] if q['text'].startswith(('#', '^00Smrt_')))")" \
  "[('0002', '#00w255'), ('0003', '^00Smrt_........S0000...0000')]"
expect "nothing unrecognised sent" "$(log "d['other']")" "[]"
expect "/api/master" "$(api 18104 /api/master | python3 -c 'import json,sys; m=json.load(sys.stdin); h=m["housekeeping"]; q=m["queries"]; print(m["tap_version"], h["sent_broadcast"] >= 2, h["sent_walk_pages"] >= 4, q["on"], q["writes"], q["sent_writes"], q["answers"] > 0)')" \
  "Mgate Version G1.00 Jan  1 2026 True True True True 2 True"
expect "logs the TAP's version" "$(grep -c 'TAP version: Mgate Version G1.00' "$out/daemon14.log")" 1
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 15: queries without writes, housekeeping off, TAP already at its id ---
mkdir -p "$out/state15"
python3 linux/tools/mock_tap.py "$out/tap15" --window 600 --nodes 44 --stats "$out/tap15.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "master_queries": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18105}\n' \
  "$PWD/$out/tap15" > "$out/state15/settings.json"
build/linux/tigomonitord --state-dir "$out/state15" --web-dir components/tigo_server/web 2> "$out/daemon15.log" &
daemon=$!
# All 30 first reads (a write would queue behind them), then a few seconds more.
for _ in $(seq 160); do [ -f "$out/tap15.json" ] && [ "$(stat "$out/tap15.json" "len(d['queries'])")" -ge 30 ] && break; sleep 0.5; done
sleep 6
expect "reads go out and are answered" "$(stat "$out/tap15.json" "(len(d['queries']) >= 20, d['answered'] >= 20)")" "(True, True)"
# The TAP counts the answers in its cursor, as power packets: 7 of 7 in the rig capture.
expect "the cursor counts the answers too" "$(stat "$out/tap15.json" "d['cursor_mismatches']")" 0
expect "no writes without the setting" "$(stat "$out/tap15.json" "[q for q in d['queries'] if q['text'].startswith(('#', '^00Smrt_'))]")" "[]"
# Only the one VERSION every start asks.
expect "warm, housekeeping off: no bring-up reads, 0x22s or walks" "$(stat "$out/tap15.json" "([x['f'] for x in d['log'] if x['f'][5:9] in ('000A', '0E02', '0010')], d['broadcast'], d['walk'], d['session'])")" "(['1201/000A'], [], [], [])"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 15b: queries to a node the TAP can't reach (0x0146 00 02) ---
# The TAP's 0x0146 is its answer: the query ends there, with no timeout, no
# retry before the usual schedule, and a count, not a lost TAP.
mkdir -p "$out/state15b"
python3 linux/tools/mock_tap.py "$out/tap15b" --window 600 --nodes 44 --not-taken 0003 --stats "$out/tap15b.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_queries": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18126}\n' \
  "$PWD/$out/tap15b" > "$out/state15b/settings.json"
build/linux/tigomonitord --state-dir "$out/state15b" --web-dir components/tigo_server/web 2> "$out/daemon15b.log" &
daemon=$!
for _ in $(seq 160); do [ -f "$out/tap15b.json" ] && [ "$(stat "$out/tap15b.json" "len(d['queries'])")" -ge 30 ] && break; sleep 0.5; done
sleep 3
expect "node 0003 was asked its five reads, once each" "$(stat "$out/tap15b.json" "(d['not_taken'], sorted(q['text'] for q in d['queries'] if q['node'] == '0003'))")" \
  "(5, ['^00Info', '^00Mppt', '^00Smrt', '^00Tests', '^00Version'])"
expect "counted as not taken, not as timeouts" "$(api 18126 /api/master | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["queries"]["not_taken"], d["timeouts"], d["tap"]["lost"], d["fault"])')" "5 0 0 none"
expect "said once, naming the node" "$(grep -c "optimizer 0003: the TAP couldn't deliver a query" "$out/daemon15b.log" || true)" 1
expect "the other nodes still answered" "$(stat "$out/tap15b.json" "d['answered'] >= 20")" True
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 16: a TAP slow back from the bring-up's RESET ---
# Deaf for 12 s, longer than the reads after the quiet: the bring-up fails,
# and after a pause the master looks again, finds it at 1201, and polls.
mkdir -p "$out/state16"
python3 linux/tools/mock_tap.py "$out/tap16" --factory --reset-quiet 12 --window 600 --nodes 44 --stats "$out/tap16.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18106}\n' \
  "$PWD/$out/tap16" > "$out/state16/settings.json"
build/linux/tigomonitord --state-dir "$out/state16" --web-dir components/tigo_server/web 2> "$out/daemon16.log" &
daemon=$!
for _ in $(seq 160); do [ -f "$out/tap16.json" ] && [ "$(stat "$out/tap16.json" "d['polls']")" -ge 5 ] && break; sleep 0.5; done
expect "one RESET, said it failed, then found it at 1201 and polled" "$(stat "$out/tap16.json" "(d['resets'], d['polls'] >= 5, d['addressed_to'])")" "(1, True, '1201')"
expect "logged the failure" "$(grep -c 'no answer to the reads at the configured id and the factory address after the RESET' "$out/daemon16.log")" 1
expect "fault cleared once polling" "$(api 18106 /api/master | python3 -c 'import json,sys; m=json.load(sys.stdin); print(m["fault"], m["phase"])')" "none running"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 17: a TAP whose power is cut every 15 s ---
# As on the roof with the CCA unplugged from the bus but still powering the
# TAP: it goes quiet for 8 s and comes back at 1201 with its discovery
# window closed. The master finds it, renews, and counts each loss and each
# return with discovery closed. Run 1's windows run down to 0 and are renewed: none of those count.
mkdir -p "$out/state17"
python3 linux/tools/mock_tap.py "$out/tap17" --power-cut-every 15 --window 600 --nodes 44 --stats "$out/tap17.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18107}\n' \
  "$PWD/$out/tap17" > "$out/state17/settings.json"
build/linux/tigomonitord --state-dir "$out/state17" --web-dir components/tigo_server/web 2> "$out/daemon17.log" &
daemon=$!
for _ in $(seq 160); do [ -f "$out/tap17.json" ] && [ "$(stat "$out/tap17.json" "len(d['renewals'])")" -ge 2 ] && break; sleep 0.5; done
sleep 2
expect "two cuts, renewed after each" "$(stat "$out/tap17.json" "(d['power_cuts'] >= 2, len(d['renewals']) >= 2)")" "(True, True)"
expect "/api/master counts the losses and returns with discovery closed" "$(api 18107 /api/master | python3 -c 'import json,sys; m=json.load(sys.stdin); t=m["tap"]; print(t["lost"] >= 2, t["back_without_discovery"] >= 2, t["last_lost_unix"] > 0, t["lost"] == len(t["recent_lost_unix"]))')" \
  "True True True True"
expect "no stalls of its own: under 2 s between requests" "$(api 18107 /api/master | python3 -c 'import json,sys; g=json.load(sys.stdin)["gap_ms"]; print(0 < g["max"] < 2000)')" True
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 18: a slow SD card ---
# Every fsync takes 0.9 s (linux/tools/slow_fsync.c). Saving state.json,
# on the Frame 27 the mock sends and every 10 s after, must not hold the
# bus up: no pause over 300 ms, and the state still lands on disk.
mkdir -p "$out/state18"
cc -shared -fPIC -o "$out/slow_fsync.so" linux/tools/slow_fsync.c -ldl
python3 linux/tools/mock_tap.py "$out/tap18" --window 600 --nodes 44 --stats "$out/tap18.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18108}\n' \
  "$PWD/$out/tap18" > "$out/state18/settings.json"
LD_PRELOAD="$PWD/$out/slow_fsync.so" build/linux/tigomonitord --state-dir "$out/state18" --web-dir components/tigo_server/web 2> "$out/daemon18.log" &
daemon=$!
for _ in $(seq 60); do [ -f "$out/state18/state.json" ] && break; sleep 0.5; done
sleep 25
expect "state saved with a slow fsync" "$(python3 -c "import json; print(len(json.load(open('$out/state18/state.json'))['nodes']) > 0)")" True
expect "no pause over 300 ms while it saved" "$(api 18108 /api/master | python3 -c 'import json,sys; g=json.load(sys.stdin)["gap_ms"]; print(g["max"] < 300, g["max"])' | cut -d' ' -f1)" True
api 18108 /api/master | python3 -c 'import json,sys; print("  (longest pause:", json.load(sys.stdin)["gap_ms"]["max"], "ms)")'
expect "no CCA discovery while the Pi runs the bus" "$(api 18108 /api/cca/discovery/start '{}')" 409
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 19: a bring-up whose replies never come ---
# (a) A TAP at the factory address that never acks the SET_485_ID broadcast.
# The master asks a few times, says which step went unanswered, and stops
# sending until it looks again 30 s later; it used to ask ~20 times a second
# forever with the fault still "none".
mkdir -p "$out/state19"
python3 linux/tools/mock_tap.py "$out/tap19" --factory --mute-claims 1000 --window 600 --nodes 44 --stats "$out/tap19.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18109}\n' \
  "$PWD/$out/tap19" > "$out/state19/settings.json"
build/linux/tigomonitord --state-dir "$out/state19" --web-dir components/tigo_server/web 2> "$out/daemon19.log" &
daemon=$!
for _ in $(seq 40); do [ "$(api 18109 /api/master 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)["fault"])' 2>/dev/null)" = discovery_failed ] && break; sleep 0.5; done
expect "unanswered claim: fault names the step" "$(api 18109 /api/master | python3 -c 'import json,sys; m=json.load(sys.stdin); print(m["fault"], "SET_485_ID" in m["detail"])')" "discovery_failed True"
sleep 1
claims="$(stat "$out/tap19.json" "d['claims']")"
sleep 5
expect "asked five times, then stopped sending" "$claims $(stat "$out/tap19.json" "d['claims']")" "5 5"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true
# (b) The hop taken, then nothing answers at 1202. The TAP is somewhere the
# master never looks again, so the RESET that puts it back goes out anyway,
# and the TAP is polled at 1201.
mkdir -p "$out/state19b"
python3 linux/tools/mock_tap.py "$out/tap19b" --factory --deaf-hop --window 600 --nodes 44 --stats "$out/tap19b.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18110}\n' \
  "$PWD/$out/tap19b" > "$out/state19b/settings.json"
build/linux/tigomonitord --state-dir "$out/state19b" --web-dir components/tigo_server/web 2> "$out/daemon19b.log" &
daemon=$!
for _ in $(seq 60); do [ -f "$out/tap19b.json" ] && [ "$(stat "$out/tap19b.json" "d['polls']")" -ge 5 ] && break; sleep 0.5; done
expect "deaf after the hop: RESET anyway, then polled at 1201" "$(stat "$out/tap19b.json" "(d['deaf'], d['resets'], d['polls'] >= 5, d['addressed_to'])")" "(5, 1, True, '1201')"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 20: a TAP that refuses renewals for a while ---
# Its first three renewals are acked but leave the discovery window out. The master
# flags discovery_refused, then tries once each 30 s rather than stopping until
# a restart, and the fourth renewal brings the discovery window back.
mkdir -p "$out/state20"
python3 linux/tools/mock_tap.py "$out/tap20" --window 2 --refuse-renewals 3 --nodes 44 --stats "$out/tap20.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "serial_port": "%s", "gateway_id": 4609, "web_port": 18111}\n' \
  "$PWD/$out/tap20" > "$out/state20/settings.json"
build/linux/tigomonitord --state-dir "$out/state20" --web-dir components/tigo_server/web 2> "$out/daemon20.log" &
daemon=$!
for _ in $(seq 40); do [ -f "$out/tap20.json" ] && [ "$(stat "$out/tap20.json" "len(d['renewals'])")" -ge 3 ] && break; sleep 0.5; done
sleep 2
expect "three renewals refused: flagged" "$(api 18111 /api/master | python3 -c 'import json,sys; print(json.load(sys.stdin)["fault"])')" discovery_refused
for _ in $(seq 90); do [ "$(stat "$out/tap20.json" "len(d['renewals'])")" -ge 4 ] && break; sleep 0.5; done
sleep 2
expect "tried again after the backoff, and the discovery window is back" "$(api 18111 /api/master | python3 -c 'import json,sys; m=json.load(sys.stdin); print(m["fault"], m["discovery"]["remaining_s"] > 0)')" "none True"
expect "no renewal between the third and the fourth for 30 s" "$(stat "$out/tap20.json" "round(d['renewals'][3]['t'] - d['renewals'][2]['t']) >= 29")" True
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 21: node table pages that take 120 ms to come back ---
# Longer than a poll's 50 ms reply wait, as a real 12-entry page is close
# to. Each page is waited for, so both walks read the whole table: from 0,
# each page from the last reply's end, to the empty page past the end.
mkdir -p "$out/state21"
python3 linux/tools/mock_tap.py "$out/tap21" --page-delay 0.12 --panels 30 --window 600 --nodes 44 --stats "$out/tap21.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_housekeeping": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18112}\n' \
  "$PWD/$out/tap21" > "$out/state21/settings.json"
build/linux/tigomonitord --state-dir "$out/state21" --web-dir components/tigo_server/web 2> "$out/daemon21.log" &
daemon=$!
for _ in $(seq 40); do [ -f "$out/tap21.json" ] && [ "$(stat "$out/tap21.json" "len(d['walk'])")" -ge 8 ] && break; sleep 0.5; done
sleep 1
expect "slow pages: both walks read to the end" "$(stat "$out/tap21.json" "d['walk'][:8]")" "['0000', '000E', '001A', '0020', '0000', '000E', '001A', '0020']"
expect "/api/master counts them" "$(api 18112 /api/master | python3 -c 'import json,sys; print(json.load(sys.stdin)["housekeeping"]["walks_done"])')" 2
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 22: the web API while the master waits on the bus ---
# Pages that take 400 ms to come back, as run 21's: /api/master (and the
# healthcheck's /api/health) must not wait for the reply with it.
mkdir -p "$out/state22"
python3 linux/tools/mock_tap.py "$out/tap22" --page-delay 0.4 --panels 30 --window 600 --nodes 44 --stats "$out/tap22.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_housekeeping": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18113}\n' \
  "$PWD/$out/tap22" > "$out/state22/settings.json"
build/linux/tigomonitord --state-dir "$out/state22" --web-dir components/tigo_server/web 2> "$out/daemon22.log" &
daemon=$!
slowest="$(python3 - <<'PY'
import time, urllib.request as u
worst, end = 0.0, time.monotonic() + 8
while time.monotonic() < end:
    t = time.monotonic()
    try:
        u.urlopen('http://127.0.0.1:18113/api/master', timeout=5).read()
    except OSError:
        time.sleep(0.05)
        continue
    worst = max(worst, time.monotonic() - t)
    time.sleep(0.02)
print(round(worst * 1000))
PY
)"
expect "/api/master answers while a page is awaited: under 150 ms" "$([ "$slowest" -lt 150 ] && echo yes || echo "no ($slowest ms)")" yes
expect "and the pages were awaited meanwhile" "$(stat "$out/tap22.json" "len(d['walk']) >= 4")" True
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

[ $fail = 0 ] && echo "master e2e passed" || { echo "master e2e FAILED; see $out/"; exit 1; }
