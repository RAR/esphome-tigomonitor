#!/bin/sh
# The CCA page without a CCA. Over Bluetooth: tigomonitord against a mock
# BlueZ (linux/tools/mock_bluez.py) on a private D-Bus bus, started after the
# daemon as can happen at boot, then every CCA command through it. Over HTTP:
# a mock of the older CCAs' local API (linux/tools/mock_cca_http.py).
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/cca-e2e
mkdir -p "$out"
rm -f "$out"/*.json "$out"/history.db*  # a fresh install every run
addr="$(dbus-daemon --session --fork --print-address | head -1)"
TIGOD_DBUS_ADDRESS="$addr" build/linux/tigomonitord --state-dir "$out" --port 18089 --serial /dev/nonexistent \
  2> "$out/daemon.log" &
daemon=$!
cleanup() {
  kill $daemon 2>/dev/null || true
  pkill -f mock_bluez.py || true
  pkill -f mock_cca_http.py || true
  pkill dbus-daemon || true
}
trap cleanup EXIT
sleep 1

DBUS_SESSION_BUS_ADDRESS="$addr" python3 - <<'PY'
import json, subprocess, sys, time, urllib.request as u
import dbus

B = 'http://127.0.0.1:18089'
fail = 0

def get(path):
    try:
        return 200, json.load(u.urlopen(B + path))
    except u.HTTPError as e:
        return e.code, json.load(e)

def post(path, body=None):
    r = u.Request(B + path, data=json.dumps(body or {}).encode(),
                  headers={'Content-Type': 'application/json'}, method='POST')
    try:
        return 200, json.load(u.urlopen(r))
    except u.HTTPError as e:
        return e.code, json.load(e)

def expect(what, got, want):
    global fail
    ok = got == want
    fail |= not ok
    print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ('' if ok else f", want {want!r}"))

def wait(fn, secs=30):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.5)
    return None

expect('no BlueZ yet', get('/api/cca/ble-scan')[0], 400)
subprocess.Popen([sys.executable, 'linux/tools/mock_bluez.py'], stdout=subprocess.DEVNULL)
time.sleep(1.5)
get('/api/cca/ble-scan')  # notices BlueZ is up now
expect('BlueZ found on a later load', wait(lambda: get('/api/cca/ble-scan')[0] == 200, 5), True)

expect('refresh queued', get('/api/cca/refresh')[1]['status'], 'ok')
info = wait(lambda: (lambda c: c if c['device_info'] != '{}' else None)(get('/api/cca')[1]))
expect('device info', json.loads(info['device_info']).get('serial'), '00000000TEST')
expect('network info', json.loads(info['network_info'])['eth']['ip'], '192.0.2.10')
expect('nearest CCA picked', info['cca_ip'], 'BLE 04:C0:5B:12:34:56')

post('/api/cca/discovery/start')
post('/api/cca/discovery/poll')
st = wait(lambda: (lambda d: d if d['status'] and 'found' in d['status'] else None)(get('/api/cca/discovery')[1]))
expect('discovery status', st and st['status']['found'], 6)

def result(cmd):
    return wait(lambda: get('/api/cca/network?cmd=' + cmd)[1]['result'])

post('/api/cca/network/poll?cmd=NETWORK_WIFI_SCAN')
expect('wifi scan', result('NETWORK_WIFI_SCAN'), [{'rssi': -55, 'ssid': 'ExampleNet'}])
post('/api/cca/network/wifi-connect', {'nid': 'My Net & co', 'pwd': 'p w%'})
expect('wifi join, params encoded', result('NETWORK_WIFI_CONNECT'), {'code': 0, 'nid': 'My Net & co', 'pwd_len': 4})
post('/api/cca/data-export')
expect('cloud export', result('DEVICE_DATA_EXPORT'), {'code': 0})
expect('unlisted network command refused', post('/api/cca/network/poll?cmd=NETWORK_WIFI_CLEAR')[0], 400)
expect('bad MAC refused', post('/api/cca/ble-mac', {'mac': 'nope'})[0], 400)
expect('MAC saved', post('/api/cca/ble-mac', {'mac': '04:c0:5b:12:34:56'})[1]['saved'], True)
expect('MAC forgotten', post('/api/cca/ble-mac', {'reset': True})[1]['saved'], False)

mock = dbus.Interface(dbus.SessionBus().get_object('org.bluez', '/test'), 'test.Mock')
log = [str(x) for x in mock.Log()]
expect('adapter powered on', bool(mock.Powered()), True)
expect('first connect timed out, retried', log.count('connect-timeout'), 1)
expect('every connection closed', (log.count('connect'), log.count('disconnect')), (6, 6))
expect('LE-only discovery', 'filter:le' in log, True)

# --- the older CCAs' local HTTP API, chosen by setting a CCA IP
bcs = ['04C05B40000000A1', '04C05B40000000B2']
post('/api/nodes/import', {'nodes': [{'addr': '0002', 'long_address': bcs[0]}, {'addr': '0003', 'long_address': bcs[1]}]})
subprocess.Popen([sys.executable, 'linux/tools/mock_cca_http.py', '18782'], stdout=subprocess.DEVNULL)
time.sleep(1)
u.urlopen(u.Request('http://127.0.0.1:18782/', data=json.dumps(bcs).encode(), method='POST'))
expect('CCA IP saved', post('/api/config', {'key': 'cca_ip', 'value': '127.0.0.1:18782'})[1]['cca_ip']['value'],
       '127.0.0.1:18782')
time.sleep(1.5)  # settings reach the main loop within a second
get('/api/cca/refresh')
info = wait(lambda: (lambda c: c if 'HTTPCCA' in c['device_info'] else None)(get('/api/cca')[1]), 10)
expect('HTTP device info', info and json.loads(info['device_info'])['serial'], 'HTTPCCA0001')
nodes = wait(lambda: (lambda n: n if n[0]['cca_label'] else None)(get('/api/nodes')[1]['nodes']), 10) or []
expect('HTTP sync labels', [(n['cca_label'], n['cca_string'], n['cca_mppt']) for n in nodes],
       [('P0', 'East', 'MPPT 1'), ('P1', 'East', 'MPPT 1')])

# --- the Tools page's adapter and web-port card (not CCA, but this test has
# the web server up)
expect('serial ports listed', isinstance(get('/api/serial-ports')[1].get('ports'), list), True)
expect('web port range checked', post('/api/settings', {'web_port': 70000})[0], 400)
expect('web port saved', post('/api/settings', {'web_port': 18090})[0], 200)
time.sleep(1.5)
s = get('/api/settings')[1]
expect('web port waits for a restart', (s['web_port'], s['web_port_active']), (18090, 18089))
# A fresh install with port 80 taken: the UI moves to 8080, no port is opened.
busy = subprocess.Popen([sys.executable, '-m', 'http.server', '80'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
time.sleep(1)
fresh = subprocess.Popen(['build/linux/tigomonitord', '--state-dir', 'build/cca-e2e/fresh'],
                         stderr=open('build/cca-e2e/fresh.log', 'w'))
s = None
end = time.time() + 10
while time.time() < end and s is None:
    try:
        s = json.load(u.urlopen('http://127.0.0.1:8080/api/settings'))
    except OSError:
        time.sleep(0.5)
fresh.terminate(); fresh.wait(); busy.terminate()
expect('port 80 taken: on 8080', s and (s['web_port_active'], s['serial_port']), (8080, ''))
expect('fresh install opens no port', 'no serial port chosen' in open('build/cca-e2e/fresh.log').read(), True)
sys.exit(1 if fail else 0)
PY
echo "cca e2e passed"
