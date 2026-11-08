#!/bin/sh
# /api/mesh end to end: a synthetic bus with real slot counters, two reports
# dropped and two NetDiag joins (one direct, one through a relay), served by the mock bridge to the daemon. Checks
# each optimizer's TAP, reports lost today, the joins and parents, the RSSI history's shape,
# that no MAC leaks into the response, and the empty answer of a quiet bus.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/mesh-e2e
rm -rf "$out" && mkdir -p "$out/state" "$out/quiet"
python3 linux/tools/synth_bus.py "$out/bus.bin" --panels 4 --rounds 20 --slots \
  --drop 0003:5 --drop 0003:6 --netdiag 0002 --netdiag 0004:0002 >/dev/null
python3 linux/tools/mock_bridge.py "$out/bus.bin" 16639 1 "$out/bridge.json" &
bridge=$!
sleep 0.3
build/linux/tigomonitord --state-dir "$out/state" --port 18091 --serial tcp://127.0.0.1:16639 \
  2> "$out/daemon.log" &
daemon=$!
build/linux/tigomonitord --state-dir "$out/quiet" --port 18092 --serial /dev/nonexistent \
  2> "$out/quiet.log" &
quiet=$!
trap 'kill $daemon $bridge $quiet 2>/dev/null || true' EXIT
for _ in $(seq 50); do [ -f "$out/bridge.json" ] && break; sleep 0.2; done
sleep 1

fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
get() {  # get PORT PATH: the body, or the HTTP status on an error
  python3 - "$@" <<'PY'
import sys, urllib.error, urllib.request as u
try:
    print(u.urlopen(f'http://127.0.0.1:{sys.argv[1]}{sys.argv[2]}', timeout=5).read().decode())
except urllib.error.HTTPError as e:
    print(e.code)
PY
}
get 18091 /api/mesh > "$out/mesh.json"
q() {  # q EXPRESSION over the mesh JSON as d, nodes by address as n
  python3 -c "import json,sys
try:
    d = json.load(open('$out/mesh.json'))
except Exception:
    print('not json: ' + open('$out/mesh.json').read().strip()); sys.exit()
if not isinstance(d, dict):
    print('got ' + str(d)); sys.exit()
n = {x['addr']: x for x in d['nodes']}
print($1)"
}
expect "GET /api/mesh" "$(q "'json'")" json
expect "optimizers" "$(q "len(d['nodes'])")" 4
expect "loss method" "$(q "d['loss_method']")" slot
expect "parent confirmed" "$(q "d['parent_confirmed']")" True
expect "TAPs" "$(q "[t['id'] for t in d['taps']]")" "['1201']"
expect "0003 reports today" "$(q "n['0003']['reports_today']")" 18
expect "0003 lost today" "$(q "n['0003']['lost_today']")" 2
expect "0002 lost today" "$(q "n['0002']['lost_today']")" 0
expect "0002 joined" "$(q "isinstance(n['0002']['last_join_unix'], int)")" True
expect "0004 joined too, no others" "$(q "sorted(a for a in n if n[a]['last_join_unix'] is not None)")" "['0002', '0004']"
expect "a day of RSSI each" "$(q "sorted({len(x['rssi_24h']) for x in d['nodes']})")" "[288]"
expect "RSSI heard now" "$(q "all(x['rssi_24h'][-1] is not None for x in d['nodes'])")" True
expect "every node via 1201" "$(q "sorted({x['tap'] for x in d['nodes']})")" "['1201']"
expect "parents from the joins" "$(q "{a: n[a]['parent'] for a in sorted(n)}")" "{'0002': '0001', '0003': None, '0004': '0002', '0005': None}"
expect "labels present" "$(q "all('label' in x and 'string' in x for x in d['nodes'])")" True
expect "no MACs in the answer" "$(grep -c 04C05B "$out/mesh.json" || true)" 0
get 18092 /api/mesh > "$out/quiet.json"
expect "a quiet bus" "$(python3 -c "import json; d = json.load(open('$out/quiet.json')); print(d['nodes'], d['taps'])")" "[] []"
[ $fail = 0 ] && echo "mesh e2e passed" || { echo "mesh e2e FAILED; see $out/"; exit 1; }
