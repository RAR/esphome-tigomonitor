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
  | **M5Stack AtomS3R** + Atomic RS485 Base | auto-direction (unverified) | no | 8 MB | **The owner's board, so the first target.** It's the reference rig's pairing; TX is on GPIO6, so far unused |

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

### OTA

An OTA reboot stops the bus for a few seconds, the same as a Pi install. With
the failover design that becomes a handoff, so the master device is only
reflashed while the other one is standing by and healthy. The docs say so; the
firmware doesn't enforce it.

## Testing

1. **The refactor (Linux):** `master-e2e.sh` and `cca-e2e.sh` pass unchanged
   after `BusMasterCore` is split out.
2. **ESP32 against a mock, hardware in the loop:**
   - The test rig is the AtomS3R and RS485 Base wired to a USB RS485 adapter
     on the PC. The PC runs `mock_tap.py` on that adapter. The first check is
     whether the Base's transmit reaches the adapter at all (auto-direction).
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

- **The AtomS3R is the first target** because the owner has one. It's an S3
  with 8 MB of PSRAM, so `execute_from_psram` is on the table if the flash
  freeze is ever a problem. It has no CAN port, but CAN is a later item. A
  thin config (no web server, no history) keeps it from writing flash. The
  T-CAN485 (auto-direction, CAN, no PSRAM) is the fallback if the RS485 Base
  turns out not to switch direction by itself.
- **A separate component** rather than a role on `tigo_monitor`, to keep the
  read-only promise literally true.
- **The role and options are set in YAML, not switchable from HA.**
- **The 5 s liveness limit on the loop.** It's long next to ESPHome's ~16 ms
  loop, but short next to the 900 s lease.

## Open questions

1. ~~Which boards?~~ An M5Stack AtomS3R (the owner, 2026-10-29). Is it the
   reference rig that runs the full firmware with history, or a spare? The
   master device should run a thin config, so it can't be the rig as it is
   now.
2. Does the Atomic RS485 Base turn its driver on by itself? It's unverified,
   and the first bench step answers it.
3. Does the Waveshare S3's SP3485 work with the IDF's RTS-timed DE, at 38400
   baud, without clipping the last byte? It only matters if the full build is
   wanted on the master device.
