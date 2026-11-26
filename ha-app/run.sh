#!/bin/sh
# Starts tigomonitord as a Home Assistant app: state in /data, the web UI on
# 8080 for Home Assistant's ingress, and, once, Home Assistant's MQTT broker.
set -eu
state=/data
settings=$state/settings.json
offered=$state/.ha-mqtt-offered

# The broker from the MQTT app, if there is one and no broker is set yet.
# Offered once: someone who clears it later doesn't get it back on restart.
if [ ! -e "$offered" ] && [ -n "${SUPERVISOR_TOKEN:-}" ]; then
  answer=$(curl -fsS --max-time 10 -H "Authorization: Bearer $SUPERVISOR_TOKEN" http://supervisor/services/mqtt 2>/dev/null || true)
  if [ -n "$answer" ] && [ "$(printf '%s' "$answer" | jq -r '.result // empty')" = ok ]; then
    current=""
    [ -f "$settings" ] && current=$(jq -r '.mqtt.host // ""' "$settings" 2>/dev/null || echo "")
    if [ -z "$current" ]; then
      broker=$(printf '%s' "$answer" | jq '.data | {host, port, username, password}')
      tmp=$state/settings.json.new
      { [ -f "$settings" ] && cat "$settings" || echo '{}'; } | jq --argjson m "$broker" '.mqtt = ((.mqtt // {}) + $m)' > "$tmp"
      mv "$tmp" "$settings"
      echo "MQTT: using Home Assistant's broker at $(printf '%s' "$broker" | jq -r '.host'):$(printf '%s' "$broker" | jq -r '.port')"
    fi
    touch "$offered"
  fi
fi

set -- --state-dir "$state" --port 8080
# What this build of the daemon takes: run.sh can be newer than the image
# it's built on, when someone rebuilds the app between releases.
help=$(/usr/bin/tigomonitord --help 2>&1 || true)
# Home Assistant signs users in before its ingress proxy passes them on.
# Builds older than the flag ask for the password instead, if one is set.
if printf '%s' "$help" | grep -q -- --trust-ingress; then
  set -- "$@" --trust-ingress 172.30.32.2
fi
# The app's Configuration tab, which Home Assistant writes to options.json.
# Passed as flags, never written into settings.json: the daemon takes a
# start without settings.json as a new install. Dangerous options as set
# there, on or off, over settings.json's; a daemon without the flag would
# refuse to start, so it gets settings.json's.
options=$state/options.json
option() { [ -f "$options" ] && [ "$(jq -r --arg k "$1" '.[$k] // false' "$options" 2>/dev/null)" = true ]; }
if [ -f "$options" ] && printf '%s' "$help" | grep -q -- --dangerous-options; then
  if option dangerous_options; then
    set -- "$@" --dangerous-options on
  else
    set -- "$@" --dangerous-options off
  fi
fi
if option debug_logging; then
  set -- "$@" --verbose
fi
exec /usr/bin/tigomonitord "$@"
