#!/bin/sh
# Topology > Array's layout: empty until saved, saved and read back,
# refused when it isn't an array (the saved one kept), kept across a
# restart, and refused from another site.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/array-e2e
rm -rf "$out" && mkdir -p "$out"
port=18096
start() {
  build/linux/tigomonitord --state-dir "$out" --port $port --serial /dev/nonexistent \
    --web-dir components/tigo_server/web 2>> "$out/daemon.log" &
  echo $! > "$out/pid"
  for _ in $(seq 50); do grep -q "web: listening on port $port" "$out/daemon.log" 2>/dev/null && break; sleep 0.1; done
}
stop() { kill "$(cat "$out/pid")" 2>/dev/null || true; wait "$(cat "$out/pid")" 2>/dev/null || true; }
trap stop EXIT
start
python3 - $port <<'PY'
import json, sys, urllib.error, urllib.request as u
port = int(sys.argv[1])
fail = 0
def expect(what, got, want):
    global fail
    ok = got == want
    print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ('' if ok else f", want {want!r}"))
    fail |= not ok
def req(path, body=None, headers=None):
    r = u.Request(f'http://127.0.0.1:{port}{path}', data=None if body is None else json.dumps(body).encode(),
                  headers={'Content-Type': 'application/json', **(headers or {})})
    try:
        with u.urlopen(r, timeout=5) as resp: return resp.status, json.load(resp)
    except urllib.error.HTTPError as e: return e.code, json.load(e)
expect('empty before anything is placed', req('/api/array'), (200, {'panels': [], 'taps': []}))
layout = {'panels': [{'addr': '0026', 'x': 3, 'y': 2, 'portrait': True}, {'addr': '000a', 'x': 4, 'y': 2, 'portrait': False}],
        'taps': [{'id': '1201', 'x': 9, 'y': 7}]}
expect('saved', req('/api/array', layout), (200, {'saved': True}))
s, got = req('/api/array')
expect('read back, addresses upper case', (s, [p['addr'] for p in got['panels']], got['panels'][1]['portrait'], got['taps']), (200, ['0026', '000A'], False, [{'id': '1201', 'x': 9, 'y': 7}]))
s, err = req('/api/array', {'panels': [{'addr': '0026', 'x': 0, 'y': 0}, {'addr': '0027', 'x': 0, 'y': 0}], 'taps': []})
expect('two in one place is refused', (s, 'same place' in err.get('error', '')), (400, True))
expect('the saved layout is kept', len(req('/api/array')[1]['panels']), 2)
expect('another site cannot save', req('/api/array', layout, {'Sec-Fetch-Site': 'cross-site'})[0], 403)
sys.exit(fail)
PY
stop
start
python3 - $port <<'PY'
import json, sys, urllib.request as u
got = json.load(u.urlopen(f'http://127.0.0.1:{sys.argv[1]}/api/array', timeout=5))
ok = [p['addr'] for p in got['panels']] == ['0026', '000A'] and got['taps'][0]['id'] == '1201'
print(f"  {'ok  ' if ok else 'FAIL'} kept across a restart: {got}")
sys.exit(0 if ok else 1)
PY
# A renumbering moves a panel's address, not where it sits: a save keeps
# each panel's barcode, and the layout follows it (roof, 2026-11-14).
stop
nodes() {  # addr of barcode ...01, then of ...02
  python3 - "$out/state.json" "$1" "$2" <<'PY'
import json, sys
n = lambda a, b, l: {'addr': a, 'long_address': b, 'cca_label': l, 'checksum': 'X', 'sensor_index': -1}
json.dump({'version': 1, 'nodes': [n(sys.argv[2], '04C05B0000000001', 'A1'), n(sys.argv[3], '04C05B0000000002', 'A2')]},
          open(sys.argv[1], 'w'))
PY
}
nodes 0026 000A
start
python3 - $port <<'PY'
import json, sys, urllib.request as u
port = int(sys.argv[1])
def req(path, body=None):
    r = u.Request(f'http://127.0.0.1:{port}{path}', data=None if body is None else json.dumps(body).encode(),
                  headers={'Content-Type': 'application/json'})
    with u.urlopen(r, timeout=5) as resp: return json.load(resp)
req('/api/array', {'panels': [{'addr': '0026', 'x': 3, 'y': 2}, {'addr': '000A', 'x': 4, 'y': 2}], 'taps': []})
got = [(p['addr'], p.get('barcode')) for p in req('/api/array')['panels']]
ok = got == [('0026', '04C05B0000000001'), ('000A', '04C05B0000000002')]
print(f"  {'ok  ' if ok else 'FAIL'} a save keeps each panel's barcode: {got}")
sys.exit(0 if ok else 1)
PY
stop
nodes 0031 0026
start
python3 - $port <<'PY'
import json, sys, urllib.request as u
got = json.load(u.urlopen(f'http://127.0.0.1:{sys.argv[1]}/api/array', timeout=5))
got = [(p['addr'], p['x']) for p in got['panels']]
ok = got == [('0031', 3), ('0026', 4)]
print(f"  {'ok  ' if ok else 'FAIL'} renumbered, each panel stays where it sits: {got}")
sys.exit(0 if ok else 1)
PY
echo "array e2e passed"
