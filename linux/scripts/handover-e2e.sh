#!/bin/sh
# Replace a TAP's handover against linux/tools/mock_taps.py's two TAPs:
# every optimizer heard through TAP 1202 is listed on 1201 at its own id,
# not joined (bit 15), as a CCA's rediscovery listed them (2026-11-15); a
# check sends nothing; the job is refused for the same TAP, a TAP nobody
# reported through, or one not on the list; and each listed optimizer is
# watched until it joins 1201.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/handover-e2e
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
  printf '{"bus_role": "master", "master_startup": false, "master_housekeeping": false, "master_queries": false, "master_optimizer_writes": false, "master_read_tap_config": false, "discovery_auto": false, "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw"%s}\n' \
    "$PWD/$out/$name.tty" "$port" "$extra" > "$out/$name/settings.json"
  build/linux/tigomonitord --state-dir "$out/$name" --web-dir components/tigo_server/web 2>> "$out/$name.log" &
  daemon=$!
  for _ in $(seq 40); do [ "$(code GET "$port" /api/health)" != 000 ] && break; sleep 0.25; done
}
stop_all() { kill $daemon $tap; wait $daemon $tap 2>/dev/null || true; }

# Two saved TAPs, three optimizers each, table writes on; an optimizer the
# mock lists joins 3 s after it is written with bit 15 set.
mkdir -p "$out/one"
echo '{"taps": [{"id": "1201", "mac": "04c05b0000a1b2a1"}, {"id": "1202", "mac": "04c05b0000a1b2a2"}]}' > "$out/one/taps.json"
start one 18451 ', "master_table_writes": true' --tap 1201,a1,3 --tap 1202,a2,3 --join-after 3
wait_for 20 18451 "len(d['taps']) == 2 and all(t['polls'] > 20 for t in d['taps'])"
links() { body GET 18451 /api/mesh | python3 -c "import json,sys; print(sorted((n['tap'] or '-') for n in json.load(sys.stdin)['nodes']))"; }
for _ in $(seq 40); do [ "$(links)" = "['1201', '1201', '1201', '1202', '1202', '1202']" ] && break; sleep 0.5; done
expect "each optimizer's TAP is known" "$(links)" "['1201', '1201', '1201', '1202', '1202', '1202']"

# Refused before anything is sent.
expect "same TAP refused" "$(code POST 18451 /api/master/taps/handover '{"from": "1201", "to": "1201", "dry_run": false}')" 409
expect "a TAP nobody reported through refused" \
  "$(body POST 18451 /api/master/taps/handover '{"from": "1203", "to": "1201", "dry_run": false}')" \
  '{"error":"no optimizer has been heard through TAP 1203"}'
expect "a TAP not on the list refused" \
  "$(code POST 18451 /api/master/taps/handover '{"from": "1202", "to": "1209", "dry_run": false}')" 409
expect "bad shape refused" "$(code POST 18451 /api/master/taps/handover '{"from": "1202"}')" 400

# A check first (the default): it reads 1201's table and lists the frames.
expect "a check starts" "$(body POST 18451 /api/master/taps/handover '{"from": "1202", "to": "1201"}' | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['dry_run'], d['count'], d['to'])")" "True 3 1201"
wait_for 30 18451 "d['table']['edit']['finished'] and d['table']['edit']['kind'] == 'handover'"
expect "the check's plan" "$(master 18451 "(d['table']['edit']['ok'], d['table']['edit']['message'])")" \
  "(True, 'dry run: would erase the table, write back the other 3 and list 3 from the other TAP, not joined')"
sleep 1.5
expect "the check sent no write or erase" "$(stat "$out/one.json" "[(t['erases'], len(t['writes'])) for t in d['taps']]")" "[(0, 0), (0, 0)]"

# The handover: erase 1201, write back its own three and 1202's three, not joined.
expect "the handover starts" "$(code POST 18451 /api/master/taps/handover '{"from": "1202", "to": "1201", "dry_run": false}')" 200
wait_for 30 18451 "d['table']['edit']['finished'] and d['table']['edit']['kind'] == 'handover' and not d['table']['edit']['dry_run']"
expect "the handover finished" "$(master 18451 "(d['table']['edit']['ok'], d['table']['edit']['message'], d['table']['tap'])")" \
  "(True, 'listed 3 from the other TAP, not joined yet; the other 3 written back as they were', '1201')"
sleep 1.5
expect "only 1201 erased and written" "$(stat "$out/one.json" "[(t['erases'], len(t['writes'])) for t in d['taps']]")" "[(1, 1), (0, 0)]"
# One write of six: three joined (1201's own), three with bit 15 (1202's).
expect "the write: 3 joined, 3 listed" "$(stat "$out/one.json" \
  "sorted((int(w[2+20*i+16:2+20*i+20], 16) >> 15) for w in d['taps'][0]['writes'] for i in range(int(w[:2], 16)))")" \
  "[0, 0, 0, 1, 1, 1]"
expect "1202's three are waited for" "$(master 18451 "len(d['table']['waiting'])")" 3
# They join 3 s after the write; the join watch reads the table once a minute.
wait_for 90 18451 "len(d['table']['waiting']) == 0"
expect "all three joined 1201" "$(master 18451 "len(d['table']['waiting'])")" 0
expect "nothing unknown sent" "$(stat "$out/one.json" "d['other']")" "[]"
expect "a given list is refused off the bench" \
  "$(code POST 18451 /api/master/taps/handover '{"from": "1201", "to": "1202", "records": [{"mac": "04C05BFE00000001", "addr": "0010"}]}')" 403
stop_all

# The bench: two TAPs with no optimizers, and a list the request brings, as
# a rehearsal on real TAPs does (20 made-up optimizers, 0010-0023).
mkdir -p "$out/bench"
echo '{"taps": [{"id": "1201", "mac": "04c05b0000a1b2a1"}, {"id": "1202", "mac": "04c05b0000a1b2a2"}]}' > "$out/bench/taps.json"
start bench 18452 ', "master_table_writes": true, "master_bench": true' --tap 1201,a1,0 --tap 1202,a2,0
wait_for 20 18452 "len(d['taps']) == 2 and all(t['polls'] > 20 for t in d['taps'])"
records=$(python3 -c "import json; print(json.dumps([{'mac': '04C05BFE%08X' % (i + 1), 'addr': '%04X' % (0x10 + i)} for i in range(20)]))")
expect "a bad record is refused" \
  "$(code POST 18452 /api/master/taps/handover '{"from": "1201", "to": "1202", "records": [{"mac": "04C05BFE00000001", "addr": "0001"}]}')" 400
expect "the check counts the given list" "$(body POST 18452 /api/master/taps/handover "{\"from\": \"1201\", \"to\": \"1202\", \"records\": $records}" | python3 -c "import json,sys; d=json.load(sys.stdin); print(d['dry_run'], d['count'])")" "True 20"
expect "the handover starts" "$(code POST 18452 /api/master/taps/handover "{\"from\": \"1201\", \"to\": \"1202\", \"dry_run\": false, \"records\": $records}")" 200
wait_for 30 18452 "d['table']['edit']['finished'] and d['table']['edit']['kind'] == 'handover' and not d['table']['edit']['dry_run']"
expect "it finished" "$(master 18452 "d['table']['edit']['ok']")" True
sleep 1.5
expect "only 1202 erased and written, ten a write" "$(stat "$out/bench.json" "[(t['erases'], len(t['writes'])) for t in d['taps']]")" "[(0, 0), (1, 2)]"
expect "all twenty listed, not joined, at their addresses" "$(stat "$out/bench.json" \
  "sorted('%04X' % int(w[2+20*i+16:2+20*i+20], 16) for w in d['taps'][1]['writes'] for i in range(int(w[:2], 16)))")" \
  "$(python3 -c "print(sorted('%04X' % (0x8000 | (0x10 + i)) for i in range(20)))")"
expect "nothing unknown sent" "$(stat "$out/bench.json" "d['other']")" "[]"
stop_all
[ $fail = 0 ] && echo "handover-e2e: ok" || { echo "handover-e2e: FAILED"; exit 1; }
