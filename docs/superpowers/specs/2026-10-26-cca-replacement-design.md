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
  2. **Record the real CCA.** A full start-up on the roof bus (power on,
     wake, steady state) captured passively with the existing `--capture`. This
     is the reference for what the CCA sends and how often; we copy it rather
     than guess.
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

1. **Where does the heartbeat come from?** Does the TAP keep the optimizers
   running on its own once in netrun, or does it need periodic traffic from
   the master? Is there a timeout? Asked of the protocol-analysis session
   (lmudcd/meshdcd). Phase 2's capture answers it empirically.
2. **Does the FlexBoss RSS transmitter also gate the optimizers?** If the
   optimizers need it as well as (or instead of) the TAP's signal, goal 2's
   behaviour changes. A daytime capture with the CCA unplugged answers it.
3. **The TAP's power:** voltage and current over the CCA cable.
4. **The GET_NEW_BINARY_DATA layout** for real optimizers. The bench TAP had
   none; phase 3 data will pin it, compared against passive readings.
