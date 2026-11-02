#!/bin/sh
# The Topology editor's layout, with no bus and no Tigo: panels come from a
# node-table import, then the layout is read, saved, renamed, refused and
# re-read after a restart.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/layout-e2e
rm -rf "$out" && mkdir -p "$out"
start() {
  build/linux/tigomonitord --state-dir "$out" --port 18091 --serial /dev/nonexistent \
    --web-dir components/tigo_server/web 2>> "$out/daemon.log" &
  echo $! > "$out/pid"
  sleep 1
}
start
cleanup() {
  kill "$(cat "$out/pid")" 2>/dev/null || true
  pkill -f mock_cca_http.py || true
}
trap cleanup EXIT

python3 - <<'PY'
import json, os, signal, subprocess, sys, time, urllib.request as u

B = 'http://127.0.0.1:18091'
OUT = 'build/layout-e2e'
fail = 0

def body(resp):
    try:
        return json.load(resp)
    except ValueError:
        return {}

def get(path):
    try:
        return 200, body(u.urlopen(B + path))
    except u.HTTPError as e:
        return e.code, body(e)

def post(path, data=None):
    raw = b'not json' if data is None else json.dumps(data).encode()
    r = u.Request(B + path, data=raw, headers={'Content-Type': 'application/json'}, method='POST')
    try:
        return 200, body(u.urlopen(r))
    except u.HTTPError as e:
        return e.code, body(e)

def expect(what, got, want):
    global fail
    ok = got == want
    fail |= not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ('' if ok else f", want {want!r}"))

def restart():
    os.kill(int(open(OUT + '/pid').read()), signal.SIGTERM)
    time.sleep(1)
    p = subprocess.Popen(['build/linux/tigomonitord', '--state-dir', OUT, '--port', '18091',
                          '--serial', '/dev/nonexistent', '--web-dir', 'components/tigo_server/web'],
                         stderr=open(OUT + '/daemon.log', 'a'))
    open(OUT + '/pid', 'w').write(str(p.pid))
    time.sleep(1)

BC = ['04C05B00000000A1', '04C05B00000000A2', '04C05B00000000A3', '04C05B00000000A4']

def seed(addrs):
    post('/api/nodes/import', {'nodes': [{'addr': a, 'long_address': b} for a, b in zip(addrs, BC)]})

def tree(strings_a, strings_b=(), display_a='', name='Inverter 1'):
    return {'inverters': [{'name': name, 'display_name': '', 'mppts': [
        {'label': 'MPPT 1', 'strings': [
            {'label': 'A', 'display_label': display_a, 'rating_w': 400, 'panels': list(strings_a)},
            {'label': 'B', 'display_label': '', 'rating_w': 0, 'panels': list(strings_b)}]}]}]}

def strings_of(layout):
    out = {}
    for inv in layout['inverters']:
        for m in inv['mppts']:
            for s in m['strings']:
                out[s['label']] = [(p['barcode'], p['name']) for p in s['panels']]
    return out

def all_barcodes(layout):
    bcs = [b for s in strings_of(layout).values() for b, _ in s]
    return bcs + [p['barcode'] for p in layout['unassigned']]

seed(['0002', '0003', '0004', '0005'])

# --- a fresh install: nothing laid out
c, l = get('/api/layout')
expect('layout served', c, 200)
expect('fresh: not manual', l.get('manual'), False)
expect('fresh: four unassigned', sorted(p['barcode'] for p in l.get('unassigned', [])), BC)
expect('fresh: no inverters', l.get('inverters'), [])
expect('fresh: nothing pending', l.get('pending', 'missing'), None)

# --- save: string A in a chosen order, string B empty
expect('save', post('/api/layout', tree([BC[2], BC[0], BC[1]]))[0], 200)
l = get('/api/layout')[1]
expect('saved: manual', l['manual'], True)
expect('saved: A in posted order, named by position', strings_of(l).get('A'),
       [(BC[2], 'A1'), (BC[0], 'A2'), (BC[1], 'A3')])
expect('saved: empty string B kept', strings_of(l).get('B'), [])
expect('saved: one left unassigned', [p['barcode'] for p in l['unassigned']], [BC[3]])
nodes = {n['long_address']: n for n in get('/api/nodes')[1]['nodes']}
expect('node label', (nodes[BC[2]]['cca_label'], nodes[BC[2]]['cca_string'], nodes[BC[2]]['cca_mppt']),
       ('A1', 'A', 'MPPT 1'))

# --- rename: the display label names the panels, the label stays
post('/api/layout', tree([BC[2], BC[0], BC[1]], display_a='East'))
l = get('/api/layout')[1]
expect('renamed: panels follow the display label', [n for _, n in strings_of(l)['A']], ['East1', 'East2', 'East3'])
nodes = {n['long_address']: n for n in get('/api/nodes')[1]['nodes']}
expect('renamed: string label kept', nodes[BC[2]]['cca_string'], 'A')

# --- refused saves change nothing
before = get('/api/layout')[1]
c, r = post('/api/layout', tree([BC[0], BC[0]]))
expect('duplicate panel refused', (c, r.get('success')), (400, False))
expect('duplicate: unchanged', get('/api/layout')[1], before)
c, r = post('/api/layout', tree(['04C05B0000000FFF']))
expect('unknown panel refused', (c, r.get('success')), (400, False))
expect('unknown: unchanged', get('/api/layout')[1], before)
expect('bad JSON refused', post('/api/layout', None)[0], 400)

# --- the panels get new short addresses; a save still goes by barcode
seed(['0011', '0012', '0013', '0014'])
post('/api/layout', tree([BC[3], BC[1]]))
l = get('/api/layout')[1]
expect('after renumber: by barcode', strings_of(l)['A'], [(BC[3], 'A1'), (BC[1], 'A2')])
expect('after renumber: new address', [p['addr'] for inv in l['inverters'] for m in inv['mppts']
                                       for s in m['strings'] if s['label'] == 'A' for p in s['panels']],
       ['0014', '0012'])

# --- two tabs: the later save wins whole
post('/api/layout', tree([BC[0], BC[1]], [BC[2]]))
post('/api/layout', tree([BC[2]], [BC[0]]))
l = get('/api/layout')[1]
expect('stale save wins whole', (strings_of(l)['A'], strings_of(l)['B']), ([(BC[2], 'A1')], [(BC[0], 'B1')]))
expect('no panel twice', sorted(all_barcodes(l)), BC)

# --- a restart keeps it, empty strings included
post('/api/layout', tree([BC[0]], []))
before = get('/api/layout')[1]
restart()
after = get('/api/layout')[1]
expect('restart: same layout', after, before)
expect('restart: empty B still there', strings_of(after).get('B'), [])

# --- what arrives after a hand-made layout waits for the user
def wait(fn, secs=10):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.3)
    return None

def labels():
    return {n['long_address']: (n['cca_label'], n['cca_string'], n['cca_mppt'])
            for n in get('/api/nodes')[1]['nodes']}

def cca_has(barcodes):
    u.urlopen(u.Request('http://127.0.0.1:18783/', data=json.dumps(barcodes).encode(), method='POST'))

def sync():
    get('/api/cca/refresh')
    time.sleep(2.5)  # the mock answers at once; the sync runs on its own thread

subprocess.Popen([sys.executable, 'linux/tools/mock_cca_http.py', '18783'], stdout=subprocess.DEVNULL)
time.sleep(1)
cca_has(BC)  # P0, P1 on East (MPPT 1); P2, P3 on West (MPPT 2)
post('/api/config', {'key': 'cca_ip', 'value': '127.0.0.1:18783'})
time.sleep(1.5)  # settings reach the main loop within a second
mine = labels()
get('/api/cca/refresh')
held = wait(lambda: get('/api/layout')[1]['pending'])
expect('manual: CCA sync held', held and held['source'], 'cca')
expect('manual: labels unchanged', labels(), mine)
c, pend = get('/api/layout/pending')
moved = {ch['barcode']: (ch['now']['name'], ch['then']['name'], ch['then']['string']) for ch in pend.get('changes', [])}
expect('pending lists what would change', moved.get(BC[0]), ('A1', 'P0', 'East'))

expect('keep', post('/api/layout/pending', {'action': 'keep'})[0], 200)
expect('kept: nothing waiting', get('/api/layout')[1]['pending'], None)
sync()
expect('same CCA layout again: not asked again', get('/api/layout')[1]['pending'], None)
restart()
sync()
expect('after a restart: still not asked', get('/api/layout')[1]['pending'], None)
expect('after a restart: labels still mine', labels(), mine)

cca_has(list(reversed(BC)))
get('/api/cca/refresh')
expect('a different CCA layout: asked again', (wait(lambda: get('/api/layout')[1]['pending']) or {}).get('source'), 'cca')
expect('use', post('/api/layout/pending', {'action': 'use'})[0], 200)
expect('used: the CCA labels', labels()[BC[3]], ('P0', 'East', 'MPPT 1'))
expect('used: no longer manual', get('/api/layout')[1]['manual'], False)
expect('nothing waiting: use refused', post('/api/layout/pending', {'action': 'use'})[0], 409)
expect('unknown action refused', post('/api/layout/pending', {'action': 'maybe'})[0], 400)

cca_has(BC)
sync()
expect('not manual: a sync applies as before', labels()[BC[0]], ('P0', 'East', 'MPPT 1'))
expect('not manual: nothing waiting', get('/api/layout')[1]['pending'], None)
post('/api/config', {'key': 'cca_ip', 'value': ''})
time.sleep(1.5)

# --- a node-table import while manual: its labels wait, its addresses don't
post('/api/layout', tree([BC[0], BC[1]], [BC[2]]))
mine = labels()
same = [{'addr': a, 'long_address': b, 'cca_label': mine[b][0], 'cca_string': mine[b][1], 'cca_mppt': mine[b][2]}
        for a, b in zip(['0021', '0022', '0023', '0024'], BC)]
post('/api/nodes/import', {'nodes': same})
expect('identical import: nothing waiting', get('/api/layout')[1]['pending'], None)
expect('identical import: new addresses taken', sorted(n['addr'] for n in get('/api/nodes')[1]['nodes']),
       ['0021', '0022', '0023', '0024'])
other = [dict(e, cca_label='Z' + str(i + 1), cca_string='Z', cca_mppt='MPPT 9') for i, e in enumerate(same)]
c, r = post('/api/nodes/import', {'nodes': other})
expect('different import: held', (r.get('layout_held'), get('/api/layout')[1]['pending']['source']), (True, 'import'))
expect('different import: labels unchanged', labels(), mine)
expect('use the import', post('/api/layout/pending', {'action': 'use'})[0], 200)
expect('import used', labels()[BC[0]], ('Z1', 'Z', 'MPPT 9'))

sys.exit(1 if fail else 0)
PY
echo "layout e2e passed"
