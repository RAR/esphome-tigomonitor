# Raspberry Pi Zero 2 W build — design

## Why

More RAM, easier to live with, and for fun. The ESP32 build spends real effort
on memory (PSRAM allocators, fragmentation rules, TLS heap tuning); a Linux
box with 512 MB makes that whole class of work disappear. Ease of use applies
to two people: the maintainer (SSH, gdb, seconds-long rebuilds, real log files)
and a newcomer, who should be able to **flash an SD card, boot, and open
`http://tigo.local`** with nothing else to install.

## Goals

- A Linux daemon, `tigomonitord`, that listens to the Tigo RS485 bus through a
  USB-RS485 dongle (or a UART HAT on `/dev/serial0`) and:
  - publishes per-panel and system sensors to Home Assistant via MQTT discovery;
  - serves the existing web UI (`components/tigo_server/web/app.html`) by
    implementing the same `/api/*` JSON shapes.
- A ready-to-flash Raspberry Pi OS Lite (arm64) image with the daemon
  preinstalled and enabled. Wi-Fi, hostname and SSH are set in Raspberry Pi
  Imager's customisation screen; the MQTT broker is set in the web UI.
- One protocol implementation shared by both builds, so a decoding fix lands
  once.

## Non-goals (for v1)

- Feature parity with the ESP32 build. Not in v1: BLE CCA source, Tigo cloud
  sync, the time-series history pages, the YAML generator, the ESPHome native
  API.
- Transmitting on the bus.
- Supporting the original single-core Pi Zero (ARMv6). The image is arm64; the
  .deb could be built for armhf later if anyone asks.

## Anti-goals

- Destabilising the ESP32 build. Shared code is extracted behaviour-for-behaviour
  and the ESP32 build is compile-checked and rig-verified after the extraction.
- An image that needs a terminal to finish setup.

## Shape

```
components/tigo_monitor/tigo_protocol.h   header-only, no ESPHome/IDF includes:
                                          unescape, CRC16, frame classification,
                                          power-packet decode, Frame 27 decode
components/tigo_monitor/tigo_node_identity.h   (already shared, #74)
linux/                                    the daemon (CMake, C++17)
  src/serial      termios reader → frame splitter → tigo_protocol
  src/state       device + node table, persisted as JSON under /var/lib/tigomonitor
  src/mqtt        libmosquitto, HA discovery
  src/http        cpp-httplib, serves app.html + /api/*
  debian/         .deb packaging, systemd unit, avahi service (tigo.local)
  image/          pi-gen stage that installs the .deb
tests/host/protocol_test.cpp              host tests for tigo_protocol.h
```

Dependencies are Debian 13 packages only: `libmosquitto-dev`,
`libcpp-httplib-dev`, `nlohmann-json3-dev`.

## Milestones

1. **Shared protocol header.** Move the pure decode functions out of
   `tigo_monitor.cpp` into `tigo_protocol.h`; the ESP32 component calls them.
   Host tests with synthesised frames (valid CRCs, escapes, both power-frame
   formats, Frame 27). ESP32 build compiles; rig flash confirms identical
   panel readings.
2. **Daemon MVP.** Serial in, decoded panels out to the log and to MQTT with
   HA discovery. Testable without hardware by replaying a byte stream through a
   pty.
3. **Web UI.** Serve `app.html` and the subset of `/api/*` its dashboard and
   node-table pages need; settings page for the MQTT broker.
4. **Packaging + image.** .deb with systemd unit; CI on GitHub's arm64 runner
   builds the .deb and a pi-gen image, attached to releases.

## Builds

The dev machine has no CMake or the libraries, and nothing is installed on it:
all builds run in a `debian:trixie` container. CI uses `ubuntu-24.04-arm` so
the arm64 binary and the image are built natively, without QEMU.

## Open

- Hardware: a Pi Zero 2 W and a USB-RS485 dongle on the rig's bus (a second
  passive listener next to the ESP32 is fine on RS485) are needed to verify
  milestone 2 onward.
- No raw bus capture exists yet. Milestone 2 adds a `--capture` flag; the first
  real capture becomes a test fixture (with serials and barcodes scrubbed).
