#!/bin/sh
# docker-compose.https.yml end to end, without Let's Encrypt: the real
# certbot image with a stand-in for the certbot command that "issues" a
# certificate from a test CA after a few seconds. Checks the web UI starts
# on plain HTTP, restarts onto HTTPS once the certificate lands, that the
# copy is tigomonitord's to read and nobody else's, that the web UI can't
# change the paths, and that a new certs volume is filled again on restart.
#
# Runs on a host with Docker Compose (not in the dev container), from the
# repo root, after: docker build -t tigomonitord:https-test -f linux/Dockerfile .
set -eu
out=build/https-compose
project=tigo-https-test
port=18443
rm -rf "$out" && mkdir -p "$out/pki"
pki="$out/pki"

openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=Test CA" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" \
  -keyout "$pki/ca.key" -out "$pki/ca.pem" 2>/dev/null
openssl req -new -newkey rsa:2048 -nodes -subj "/CN=tigo.test" -keyout "$pki/leaf.key" -out "$pki/leaf.csr" 2>/dev/null
printf 'subjectAltName=DNS:tigo.test,IP:127.0.0.1\nauthorityKeyIdentifier=keyid\nextendedKeyUsage=serverAuth\n' > "$pki/san.ext"
openssl x509 -req -in "$pki/leaf.csr" -CA "$pki/ca.pem" -CAkey "$pki/ca.key" -CAcreateserial -days 1 \
  -extfile "$pki/san.ext" -out "$pki/leaf.pem" 2>/dev/null
cat "$pki/leaf.pem" "$pki/ca.pem" > "$pki/fullchain.pem"
chmod 644 "$pki"/*

# The stand-in: certonly issues once (after 5 s, as a DNS challenge takes a
# while) and then, like --keep-until-expiring, leaves it alone; renew does
# nothing. Every call is logged.
cat > "$out/certbot" <<'EOF'
#!/bin/sh
echo "certbot $*" >> /etc/letsencrypt/calls.log
[ "$1" = certonly ] || exit 0
live=/etc/letsencrypt/live/$DOMAIN
[ -f "$live/privkey.pem" ] && exit 0
sleep 5
mkdir -p "$live"
cp /pki/fullchain.pem "$live/fullchain.pem"
cp /pki/leaf.key "$live/privkey.pem"
hook=$(echo "$*" | sed -n 's/.*--deploy-hook \([^ ]*\).*/\1/p')
RENEWED_LINEAGE=$live "$hook"
EOF
chmod 755 "$out/certbot"

cat > "$out/override.yml" <<EOF
services:
  tigomonitord:
    image: tigomonitord:https-test
    container_name: !reset null
    ports: !override
      - "127.0.0.1:$port:8080"
  certbot:
    volumes:
      - $PWD/$out/certbot:/usr/local/bin/certbot:ro
      - $PWD/$pki:/pki:ro
EOF
compose() {
  DOMAIN=tigo.test DNS_PROVIDER=route53 docker compose -p $project \
    -f linux/docker-compose.https.yml -f "$out/override.yml" "$@"
}
cleanup() { compose down -v >/dev/null 2>&1 || true; }
trap cleanup EXIT
cleanup

fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
wait_for() {  # wait_for SECONDS COMMAND...: until COMMAND succeeds
  n=$(( $1 * 5 )); shift
  while [ $n -gt 0 ]; do "$@" >/dev/null 2>&1 && return 0; sleep 0.2; n=$((n - 1)); done
  return 1
}
https_cn() {
  echo | openssl s_client -connect 127.0.0.1:$port -servername tigo.test -CAfile "$pki/ca.pem" 2>/dev/null \
    | sed -n 's/^subject=.*CN *= *\([^,]*\).*/\1/p'
}
logs() { compose logs --no-color "$1" 2>&1; }

compose up -d --quiet-pull 2>"$out/up.log"
expect "plain HTTP at first" "$(wait_for 20 curl -sf http://127.0.0.1:$port/api/status && echo yes || echo no)" yes
expect "waiting, and says so" "$(logs tigomonitord | grep -c 'plain HTTP until the certificate loads')" 1
wait_for 60 sh -c "[ -n \"\$(echo | openssl s_client -connect 127.0.0.1:$port 2>/dev/null | grep subject=)\" ]" || true
expect "HTTPS once it lands" "$(https_cn)" tigo.test
expect "restarted onto it" "$(logs tigomonitord | grep -c 'loads now; restarting to serve HTTPS')" 1
expect "certbot copied it" "$(logs certbot | grep -c 'copied the certificate for tigomonitord')" 2
perms="$(compose exec -T certbot stat -c '%u %a' /certs/privkey.pem)"
expect "key owned by 999, 0600" "$perms" "999 600"
settings() { curl -sf --cacert "$pki/ca.pem" --resolve tigo.test:$port:127.0.0.1 "https://tigo.test:$port/api/settings" "$@"; }
expect "paths set by the command line" "$(settings | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["tls_from_args"], d["tls_cert"])')" \
  "True /certs/fullchain.pem"
# Accepted (a path that doesn't exist would otherwise be refused), and ignored.
expect "a save with other paths succeeds" \
  "$(settings -H 'Content-Type: application/json' -d '{"tls_cert": "/elsewhere.pem", "tls_key": "/elsewhere.key"}')" '{"saved":true}'
expect "and doesn't change them" "$(settings | python3 -c 'import json,sys; print(json.load(sys.stdin)["tls_cert"])')" /certs/fullchain.pem
expect "certonly called once" "$(compose exec -T certbot grep -c 'certbot certonly' /etc/letsencrypt/calls.log)" 1

# A new certs volume, as after `docker volume rm`: certbot fills it again
# from the certificate it kept, without asking Let's Encrypt for another.
compose exec -T certbot sh -c 'rm /certs/*.pem'
compose restart certbot >/dev/null 2>&1
wait_for 30 compose exec -T certbot test -s /certs/privkey.pem || true
expect "refilled on restart" "$(compose exec -T certbot stat -c '%u %a' /certs/privkey.pem 2>/dev/null)" "999 600"
expect "kept certificate reused" "$(logs certbot | grep -c 'copied the certificate for tigomonitord')" 3

[ $fail = 0 ] && echo "https compose test passed" || { echo "https compose test FAILED; see $out/"; exit 1; }
