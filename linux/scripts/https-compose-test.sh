#!/bin/sh
# docker-compose.https.yml end to end, without Let's Encrypt: the real
# certbot image with a stand-in for the certbot command that "issues" a
# certificate from a test CA after a few seconds. Checks the web UI starts
# on plain HTTP, restarts onto HTTPS once the certificate lands, that the
# copy is tigomonitord's to read and nobody else's, that the web UI can't
# change the paths, and that a new certs volume is filled again on restart.
# Then that a stuck main loop marks the container unhealthy, and that the
# watchdog's exit has Docker restart it.
#
# Runs on a host with Docker Compose (not in the dev container), from the
# repo root, after: docker build -t tigomonitord:https-test -f linux/Dockerfile .
set -eu
# Not under build/: the steps before this one run as root in the dev
# container and leave it root-owned.
out=$(mktemp -d "${TMPDIR:-/tmp}/https-compose.XXXXXX")
project=tigo-https-test
port=18443
mkdir -p "$out/pki"
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
    # No adapter, but a port chosen, so /api/health has nothing to report;
    # a short watchdog and a fast healthcheck, for the stuck-loop test.
    command: ["--tls-cert", "/certs/fullchain.pem", "--tls-key", "/certs/privkey.pem",
              "--serial", "/dev/nonexistent", "--watchdog", "15"]
    healthcheck:
      interval: 1s
      retries: 1
      start_period: 1s  # 0 would mean the image's 60 s
    ports: !override
      - "127.0.0.1:$port:8080"
  certbot:
    volumes:
      - $out/certbot:/usr/local/bin/certbot:ro
      - $pki:/pki:ro
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

# A stuck main loop. As in watchdog-e2e.sh, the certificate becomes a FIFO,
# so the loop's reload check blocks opening it while HTTPS serves the pair in
# memory: unhealthy once the loop is 10 s late, the watchdog's exit at 15 s,
# and Docker's restart. The restarted daemon blocks opening the FIFO too,
# until it's fed the real certificate.
cid="$(compose ps -q tigomonitord)"
health() { docker inspect -f '{{.State.Health.Status}}' "$cid"; }
restarts() { docker inspect -f '{{.RestartCount}}' "$cid"; }
wait_for 30 sh -c "[ \"\$(docker inspect -f '{{.State.Health.Status}}' $cid)\" = healthy ]" || true
expect "healthy before" "$(health)" healthy
before="$(restarts)"
compose exec -T certbot sh -c 'mv /certs/fullchain.pem /certs/real.pem && mkfifo -m 644 /certs/fullchain.pem'
wait_for 30 sh -c "[ \"\$(docker inspect -f '{{.State.Health.Status}}' $cid)\" = unhealthy ]" || true
expect "unhealthy once stuck" "$(health)" unhealthy
wait_for 30 sh -c "[ \$(docker inspect -f '{{.RestartCount}}' $cid) -gt $before ]" || true
expect "restarted by Docker" "$(restarts)" $((before + 1))
expect "after the watchdog's exit" "$(logs tigomonitord | grep -c 'watchdog: the main loop has not run for 15 s; exiting')" 1
compose exec -T certbot sh -c 'cat /certs/real.pem > /certs/fullchain.pem && mv /certs/real.pem /certs/fullchain.pem'
wait_for 60 sh -c "[ \"\$(docker inspect -f '{{.State.Health.Status}}' $cid)\" = healthy ]" || true
expect "healthy again" "$(health)" healthy
expect "on HTTPS" "$(https_cn)" tigo.test

[ $fail = 0 ] && echo "https compose test passed" || { echo "https compose test FAILED; see $out/"; exit 1; }
