#!/bin/sh
# The bus over the network: tigomonitord reading a mock bridge
# (linux/tools/mock_bridge.py) at tcp://, which serves a synthetic capture,
# hangs up, and serves it again. Checks the daemon decodes it, reconnects,
# never sends anything back, and refuses a bad bridge address.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/bridge-e2e
rm -rf "$out" && mkdir -p "$out/state"
panels=12
rounds=20
python3 linux/tools/synth_bus.py "$out/bus.bin" --panels $panels --rounds $rounds >/dev/null
python3 linux/tools/mock_bridge.py "$out/bus.bin" 16638 2 "$out/bridge.json" &
bridge=$!
sleep 0.3
build/linux/tigomonitord --state-dir "$out/state" --port 18090 --serial tcp://127.0.0.1:16638 \
  2> "$out/daemon.log" &
daemon=$!
trap 'kill $daemon $bridge 2>/dev/null || true' EXIT
# Both connections are served and closed; the daemon is then retrying.
for _ in $(seq 50); do [ -f "$out/bridge.json" ] && break; sleep 0.2; done
sleep 1

fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
api() {  # api PATH [JSON]: prints the HTTP status, or the body for a GET
  python3 - "$@" <<'PY'
import sys, urllib.error, urllib.request as u
url = 'http://127.0.0.1:18090' + sys.argv[1]
req = u.Request(url, sys.argv[2].encode(), {'Content-Type': 'application/json'}) if len(sys.argv) > 2 else url
try:
    r = u.urlopen(req, timeout=5)
    print(r.status if len(sys.argv) > 2 else r.read().decode())
except urllib.error.HTTPError as e:
    print(e.code)
PY
}
devices="$(api /api/devices | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["devices"]))')"
expect "panels from the bridge" "$devices" $panels
expect "connections served" "$(python3 -c "import json; print(json.load(open('$out/bridge.json'))['served'])")" 2
expect "bytes sent to the bus" "$(python3 -c "import json; print(json.load(open('$out/bridge.json'))['received'])")" 0
expect "connected twice" "$(grep -c 'listening to the bridge at tcp://127.0.0.1:16638' "$out/daemon.log")" 2
expect "noticed the hang-up" "$([ "$(grep -c 'lost the bridge' "$out/daemon.log")" -ge 1 ] && echo yes || echo no)" yes
for bad in 'tcp://host' 'tcp://host:0' 'tcp://host:70000' 'tcp://:6638' 'tcp://a:b:6638'; do
  code="$(api /api/settings "{\"serial_port\": \"$bad\"}")"
  expect "refused $bad" "$code" 400
done
for good in 'tcp://tigo-bridge.local:6638' 'tcp://[fd00::5]:6638'; do
  code="$(api /api/settings "{\"serial_port\": \"$good\"}")"
  expect "accepted $good" "$code" 200
done
kill $daemon
wait $daemon 2>/dev/null || true
summary="$(grep 'stopping:' "$out/daemon.log")"
# One bad checksum per pass of the capture, as in e2e.sh.
expect "readings" "$(echo "$summary" | sed -E 's/.* ([0-9]+) readings.*/\1/')" $((2 * panels * rounds))
expect "bad checksums" "$(echo "$summary" | sed -E 's/.* ([0-9]+) bad checksums.*/\1/')" 2
[ $fail = 0 ] && echo "bridge e2e passed" || { echo "bridge e2e FAILED; see $out/"; exit 1; }
