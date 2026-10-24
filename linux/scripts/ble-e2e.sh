#!/bin/sh
# The CCA page over Bluetooth without a CCA: tigomonitord against a mock BlueZ
# (linux/tools/mock_bluez.py) on a private D-Bus bus. Starts the daemon before
# BlueZ, as can happen at boot, then runs every CCA command through it.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/ble-e2e
mkdir -p "$out"
rm -f "$out"/*.json "$out"/history.db*  # a fresh install every run
addr="$(dbus-daemon --session --fork --print-address | head -1)"
TIGOD_DBUS_ADDRESS="$addr" build/linux/tigomonitord --state-dir "$out" --port 18089 --serial /dev/nonexistent \
  2> "$out/daemon.log" &
daemon=$!
cleanup() { kill $daemon 2>/dev/null || true; pkill -f mock_bluez.py || true; pkill dbus-daemon || true; }
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
expect('every connection closed', (log.count('connect'), log.count('disconnect')), (6, 6))
expect('LE-only discovery', 'filter:le' in log, True)
sys.exit(1 if fail else 0)
PY
echo "ble e2e passed"
