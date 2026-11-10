#!/bin/sh
# More than one TAP (docs/superpowers/specs/2026-11-10-multi-tap-design.md):
# the bus master against linux/tools/mock_taps.py's several TAPs on one bus.
# Checks that two saved TAPs are both polled, with housekeeping but no 0x41s
# (a CCA's copied 0x41 could steer an optimizer to the wrong TAP); that a
# table add runs on the TAP it names and skips ids the other lists, and a
# removal goes to the TAP its optimizer reports through; that one TAP going
# quiet doesn't stop the other; that with adopting on, a new TAP at the
# factory address starts the CCA's herd, which stores its new id and saves
# the list, and with it off, the TAP is only reported; and that the owner's
# herd keeps the known ids; and that a TAP gone for good can be retired.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/multitap-e2e
rm -rf "$out" && mkdir -p "$out"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
# req METHOD PORT PATH [JSON]: prints "<code> <body>", as the owner (admin:pw)
req() {
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
except Exception:
    print('000 {}')
PY
}
code() { req "$@" | cut -d' ' -f1; }
body() { req "$@" | cut -d' ' -f2-; }
master() { body GET "$1" /api/master | python3 -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$2"; }
stat() { python3 -c "import json,sys; d=json.load(open('$1')); print(eval(sys.argv[1]))" "$2"; }
wait_for() {  # wait_for SECONDS PORT PYTHON-EXPR: until /api/master makes it True
  for _ in $(seq $(($1 * 4))); do [ "$(master "$2" "$3" 2>/dev/null)" = True ] && return 0; sleep 0.25; done
  return 0
}
tap=0 daemon=0
trap 'kill $daemon $tap 2>/dev/null || true' EXIT
start() {  # start NAME PORT EXTRA-SETTINGS [MOCK FLAGS...]
  name=$1 port=$2 extra=$3
  shift 3
  mkdir -p "$out/$name"
  python3 linux/tools/mock_taps.py "$out/$name.tty" --stats "$out/$name.json" "$@" &
  tap=$!
  sleep 0.5
  printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw"%s}\n' \
    "$PWD/$out/$name.tty" "$port" "$extra" > "$out/$name/settings.json"
  build/linux/tigomonitord --state-dir "$out/$name" --web-dir components/tigo_server/web 2>> "$out/$name.log" &
  daemon=$!
  for _ in $(seq 40); do [ "$(code GET "$port" /api/health)" != 000 ] && break; sleep 0.25; done
}
stop_all() { kill $daemon $tap; wait $daemon $tap 2>/dev/null || true; }
saved_taps() { python3 -c "import json; print([(t['id'], t['mac'][-2:].lower()) for t in json.load(open('$1'))['taps']])"; }

# --- run 1: two saved TAPs, housekeeping on, table writes on; then TAP 2 unplugged ---
mkdir -p "$out/one"
echo '{"taps": [{"id": "1201", "mac": "04c05b0000a1b2a1"}, {"id": "1202", "mac": "04c05b0000a1b2a2"}]}' > "$out/one/taps.json"
start one 18401 ', "master_housekeeping": true, "master_table_writes": true' \
  --tap 1201,a1,3 --tap 1202,a2,3 --down-file "2,$out/one.down" --join-after 0
wait_for 20 18401 "len(d['taps']) == 2 and all(t['polls'] > 20 for t in d['taps'])"
expect "both TAPs polled" "$(master 18401 "[(t['id'], t['state'], t['polls'] > 20) for t in d['taps']]")" \
  "[('1201', 'running', True), ('1202', 'running', True)]"
expect "polls summed" "$(master 18401 "d['polls'] == sum(t['polls'] for t in d['taps'])")" True
expect "each TAP's version" "$(master 18401 "[t['version'][:19] for t in d['taps']]")" "['Mgate Version G8.59', 'Mgate Version G8.60']"
expect "no fault" "$(master 18401 "d['fault']")" none
expect "panels from both TAPs" "$(body GET 18401 /api/devices | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["devices"]))')" 6
expect "polls alternate at the mock" "$(stat "$out/one.json" "abs(d['taps'][0]['polls'] - d['taps'][1]['polls']) <= 3")" True
expect "nothing unknown sent" "$(stat "$out/one.json" "d['other']")" "[]"
# Housekeeping runs on each TAP, but with two TAPs no 0x41 goes to either:
# the first would be due within 11 s of housekeeping starting.
for _ in $(seq 60); do [ "$(stat "$out/one.json" "all(t['sessions'] for t in d['taps'])")" = True ] && break; sleep 1; done
sleep 15
expect "housekeeping on both, no 0x41s" "$(stat "$out/one.json" \
  "[(bool(t['sessions']), t['housekeeping']) for t in d['taps']]")" "[(True, []), (True, [])]"
# A table add on TAP 1202: ids 0002-0007 are listed across the two, so 0008.
expect "an add on 1202 starts" "$(code POST 18401 /api/master/table/add '{"label": "0-1234R", "tap": "1202"}')" 200
wait_for 30 18401 "d['table']['edit']['finished']"
sleep 1.5  # the mock writes its stats once a second
expect "it ran on 1202, at 0008" "$(master 18401 "(d['table']['tap'], d['table']['edit']['ok'], d['table']['edit']['id'])")" "('1202', True, '0008')"
expect "written to 1202 only" "$(stat "$out/one.json" "(len(d['taps'][0]['writes']), len(d['taps'][1]['writes']))")" "(0, 1)"
expect "a TAP not on the bus is refused" "$(code POST 18401 /api/master/table/add '{"label": "0-1234R", "tap": "1209", "dry_run": true}')" 409
# A removal of one of 1202's, named by MAC only: it goes to 1202.
mac=$(body GET 18401 /api/nodes | python3 -c "import json,sys; n=json.load(sys.stdin)['nodes']; n=n.values() if isinstance(n, dict) else n; print([x['long_address'] for x in n if x['addr'] == '0006'][0])")
expect "a removal starts" "$(code POST 18401 /api/master/table/remove "{\"mac\": \"$mac\"}")" 200
wait_for 30 18401 "d['table']['edit']['finished'] and d['table']['edit']['kind'] == 'remove'"
sleep 1.5  # the mock writes its stats once a second
expect "it erased 1202 only" "$(stat "$out/one.json" "(d['taps'][0]['erases'], d['taps'][1]['erases'])")" "(0, 1)"
expect "the removal ran on 1202" "$(master 18401 "(d['table']['tap'], d['table']['edit']['ok'])")" "('1202', True)"
# TAP 2 unplugged: lost, and 1201 goes on being polled with no fault.
touch "$out/one.down"
wait_for 20 18401 "d['taps'][1]['state'] == 'lost'"
before=$(master 18401 "d['taps'][0]['polls']")
sleep 3
expect "1202 lost" "$(master 18401 "d['taps'][1]['state']")" lost
expect "1201 still polled" "$(master 18401 "d['taps'][0]['polls'] - $before > 50")" True
expect "no fault with one TAP answering" "$(master 18401 "d['fault']")" none
expect "the loss logged" "$(grep -c 'TAP 1202 stopped answering' "$out/one.log")" 1
# Retired: refused for the TAP still answering, then 1202 is dropped and the
# saved list keeps 1201 alone. (That it is no longer asked is the host test's:
# the mock doesn't count frames to a TAP that is down.)
expect "retiring 1201, answering, refused" "$(code POST 18401 /api/master/taps/forget '{"tap": "1201"}')" 409
expect "retiring 1202 accepted" "$(code POST 18401 /api/master/taps/forget '{"tap": "1202"}')" 200
wait_for 10 18401 "len(d['taps']) == 1"
expect "1202 retired" "$(master 18401 "[t['id'] for t in d['taps']]")" "['1201']"
expect "the saved list without it" "$(saved_taps "$out/one/taps.json")" "[('1201', 'a1')]"
stop_all

# --- run 2: adopting on, a new TAP at the factory address: the herd ---
start two 18402 ', "master_adopt_taps": true, "master_housekeeping": true' --tap 1201,a1,3 --tap 1235,b2,3,new
wait_for 60 18402 "len(d['taps']) == 2 and all(t['state'] == 'running' and t['polls'] > 10 for t in d['taps'])"
expect "two rounds of five claims" "$(stat "$out/two.json" "d['claims']")" 10
expect "two RESETs" "$(stat "$out/two.json" "d['resets']")" 2
expect "the new TAP's id stored, the old one's not" "$(stat "$out/two.json" "[(t['id'], t['stores']) for t in d['taps']]")" \
  "[('1201', 0), ('1202', 1)]"
expect "both polled" "$(master 18402 "[(t['id'], t['state']) for t in d['taps']]")" "[('1201', 'running'), ('1202', 'running')]"
expect "the list saved" "$(saved_taps "$out/two/taps.json")" "[('1201', 'a1'), ('1202', 'b2')]"
expect "no fault" "$(master 18402 "d['fault']")" none
expect "nothing unknown sent" "$(stat "$out/two.json" "d['other']")" "[]"
# Adopted by the herd, not saved at the start: now two TAPs, so no 0x41s on
# either, the first TAP included, though it had one TAP's housekeeping before.
for _ in $(seq 90); do [ "$(stat "$out/two.json" "all(t['sessions'] for t in d['taps'])")" = True ] && break; sleep 1; done
sleep 15
expect "after the herd, no 0x41s on either TAP" "$(stat "$out/two.json" \
  "[t['housekeeping'] for t in d['taps']]")" "[[], []]"
stop_all

# --- run 3: adopting off: the new TAP is only reported ---
start three 18403 '' --tap 1201,a1,3 --tap 1235,b2,3,new
wait_for 20 18403 "d['fault'] == 'extra_tap'"
expect "reported" "$(master 18403 "(d['fault'], 'master_adopt_taps' in d['detail'], len(d['extra_taps']))")" "('extra_tap', True, 1)"
expect "no herd" "$(stat "$out/three.json" "d['claims']")" 0
expect "the owner's herd refused" "$(body POST 18403 /api/master/taps/herd | python3 -c "import json,sys; print('master_adopt_taps' in json.load(sys.stdin)['error'])")" True
wait_for 20 18403 "d['taps'][0]['polls'] > 20"
expect "the old one polled" "$(master 18403 "d['taps'][0]['polls'] > 20")" True
stop_all

# --- run 4: the owner's herd, two known TAPs: one round, ids kept ---
mkdir -p "$out/four"
echo '{"taps": [{"id": "1201", "mac": "04c05b0000a1b2a1"}, {"id": "1202", "mac": "04c05b0000a1b2a2"}]}' > "$out/four/taps.json"
start four 18404 ', "master_adopt_taps": true' --tap 1201,a1,3 --tap 1202,a2,3
wait_for 20 18404 "all(t['polls'] > 10 for t in d['taps'])"
expect "no herd at start" "$(stat "$out/four.json" "d['claims']")" 0
expect "the owner's herd starts" "$(code POST 18404 /api/master/taps/herd)" 200
sleep 2
wait_for 40 18404 "d['phase'] == 'running' and all(t['state'] == 'running' for t in d['taps'])"
sleep 2
expect "one round" "$(stat "$out/four.json" "(d['claims'], d['resets'])")" "(5, 1)"
expect "ids kept" "$(stat "$out/four.json" "[t['id'] for t in d['taps']]")" "['1201', '1202']"
expect "no STORE for TAPs that keep their ids" "$(stat "$out/four.json" "[t['stores'] for t in d['taps']]")" "[0, 0]"
expect "the list saved" "$(saved_taps "$out/four/taps.json")" "[('1201', 'a1'), ('1202', 'a2')]"
expect "polled again" "$(master 18404 "all(t['polls'] > 20 for t in d['taps'])")" True
stop_all

if [ $fail -ne 0 ]; then
  echo "multitap-e2e: FAILED"
  exit 1
fi
echo "multitap-e2e: all passed"
