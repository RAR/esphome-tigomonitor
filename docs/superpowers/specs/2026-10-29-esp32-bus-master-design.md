# ESP32 as the TAP's bus master

Branch: `feature/cca-replacement`. Status: built, and compiles for the
Relay-1CH (`boards/example-waveshare-relay-1ch-standby.yaml`). It has not run
on hardware yet. See "As built" at the end. Builds on
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
  | **Waveshare ESP32-S3-Relay-1CH** | isolated; TX GPIO7, RX GPIO8, DE/RE on **GPIO21**, driven as the UART's RTS in the IDF RS485 half-duplex mode (Waveshare's "hardware flow") | no | 8 MB (ESP32-S3R8) | **The owner's board, and the target.** Its relay is left unwired (see below). It runs on 5 V only. The relay is GPIO47, with an RTC (PCF85063A) on I2C GPIO39/38 |

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

### The relay: not used

The board's relay stays unwired in the first version. It would let us
restart the TAP by cutting its power, as the CCA did, but the relay is NO-only:
wired straight into the TAP's feed, every ESP32 reboot would cut the TAP. Doing
it safely needs an external NC relay, and a miswired or stuck relay is the kind
of fault that silently stops the array. Nothing needs TAP restarts today. The
TAP resets on the 2026-10-28 captures were the CCA power-cycling it, and with
the TAP on the RSD supply it has not reset since. See Later.

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
     The PC runs `mock_tap.py` on that adapter. The first checks:
     - whether the board's transmit reaches the adapter at all, which tests
       RTS-driven DE on GPIO21;
     - that the last byte of each frame isn't clipped at 38400 baud;
     - that the board doesn't drive the bus while it resets (bad checksums
       on the adapter during a reboot).
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
  has RS485 in an industrial package).
  - It's an S3 with 8 MB of PSRAM, so `execute_from_psram` is on the table if
    the flash freeze is ever a problem.
  - A thin config (no web server, no history) keeps it from writing flash.
  - It has no CAN port; CAN is a later item.
  - The AtomS3R stays the test rig.
- **The relay is left unwired** (agreed with the owner, 2026-10-29): it
  isn't worth an external NC relay and a new way for the array to stop.
- **A separate component** rather than a role on `tigo_monitor`, to keep the
  read-only promise literally true.
- **The role and options are set in YAML, not switchable from HA.**
- **The 5 s liveness limit on the loop.** It's long next to ESPHome's ~16 ms
  loop, but short next to the 900 s lease.

## Open questions

1. ~~Which boards?~~ A Waveshare ESP32-S3-Relay-1CH (the owner, 2026-10-29).
2. ~~The GPIOs?~~ From the owner's config for the board: RS485 TX 7, RX 8,
   DE/RE 21, relay 47 (held off), RTC on I2C 39/38. Flash is 16 MB. The
   board ran the owner's Solar Assistant proxy (`yambms/sa-proxy.yaml`):
   RS485 on these pins at 115200 baud, with GPIO21 as the flow-control pin.
   So RTS-driven DE has worked on this board, though not yet at 38400 or
   with our framing. Still unknown:
   - whether GPIO21 has a pull-down. If DE floats high while the ESP32 is
     resetting, the board drives the bus during every boot and corrupts the
     active master's exchanges. The first bench step checks this: reboot
     the board on a bus the mock is running, and look for bad checksums.
3. What will feed the board's 5 V? If it is fed from the RSD-switched
   supply, the RSD cuts the board too, which is fine.

## Later

- **Restarting the TAP from the relay**, if the TAP ever needs it. The design:
  - The board's NO contact drives an external **normally-closed** relay in
    the TAP's feed, in series after the RSD cut. Idle, the TAP is powered, and
    an ESP32 reboot can't cut it.
  - On request only, and only from the active master.
  - It needs the bring-up proven on the roof first.

## As built (2026-10-29)

- **The core:** `components/tigo_master/master_core.{h,cpp}`, split out of
  the Pi's bus master with a `Transport` interface and a log callback. The
  Pi's `linux/src/bus_master.{h,cpp}` is now a serial-port transport and a
  thin wrapper. Every Linux e2e suite passes on it unchanged.
- **The ESP32 uses ESPHome's own `uart:`, not raw IDF calls.** On ESP-IDF it
  already runs the RS485 half-duplex mode when `flow_control_pin` is set. It
  has no loop or event task of its own, so the master task can be its only
  user.
- **tigo_monitor reads the bus through tigo_master,** which is also a
  `uart::UARTComponent`. Its read side is a lock-free ring (8 KB), filled by
  the master task and drained by tigo_monitor in the ESPHome loop, so
  tigo_monitor is unchanged. Writes through it are refused and logged.
  tigo_master's config carries the four 38400 8N1 keys that tigo_monitor's
  uart check reads.
- **The liveness gate is at the task:** the core isn't ticked at all while
  the ESPHome loop has been silent for 5 s, so a hung loop stops every
  request, not only the renewals.
- **The 0x41s heard while standing by are learned in RAM only.** After a
  reboot it uses the built-in content until it hears them again.
- **The config refuses `tigo_server` and an `esp_tsdb` framework
  component** (the flash writes).
- Compiled with ESPHome 2026.10.0: 908 KB flash, 122 KB static RAM. The new
  component builds with no warnings.

