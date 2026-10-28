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
python3 linux/tools/mock_tap.py "$out/tap8" --lease 600 --nodes 44 --stats "$out/tap8.json" &
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
python3 linux/tools/mock_tap.py "$out/tap9" --lease 600 --nodes 44 --stats "$out/tap9.json" &
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
python3 linux/tools/mock_tap.py "$out/tap9" --factory --lease 600 --nodes 44 --stats "$out/tap9b.json" &
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
  python3 linux/tools/mock_tap.py "$out/tap10$mode" $flag --second-tap --lease 600 --nodes 44 --stats "$out/tap10$mode.json" &
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
  python3 linux/tools/mock_tap.py "$out/tap12$mode" $flag --lease 600 --nodes 44 --stats "$out/tap12$mode.json" &
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
python3 linux/tools/mock_tap.py "$out/tap13" --lease 600 --nodes 44 --stats "$out/tap13.json" &
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
python3 linux/tools/mock_tap.py "$out/tap14" --factory --lease 600 --nodes 44 --stats "$out/tap14.json" &
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
python3 linux/tools/mock_tap.py "$out/tap15" --lease 600 --nodes 44 --stats "$out/tap15.json" &
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
expect "no writes without the setting" "$(stat "$out/tap15.json" "[q for q in d['queries'] if q['text'].startswith(('#', '^00Smrt_'))]")" "[]"
expect "warm, housekeeping off: no bring-up reads, 0x22s or walks" "$(stat "$out/tap15.json" "([x['f'] for x in d['log'] if x['f'][5:9] in ('000A', '0E02', '0010')], d['broadcast'], d['walk'], d['session'])")" "([], [], [], [])"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

# --- run 16: a TAP slow back from the bring-up's RESET ---
# Deaf for 12 s, longer than the reads after the quiet: the bring-up fails,
# and after a pause the master looks again, finds it at 1201, and polls.
mkdir -p "$out/state16"
python3 linux/tools/mock_tap.py "$out/tap16" --factory --reset-quiet 12 --lease 600 --nodes 44 --stats "$out/tap16.json" &
tap=$!
sleep 0.5
printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": 18106}\n' \
  "$PWD/$out/tap16" > "$out/state16/settings.json"
build/linux/tigomonitord --state-dir "$out/state16" --web-dir components/tigo_server/web 2> "$out/daemon16.log" &
daemon=$!
for _ in $(seq 160); do [ -f "$out/tap16.json" ] && [ "$(stat "$out/tap16.json" "d['polls']")" -ge 5 ] && break; sleep 0.5; done
expect "one RESET, said it failed, then found it at 1201 and polled" "$(stat "$out/tap16.json" "(d['resets'], d['polls'] >= 5, d['addressed_to'])")" "(1, True, '1201')"
expect "logged the failure" "$(grep -c 'did not answer at the id it was given' "$out/daemon16.log")" 1
expect "fault cleared once polling" "$(api 18106 /api/master | python3 -c 'import json,sys; m=json.load(sys.stdin); print(m["fault"], m["phase"])')" "none running"
kill $daemon; wait $daemon 2>/dev/null || true
kill $tap; wait $tap 2>/dev/null || true

[ $fail = 0 ] && echo "master e2e passed" || { echo "master e2e FAILED; see $out/"; exit 1; }
