# ESP32 as the TAP's bus master

Branch: `feature/cca-replacement`. Status: design, not started. Builds on
[the CCA replacement design](2026-10-26-cca-replacement-design.md), which kept
an ESP32 version out of scope "until the Pi version proves out". The Pi has now
run the roof alone since the CCA died (2026-10-29). Pairs with
[standby failover](2026-10-29-standby-failover-design.md).

## Why

- A second master for failover should be cheap, with no SD card and nothing to
  run apt on.
- An ESP32 has better timing than a Pi Zero for this job. On the Pi, every
  stall we chased came from Linux: the apt-daily refresh (1.1 s), SD fsync, and
  scheduling (fixed with Nice=-10 and real-time I/O, 7b41007). A FreeRTOS task
  with the UART interrupt in IRAM has none of those.
- The ESPHome users of this project already have the hardware.

## Goals

1. **An ESP32 runs the bus as the Pi does:**
   - the polls, the lease check and renewal;
   - optionally the housekeeping, the queries and the bring-up, behind the same
     settings, with the same safety rules.
2. **First as the standby** in the failover design; only later, once it has
   proven itself, as the primary.
3. **The fail-safe heartbeat holds:** a hung ESP32 stops renewing the lease.
4. **Listening-only configs stay listening-only, provably:**
   - the transmit code isn't compiled into them;
   - and the boards' "read-only rests on software" notes stay true for them.

## Non-goals

- History, the web UI or the CCA BLE bridge on the master device. The Pi
  already does that job.
- Classic ESP32 boards without auto-direction RS485 (see Hardware).

## Anti-goals

- **Any write path in `tigo_monitor`.** The LilyGO board file promises that
  "tigo_monitor contains no UART write calls at all". The master goes in a
  separate component, so that promise stays literally true for every existing
  config.
- **A flash stall during a request,** e.g. a stray preference write.
- Everything the parent design rules out still applies here:
  - weakened rapid shutdown;
  - stranding hardware;
  - the frames that must never be sent (STORE, CLEAR_ADT, SET_LIST_ENTRIES,
    SET_NETRUN, 0x22 with data).

## What we know

- **The protocol logic is already shared and header-only:**
  - `tigo_master.h` (Lease, LeaseKeeper, schedule) and `tigo_protocol.h`;
  - `tigo_node_identity.h`.

  All of these live in `components/tigo_monitor/`. What is Linux-only is
  `linux/src/bus_master.cpp`: termios, poll(), tcdrain() and the `transact()`
  loop around them.
- **Timing:** a ~50 ms poll timeout (openTAPtoX) and a 900 s lease. The roof
  tolerated ~2 s gaps on every Pi restart, so the real limit is jitter during a
  request, not overall uptime.
- **The flash stall is the danger.** On the classic ESP32 and the S3, a write
  to SPI flash disables the cache on both cores. Any code not in IRAM stops
  until the write finishes. The history writes in this project measured 0.63 s
  median and 1.02 s at worst (Saving History to Flash). A master device must
  not write flash while running: no history, and no periodic preference saves.
- **Hardware already in the repo:**

  | Board | RS485 | CAN | PSRAM | Fit |
  |---|---|---|---|---|
  | LilyGO T-CAN485 | MAX13487E, **auto-direction** | yes | none | **The thin standby.** Its UART TX is already wired; the driver turns itself on |
  | Waveshare ESP32-S3-RS485-CAN | SP3485, DE+/RE on GPIO21 | yes | 8 MB | Full build possible. Needs DE driven by the IDF RS485 half-duplex mode (RTS on GPIO21) |
  | M5Stack AtomS3R + Atomic RS485 Base | auto-direction (unverified) | no | 8 MB | The reference rig's pairing; TX is on GPIO6, so far unused |
  | **Waveshare ESP32-S3-Relay-1CH** | isolated; direction "by the main controller's hardware flow" (most likely the UART's RTS, IDF RS485 half-duplex mode) | no | 8 MB (ESP32-S3R8) | **The owner's board, and the target.** It adds a relay that can restart the TAP. It runs on 5 V only, and its RS485 and relay GPIOs are not on the wiki page: read them off the schematic |

  The relay is **1NO only**, rated 10 A at 30 V DC, with an RTC on the board
  ([Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Relay-1CH)).

## How it will be built (decided)

### A separate component, `tigo_master`

- It lives in `components/tigo_master/` and holds the ESP32 port of the bus
  master.
- It sets `tigo_monitor`'s frame input and owns the UART. Configs without
  `tigo_master:` compile no UART write.
- The YAML is roughly:

  ```yaml
  tigo_master:
    monitor_id: tigo_hub
    uart_id: tigo_uart
    role: standby          # standby | master (no listen: leave the component out)
    takeover_after: 8s     # standby only
    gateway_id: 0x1201
    housekeeping: false    # as master_housekeeping on the Pi
    queries: false
    startup: false
  ```

- There are no runtime switches from Home Assistant for the role or the
  options. Changing who runs the bus is a reflash, deliberately.
- Read-only status goes out as entities:
  - status (`standing_by`/`active`/fault);
  - a problem binary sensor;
  - lease remaining, takeovers, and gap max.

  These match the Pi's MQTT entities.

### One core, two transports

- Split `bus_master.cpp` into:
  - a transport-agnostic `BusMasterCore` (`master_core.h`, shared like
    `tigo_master.h`). It holds the schedule, `transact()`'s reply matching and
    foreign-request check, the faults, the counters and the standby state
    machine.
  - a small transport interface: `write(bytes)`, `drain()`, `read(timeout)`,
    `now_ms()`.
- Linux keeps termios behind that interface, and the ESP32 implements it with
  the IDF UART driver.
- This refactor lands on Linux first and goes through the existing
  `master-e2e.sh` unchanged, before any ESP32 code exists.

### Tasks and timing on the ESP32

- **The master task** is a FreeRTOS task:
  - pinned to the core away from Wi-Fi (core 1);
  - at a priority above ESPHome's loop task;
  - talking to the UART through the IDF driver directly (`uart_write_bytes`,
    `uart_wait_tx_done`, `uart_read_bytes`), not through ESPHome's polled UART
    component.
- **Bytes it reads** go into a ring buffer. The ESPHome loop drains it into
  `tigo_monitor`, so frame decoding and sensor publishing never run in the
  master task.
- **The heartbeat stays fail-safe.** The ESPHome loop stamps a liveness
  counter every iteration. Before every lease renewal, and every poll, the
  master task checks it, and stops transmitting if the loop has not run for 5
  s. A device whose main loop hangs then goes quiet, as the Pi does when its
  main loop stops. The task watchdog covers the master task itself.
- **No allocation** once the task is running: static buffers sized for the
  largest frame.
- **No flash writes while it runs:**
  - a configuration check refuses `tigo_server` history and `restore_value`
    globals alongside `tigo_master`;
  - the build sets `preferences: flash_write_interval` to never.
- **The gap and overrun counters** from the Pi port over: how long each
  request's write, drain and read ran past what the wire needs. These are
  the numbers that tell us whether the ESP32's timing is actually better.

### Restarting the TAP (the relay)

The CCA restarted the TAP by cutting its power whenever it couldn't hear it.
That's what the TAP resets in the 2026-10-28 captures were. The relay gives us
the same ability, under our control. A TAP that loses power comes back at the
factory address, so every restart depends on the bring-up (`startup`).
**The bring-up has not been run on the roof TAP yet.**

**Wiring (decided).** The board's relay contact is normally open. Putting the
TAP's feed straight through it would mean the TAP is only powered while the
relay is energized. Every reboot, OTA, crash or brown-out of the ESP32 would
then cut the TAP, including while the Pi is the active master. So the board's
contact drives an **external normally-closed relay** in the TAP's feed instead:
- idle, nothing energized, the TAP is powered;
- energizing cuts it.
The contact sits **in series after the RSD cut**. The RSD still removes the
TAP's power whatever the relay does, so rapid shutdown is unchanged.

**When it fires (decided for the first version):**
- **Only on request:** a `restart_tap` action, a button entity, held
  `restart_tap_off` (default 10 s) before power is restored.
- **Never automatically.** An automatic restart on `no_reply`, as the CCA did,
  waits until the bring-up has been proven on the roof. Even then it gets
  limits: at most once an hour, never at night, and only on the active master.
- **Only from the active master.** A standby restarting the TAP under the Pi
  would pull the bus out from under a healthy master. A request on a device
  that isn't the active master is refused and logged.
- After a restart, the device runs the bring-up as at boot.
- `/api/master` / the entities report `tap_restarts` and `last_tap_restart`,
  so the restarts are visible and can't be mistaken for TAP faults.

**First roof test.** Have the backup CCA on hand as the recovery path, since a
failed bring-up leaves the array without a master that can start it. Do it in
daylight, so that production coming back shows the bring-up worked.

### OTA

An OTA reboot stops the bus for a few seconds, the same as a Pi install. With
the failover design that becomes a handoff, so the master device is only
reflashed while the other one is standing by and healthy. The docs say so; the
firmware doesn't enforce it.

## Testing

1. **The refactor (Linux):** `master-e2e.sh` and `cca-e2e.sh` pass unchanged
   after `BusMasterCore` is split out.
2. **ESP32 against a mock, hardware in the loop:**
   - The test rig is the Relay-1CH wired to a USB RS485 adapter on the PC.
     The PC runs `mock_tap.py` on that adapter. The first check is whether the
     board's transmit reaches the adapter at all: the direction control, and
     that the last byte of each frame isn't clipped at 38400 baud.
   - The relay: `restart_tap` on a standby is refused; on the active master,
     it energizes for `restart_tap_off` and then releases. Measure it with a
     meter on the external NC relay's contact, with no TAP attached.
   - A reboot or OTA of the board leaves the NC contact closed (meter).
   - Check that it is taking the role, that its polls are answered, that the
     lease is renewed when the mock reads 0, and the gap/overrun counters.
   - Then force flash writes (an OTA upload aborted halfway) and Wi-Fi
     reconnect storms, and record what the counters show.
   - A transmit count of zero while standing by.
3. **The roof, as standby, behind the Pi master,** following the failover
   design's roof steps:
   - it stands by;
   - it takes over when the Pi stops;
   - it yields when the Pi comes back as master.

   Leave it as standby for a week, comparing panel data between the two.
4. Only then consider it as the primary.

## Calls I made (check these)

- **The Waveshare ESP32-S3-Relay-1CH is the target** (the owner's choice; it
  has RS485 and a relay to restart the TAP).
  - It's an S3 with 8 MB of PSRAM, so `execute_from_psram` is on the table if
    the flash freeze is ever a problem.
  - A thin config (no web server, no history) keeps it from writing flash.
  - It has no CAN port; CAN is a later item.
  - The AtomS3R stays the test rig.
- **The external NC relay.** Without it, the board's NO-only relay makes the
  TAP depend on the ESP32 staying up.
- **The relay fires only on request** at first, and only on the active
  master.
- **A separate component** rather than a role on `tigo_monitor`, to keep the
  read-only promise literally true.
- **The role and options are set in YAML, not switchable from HA.**
- **The 5 s liveness limit on the loop.** It's long next to ESPHome's ~16 ms
  loop, but short next to the 900 s lease.

## Open questions

1. ~~Which boards?~~ A Waveshare ESP32-S3-Relay-1CH (the owner, 2026-10-29).
2. Its RS485 TX/RX, direction and relay GPIOs, and its flash size, aren't on
   Waveshare's wiki page. Read them off the schematic or the board itself
   before writing the board file.
3. What voltage is the TAP fed from the RSD supply? The external NC relay's
   contacts must be rated for it. And what will feed the board's 5 V? If it
   is fed from the RSD-switched supply, the RSD cuts the board too, which is
   fine.
4. Should the restart button be exposed in Home Assistant at all, or only on
   the device (its BOOT button, or a long press)? A one-tap TAP restart from a
   dashboard is easy to press by mistake.
