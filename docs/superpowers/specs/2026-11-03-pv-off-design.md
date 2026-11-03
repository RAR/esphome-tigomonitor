# PV off: turning the array's output off and on from the monitor

Branch: `feature/cca-replacement`. Status: design, not built. Builds on
[the CCA replacement design](2026-10-26-cca-replacement-design.md) and
[standby failover](2026-10-29-standby-failover-design.md).

## Why

The CCA had a PV-safe button: it told the TAP to turn the optimizers' output
off, and on again later. With the CCA gone the monitor runs the bus, so the
owner wants the same switch: stop production without walking to the RSD,
for example while working near the inverter or to test something, and
bring it back from the same page.

## The frame (what we know)

From the 4.0.3 CCA's `meshdcd`, class `CUpdaterPvOff`, read statically
(the analysis note "CUpdaterPvOff — frame layout and value meanings",
2026-11-03; nothing was sent on a bus):

- The PV switch is **command 0x0B00 with one payload byte: `00` = off,
  anything else = on** (the CCA sends `01`). Zero means off, the reverse
  of the usual convention, so the code names the two values and never
  writes a bare byte.
- The CCA sends it **ahead of its normal queue** ("Overriding queue,
  sending PV-OFF command instead").
- It **checks the reply's value**, not just that a reply came
  ("PV-OFF response … did not match what we wanted it to be").

From our own 24 h capture (2026-10-26/27), the roof CCA sent:

- `0x0B00 01` every hour: PV on, re-asserted;
- `0x0B00 00` then `01` once after each of its own reboots: off, then on.

We copied both as "session" frames (`CMD_SESSION`) before we knew what
they were, so **the Zero has been sending PV on every hour since it took
the bus.** The mock answers 0x0B00 with 0x0B01 carrying the same byte;
check that against the capture before relying on it.

Still unproven:

- that the roof TAP acts on `00` at all, and how fast the panels drop;
- whether the TAP keeps PV off with no master re-asserting it;
- whether it refuses `00` outside some state (an older G5 bench TAP
  answered 0x0B00 with 0x0006 "not supported").

## Goals

1. **Turn PV off and on** from the web UI and the API, confirmed by the
   TAP's reply.
2. **Off stays off** until someone turns it on: across the hourly
   re-assert, a daemon restart or install, a TAP power cycle and a standby
   takeover.
3. **You can always see it:** every page says so while PV is off, and the
   state, who set it and when are on `/api/master` and MQTT.

## Non-goals

- Per-node or per-string off. The frame carries no node.
- Schedules, export limits or anything that sets a level.
- Rapid shutdown. See the anti-goals.

## Anti-goals

- **Any role in rapid shutdown.** The RSD cuts the TAP's power directly,
  outside the Pi (owner-confirmed, 2026-10-29). PV off is a convenience:
  a crashed Pi, a lost reply or a TAP that forgets could all bring PV back.
  Nobody works on the array relying on it, and the UI says so. Nothing in
  this design sends anything that could stand in the way of the RSD: a
  TAP without power hears nothing.
- **A PV on that happens by accident.** Restarts, installs, takeovers and
  bring-ups must never send `01` while off is held.
- **A new frame.** Only `0x0B00 00` and `0x0B00 01` are sent, both of
  which the CCA has sent on this bus.

## How it works

### The state

`pv_off`: false (default) or true. The owner sets it. On the Pi it's kept
in `settings.json`; on the ESP32 in flash (an ESPHome preference).

### What the master sends

A small `PvSwitch` in `tigo_master.h`, beside `Housekeeping`, owns every
0x0B00. Housekeeping stops building 0x0B00 itself and asks it instead.

| When | PV on (default) | PV off held |
|---|---|---|
| The setting changes | `01` at once | `00` at once |
| Every hour | `01` (today's frame) | `00` |
| Bring-up after re-addressing a TAP | `00`, then `01` (today's) | `00` only |
| Start or takeover of a TAP already running | `01` with the first housekeeping, as today | `00` first |

- "At once" means the next request slot, ahead of polls, as the CCA's
  queue override does.
- The hourly frame and the bring-up are sent with housekeeping off too
  while PV off is held: off must not depend on another setting.
- **The reply must carry the byte that was sent.** If it doesn't, or
  nothing answers, it retries every 5 s, up to 5 times, then raises a new
  fault, `pv_unconfirmed` ("asked the TAP for PV off; it didn't
  confirm"), and keeps re-asserting on the hourly tick. A matching reply
  clears it.

### The guard

Today `classify_request` allows `0x0B00 00` only in a bring-up. It becomes
a new `Allow::pv_off`: sent only while off is held (the bring-up's `00`
keeps its own startup permission). `01` stays allowed. Any other payload
stays bench-only, and nothing else changes on the never-send list.

### A standby keeps it

A standby hears every 0x0B00 the active master sends: the last one heard
is the bus's PV state. It saves that state, Pi state dir or ESP32 flash,
like the learned 0x41s, and a takeover carries it on. So a master that
dies while off is held is replaced by one that keeps it off. This also
works with a CCA as the active master.

While a device is standing by, its switch is read-only: it shows the state
it heard and says to change it on the device that's running the bus. (The
coordination design could later forward the request; not here.)

**Ruling (mine):** a heard state wins over the device's own setting when
it takes over. The bus is what the TAP was last told; a stale setting on a
standby that was off for a week isn't.

### Who can switch it

- **Web UI: a new Bus master page.** It appears only when the device
  runs the bus or stands by for it (as the CCA page hides itself), and
  takes over the master card now on Diagnostics: role and standby state,
  lease, takeovers, faults, housekeeping counts. The "PV output" control
  sits at the top, with a confirm step for off ("Turn the array's output
  off? This is not rapid shutdown."). On a standby that's standing by it
  is read-only.
- **Sidebar:** a "PV on / PV off" pill, always visible, red while off,
  linking to the Bus master page. It shows the state; it doesn't switch
  it, so a misclick while reaching for a page link can't stop the array.
  While off is held, every page also shows a banner: "PV output is off,
  turned off from <device> at <time>", with a link to the control.
- **API:** `POST /api/master/pv` `{"on": false}`. The Pi refuses it (403)
  while no web password is set: anyone on the LAN could otherwise turn the
  array off. The existing cross-site refusal applies.
- **MQTT / Home Assistant:** not in the first version. Later, an opt-in
  `mqtt.pv_switch` setting adds a switch entity.
- **ESP32:** a `pv_output` switch in the `tigo_master` YAML (optional),
  restored from flash, which Home Assistant gets through the native API.

### What you see

`/api/master` and the MQTT master state gain `pv`:

```json
{"on": false, "source": "setting", "since": "2026-11-03T15:02:11Z",
 "confirmed": true, "last_sent": "00", "last_reply": "00"}
```

`source` is `setting` (this device's own) or `heard` (learned from the
bus). `/api/health` gains `pv_unconfirmed` as a problem. Every change is
logged at warning level.

## Testing

**Host tests** (`master_protocol_test.cpp`):

- the guard: `00` allowed only while held; other payloads bench-only;
- the table above, row by row, including no `01` anywhere while held;
- the reply check: a mismatch and a timeout retry, then the fault, and a
  matching reply clears it;
- a standby's heard state survives a reload and is used at takeover.

**Against the mock** (`master-e2e.sh`, `standby-e2e.sh`): the mock keeps a
PV state from the 0x0B00s it gets and answers with it.

- Off from the API: `00` within a second, confirmed; the hourly tick
  (shortened for the test) sends `00`.
- Restart the daemon while held: it sends `00` first and never `01`.
- A mock TAP power cut while held: the bring-up sends `00` and no `01`.
- A mock that answers the wrong byte: the fault appears.
- Two masters: the active one holds off; kill it; the standby takes over
  and sends `00`.
- No web password: the API answers 403.

**On the roof, in daylight, with the owner present:**

1. Turn PV off; time how long until per-panel power reads ~0, and watch
   the inverter.
2. Leave it 10 minutes; check it stays off between hourly re-asserts.
3. Turn PV on; time the recovery.
4. Restart the daemon while off; it must stay off.

Only after that does the switch show up for anyone but the owner.

## Open questions (the owner's)

1. ~~Where should the control live?~~ A new Bus master page, with a
   state-only pill in the sidebar (owner leaning, 2026-11-03; the pill
   that doesn't toggle is my call).
2. After an RSD release (the TAP powers back up), should held-off stay off
   (as above) or should PV come back on?
3. Home Assistant switch in the first version, or later as above?
4. Is a confirm step enough for off, or should turning it off need the web
   password typed again?
