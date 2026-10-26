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
4. **Later: commission without a CCA.** Wake a new array and enroll its
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
- **What survives a power cut:** STORE keeps the channel, RS485 id, device
  flags, gateway mode, TX power and PAN in the TAP's flash. Netrun and
  beaconing don't survive. So after every rapid-shutdown cut, the TAP comes
  back configured but silent until a master re-asserts netrun and StartPWM.
  That is the fail-safe property goal 2 relies on.
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
3. **The TAP's power:** voltage and current over the CCA cable.
4. **The GET_NEW_BINARY_DATA layout** for real optimizers. The bench TAP had
   none; phase 3 data will pin it, compared against passive readings.
