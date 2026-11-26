#!/bin/sh
# --dangerous-options on|off overrides settings.json's dangerous_options, as
# --serial overrides its serial port: the Home Assistant app sets it from its
# Configuration tab. It holds across a settings save, which reloads the file,
# and isn't written into the file. Any other value is refused at start.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/dangerous-options-e2e
rm -rf "$out" && mkdir -p "$out/on" "$out/off" "$out/bad"
printf '{}\n' > "$out/on/settings.json"
printf '{"dangerous_options": true}\n' > "$out/off/settings.json"
build/linux/tigomonitord --state-dir "$out/on" --port 18098 --serial /dev/nonexistent \
  --web-dir components/tigo_server/web --dangerous-options on 2>> "$out/on.log" &
a=$!
build/linux/tigomonitord --state-dir "$out/off" --port 18099 --serial /dev/nonexistent \
  --web-dir components/tigo_server/web --dangerous-options off 2>> "$out/off.log" &
b=$!
trap 'kill $a $b 2>/dev/null || true' EXIT
for _ in $(seq 50); do
  grep -q "web: listening on port 18098" "$out/on.log" 2>/dev/null && grep -q "web: listening on port 18099" "$out/off.log" 2>/dev/null && break
  sleep 0.1
done
bad=0
build/linux/tigomonitord --state-dir "$out/bad" --port 0 --serial /dev/nonexistent \
  --dangerous-options maybe 2> "$out/bad.log" || bad=$?

python3 - "$out" "$bad" <<'PY'
import json, sys, time, urllib.error, urllib.request as u
out, bad = sys.argv[1], int(sys.argv[2])
fail = 0
def expect(what, got, want):
    global fail
    ok = got == want
    print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ('' if ok else f", want {want!r}"))
    fail |= not ok
def req(port, path, body=None):
    r = u.Request(f'http://127.0.0.1:{port}{path}', data=None if body is None else json.dumps(body).encode(),
                  headers={'Content-Type': 'application/json'})
    try:
        with u.urlopen(r, timeout=5) as resp:
            return resp.status, json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b'{}')
def settings(port):
    return req(port, '/api/settings')[1]

expect('on, with none in settings.json: on', settings(18098).get('dangerous_options'), True)
expect('off, with true in settings.json: off', settings(18099).get('dangerous_options'), False)

# A save reloads settings.json; the override is applied again, not lost.
for port in (18098, 18099):
    expect(f'a save on {port}', req(port, '/api/settings', {'publish_interval_s': 30})[0], 200)
for _ in range(50):
    if all(settings(p).get('publish_interval_s') == 30 for p in (18098, 18099)):
        break
    time.sleep(0.1)
expect('the save was applied', [settings(p).get('publish_interval_s') for p in (18098, 18099)], [30, 30])
expect('on still on after it', settings(18098).get('dangerous_options'), True)
expect('off still off after it', settings(18099).get('dangerous_options'), False)
files = [json.load(open(f'{out}/{d}/settings.json')).get('dangerous_options') for d in ('on', 'off')]
expect('and settings.json keeps its own values', files, [False, True])

expect('any other value: refused, with the usage', (bad, '--dangerous-options' in open(f'{out}/bad.log').read()), (2, True))
raise SystemExit(fail)
PY
echo "dangerous-options e2e: all passed"
