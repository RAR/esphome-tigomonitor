# CCA replacement: tigomonitord as the TAP's bus master

Branch: `feature/cca-replacement` (off `feature/pi-zero-build`). Status: design,
exploratory.

## Why

The owner doesn't want the array tied to vendor cloud hardware. Today the Tigo
CCA is the RS485 master: it keeps the optimizers running, and tigomonitor can
only listen to it. If Tigo ends the cloud, locks the firmware further (4.0.4
already closed local HTTP), or the CCA dies, the array depends on a box we
don't control. The goal is a replacement on hardware and software we own.

## Goals

1. **Take over the bus with the CCA removed.** The Pi build (tigomonitord) is
   the RS485 master to the TAP, and the array produces as it does under the CCA.
2. **Own the heartbeat.** The optimizers stay out of safe mode only while our
   master is alive and on. The owner's work procedure stays valid: cutting
   power to the master (today the CCA), then opening the PV disconnect, must
   put the array in safe mode.
3. **Keep everything tigomonitor already shows,** now pulled from the TAP
   instead of overheard: per-panel readings, the node table, history, MQTT.
4. **Bring up a TAP after a power cut.** A TAP that lost power comes back at
   the factory address; re-address it to the running id and start it, as the
   CCA does at every boot. **Partly built** (`master_startup`): the gateway
   discovery/re-addressing works against a mock; whether the array also needs
   the CCA's session-init (0x0E02, 0x0B00, the init polls) is a roof question.
5. **Later: commission without a CCA.** Wake a new array and enroll its
   optimizers, from a barcode list (cloud import or node table backup), with
   no CCA ever involved.

## Non-goals

- Tigo's cloud, app, or any reporting to Tigo.
- An ESP32 version. The Pi was chosen as more stable and easier to work with;
  an ESP32 port can come later if the Pi version proves out.
- Running alongside a live CCA as a second master.

## Anti-goals (failure even if it "works")

- **Weakened rapid shutdown.** If anything keeps the heartbeat going after the
  master loses power, crashes or hangs, the design has failed. Failure must
  mean safe mode, never "stuck producing".
- **Less production than under the CCA.**
- **Stranding hardware.** Any command that leaves a TAP or optimizer in a state
  the stock CCA can't recover (a changed RS485 id, a cleared roster, a stored
  setting) is a failure. The CCA must always be able to go back in.

## What we know (and where from)

- **Framing is solved:** `00 FF FF | 7E 07 | escaped(DEST CMD PAYLOAD CRC16le) | 7E 08`,
  CRC init 0x8408, escape table `{7E,24,23,25,A4,A3,A5}`. Replies are
  `dest|0x8000`, `cmd|1`; NAK is CMD 0x0006. Verified byte-exact on a spare TAP.
  Source: `~/tigo-protocol-analysis/docs/HANDOFF_TIGOMONITOR.md`,
  `CCA_TAP_PROTOCOL.md`, `LIVE_TAP_SESSION.md`; codec in
  `scripts/tggw_crc16.py` (or `crc16_tggw.py`).
- **Read commands are confirmed:** VERSION 0x000A, GET_CHANNEL 0x000E,
  GET_MAC_W_BACKOFF 0x0038, GET_ADT 0x001C, GET_NEW_ANSWER 0x0142,
  GET_NEW_BINARY_DATA 0x0042, GET_GATEWAY_STAT 0x0300. Several NAK without
  their payload.
- **Writes carry the key `37 24 92 66`** at payload[0:4] (all but SET_CHANNEL).
  SET_NETRUN 0x0058 starts and stops production.
- **Discovery** (`docs/DISCOVERY.md`): gateways by GET_MAC_W_BACKOFF broadcast,
  then SET_ID_BY_MAC; optimizers matched against the expected barcode list,
  not open enrollment.
- **Working TX hardware** (openTAPtoX): an auto-direction RS485 module, a common
  ground with the TAP, `00 FF FF` preamble, and a poll timeout of ~50 ms.
  Manual DE/RE control is the likely reason our earlier attempts got silence.
- **The known wall:** openTAPtoX could address and poll a TAP without a CCA
  but couldn't wake sleeping optimizers (TAP status 0x0E where the CCA gets an
  ACK). Goal 4 lives behind this.
- **Rapid shutdown on this site:** the owner shuts down by cutting CCA power,
  which also drops the TAP (fed over the CCA's bus cable, unverified). The
  EG4 FlexBoss inverters also carry an RSS transmitter. Which signal the
  optimizers actually follow is **not yet known** (see Open questions); goal 2
  holds either way.

### The keep-alive, from the CCA's binaries

From `~/tigo-protocol-analysis/docs/KEEPALIVE_NETRUN.md` (lmudcd strings and
counters; nothing was put on a bus):

- **Both layers take part.** Once the TAP is in Running mode with netrun
  started, it sends the mesh timing beacon on its own clock. The master
  stays in the loop all the same:
  - it enforces StartPWM=252 on the optimizers continuously ("[Mass
    StartPWM]", "Changing StartPWM from %d to %d");
  - it runs an AdvSync about every 12 minutes: Netrun Stop, set TXID,
    Netrun Start, then a check of GET_GATEWAY_STAT's running flag;
  - it stops queueing beacons in PV-safe mode.
- **No optimizer keep-alive timeout exists in lmudcd or meshdcd.** How long an
  optimizer runs without an enabling beacon lives in the TS4 firmware. Whether
  the TAP stops beaconing when the master goes quiet lives in the TAP
  firmware. Measure both; don't assume "set netrun once and walk away".
- **CCA start-up, PV-safe to producing:**
  1. verify the gateway's MAC, version, channel and mode;
  2. set the PAN;
  3. SET_GATEWAY_MODE to Running (0x5030);
  4. STORE;
  5. reset, and let the channel settle;
  6. AdvSync (Netrun Stop, TXID, Netrun Start, verify);
  7. mass StartPWM=252.
- **What survives a power cut** (from the binaries; corrected by the roof
  capture below): the binaries suggested STORE keeps the channel, RS485 id,
  mode, TX power and PAN. **The roof TAP does not keep its RS485 id:** after
  a CCA power cycle it answers at the factory address 0x1235 and the CCA
  re-addresses it. Either way it comes back silent until a master sets it
  running again. That is the fail-safe property goal 2 relies on. The
  observed power-up sequence is written up in the analysis repo,
  `docs/ROOF_STEADY_STATE.md`, "Roof CCA power-up sequence"; the raw capture
  is `build/roof/coldstart-20261026.bin` (git-ignored, on the owner's
  machine).
- **The netrun opcode and arguments are NOT known.** The binary-derived 0x0058
  conflicts with the analysis repo's other docs (0x005A, 0x0054), and the
  start/stop argument bytes were never captured. Do not hardcode them. The
  real CCA does Netrun Stop/Start on every AdvSync, so a passive roof
  capture of 15 minutes or more contains the exact bytes.
- GET_GATEWAY_STAT (0x0300, read-only) shows whether netrun took, as lmudcd
  itself checks.

### What the roof CCA actually sends (passive capture, 2026-10-26)

`build/roof/steady-20261026.bin`: 20 minutes, daylight, 203,686 frames, 0 bad,
gateway id 0x1201. The roof CCA does not use the bench opcode set above. It
sends only 0x0148 power polls and a tunnel, 0x0B0F with replies on 0x0B10,
whose payload is `00 00 00 <sub> <seq> ...`:

| sub | cadence | what it looks like |
|---|---|---|
| 0x2e | every 20 s | reads a **run lease**: reply 0x2f carries a countdown in seconds |
| 0x2d | when the lease hits 0 | `<seq> ba be 02 03 84 00 2c 01 00`: lease back to 900 s; 0x2c = 44 nodes. The same frame (with 36 nodes) is in the analysis repo's MESH_COMMANDS.md |
| 0x41 | every ~17.5 s | telemetry poll spec, read-only: cycles nodes 1, 16, 17, 35, 38-41 with a fixed query template; the data comes back as 0x0149 |
| 0x06 | occasional | ASCII to one node: `^00Version`, `^00Smrt`, `^00Mppt`, `^00Info` |
| 0x26 | a burst | reads the roster in pages (index 0, 14, 26, 38, 46) |
| 0x22, 0x0B00 | once | unknown |

**The keep-alive is a 15-minute lease, renewed once it reads 0.** The CCA reads
it on a plain 20 s cycle, not timed to expiry, so the lease sat lapsed for
about 10 s before the read that saw 0 (12 s left at 418.1 s, 0 at 438.4 s,
renewed at 440.3 s). During that, the state byte and the first count read 0,
and power packets kept flowing at their usual rate: a short lapse didn't stop
telemetry. A renewal (sub 0x2D) is answered with a 0x2F lease reading, not
0x2E. What happens if nobody renews it is the question phase 3 answers.
The 900 s lease is not the rapid-shutdown path: cutting CCA power also cuts
the TAP's, which silences the mesh at once. It is the "master is alive"
check. Per the analysis session (`docs/ROOF_STEADY_STATE.md`), the bench opcodes
(netrun, the 37 24 92 66 key) belong to initial gateway setup, not daily
running. The opcode table above came from a bench TAP and the analysis notes;
the roof gateway's tunnel supersedes it here.

### The rest of the CCA's schedule (24 h passive capture, 2026-10-26/27)

Every request the CCA sent in 24 h, by count: 0x0148 polls (7.7 M), tunnel
0x2E (4,223), 0x41 (2,265), 0x06 (1,761), 0x26 (970), 0x2D (97), 0x22 (72),
0x0B00 (41), 0x0D (32), and the bring-up frames (16 bring-ups: four CCA
reboots and twelve overnight watchdog cuts). What each is, and what the
master now does about it:

- **0x41**: three streams, once a minute each, a new target every
  10 minutes. Its 22 content bytes are the same for every target, and the
  same on another site (taptap issue #20's capture); taptap's protocol.md
  shows an older firmware's version, which differs in two 16-bit values. So
  they look like a firmware constant: built in, and a learned copy wins.
  Targets: random over the optimizers heard (the CCA favours 0x0001 and the
  four undersized optimizers on streams 01/02 for reasons unknown).
- **0x0B00 01**: hourly. The overnight watchdog is the same tick: with no
  data for an hour, the CCA cuts the TAP's power right after it. 00 then 01
  only after a CCA reboot.
- **0x22**, empty, to node 0000 then 0001, a second apart, a second after
  each 0x0B00 01 and in each bring-up. taptap: an older CCA used 0x22 *with
  data* for "PV off", and sent it empty when PV off was not asserted. Only
  the empty form is ever sent; with data, never.
- **0x26**: the node table, twice over, every ~15 minutes and after each
  bring-up; each page starts at the last reply's start + count, ending on
  an empty page. The replies are the Frame 27s the Monitor turns into
  barcodes.
- **Bring-up**, after re-addressing: SET_ID_BY_MAC to 1202, a read there, a
  RESET broadcast, ~5 s silence, back at 1201; then VERSION, 0x0E02,
  [0x0B00 00], 0x0D 00 00, 0x0B00 01, 0x0D 00 01, the 0x22 pair, the walks,
  the lease renewal. The 0x0D reply holds 16 random-looking bytes (probably
  the network key): never logged or committed.
- **0x06**: ASCII queries to optimizers, answered later as type 0x07
  packets in 0x0149 (668 answers). Reads: ^00Version, ^00Tests, ^00Info,
  ^00Smrt, ^00Mppt, repeated 3, 5, 7… up to 30 minutes apart until answered.
  All five to every node after a CCA reboot; ^00Tests to each at sunrise.
  Two writes, each only after a particular answer: #00w255 after an Info
  whose ninth word is 00 (50 of 50), ^00Smrt_........S0000...0000 after a
  Smrt in mode N (30 of 30; the two H9.0018 optimizers). What the fields
  mean is not known.

Not replicated: the watchdog power cut itself, which needs a switch on the
TAP's supply.

### What a spare gateway showed (analysis repo, LIVE_TAP_SESSION.md "Second unit")

An older spare, from the roof's G5 generation (24 V only), standalone with no
optimizers:
- **SET_NETRUN 0x0058 latches.** Stop (`37 24 92 66 00 01 00 00`) halts the
  beacon, and neither a start argument nor re-entering running mode brings it
  back; only RESET 0x0010 plus reconfiguring does. The replacement never sends
  it, since the allow-list blocks it as a write.
- **The 0x0B0F tunnel and 0x0B00 session are refused (0x0006)** without a mesh
  behind the gateway, even configured like the roof's. The lease can only be
  tested on the roof.
- Running mode is SET_CHANNEL, SET_PAN and SET_GATEWAY_MODE 0x5030; power-on
  is 0x5070. The TAP stores these (STORE), so the roof TAP should come back
  configured after a power cut. (The roof capture contradicts this for the
  RS485 id; see above.)
- **Configured is not beaconing on newer gateways.** The G5.26 beacons as soon
  as it's in running mode; the newer H1.0006 (GW-H158, the roof's model line)
  shows no beacon from the mode alone. And lmudcd's netrun start is timed to
  the beacon clock, not a replayable argument. So after a rapid-shutdown power
  cut, the roof TAP may need a start the master doesn't send. The CCA
  cold-start capture (plan Task 10) decides this before any takeover.
  GET_GATEWAY_STAT 0x0300 is the run-state observable (H1.0006: 0xA000
  running, 0x9000 netrun-stopped; G5.26: 0xB000 running); the LED isn't, on
  newer units.

## How it will be built (decided)

- **Hardware:** a Pi (the Pi build's target) with an auto-direction RS485
  interface, sharing ground with the TAP, on the power circuit the CCA uses
  today, so the existing shutdown procedure cuts it. The Pi side has to power
  the TAP as the CCA's cable does.
- **Software:** inside tigomonitord, a master mode beside the existing passive
  mode. The frame decoding in `tigo_protocol.h` is reused. Transmit and
  framing go in a new shared header so both builds could use it later.
- **Phases, each gated on the one before:**
  1. **Bench, spare TAP, no array.** Get replies: version, channel, roster.
     Build and check the keep-running command sequence. Nothing reaches the
     roof.
  2. **Record the real CCA.** Two passive captures on the roof bus, made
     with the existing `--capture`:
     - at least 15 minutes of steady state, which covers one AdvSync and
       gives the netrun and StartPWM bytes;
     - a full start-up: CCA power on, through to producing.

     These are the reference for what the CCA sends and how often; we copy
     them rather than guess. The passive decoder needs a classifier for
     the master's request frames, not just the TAP's replies.
  3. **Roof takeover, daylight, owner present.** CCA unplugged, the Pi powers
     the TAP and replays the start-up sequence. Pass: the array produces
     within the CCA's normal start time, per-panel readings flow, and pulling
     the Pi's power drops the array to safe mode. Rollback: CCA back in.
  4. **Commissioning without a CCA** (goal 4), starting from the wake wall.
- **Safety rules, enforced in code:**
  - The roof build sends only commands on an allow-list: reads plus the
    heartbeat/netrun command proven in phases 1 and 3.
  - STORE, RESET, SET_485_ID, SET_ID_BY_MAC, CLEAR_ADT and SET_LIST_ENTRIES
    are blocked unless a bench-only flag is set.
  - The heartbeat comes from the main loop and stops if it stalls. A watchdog
    must not keep it going on its own.

## Calls made during design (not the owner's decisions; check these)

- **Phase 1 starts with the Waveshare RS485 CAN HAT already on the test Pi.**
  It switches direction by itself, which is what openTAPtoX needed. Fall back
  to a USB auto-direction module if the TAP stays silent.
- **The heartbeat is goal 2 whatever open question 2 finds.** The owner kept
  it in scope even though the FlexBoss carries an RSS transmitter.

## Left to the builder

- The RS485 interface for the Pi (a USB auto-direction adapter or a HAT that
  transmits), and how the Pi powers the TAP. This needs the TAP's supply
  voltage and current, measured on the CCA cable.
- How master mode surfaces in the web UI (a mode switch under Tools, status on
  Diagnostics).
- Whether the master polls GET_NEW_BINARY_DATA for readings or the TAP's
  responses can feed the existing decoder unchanged.

## Open questions

1. **How long does the array keep running if the master goes quiet?** The
   TAP beacons on its own clock once netrun is on, but the master
   re-asserts netrun (AdvSync, about every 12 minutes) and StartPWM, and
   the drop-out window lives in TAP and TS4 firmware. Measure it in phase 3.
   The design assumes the master must keep the loop going.
2a. **What the 0x2d fields mean** (02, the trailing 01 00), what stops the
   run, and what 0x41 sets. The capture gives the bytes; the binaries should
   give the meaning. Phase 3 can replay the captured renewal as-is.
2. **Does the FlexBoss RSS transmitter also gate the optimizers?** If the
   optimizers need it as well as (or instead of) the TAP's signal, goal 2's
   behaviour changes. A daytime capture with the CCA unplugged answers it.
3. **The TAP's power:** voltage and current over the CCA cable. No figure
   exists in the binaries or captures. The roof gateway is a GW-H158 (G5.27).
   The analysis repo's newer GW-H158 bench unit takes 5-26 V and its older
   GW-H150 takes 24 V only; the roof unit's range is unconfirmed. Measure the
   CCA's output on the cable pair, and the TAP's current in series (steady
   and peak, since transmit bursts draw more), or read the label, before
   sizing the Pi-side supply.
4. **The GET_NEW_BINARY_DATA layout** for real optimizers. The bench TAP had
   none; phase 3 data will pin it, compared against passive readings.
