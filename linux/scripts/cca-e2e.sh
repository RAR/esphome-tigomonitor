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
  --web-dir components/tigo_server/web \
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
# A non-UTF-8 SSID once ended the daemon when the reply was saved.
expect('Latin-1 SSID survives', json.loads(info['network_info'])['wlan']['ssid'], 'Caf\ufffd')
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

# --- requests from other sites, the MQTT password, the web login
import base64
def code(path, method='GET', body=None, headers=None):
    r = u.Request(B + path, data=body, headers=headers or {}, method=method)
    try:
        return u.urlopen(r).status
    except u.HTTPError as e:
        return e.code
expect('cross-site POST refused', code('/api/restart', 'POST', b'', {'Sec-Fetch-Site': 'cross-site'}), 403)
expect('cross-site GET refused', code('/api/cca/refresh', headers={'Sec-Fetch-Site': 'cross-site', 'Sec-Fetch-Mode': 'no-cors'}), 403)
expect('link from another site opens', code('/api/status', headers={'Sec-Fetch-Site': 'cross-site', 'Sec-Fetch-Mode': 'navigate'}), 200)
expect('foreign Origin refused', code('/api/settings', 'POST', b'{}', {'Origin': 'http://evil.example'}), 403)
expect('same-origin POST allowed', code('/api/settings', 'POST', b'{}', {'Sec-Fetch-Site': 'same-origin'}), 200)
expect('oversized body refused', code('/api/nodes/import', 'POST', b'x' * (5 << 20)), 413)
# GET /api/settings shows the running copy, which takes a save within a second.
post('/api/settings', {'mqtt': {'host': '127.0.0.1', 'port': 1, 'username': 'ha', 'password': 'secret'}})
post('/api/settings', {'mqtt': {'host': '127.0.0.1', 'port': 1, 'username': 'ha', 'base_topic': 't2'}})
time.sleep(1.5)
expect('MQTT password kept for a topic change', get('/api/settings')[1]['mqtt']['password_set'], True)
post('/api/settings', {'mqtt': {'host': 'elsewhere.example', 'port': 1, 'username': 'ha'}})
time.sleep(1.5)
expect('MQTT password dropped for a new broker', get('/api/settings')[1]['mqtt']['password_set'], False)
post('/api/settings', {'mqtt': {'host': ''}})
expect('web username checked', post('/api/settings', {'web_username': 'a:b'})[0], 400)
expect('bus role checked', post('/api/settings', {'bus_role': 'both'})[0], 400)
expect('gateway id checked', post('/api/settings', {'gateway_id': 0x8001})[0], 400)
expect('no master on a bridge', post('/api/settings', {'bus_role': 'master', 'serial_port': 'tcp://example.com:6638'})[0], 400)
post('/api/settings', {'web_password': 'pw'})
time.sleep(1.5)
auth = {'Authorization': 'Basic ' + base64.b64encode(b'admin:pw').decode()}
expect('login required', code('/api/status'), 401)
expect('wrong password refused', code('/api/status', headers={'Authorization': 'Basic ' + base64.b64encode(b'admin:no').decode()}), 401)
expect('logged in', code('/api/status', headers=auth), 200)
# The page's API calls carry its token as a Bearer header, which a browser
# sends instead of the login; the ESP32's placeholder must not go out.
page = u.urlopen(u.Request(B + '/', headers=auth)).read()
expect('page has no API token', (b'__TIGO_API_TOKEN__' in page, b'const API_TOKEN = "";' in page), (False, True))
code('/api/settings', 'POST', b'{"web_password": ""}', dict(auth, **{'Content-Type': 'application/json'}))
time.sleep(1.5)
expect('login removed', code('/api/status'), 200)
expect('total_energy_in is input energy', 'total_energy_in' in get('/api/overview')[1], True)
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
