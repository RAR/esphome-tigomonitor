#!/bin/sh
# Setup mode: a fresh install (no settings.json yet) lets System setup make
# its changes to the bus without a web password for 24 hours, so a new
# system can be set up first and locked last. PV off always needs the
# password. An install that already has a settings file never gets setup
# mode, password or not. Setting a password ends it.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/setup-e2e
rm -rf "$out" && mkdir -p "$out/fresh" "$out/old"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
req() {  # req METHOD PORT PATH [JSON] [USER:PASS]: prints "<code> <body>"
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
setting() { body GET "$1" /api/settings '' ${3:-} | python3 -c "import json,sys; print(json.load(sys.stdin).get(sys.argv[1]))" "$2"; }
daemon=0
trap 'kill $daemon 2>/dev/null || true' EXIT
up() {  # up DIR PORT
  build/linux/tigomonitord --state-dir "$1" --port "$2" --serial /dev/nonexistent --web-dir components/tigo_server/web 2>> "$1.log" &
  daemon=$!
  for _ in $(seq 40); do [ "$(code GET "$2" /api/health)" != 000 ] 2>/dev/null && break; sleep 0.25; done
}

# --- a fresh install: no settings.json ---
up "$out/fresh" 18501
expect "setup is open" "$(setting 18501 setup_open)" True
until=$(setting 18501 setup_open_until)
expect "for a day" "$(python3 -c "import time; print(23.9*3600 < $until - time.time() <= 24*3600)")" True
expect "and it's saved" "$(python3 -c "import json; print(json.load(open('$out/fresh/settings.json')).get('setup_open_until') == $until)")" True
# The bus changes System setup makes are let through the password check;
# they then answer as they would with one (here: not running the bus).
for p in /api/master/discovery /api/master/taps/herd /api/master/table/add; do
  expect "no password needed: $p" "$(code POST 18501 "$p" '{"renew": true, "label": "0-1234R"}')" 409
done
expect "PV off still needs one" "$(code POST 18501 /api/master/pv '{"on": false}')" 403
# Restarting doesn't start the day again.
kill $daemon; wait $daemon 2>/dev/null || true
up "$out/fresh" 18502
expect "same deadline after a restart" "$(setting 18502 setup_open_until)" "$until"
# A password ends it.
expect "set a password" "$(code POST 18502 /api/settings '{"web_password": "pw"}')" 200
# Applied by the main loop's reload, as any saved setting is.
for _ in $(seq 40); do [ "$(code GET 18502 /api/settings)" = 401 ] && break; sleep 0.25; done
expect "setup is closed" "$(setting 18502 setup_open admin:pw)" False
expect "a change needs the login now" "$(code POST 18502 /api/master/taps/herd '{}')" 401
kill $daemon; wait $daemon 2>/dev/null || true

# --- a deadline passed ---
python3 -c "import json; p='$out/fresh/settings.json'; d=json.load(open(p)); d['web_password']=''; d['setup_open_until']=1; json.dump(d, open(p,'w'))"
up "$out/fresh" 18503
expect "past its day, setup is closed" "$(setting 18503 setup_open)" False
expect "and changes need a password" "$(code POST 18503 /api/master/taps/herd '{}')" 403
kill $daemon; wait $daemon 2>/dev/null || true

# --- an install from before, no password: never opened ---
printf '{"serial_port": "/dev/nonexistent"}\n' > "$out/old/settings.json"
up "$out/old" 18504
expect "an existing install isn't in setup" "$(setting 18504 setup_open)" False
expect "and changes need a password, as before" "$(code POST 18504 /api/master/taps/herd '{}')" 403
kill $daemon; wait $daemon 2>/dev/null || true

[ $fail = 0 ] && echo "setup e2e passed" || { echo "setup e2e FAILED; see $out/"; exit 1; }
