#!/bin/sh
# The systemd watchdog, without systemd: tigomonitord against a stand-in for
# its notify socket. Checks it says READY=1 once the web UI is up, sends
# WATCHDOG=1 at least twice per watchdog interval, stops sending while the
# process is frozen, and says STOPPING=1 on its way out; and that without
# WatchdogSec it sends no pings at all.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/watchdog-e2e
rm -rf "$out" && mkdir -p "$out/state"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
# run WATCHDOG_USEC: runs the daemon for a while against the stand-in, and
# writes each message received, with its time, to $out/notify.log.
run() {
  python3 - "$out" "$1" <<'PY'
import os, signal, socket, subprocess, sys, time
out, usec = sys.argv[1], sys.argv[2]
path = os.path.abspath(out + '/notify.sock')
if os.path.exists(path): os.unlink(path)
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind(path)
s.settimeout(0.2)
env = dict(os.environ, NOTIFY_SOCKET=path)
if usec != '0': env['WATCHDOG_USEC'] = usec
d = subprocess.Popen(['build/linux/tigomonitord', '--state-dir', out + '/state', '--port', '18092',
                      '--serial', '/dev/nonexistent'], env=env, stderr=open(out + '/daemon.log', 'a'))
log = open(out + '/notify.log', 'w')
start = time.monotonic()
def drain(until):
    while time.monotonic() < until:
        try:
            msg = s.recv(4096).decode()
        except socket.timeout:
            continue
        for line in msg.split('\n'):
            log.write(f'{time.monotonic() - start:.2f} {line}\n')
        log.flush()
drain(start + 5)
d.send_signal(signal.SIGSTOP)  # a hang, as far as the watchdog can tell
log.write(f'{time.monotonic() - start:.2f} -- frozen\n')
drain(start + 8)
d.send_signal(signal.SIGCONT)
log.write(f'{time.monotonic() - start:.2f} -- thawed\n')
drain(start + 10)
d.terminate()
drain(start + 12)
d.wait()
PY
}

run 2000000
log="$out/notify.log"
ready_line="$(grep -n 'READY=1' "$log" | head -1 | cut -d: -f1)"
first_ping_line="$(grep -n 'WATCHDOG=1' "$log" | head -1 | cut -d: -f1)"
expect "READY=1 sent" "$(grep -c 'READY=1' "$log")" 1
expect "READY=1 before the first ping" "$([ "$ready_line" -lt "$first_ping_line" ] && echo yes || echo no)" yes
expect "web UI up by READY=1" "$(grep -c 'web: listening on port 18092' "$out/daemon.log")" 1
# Pings while running: from READY to the freeze (~4 s), and after the thaw.
expect "pings while running (>= 3)" "$(awk '/-- frozen/{exit} /WATCHDOG=1/{n++} END{print (n>=3)?"yes":"no"}' "$log")" yes
expect "no pings while frozen" "$(awk '/-- frozen/{f=1;next} /-- thawed/{f=0} f&&/WATCHDOG=1/{n++} END{print n+0}' "$log")" 0
expect "pings again after the thaw" "$(awk '/-- thawed/{t=1;next} t&&/WATCHDOG=1/{n++} END{print (n>=1)?"yes":"no"}' "$log")" yes
# The longest gap between pings while running, under half the 2 s interval
# plus slack: they come once a second.
gap="$(awk '/-- frozen/{exit} /WATCHDOG=1/{if(p!="")g=($1-p>g)?$1-p:g; p=$1} END{print (g<1.6)?"ok":g}' "$log")"
expect "pings at least every 1.6 s" "$gap" ok
expect "STOPPING=1 on exit" "$(grep -c 'STOPPING=1' "$log")" 1

: > "$out/daemon.log"
run 0
expect "no pings without WatchdogSec" "$(grep -c 'WATCHDOG=1' "$out/notify.log")" 0
expect "READY=1 still sent" "$(grep -c 'READY=1' "$out/notify.log")" 1

[ $fail = 0 ] && echo "watchdog e2e passed" || { echo "watchdog e2e FAILED; see $out/"; exit 1; }
