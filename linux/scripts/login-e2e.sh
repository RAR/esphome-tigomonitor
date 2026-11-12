#!/bin/sh
# The web UI's sign-in: with a password set, the page loads and the API
# answers 401 without WWW-Authenticate (no browser popup); POST /api/login
# gives an HttpOnly, SameSite=Strict session cookie, logout ends it, HTTP
# Basic still works for scripts, another site can't use the cookie, a new
# password ends every session but the one that set it, and five wrong
# passwords lock the address out.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/login-e2e
rm -rf "$out" && mkdir -p "$out/state"
port=18095
printf '{"web_username": "admin", "web_password": "pw"}\n' > "$out/state/settings.json"
build/linux/tigomonitord --state-dir "$out/state" --port $port --serial /dev/nonexistent \
  --web-dir components/tigo_server/web 2>> "$out/daemon.log" &
daemon=$!
trap 'kill $daemon 2>/dev/null || true' EXIT
for _ in $(seq 50); do grep -q "web: listening on port $port" "$out/daemon.log" 2>/dev/null && break; sleep 0.1; done

python3 - $port <<'PY'
import base64, json, sys, time, urllib.error, urllib.request as u
port = int(sys.argv[1])
fail = 0
def expect(what, got, want):
    global fail
    ok = got == want
    print(f"  {'ok  ' if ok else 'FAIL'} {what}: {got!r}" + ('' if ok else f", want {want!r}"))
    fail |= not ok
def req(path, body=None, headers=None, method=None):
    r = u.Request(f'http://127.0.0.1:{port}{path}', data=None if body is None else json.dumps(body).encode(),
                  headers={'Content-Type': 'application/json', **(headers or {})}, method=method)
    try:
        with u.urlopen(r, timeout=5) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()
def basic(user, pw): return {'Authorization': 'Basic ' + base64.b64encode(f'{user}:{pw}'.encode()).decode()}
def cookie_of(h):
    c = h.get('Set-Cookie', '')
    return c.split(';')[0].split('=', 1)[1] if c.startswith('tm_session=') else None
def jar(t): return {'Cookie': f'theme=dark; tm_session={t}'}

s, h, b = req('/')
expect('the page loads without signing in', (s, 'WWW-Authenticate' in h, b'<html' in b[:300].lower()), (200, False, True))
s, h, b = req('/api/settings')
expect('the API asks to sign in, with no popup', (s, 'WWW-Authenticate' in h, json.loads(b).get('sign_in')), (401, False, True))
expect('health needs no sign-in', req('/api/health')[0] in (200, 503), True)
expect('HTTP Basic still works for scripts', req('/api/settings', headers=basic('admin', 'pw'))[0], 200)
expect('a wrong HTTP Basic is refused', req('/api/settings', headers=basic('admin', 'no'))[0], 401)

s, h, b = req('/api/login', {'username': 'admin', 'password': 'nope'})
expect('a wrong password', (s, json.loads(b)['error'], 'Set-Cookie' in h), (401, "That username and password don't match.", False))
s, h, b = req('/api/login', {'username': 'admin', 'password': 'pw'})
c = h.get('Set-Cookie', '')
expect('signed in', (s, json.loads(b)), (200, {'signed_in': True}))
expect('the cookie', sorted(p.strip().split('=')[0] for p in c.split(';')), sorted(['tm_session', 'Path', 'HttpOnly', 'SameSite', 'Max-Age']))
expect('SameSite=Strict, for 30 days, no Secure on http', ('SameSite=Strict' in c, 'Max-Age=2592000' in c, 'Secure' in c), (True, True, False))
tok = cookie_of(h)
expect('the token', (len(tok), all(ch in '0123456789abcdef' for ch in tok)), (64, True))
expect('the cookie opens the API', req('/api/settings', headers=jar(tok))[0], 200)
expect('a made-up cookie does not', req('/api/settings', headers=jar('0' * 64))[0], 401)
expect('another site cannot use it', req('/api/settings', {'publish_interval_s': 30}, headers={**jar(tok), 'Sec-Fetch-Site': 'cross-site'})[0], 403)

# A new password: every session ends; the browser that set it gets a new one.
s2, h2, _ = req('/api/login', {'username': 'admin', 'password': 'pw'})
other = cookie_of(h2)
s, h, b = req('/api/settings', {'web_password': 'pw2'}, headers=jar(tok))
fresh = cookie_of(h)
expect('saved, with a new session for this browser', (s, fresh is not None and fresh != tok), (200, True))
time.sleep(2)  # the main loop applies the settings
expect('the old session ended', req('/api/settings', headers=jar(tok))[0], 401)
expect('so did the other browser', req('/api/settings', headers=jar(other))[0], 401)
expect('the new one works', req('/api/settings', headers=jar(fresh))[0], 200)
expect('the new password over HTTP Basic', req('/api/settings', headers=basic('admin', 'pw2'))[0], 200)

s, h, _ = req('/api/logout', None, headers=jar(fresh), method='POST')
expect('signed out: the cookie is cleared', (s, 'Max-Age=0' in h.get('Set-Cookie', '')), (200, True))
expect('and the session is over', req('/api/settings', headers=jar(fresh))[0], 401)

# A browser still sending the login it saved before the change, with every
# request: it counts once, so its owner can still sign in.
for _ in range(10): req('/api/settings', headers=basic('admin', 'pw'))
s, h, _ = req('/api/login', {'username': 'admin', 'password': 'pw2'})
expect('a stale login sent again and again locks no one out', s, 200)

# Five different wrong ones: locked out, even with the right password, and
# over HTTP Basic too, for 15 minutes.
for i in range(5): req('/api/login', {'username': 'admin', 'password': f'guess{i}'})
s, _, b = req('/api/login', {'username': 'admin', 'password': 'pw2'})
expect('locked out after five wrong', (s, json.loads(b)['error']), (429, 'Too many wrong passwords. Wait 15 minutes, then try again.'))
expect('HTTP Basic is locked out too', req('/api/settings', headers=basic('admin', 'pw2'))[0], 401)
sys.exit(fail)
PY
echo "login e2e passed"
