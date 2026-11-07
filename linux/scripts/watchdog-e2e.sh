#!/bin/sh
# The systemd watchdog, without systemd: tigomonitord against a stand-in for
# its notify socket. Checks it says READY=1 once the web UI is up, sends
# WATCHDOG=1 at least twice per watchdog interval, stops sending while the
# process is frozen, and says STOPPING=1 on its way out; and that without
# WatchdogSec it sends no pings at all.
#
# Then its own watchdog, for where there's no systemd (Docker): with the main
# loop hung and the web threads still answering, /api/health and
# --health-check report it, and the process exits once the loop has been
# stuck for --watchdog seconds. The hang is real: the certificate file
# becomes a FIFO, so the loop's 10 s reload check blocks opening it, as on a
# hung network mount, while HTTPS keeps serving the pair it has in memory.
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
expect "own watchdog off under systemd's" "$(grep -c 'watchdog: exits if' "$out/daemon.log")" 0

: > "$out/daemon.log"
run 0
expect "no pings without WatchdogSec" "$(grep -c 'WATCHDOG=1' "$out/notify.log")" 0
expect "READY=1 still sent" "$(grep -c 'READY=1' "$out/notify.log")" 1
expect "own watchdog on without systemd's" "$(grep -c 'watchdog: exits if the main loop stops for 60 s' "$out/daemon.log")" 1

# Its own watchdog, with no systemd at all.
port=18093
pki="$out/pki"
mkdir -p "$pki"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=localhost" -addext "subjectAltName=IP:127.0.0.1" \
  -keyout "$pki/key.pem" -out "$pki/cert.pem" 2>/dev/null
check() { if build/linux/tigomonitord --health-check $port >"$out/check.out" 2>&1; then echo 0; else echo $?; fi; }
: > "$out/daemon.log"
build/linux/tigomonitord --state-dir "$out/state" --port $port --serial /dev/nonexistent \
  --tls-cert "$pki/cert.pem" --tls-key "$pki/key.pem" --watchdog 15 2>>"$out/daemon.log" &
pid=$!
n=50; while [ $n -gt 0 ] && ! grep -q 'listening on port' "$out/daemon.log"; do sleep 0.2; n=$((n - 1)); done
expect "says it's armed" "$(grep -c 'watchdog: exits if the main loop stops for 15 s' "$out/daemon.log")" 1
expect "health check passes" "$(check)" 0
mv "$pki/cert.pem" "$pki/cert.real"
mkfifo "$pki/cert.pem"
hung=$(date +%s)
# The reload check finds the new file within 10 s and blocks; health flags
# a loop more than 10 s late.
n=150; while [ $n -gt 0 ] && [ "$(check)" = 0 ]; do sleep 0.2; n=$((n - 1)); done
expect "health check fails once hung" "$(check)" 1
expect "and says why" "$(grep -c 'main loop: no pass for' "$out/check.out")" 1
expect "HTTPS still answering" "$(python3 -c '
import ssl, sys, urllib.request, urllib.error
try: print(urllib.request.urlopen(sys.argv[1], context=ssl._create_unverified_context(), timeout=5).status)
except urllib.error.HTTPError as e: print(e.code)' https://127.0.0.1:$port/api/health)" 503
n=150; while [ $n -gt 0 ] && kill -0 $pid 2>/dev/null; do sleep 0.2; n=$((n - 1)); done
if kill -0 $pid 2>/dev/null; then kill -9 $pid; code="still running"; wait $pid || true; else code=0; wait $pid || code=$?; fi
expect "exits once stuck for 15 s" "$code" 1
expect "and logs why" "$(grep -c 'watchdog: the main loop has not run for 15 s; exiting' "$out/daemon.log")" 1
expect "within 25 s of the hang" "$([ $(( $(date +%s) - hung )) -le 25 ] && echo yes || echo no)" yes
rm -f "$pki/cert.pem"

# Off with --watchdog 0, and the health check speaks plain HTTP too.
: > "$out/daemon.log"
build/linux/tigomonitord --state-dir "$out/state" --port $port --serial /dev/nonexistent --watchdog 0 2>>"$out/daemon.log" &
pid=$!
n=50; while [ $n -gt 0 ] && ! grep -q 'listening on port' "$out/daemon.log"; do sleep 0.2; n=$((n - 1)); done
expect "--watchdog 0 turns it off" "$(grep -c 'watchdog: exits if' "$out/daemon.log")" 0
expect "health check over plain HTTP" "$(check)" 0
# A save from the web UI reloads settings.json, which has no serial port:
# --serial must outlast it, or health reports none chosen.
python3 -c 'import sys, urllib.request; urllib.request.urlopen(urllib.request.Request(sys.argv[1], b"{}", {"Content-Type": "application/json"}))' \
  http://127.0.0.1:$port/api/settings
sleep 2
expect "--serial outlasts a settings save" "$(check)" 0
kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true
expect "health check, nothing listening" "$(check)" 1

[ $fail = 0 ] && echo "watchdog e2e passed" || { echo "watchdog e2e FAILED; see $out/"; exit 1; }
