#!/bin/sh
# End-to-end check without hardware: a synthetic bus stream through the daemon
# into a local mosquitto, then the MQTT traffic and saved state are checked.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/e2e
rm -rf "$out" && mkdir -p "$out/state"
panels=12
rounds=20
python3 linux/tools/synth_bus.py "$out/bus.bin" --panels $panels --rounds $rounds >/dev/null
printf '{"device_id": "test", "publish_interval_s": 5, "mqtt": {"host": "127.0.0.1", "port": 18830}}\n' \
  > "$out/state/settings.json"

mosquitto -p 18830 -d
sleep 0.5
mosquitto_sub -p 18830 -v -t '#' > "$out/mqtt.txt" &
sub=$!
sleep 0.3
build/linux/tigomonitord --state-dir "$out/state" --port 0 --replay "$out/bus.bin" --speed 0 2> "$out/daemon.log"
sleep 0.5
kill $sub
pkill mosquitto || true

fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
summary="$(grep 'stopping:' "$out/daemon.log")"
expect "readings" "$(echo "$summary" | sed -E 's/.* ([0-9]+) readings.*/\1/')" $((panels * rounds))
expect "bad checksums" "$(echo "$summary" | sed -E 's/.* ([0-9]+) bad checksums.*/\1/')" 1
expect "panels" "$(echo "$summary" | sed -E 's/.* ([0-9]+) panels.*/\1/')" $panels
expect "panel discovery configs" "$(grep -c '^homeassistant/sensor/tigo_04C05B' "$out/mqtt.txt")" $((panels * 9))
expect "system discovery configs" "$(grep -c '^homeassistant/sensor/tigomonitor_test_' "$out/mqtt.txt")" 3
# One publish per interval plus a final one at the end of the replay, so at
# least one message per panel; how many depends on timing.
states="$(grep -c '^tigomonitor/test/panel/' "$out/mqtt.txt")"
expect "panel state messages >= $panels" "$([ "$states" -ge $panels ] && echo yes || echo "no ($states)")" yes
expect "went online" "$(grep -c '^tigomonitor/test/status online' "$out/mqtt.txt")" 1
expect "went offline" "$(grep -c '^tigomonitor/test/status offline' "$out/mqtt.txt")" 1
expect "nodes saved" "$(grep -c '"long_address"' "$out/state/state.json")" $panels
[ $fail = 0 ] && echo "e2e passed" || { echo "e2e FAILED; see $out/"; exit 1; }
