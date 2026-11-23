#!/bin/sh
# The re-sort (docs/superpowers/specs/2026-11-23-tap-resort-design.md), against
# linux/tools/mock_taps.py's three TAPs on one bus: 1201 and 1202 with 20
# optimizers each, 1203 new with none, all on one made-up key. The mock lets
# 1201's own optimizers join only 1201, 1202's only 1203 (as string C going to
# Mid Roof), and one of them never.
#
# Checks: a dry run writes nothing; the re-sort's PV off goes out before every
# network write and PV on after; each TAP gets a new PAN and all three one new
# key; resort.json is 0600 while it runs; every TAP is trimmed to the
# optimizers that joined it, and the one that never joins stays listed on all
# three; a restart carries on waiting; Stop waiting deletes the file; a second
# re-sort's put-back gives each TAP the network it had; and no key, and no PAN
# in the re-sort's log lines or its API answer, leaks.
# Run from the repo root inside the dev container, after build.sh.
set -eu
out=build/resort-e2e
rm -rf "$out" && mkdir -p "$out/one"
fail=0
expect() {  # expect DESCRIPTION ACTUAL WANTED
  if [ "$2" = "$3" ]; then echo "  ok   $1: $2"; else echo "  FAIL $1: got $2, want $3"; fail=1; fi
}
# req METHOD PORT PATH [JSON]: prints "<code> <body>", as the owner (admin:pw)
req() {
  python3 - "$@" <<'PY'
import base64, sys, urllib.error, urllib.request as u
method, port, path = sys.argv[1:4]
body = sys.argv[4].encode() if len(sys.argv) > 4 and sys.argv[4] else None
h = {'Content-Type': 'application/json', 'Authorization': 'Basic ' + base64.b64encode(b'admin:pw').decode()}
try:
    r = u.urlopen(u.Request(f'http://127.0.0.1:{port}{path}', body, h, method=method), timeout=5)
    print(r.status, r.read().decode())
except urllib.error.HTTPError as e:
    print(e.code, e.read().decode())
except Exception:
    print('000 {}')
PY
}
code() { req "$@" | cut -d' ' -f1; }
body() { req "$@" | cut -d' ' -f2-; }
master() { body GET "$1" /api/master | python3 -c "import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1]))" "$2"; }
stat() { python3 -c "import json,sys; d=json.load(open('$1')); print(eval(sys.argv[1]))" "$2"; }
wait_for() {  # wait_for SECONDS PORT PYTHON-EXPR: until /api/master makes it True
  for _ in $(seq $(($1 * 4))); do [ "$(master "$2" "$3" 2>/dev/null)" = True ] && return 0; sleep 0.25; done
  return 0
}
wait_mock() {  # wait_mock SECONDS PYTHON-EXPR: until the mock's stats make it True
  for _ in $(seq $(($1 * 4))); do [ "$(stat "$out/mock.json" "$2" 2>/dev/null)" = True ] && return 0; sleep 0.25; done
  return 0
}
port=18601 tap=0 daemon=0
trap 'kill $daemon $tap 2>/dev/null || true' EXIT
start_daemon() {
  build/linux/tigomonitord --state-dir "$out/one" --web-dir components/tigo_server/web 2>> "$out/daemon.log" &
  daemon=$!
  for _ in $(seq 40); do [ "$(code GET "$port" /api/health)" != 000 ] && break; sleep 0.25; done
}

# Ids: 1201's optimizers are 0002-0015, 1202's 0016-0029.
python3 linux/tools/mock_taps.py "$out/mock.tty" --stats "$out/mock.json" --keys-file "$out/keys.txt" \
  --tap 1201,a1,20 --tap 1202,a2,20 --tap 1203,a3,0 \
  --radio 1,15,7E21,30 --radio 2,18,5A5A,30 --radio 3,20,1A2B,30 \
  --window 6 --join-after 2 --home 1:0002-0015 --home 3:0016-0029 --never-join 0029 &
tap=$!
sleep 0.5
echo '{"taps": [{"id": "1201", "mac": "04c05b0000a1b2a1"}, {"id": "1202", "mac": "04c05b0000a1b2a2"}, {"id": "1203", "mac": "04c05b0000a1b2a3"}]}' > "$out/one/taps.json"
printf '{"bus_role": "master", "master_startup": true, "master_housekeeping": false, "master_queries": false, "master_optimizer_writes": false, "master_read_tap_config": false, "discovery_auto": false, "master_table_writes": true, "serial_port": "%s", "gateway_id": 4609, "web_port": %s, "web_password": "pw"}\n' \
  "$PWD/$out/mock.tty" "$port" > "$out/one/settings.json"
start_daemon
wait_for 60 $port "len(d['taps']) == 3 and all(t['key'] == 'same' for t in d['taps'])"
expect "three TAPs on one key" "$(master $port "[t['key'] for t in d['taps']]")" "['same', 'same', 'same']"

# Nothing to stop or put back yet.
expect "no re-sort to stop" "$(code POST $port /api/master/resort/stop)" 409
expect "nothing to put back" "$(code POST $port /api/master/resort/put-back '{"dry_run": false}')" 409

# --- a dry run: reads the tables, writes nothing ---
expect "a dry run starts" "$(code POST $port /api/master/resort '{}')" 200
wait_for 30 $port "d['resort']['finished'] and d['resort']['dry_run']"
expect "and says what it would send" "$(master $port "(d['resort']['ok'], len(d['resort']['dry_frames']) > 5)")" "(True, True)"
expect "and lists all 40" "$(master $port "'list all 40 optimizers' in d['resort']['message']")" True
sleep 1.5
expect "having written nothing" "$(stat "$out/mock.json" "[t['cfg_writes'] for t in d['taps']]")" "[[], [], []]"

# --- the re-sort ---
expect "the re-sort starts" "$(code POST $port /api/master/resort '{"dry_run": false}')" 200
wait_for 60 $port "d['resort']['state'] == 'waiting'"
expect "and waits" "$(master $port "(d['resort']['state'], d['resort']['can_put_back'])")" "('waiting', True)"
expect "resort.json is the service's alone" "$(command stat -c %a "$out/one/resort.json")" 600
sleep 1.5
expect "PV off then on, once each, on every TAP" "$(stat "$out/mock.json" "[[p['b'] for p in t.get('pv', [])] for t in d['taps']]")" \
  "[['00', '01'], ['00', '01'], ['00', '01']]"
expect "every PV off before every write, every PV on after" "$(stat "$out/mock.json" "(lambda pv, w: max(p['seq'] for p in pv if p['b'] == '00') < min(w) and max(w) < min(p['seq'] for p in pv if p['b'] == '01'))([p for t in d['taps'] for p in t['pv']], [c['seq'] for t in d['taps'] for c in t['cfg_writes']])")" True
expect "a new PAN each" "$(stat "$out/mock.json" "[t['cfg_writes'][0]['pan'] not in ('7E21', '5A5A', '1A2B') for t in d['taps']] + [len({t['cfg_writes'][0]['pan'] for t in d['taps']})]")" \
  "[True, True, True, 3]"
expect "one new key for all three, their channels kept" "$(stat "$out/mock.json" "[(c['channel'], c['key_same_as'], c['key_id']) for c in (t['cfg_writes'][0] for t in d['taps'])]")" \
  "[(15, None, 1), (18, None, 1), (20, None, 1)]"

# Each TAP trimmed to its own; the one that never joins stays listed on all.
own1="[['%04X' % i, True] for i in range(0x02, 0x16)]"
own3="[['%04X' % i, True] for i in range(0x16, 0x29)]"
wait_mock 150 "[t['table'] for t in d['taps']] == [$own1 + [['0029', False]], [['0029', False]], $own3 + [['0029', False]]]"
expect "1201 kept its own" "$(stat "$out/mock.json" "d['taps'][0]['table'] == $own1 + [['0029', False]]")" True
expect "1202's moved to 1203" "$(stat "$out/mock.json" "(d['taps'][1]['table'], d['taps'][2]['table'] == $own3 + [['0029', False]])")" \
  "([['0029', False]], True)"
wait_for 30 $port "[m['id'] for m in d['resort']['missing']] == ['0029']"
expect "the one missing" "$(master $port "[m['id'] for m in d['resort']['missing']]")" "['0029']"
expect "joined per TAP" "$(master $port "[(j['tap'], j['count']) for j in d['resort']['joined']]")" "[('1201', 20), ('1202', 0), ('1203', 19)]"
body GET $port /api/master > "$out/master-waiting.json"

# --- a restart carries on ---
kill $daemon; wait $daemon 2>/dev/null || true
start_daemon
wait_for 60 $port "d['resort']['state'] == 'waiting' and [m['id'] for m in d['resort']['missing']] == ['0029']"
expect "after a restart, still waiting" "$(master $port "(d['resort']['state'], d['resort']['can_put_back'], [m['id'] for m in d['resort']['missing']])")" \
  "('waiting', True, ['0029'])"
grep -q "re-sort: found one waiting" "$out/daemon.log" && found=yes || found=no
expect "and said so" "$found" yes

# --- Stop waiting ---
expect "stop waiting" "$(code POST $port /api/master/resort/stop)" 200
wait_for 20 $port "d['resort']['state'] == 'idle'"
[ -e "$out/one/resort.json" ] && gone=no || gone=yes
expect "resort.json is gone" "$gone" yes
expect "the missing one stays listed on every TAP" "$(stat "$out/mock.json" "[['0029', False] in t['table'] for t in d['taps']]")" "[True, True, True]"

# --- a second re-sort, then its put-back ---
expect "a second re-sort" "$(code POST $port /api/master/resort '{"dry_run": false}')" 200
wait_for 60 $port "d['resort']['state'] == 'waiting' and d['resort']['can_put_back']"
expect "a put-back starts" "$(code POST $port /api/master/resort/put-back '{"dry_run": false}')" 200
wait_for 60 $port "d['resort']['state'] == 'waiting' and not d['resort']['can_put_back']"
sleep 1.5
expect "each TAP has the network it had" "$(stat "$out/mock.json" "[(len(t['cfg_writes']), t['cfg_writes'][2]['pan'] == t['cfg_writes'][0]['pan'], t['cfg_writes'][2]['key_id'] == t['cfg_writes'][0]['key_id']) for t in d['taps']]")" \
  "[(3, True, True), (3, True, True), (3, True, True)]"
body GET $port /api/master > "$out/master-putback.json"

# --- no key, and no PAN in the re-sort's own words ---
leaks=$(python3 - "$out" <<'PY'
import json, re, sys
out = sys.argv[1]
keys = [l.strip() for l in open(f'{out}/keys.txt') if l.strip()]
stats = json.load(open(f'{out}/mock.json'))
pans = {'7E21', '5A5A', '1A2B'} | {c['pan'] for t in stats['taps'] for c in t['cfg_writes']}
log = open(f'{out}/daemon.log', errors='replace').read()
answers = [open(f'{out}/{n}').read() for n in ('master-waiting.json', 'master-putback.json')]
n = 0
for k in keys:  # any 4 bytes of a key, joined or spaced, either case
    for i in range(0, 32 - 7, 2):
        run = k[i:i + 8]
        spaced = ' '.join(run[j:j + 2] for j in range(0, 8, 2))
        for text in [log] + answers:
            if run.lower() in text.lower() or spaced.lower() in text.lower():
                n += 1
resort_lines = '\n'.join(l for l in log.splitlines() if 're-sort' in l)
resort_json = ' '.join(json.dumps(json.loads(a.split(' ', 1)[-1] if a[:3].isdigit() else a).get('resort', {})) for a in answers)
for p in pans:
    if re.search(r'\b' + p + r'\b', resort_lines, re.I) or re.search(p, resort_json, re.I):
        n += 1
print(n)
PY
)
expect "no key or PAN leaked" "$leaks" 0

[ $fail = 0 ] && echo "re-sort e2e passed" || { echo "re-sort e2e FAILED; see $out/"; exit 1; }
