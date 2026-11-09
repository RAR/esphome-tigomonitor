#!/bin/sh
# Adding an optimizer to the TAP's table (table_edit.h,
# docs/superpowers/specs/2026-11-09-optimizer-table-edits-design.md):
# /api/master/table/add against a mock TAP that takes one record. Checks the
# API's guards, that a dry run and the setting off send no write, that the
# add sends exactly one 0x29 in the CCA's shape and reads it back, and that
# a refusal from the TAP stops it.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/table-e2e
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
edit() { body GET "$1" /api/master '' admin:pw | python3 -c "import json,sys; e=json.load(sys.stdin)['table']['edit']; print(eval(sys.argv[1]))" "$2"; }
stat() { python3 -c "import json,sys; d=json.load(open('$1')); print(eval(sys.argv[1]))" "$2"; }
finished() {  # finished PORT: waits up to 30 s for the add to end, then for the mock's stats
  for _ in $(seq 120); do [ "$(edit "$1" "e['finished']")" = True ] && break; sleep 0.25; done
  sleep 1.5
}
tap=0 daemon=0
trap 'kill $daemon $tap 2>/dev/null || true' EXIT
start() {  # start NAME PORT EXTRA-SETTINGS [MOCK FLAGS...]
  name=$1 port=$2 extra=$3
  shift 3
  mkdir -p "$out/$name"
  python3 linux/tools/mock_tap.py "$out/$name.tty" --window 600 --nodes 6 --panels 6 --stats "$out/$name.json" "$@" &
  tap=$!
  sleep 0.5
  printf '{"bus_role": "master", "master_startup": true, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw"%s}\n' \
    "$PWD/$out/$name.tty" "$port" "$extra" > "$out/$name/settings.json"
  build/linux/tigomonitord --state-dir "$out/$name" --web-dir components/tigo_server/web 2>> "$out/$name.log" &
  daemon=$!
  for _ in $(seq 40); do [ "$(code GET "$port" /api/health)" != 000 ] 2>/dev/null && break; sleep 0.25; done
  sleep 2
}
stop_all() { kill $daemon $tap; wait $daemon $tap 2>/dev/null || true; }
# A made-up optimizer: 04C05B0000001234, printed as 0-1234R.
LABEL='"label": "0-1234R"'

# --- run 1: the guards, a dry run, then the add ---
start one 18301 '' --join-after 0
expect "no password, no add" "$(code POST 18301 /api/master/table/add "{$LABEL}")" 401
expect "a misread label is refused" "$(req POST 18301 /api/master/table/add '{"label": "0-1234S"}' admin:pw)" \
  "400 {\"error\":\"that label doesn't check out: look for a misread character\"}"
expect "an address out of range is refused" "$(code POST 18301 /api/master/table/add "{$LABEL, \"id\": \"0400\"}" admin:pw)" 400
expect "a MAC that isn't Tigo's is refused" "$(code POST 18301 /api/master/table/add '{"mac": "0011223344556677"}' admin:pw)" 400
expect "the setting off: refused" "$(body POST 18301 /api/master/table/add "{$LABEL}" admin:pw | python3 -c "import json,sys; print('master_table_writes' in json.load(sys.stdin)['error'])")" True
expect "/api/master says writes are off" "$(body GET 18301 /api/master '' admin:pw | python3 -c "import json,sys; print(json.load(sys.stdin)['table']['writes'])")" False
expect "a dry run starts with the setting off" "$(req POST 18301 /api/master/table/add "{$LABEL, \"dry_run\": true}" admin:pw)" \
  '200 {"dry_run":true,"label":"0-1234R","mac":"04C05B0000001234","started":true}'
finished 18301
expect "the dry run ends with the write it would send" "$(edit 18301 "(e['ok'], e['dry_run'], e['id'], e['dry_frames'])")" \
  "(True, True, '0008', ['290104C05B00000012348008'])"
expect "and sent none" "$(stat "$out/one.json" "d['table_writes']")" "[]"
# The setting on: the master restarts with it.
expect "table writes on" "$(code POST 18301 /api/settings '{"master_table_writes": true}' admin:pw)" 200
sleep 3
expect "/api/master says writes are on" "$(body GET 18301 /api/master '' admin:pw | python3 -c "import json,sys; print(json.load(sys.stdin)['table']['writes'])")" True
expect "the add starts" "$(code POST 18301 /api/master/table/add "{$LABEL}" admin:pw)" 200
expect "one at a time" "$(body POST 18301 /api/master/table/add "{$LABEL}" admin:pw | python3 -c "import json,sys; print(json.load(sys.stdin)['error'])")" \
  "a change to the table is already running"
finished 18301
expect "listed and joined" "$(edit 18301 "(e['ok'], e['dry_run'], e['id'], e['ack'], e['label'], e['message'])")" \
  "(True, False, '0008', 0, '0-1234R', 'listed at 0008 and joined')"
expect "exactly one write, the CCA's shape" "$(stat "$out/one.json" "d['table_writes']")" "['0104c05b00000012348008']"
expect "logged" "$(grep -c 'wrote 1 record to the TAP' "$out/one.log")" 1
# Again: it's listed now, so nothing is written.
expect "a second add of the same one starts" "$(code POST 18301 /api/master/table/add "{$LABEL}" admin:pw)" 200
finished 18301
expect "and finds it listed" "$(edit 18301 "(e['ok'], e['id'], e['message'].startswith('already in the table at 0008'))")" "(True, '0008', True)"
expect "still one write" "$(stat "$out/one.json" "len(d['table_writes'])")" 1
stop_all

# --- run 2: the TAP refuses the record (4: the address is in use) ---
start two 18302 ', "master_table_writes": true' --table-ack 4
expect "the add starts" "$(code POST 18302 /api/master/table/add "{$LABEL, \"id\": \"0020\"}" admin:pw)" 200
finished 18302
expect "stopped with the TAP's reason" "$(edit 18302 "(e['ok'], e['ack'], e['message'])")" \
  "(False, 4, 'the TAP refused the write: that address is already in use')"
expect "one write, at the address asked" "$(stat "$out/two.json" "d['table_writes']")" "['0104c05b00000012348020']"
stop_all

# --- run 3: removing one, as the CCA does ---
start three 18303 ', "master_table_writes": true'
mac3=''
for _ in $(seq 40); do  # the mock sends its table after the first poll
  mac3="$(body GET 18303 /api/nodes '' admin:pw | python3 -c "import json,sys; print(next((n['long_address'] for n in json.load(sys.stdin)['nodes'] if n['addr'] == '0003'), ''))")"
  [ -n "$mac3" ] && break
  sleep 0.25
done
expect "a dry run lists the erase and the write back" "$(code POST 18303 /api/master/table/remove "{\"mac\": \"$mac3\", \"dry_run\": true}" admin:pw)" 200
finished 18303
expect "the dry run's frames" "$(edit 18303 "(e['kind'], e['ok'], len(e['dry_frames']), e['dry_frames'][0], e['dry_frames'][1][:4])")" "('remove', True, 2, '2BBABE', '2905')"
expect "and sent nothing" "$(stat "$out/three.json" "(d['erases'], d['table_writes'])")" "(0, [])"
expect "the removal starts" "$(code POST 18303 /api/master/table/remove "{\"mac\": \"$mac3\"}" admin:pw)" 200
finished 18303
expect "removed, the rest written back" "$(edit 18303 "(e['kind'], e['ok'], e['id'], e['message'])")" \
  "('remove', True, '0003', 'removed 0003; the other 5 written back as they were')"
expect "one erase, one write of five, joined" "$(stat "$out/three.json" "(d['erases'], len(d['table_writes']), d['table_writes'][0][:2], [d['table_writes'][0][2 + 20 * k + 16:2 + 20 * k + 20] for k in range(5)])")" \
  "(1, 1, '05', ['0002', '0004', '0005', '0006', '0007'])"
expect "the saved list is gone" "$([ -e "$out/three/table-remove.json" ] && echo kept || echo gone)" gone
expect "logged the erase" "$(grep -c 'erased the TAP.s table (done)' "$out/three.log")" 1
stop_all

# --- run 4: a removal that stopped after its erase, found at the start ---
mkdir -p "$out/four"
printf '{"mac": "04C05B0000001234", "stay": [["04C05B0000000102", 2], ["04C05B0000000103", 32771]]}\n' > "$out/four/table-remove.json"
start four 18304 ''
expect "with table writes off: nothing sent" "$(stat "$out/four.json" "(d['erases'], d['table_writes'])")" "(0, [])"
expect "it says so" "$(edit 18304 "(e['interrupted'], e['finished'])")" "(True, False)"
expect "and refuses a new change" "$(body POST 18304 /api/master/table/add "{$LABEL, \"dry_run\": true}" admin:pw | python3 -c "import json,sys; print(json.load(sys.stdin)['error'])")" \
  "an interrupted removal is waiting to be finished"
expect "the list is kept" "$([ -e "$out/four/table-remove.json" ] && echo kept || echo gone)" kept
expect "table writes on" "$(code POST 18304 /api/settings '{"master_table_writes": true}' admin:pw)" 200
sleep 3
finished 18304
expect "finished it" "$(edit 18304 "(e['kind'], e['ok'], e['message'])")" "('remove', True, 'finished the interrupted removal: 2 written back')"
expect "erased and wrote back the saved two" "$(stat "$out/four.json" "(d['erases'], d['table_writes'])")" \
  "(1, ['0204c05b0000000102000204c05b00000001038003'])"
expect "the saved list is gone" "$([ -e "$out/four/table-remove.json" ] && echo kept || echo gone)" gone
stop_all

[ $fail = 0 ] && echo "table e2e passed" || { echo "table e2e FAILED"; exit 1; }
