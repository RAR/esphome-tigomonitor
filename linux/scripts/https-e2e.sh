#!/bin/sh
# HTTPS: tigomonitord serving the web UI with throwaway certificates from a
# test CA. Checks it sends the chain, takes a renewed pair without a
# restart, keeps the old one when a renewal is broken, refuses a bad pair in
# settings, and falls back to plain HTTP when its pair won't load at start.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/https-e2e
rm -rf "$out" && mkdir -p "$out/state" "$out/pki"
pki="$out/pki"
port=18091

openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=Test CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -keyout "$pki/ca.key" -out "$pki/ca.pem" 2>/dev/null
leaf() {  # leaf NAME: a key and a certificate for 127.0.0.1, signed by the CA
  openssl req -new -newkey rsa:2048 -nodes -subj "/CN=$1" -keyout "$pki/$1.key" -out "$pki/$1.csr" 2>/dev/null
  printf 'subjectAltName=IP:127.0.0.1\nauthorityKeyIdentifier=keyid\nextendedKeyUsage=serverAuth\n' > "$pki/san.ext"
  openssl x509 -req -in "$pki/$1.csr" -CA "$pki/ca.pem" -CAkey "$pki/ca.key" -CAcreateserial -days 1 \
    -extfile "$pki/san.ext" -out "$pki/$1.pem" 2>/dev/null
  cat "$pki/$1.pem" "$pki/ca.pem" > "$pki/$1.full.pem"  # as Let's Encrypt's fullchain.pem
}
leaf first
leaf second
leaf other

# Where a deploy hook would put them. mv, as a hook should, so a check never
# reads a half-written file.
install_pair() {  # install_pair CERT KEY
  cp "$1" "$out/state/cert.tmp" && mv "$out/state/cert.tmp" "$out/state/cert.pem"
  cp "$2" "$out/state/key.tmp" && mv "$out/state/key.tmp" "$out/state/key.pem"
}
install_pair "$pki/first.full.pem" "$pki/first.key"
printf '{"tls_cert": "%s", "tls_key": "%s"}\n' "$PWD/$out/state/cert.pem" "$PWD/$out/state/key.pem" \
  > "$out/state/settings.json"

start() {
  build/linux/tigomonitord --state-dir "$out/state" --port $port --serial /dev/nonexistent 2>> "$out/daemon.log" &
  daemon=$!
  for _ in $(seq 50); do grep -q "web: listening on port $port" "$out/daemon.log" 2>/dev/null && break; sleep 0.1; done
}
daemon=
trap 'kill $daemon 2>/dev/null || true' EXIT
start

fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
# client WHAT [PATH [JSON [ORIGIN]]]: WHAT is "cn" (the served certificate's
# name), "chain" (how many certificates came), "status" (of the request,
# verified against the test CA) or "http" (status over plain HTTP).
client() {
  python3 - "$pki/ca.pem" $port "$@" <<'PY'
import json, ssl, sys, urllib.error, urllib.request as u
ca, port, what = sys.argv[1], int(sys.argv[2]), sys.argv[3]
path = sys.argv[4] if len(sys.argv) > 4 else '/api/settings'
body = sys.argv[5].encode() if len(sys.argv) > 5 else None
origin = sys.argv[6] if len(sys.argv) > 6 else None
ctx = ssl.create_default_context(cafile=ca)
if what in ('cn', 'chain'):
    import socket
    try:
        with socket.create_connection(('127.0.0.1', port), timeout=5) as s, ctx.wrap_socket(s, server_hostname='127.0.0.1') as t:
            if what == 'chain':
                print(len(t.get_unverified_chain()))
            else:
                print(dict(x[0] for x in t.getpeercert()['subject'])['commonName'])
    except Exception as e:
        print(type(e).__name__)
    sys.exit()
scheme = 'http' if what == 'http' else 'https'
headers = {'Content-Type': 'application/json'}
if origin: headers['Origin'] = origin
req = u.Request(f'{scheme}://127.0.0.1:{port}{path}', body, headers)
try:
    r = u.urlopen(req, timeout=5, context=None if scheme == 'http' else ctx)
    print(r.status if what == 'status' or scheme == 'http' else r.read().decode())
except urllib.error.HTTPError as e:
    print(e.code)
except Exception as e:
    print(type(e).__name__)
PY
}
field() { client get /api/settings | python3 -c "import json,sys; print(json.load(sys.stdin)['$1'])"; }

expect "served over HTTPS" "$(client status)" 200
expect "certificate" "$(client cn)" first
expect "chain sent" "$(client chain)" 2
expect "tls_active" "$(field tls_active)" True
expect "tls_expires is in the future" "$([ "$(field tls_expires)" -gt "$(date +%s)" ] && echo yes || echo no)" yes
expect "plain HTTP refused" "$(client http)" RemoteDisconnected
expect "a same-site POST is allowed" "$(client status /api/settings '{}' "https://127.0.0.1:$port")" 200
expect "a cross-site POST is refused" "$(client status /api/settings '{}' "https://evil.example")" 403

# A renewal, picked up without a restart.
install_pair "$pki/second.full.pem" "$pki/second.key"
for _ in $(seq 150); do grep -q "loaded the renewed certificate" "$out/daemon.log" && break; sleep 0.1; done
expect "renewal served" "$(client cn)" second
expect "same process" "$(kill -0 $daemon && echo running)" running

# A broken renewal: the new certificate, but a key that isn't its own.
install_pair "$pki/first.full.pem" "$pki/other.key"
for _ in $(seq 150); do grep -q "did not load" "$out/daemon.log" && break; sleep 0.1; done
expect "broken renewal logged" "$(grep -c 'did not load' "$out/daemon.log")" 1
expect "previous one still served" "$(client cn)" second

# Settings refuse a pair that won't load, and half a pair.
expect "mismatched pair refused" "$(client status /api/settings \
  "{\"tls_cert\": \"$PWD/$pki/first.full.pem\", \"tls_key\": \"$PWD/$pki/other.key\"}")" 400
expect "missing file refused" "$(client status /api/settings \
  "{\"tls_cert\": \"$PWD/$pki/nope.pem\", \"tls_key\": \"$PWD/$pki/other.key\"}")" 400
expect "a certificate alone refused" "$(client status /api/settings '{"tls_cert": "/x.pem", "tls_key": ""}')" 400
expect "a good pair accepted" "$(client status /api/settings \
  "{\"tls_cert\": \"$PWD/$out/state/cert.pem\", \"tls_key\": \"$PWD/$pki/first.key\"}")" 200
# Back to the pair in place, which is broken (other.key), for the restart.
# Settings would refuse it, so as an edit by hand would.
printf '{"tls_cert": "%s", "tls_key": "%s"}\n' "$PWD/$out/state/cert.pem" "$PWD/$out/state/key.pem" \
  > "$out/state/settings.json"

# Restarted with a pair that won't load: plain HTTP, and it says why.
kill $daemon
wait $daemon 2>/dev/null || true
: > "$out/daemon.log"
start
expect "falls back to HTTP" "$(client http)" 200
expect "says why" "$(grep -c 'HTTPS is off, serving plain HTTP' "$out/daemon.log")" 1

[ $fail = 0 ] && echo "https e2e passed" || { echo "https e2e FAILED; see $out/"; exit 1; }
