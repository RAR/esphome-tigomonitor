#!/bin/sh
# Home Assistant's ingress: with --trust-ingress, requests from that address
# (the proxy, which has signed the user in) need no sign-in here, and
# /api/settings says so (via_ingress), so the page hides Sign out. Requests
# from any other address still sign in, a forwarded header can't claim the
# proxy's address, and another site is still refused.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/ingress-e2e
rm -rf "$out" && mkdir -p "$out/a" "$out/b"
printf '{"web_username": "admin", "web_password": "pw"}\n' > "$out/a/settings.json"
cp "$out/a/settings.json" "$out/b/settings.json"
build/linux/tigomonitord --state-dir "$out/a" --port 18096 --serial /dev/nonexistent \
  --web-dir components/tigo_server/web --trust-ingress 127.0.0.1 2>> "$out/a.log" &
a=$!
build/linux/tigomonitord --state-dir "$out/b" --port 18097 --serial /dev/nonexistent \
  --web-dir components/tigo_server/web --trust-ingress 10.9.9.9 2>> "$out/b.log" &
b=$!
trap 'kill $a $b 2>/dev/null || true' EXIT
for _ in $(seq 50); do
  grep -q "web: listening on port 18096" "$out/a.log" 2>/dev/null && grep -q "web: listening on port 18097" "$out/b.log" 2>/dev/null && break
  sleep 0.1
done

python3 - <<'PY'
import base64, json, urllib.error, urllib.request as u
fail = 0
def expect(what, got, want):
    global fail
    ok = got == want
    print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ('' if ok else f", want {want!r}"))
    fail |= not ok
def req(port, path, body=None, headers=None):
    r = u.Request(f'http://127.0.0.1:{port}{path}', data=None if body is None else json.dumps(body).encode(),
                  headers={'Content-Type': 'application/json', **(headers or {})})
    try:
        with u.urlopen(r, timeout=5) as resp:
            return resp.status, resp.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
basic = {'Authorization': 'Basic ' + base64.b64encode(b'admin:pw').decode()}

s, b = req(18096, '/api/settings')
expect('from the ingress proxy: no sign-in', (s, json.loads(b).get('via_ingress')), (200, True))
expect('it can save settings too', req(18096, '/api/settings', {'publish_interval_s': 30})[0], 200)
expect('another site is still refused', req(18096, '/api/settings', {'publish_interval_s': 30},
                                            headers={'Sec-Fetch-Site': 'cross-site'})[0], 403)

s, b = req(18097, '/api/settings')
expect('from any other address: sign in', (s, json.loads(b).get('sign_in')), (401, True))
expect('a forwarded header does not make it the proxy', req(18097, '/api/settings', headers={'X-Forwarded-For': '10.9.9.9'})[0], 401)
s, b = req(18097, '/api/settings', headers=basic)
expect('signed in the usual way: not via ingress', (s, json.loads(b).get('via_ingress')), (200, False))
raise SystemExit(fail)
PY
echo "ingress e2e: all passed"
