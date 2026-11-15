#!/bin/sh
# Installs a .deb on a Pi over ssh and checks it came back: the version,
# the service, its restart count, and a capture still growing if the unit
# has one (a --capture drop-in survives the install; the restart reopens
# the file and appends).
#
#   linux/scripts/push-pi.sh root@HOST [build/deb/tigomonitord_<ver>_arm64.deb]
#
# The newest .deb in build/deb by default. Restarting tigomonitord is a
# short gap on the bus if that Pi is its master.
set -eu
host="${1:?usage: push-pi.sh root@HOST [DEB]}"
deb="${2:-$(ls -t build/deb/*_arm64.deb | head -1)}"
name="$(basename "$deb")"
echo "installing $name on $host"
scp -q "$deb" "$host:/tmp/$name"
ssh "$host" sh -s "$name" <<'REMOTE'
set -eu
name="$1"
cap="$(systemctl cat tigomonitord 2>/dev/null | sed -n 's/.*--capture[= ]\([^ ]*\).*/\1/p' | tail -1)"
before="$( [ -n "$cap" ] && stat -c %s "$cap" 2>/dev/null || echo 0)"
t0="$(date +%s)"
dpkg -i "/tmp/$name" >/dev/null
rm -f "/tmp/$name"
for _ in $(seq 30); do systemctl is-active -q tigomonitord && break; sleep 1; done
echo "installed: $(dpkg-query -W -f '${Version}' tigomonitord)"
echo "service:   $(systemctl is-active tigomonitord), restarts $(systemctl show -p NRestarts --value tigomonitord), up after $(( $(date +%s) - t0 )) s"
if [ -n "$cap" ]; then
  sleep 10
  after="$(stat -c %s "$cap" 2>/dev/null || echo 0)"
  echo "capture:   $cap $before -> $after bytes $( [ "$after" -gt "$before" ] && echo '(growing)' || echo '(NOT growing)')"
fi
journalctl -u tigomonitord --since "@$t0" --no-pager -o cat 2>/dev/null | grep -iE 'error|bus master|listen' | head -5 || true
REMOTE
