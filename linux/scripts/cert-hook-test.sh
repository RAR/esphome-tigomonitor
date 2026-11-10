#!/bin/sh
# The certbot deploy hook the .deb ships, run as certbot would run it, in a
# clean Debian 13 container with the package installed. Checks the copy is
# the monitor's to read and nobody else's, and that a lineage missing a file
# leaves the previous pair alone rather than half replacing it.
#
# Runs on a host with Docker, from the repo root, after build-deb.sh:
#   linux/scripts/cert-hook-test.sh build/deb/tigomonitord_*.deb
set -eu
deb="${1:?usage: cert-hook-test.sh PATH.deb}"
docker run --rm -v "$PWD/$deb":/tmp/tigomonitord.deb:ro debian:trixie sh -c '
set -u
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
apt-get update -qq >/dev/null && apt-get install -y -qq /tmp/tigomonitord.deb >/dev/null 2>&1 \
  || { echo "  FAIL the package did not install"; exit 1; }
hook=/usr/libexec/tigomonitor/cert-hook
expect "hook shipped, executable" "$(stat -c %a "$hook" 2>/dev/null)" 755

live=/etc/letsencrypt/live/tigo.test
mkdir -p "$live"
echo first-chain > "$live/fullchain.pem"
echo first-key > "$live/privkey.pem"
RENEWED_LINEAGE="$live" "$hook"
expect "hook succeeds" $? 0
for f in fullchain.pem privkey.pem; do
  expect "$f owned by tigomonitor, 0600" "$(stat -c "%U %G %a" /var/lib/tigomonitor/$f 2>/dev/null)" "tigomonitor tigomonitor 600"
done
expect "certificate copied" "$(cat /var/lib/tigomonitor/fullchain.pem 2>/dev/null)" first-chain
expect "key copied" "$(cat /var/lib/tigomonitor/privkey.pem 2>/dev/null)" first-key

# A renewal: replaced in place, no temporary files left behind.
echo second-chain > "$live/fullchain.pem"
echo second-key > "$live/privkey.pem"
RENEWED_LINEAGE="$live" "$hook"
expect "renewal copied" "$(cat /var/lib/tigomonitor/fullchain.pem /var/lib/tigomonitor/privkey.pem | tr "\n" " ")" "second-chain second-key "
expect "nothing left over" "$(ls /var/lib/tigomonitor | grep -c new)" 0

# A lineage missing its key: refused, and the pair in use kept whole.
echo third-chain > "$live/fullchain.pem"
rm "$live/privkey.pem"
RENEWED_LINEAGE="$live" "$hook" 2>/dev/null
expect "broken lineage refused" "$([ $? -ne 0 ] && echo yes || echo no)" yes
expect "previous pair kept" "$(cat /var/lib/tigomonitor/fullchain.pem /var/lib/tigomonitor/privkey.pem | tr "\n" " ")" "second-chain second-key "
expect "nothing left over" "$(ls /var/lib/tigomonitor | grep -c new)" 0

env -u RENEWED_LINEAGE "$hook" 2>/dev/null
expect "run outside certbot refused" "$([ $? -ne 0 ] && echo yes || echo no)" yes

[ $fail = 0 ] && echo "cert hook test passed" || { echo "cert hook test FAILED"; exit 1; }
'
