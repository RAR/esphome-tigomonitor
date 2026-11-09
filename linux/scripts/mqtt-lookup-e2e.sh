#!/bin/sh
# A broker name that takes a while to look up doesn't hold up the daemon's
# main loop: saving it, then any other setting, applies the second as fast
# as usual. linux/tools/slow_lookup.c makes a lookup of a ".slow" name take
# 3 s, as an unreachable DNS server does.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/mqtt-lookup-e2e
rm -rf "$out" && mkdir -p "$out/state"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
gcc -shared -fPIC -o "$out/slow_lookup.so" linux/tools/slow_lookup.c -ldl
printf '{"web_port": 18140}\n' > "$out/state/settings.json"
LD_PRELOAD="$PWD/$out/slow_lookup.so" build/linux/tigomonitord --state-dir "$out/state" --serial /dev/nonexistent \
  2> "$out/daemon.log" &
daemon=$!
sleep 1
# Seconds from saving a setting to its taking effect: first with a broker
# that's an address, then with one whose name takes 3 s to look up.
apply_time() {  # apply_time HOST
  python3 - "$1" <<'PY'
import json, sys, time, urllib.request as u
B = 'http://127.0.0.1:18140'
def post(body):
    u.urlopen(u.Request(B + '/api/settings', json.dumps(body).encode(), {'Content-Type': 'application/json'}))
def running():
    return json.load(u.urlopen(B + '/api/settings'))['mqtt']
post({'mqtt': {'host': sys.argv[1], 'port': 1, 'discovery': True}})
while running()['host'] != sys.argv[1]:
    time.sleep(0.02)
t0 = time.time()
post({'mqtt': {'discovery': False}})
while running()['discovery'] is not False and time.time() - t0 < 10:
    time.sleep(0.02)
print(f'{time.time() - t0:.1f}')
PY
}
usual=$(apply_time 127.0.0.1)
slow=$(apply_time broker.slow)
echo "  a save applies in ${usual} s with an address, ${slow} s with a slow name"
expect "no slower with a slow broker name" "$(python3 -c "print($slow < $usual + 1.0)")" True
# On the worker, the lookup itself still takes its 3 s.
for _ in $(seq 40); do grep -q 'broker.slow' "$out/daemon.log" && break; sleep 0.2; done
expect "the slow lookup was tried, and failed" "$(grep -c 'cannot reach broker.slow' "$out/daemon.log" | python3 -c 'import sys; print(int(sys.stdin.read()) >= 1)')" True
kill $daemon; wait $daemon 2>/dev/null || true
[ $fail = 0 ] && echo "mqtt lookup e2e passed" || { echo "mqtt lookup e2e FAILED; see $out/"; exit 1; }
