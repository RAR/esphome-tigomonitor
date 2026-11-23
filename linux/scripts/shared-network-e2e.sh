#!/bin/sh
# One network for every TAP (docs/superpowers/specs/2026-11-22-shared-tap-network-design.md),
# against linux/tools/mock_taps.py's several TAPs on one bus, each with made-up
# radio settings. Checks that every TAP's settings are read at start and each
# says whether its key is the one the others share; that each TAP is told the
# others' networks (0xE1) once, naming the other two; that a re-key gives a TAP
# the shared key and keeps its channel and PAN (a dry run writes nothing), and
# every TAP is told again; that a TAP a herd brings in on factory settings is
# named in no 0xE1 until Add a TAP gives it its settings, with the key the others
# share; that with more than one TAP every window renewal ends 01 01; and that
# no key appears in the daemon's log.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/shared-network-e2e
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
wait_mock() {  # wait_mock SECONDS STATS-FILE PYTHON-EXPR: until the mock's stats make it True
  for _ in $(seq $(($1 * 4))); do [ "$(stat "$2" "$3" 2>/dev/null)" = True ] && return 0; sleep 0.25; done
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
  printf '{"bus_role": "master", "master_startup": true, "master_housekeeping": false, "master_queries": false, "master_optimizer_writes": false, "master_read_tap_config": false, "discovery_auto": false, "master_table_writes": true, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw"%s}\n' \
    "$PWD/$out/$name.tty" "$port" "$extra" > "$out/$name/settings.json"
  build/linux/tigomonitord --state-dir "$out/$name" --web-dir components/tigo_server/web 2>> "$out/$name.log" &
  daemon=$!
  for _ in $(seq 40); do [ "$(code GET "$port" /api/health)" != 000 ] && break; sleep 0.25; done
}
stop_all() { kill $daemon $tap; wait $daemon $tap 2>/dev/null || true; }
finished() {  # finished PORT: until the table edit (a provision here) has finished
  wait_for 40 "$1" "d['table']['edit']['finished'] and d['table']['edit']['kind'] == 'provision'"
  sleep 1.5  # the mock writes its stats once a second
}
no_key_logged() {  # no_key_logged LOG: none of the made-up keys' bytes
  grep -c -i -E '30 ?30 ?30 ?30|77 ?77 ?77 ?77' "$1" || true
}

# --- run 1: three saved TAPs, two on one key, one on another ---
mkdir -p "$out/one"
echo '{"taps": [{"id": "1201", "mac": "04c05b0000a1b2a1"}, {"id": "1202", "mac": "04c05b0000a1b2a2"}, {"id": "1203", "mac": "04c05b0000a1b2a3"}]}' > "$out/one/taps.json"
start one 18501 '' --tap 1201,a1,3 --tap 1202,a2,3 --tap 1203,a3,0 \
  --radio 1,15,7E21,30 --radio 2,18,5A5A,30 --radio 3,11,1A2B,77 --window 3
wait_for 40 18501 "len(d['taps']) == 3 and all(t['key'] != 'unread' for t in d['taps'])"
expect "each TAP's key against the others'" "$(master 18501 "[t['key'] for t in d['taps']]")" "['same', 'same', 'different']"
wait_mock 20 "$out/one.json" "all(len(t['e1']) == 1 for t in d['taps'])"
expect "each told the other two, in order" "$(stat "$out/one.json" "[t['e1'] for t in d['taps']]")" \
  "[[{'count': 2, 'channels': [18, 11]}], [{'count': 2, 'channels': [15, 11]}], [{'count': 2, 'channels': [15, 18]}]]"
expect "and each accepted" "$(master 18501 "[t['known_networks'] for t in d['taps']]")" \
  "['accepted (2 other TAPs)', 'accepted (2 other TAPs)', 'accepted (2 other TAPs)']"
# The re-key: a dry run reads and writes nothing; then the real one.
expect "a re-key check starts" "$(code POST 18501 /api/master/taps/provision '{"mac": "04C05B0000A1B2A3", "keep_network": true, "shared_key": true}')" 200
finished 18501
expect "and says the key stays hidden" "$(master 18501 "(d['table']['edit']['ok'], 'key hidden' in d['table']['edit']['message'])")" "(True, True)"
expect "having written nothing" "$(stat "$out/one.json" "d['taps'][2]['cfg_writes']")" "[]"
expect "the re-key starts" "$(code POST 18501 /api/master/taps/provision '{"mac": "04C05B0000A1B2A3", "keep_network": true, "shared_key": true, "dry_run": false}')" 200
finished 18501
expect "and is given" "$(master 18501 "d['table']['edit']['ok']")" True
expect "the shared key, its channel and PAN kept" "$(stat "$out/one.json" "d['taps'][2]['cfg_writes'][-1]")" \
  "{'channel': 11, 'pan': '1A2B', 'key_same_as': 1}"
wait_for 20 18501 "all(t['key'] == 'same' for t in d['taps'])"
expect "now all the same" "$(master 18501 "[t['key'] for t in d['taps']]")" "['same', 'same', 'same']"
wait_mock 20 "$out/one.json" "all(len(t['e1']) == 2 for t in d['taps'])"
expect "and each told again" "$(stat "$out/one.json" "[len(t['e1']) for t in d['taps']]")" "[2, 2, 2]"
expect "nothing unknown sent" "$(stat "$out/one.json" "d['other']")" "[]"
# With more than one TAP, every renewal carries the known-networks flag (01 01).
wait_mock 20 "$out/one.json" "all(t.get('renewal_flags') for t in d['taps'][:2])"
expect "renewals carry the known-networks flag" \
  "$(stat "$out/one.json" "sorted(set(sum((t.get('renewal_flags', []) for t in d['taps']), [])))")" "['0101']"
expect "no key in the log" "$(no_key_logged "$out/one.log")" 0
stop_all

# --- run 2: a herd brings in a new TAP on factory settings; Add a TAP gives it the shared key ---
mkdir -p "$out/two"
echo '{"taps": [{"id": "1201", "mac": "04c05b0000a1b2a1"}, {"id": "1202", "mac": "04c05b0000a1b2a2"}]}' > "$out/two/taps.json"
start two 18502 ', "master_adopt_taps": true' --tap 1201,a1,3 --tap 1202,a2,3 --tap 1235,b3,0,new \
  --radio 1,15,7E21,30 --radio 2,18,5A5A,30 --radio 3,0,1234,00
wait_for 90 18502 "len(d['taps']) == 3 and all(t['state'] == 'running' for t in d['taps']) and all(t['key'] != 'unread' for t in d['taps'])"
expect "three TAPs after the herd" "$(master 18502 "[t['id'] for t in d['taps']]")" "['1201', '1202', '1203']"
wait_mock 20 "$out/two.json" "len(d['taps'][0]['e1']) >= 1 and len(d['taps'][1]['e1']) >= 1"
expect "the new TAP, on channel 0, named in neither" "$(stat "$out/two.json" "[t['e1'][-1]['channels'] for t in d['taps'][:2]]")" "[[18], [15]]"
expect "no refusal" "$(master 18502 "d['fault']")" none
# Add a TAP's radio step: a new network, the key the others share.
expect "the new TAP's settings start" "$(code POST 18502 /api/master/taps/provision '{"mac": "04C05B0000A1B2B3", "shared_key": true, "dry_run": false}')" 200
finished 18502
expect "given" "$(master 18502 "d['table']['edit']['ok']")" True
expect "with the others' key, on a channel of its own" "$(stat "$out/two.json" "(d['taps'][2]['cfg_writes'][-1]['key_same_as'], d['taps'][2]['cfg_writes'][-1]['channel'] not in (0, 15, 18))")" "(1, True)"
wait_mock 20 "$out/two.json" "all(t['e1'] and t['e1'][-1]['count'] == 2 for t in d['taps'])"
expect "then each told the other two" "$(stat "$out/two.json" "[t['e1'][-1]['count'] for t in d['taps']]")" "[2, 2, 2]"
expect "no key in the log" "$(no_key_logged "$out/two.log")" 0
stop_all

if [ $fail -ne 0 ]; then
  echo "shared network e2e FAILED; see $out/"
  exit 1
fi
echo "shared network e2e passed"
