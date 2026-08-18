# Board Configuration Files

This directory contains optimized ESPHome configurations for different ESP32 boards used with the Tigo Monitor system.

**PSRAM is required for the web server (`tigo_server`)**, which builds whole HTML
pages and JSON API responses in memory. It is *not* required by `tigo_monitor` —
the device and node tables are a few KB and sit happily on the internal heap. A
board without PSRAM can therefore run a sensors-only build that feeds Home
Assistant over the native API; it just cannot serve the dashboard.

## Available Configurations

### ESP32-S3 Boards

#### `esp32s3-atoms3r.yaml` - M5Stack AtomS3R (8MB PSRAM)
- **Board**: M5Stack AtomS3R (same as AtomS3 with different model)
- **PSRAM**: 8MB (Octal)
- **CPU**: 240MHz
- **Recommended for**: Standard installations (up to ~30 devices)
- **UART Buffers**: 2048 RX / 512 TX (increase RX to 8192 if using display package)
- **Note**: When using `atoms3r-display.yaml` package, display updates compete with UART. See UART_OPTIMIZATION.md for packet loss mitigation. TX buffer can stay small since we only listen to the bus.

#### `esp32s3-lilygo-t-connect-pro-lite.yaml` - LilyGO T-Connect Pro Lite (8MB PSRAM, wired)
- **Board**: LilyGO T-Connect Pro Lite (ESP32-S3), 8MB flash, 8MB octal PSRAM
- **Network**: wired Ethernet via an on-board W5500 — **no `wifi:` block**. Do not
  add one; the radio would compete for the same lwIP socket budget for nothing
- **CPU**: 240MHz
- **RS485**: built-in transceiver on GPIO17 (TX) / GPIO18 (RX), on its own
  hardware UART, so the USB serial console stays available
- **Recommended for**: installations that want the full web UI on a wired
  network. PSRAM and 8MB of flash mean `tigo_server` and on-flash history both fit
- **Bluetooth**: compiled out to buy back flash, so the CCA-over-BLE bridge is
  not available. HTTP CCA import works normally. Re-enabling BLE means dropping
  the four `CONFIG_BT_*` lines *and* switching to `partitions/tigo-8mb-ble.csv`
- **Ready-to-flash example**: `example-t-connect-pro-lite.yaml`
- **Provenance**: contributed by @davidcoulson from a working install
  ([discussion #30](https://github.com/RAR/esphome-tigomonitor/discussions/30)).
  The pin map is the contributor's — no maintainer unit exists to check it
  against, though the config compiles clean here. The screen-equipped "Pro" is a
  different board and is not configured by this file
- **Other on-board hardware** (pins documented in the board file, nothing
  instantiated): RS232, CAN, four WS2812 LEDs, two buttons, a relay

#### `esp32s3-waveshare-rs485-can.yaml` - Waveshare ESP32-S3-RS485-CAN (8MB PSRAM, DIN rail)
- **Board**: Waveshare ESP32-S3-RS485-CAN, 16MB flash, 8MB octal PSRAM
- **CPU**: 240MHz
- **Power**: 7-36V DC wide input (or USB-C), DIN-rail mountable — it can run off
  the same supply as the CCA rather than a separate USB brick
- **RS485**: on-board *isolated* transceiver (SP3485EN) on GPIO17 (TX) /
  GPIO18 (RX), on its own hardware UART, so the USB serial console stays available
- **⚠ The transceiver enable on GPIO21 is not optional.** DE and /RE are tied to
  one net that floats at reset, so a config setting only `tx_pin`/`rx_pin` boots
  with the **receiver disabled** and reads zero bytes off the bus forever — no
  frames, no checksum errors, `Buffer: 0 bytes` ([issue #22](https://github.com/RAR/esphome-tigomonitor/issues/22)).
  The board file holds it LOW with a `switch:` entry. That also disables the
  driver, which is the same hardware read-only guarantee the wiring guide gets
  from strapping a discrete MAX485 — the ESP32 physically cannot transmit onto
  the Tigo bus
- **Recommended for**: DIN-rail / cabinet installs that want the full web UI.
  PSRAM and 16MB of flash mean `tigo_server`, the full 8MB history partition and
  BLE all fit — BLE needs no repartition here, unlike the 8MB boards
- **Ready-to-flash example**: `example-waveshare-rs485-can.yaml`
- **Provenance**: verified working by @Brooklyn18m in
  [issue #22](https://github.com/RAR/esphome-tigomonitor/issues/22); pin map from
  Waveshare's schematic plus that report. No maintainer unit exists to bench-test
  it against, though the config compiles clean here (40.4% of a 3MB slot)
- **Other on-board hardware** (not instantiated): CAN (TWAI) on GPIO15/GPIO16
- **Note**: if you have a variant with 8MB flash rather than 16MB, upload will
  refuse the image — switch to `flash_size: 8MB` and `partitions/tigo-8mb.csv`

### ESP32-P4 Boards

#### `esp32p4-evboard.yaml` - ESP32-P4 Evaluation Board (32MB PSRAM)
- **Board**: ESP32-P4 EVBoard
- **PSRAM**: 32MB (Octal)
- **CPU**: 400MHz (dual-core)
- **Recommended for**: Large installations (50+ devices)
- **UART Buffers**: 16384 RX / 1024 TX (listen-only, no transmission)
- **Special optimizations**: High-frequency FreeRTOS tick, tickless idle disabled

### ESP32 Boards (no PSRAM — sensors only)

#### `esp32-lilygo-t-can485.yaml` - LilyGO T-CAN485 (no PSRAM)
- **Board**: LilyGO T-CAN485 (`esp32dev`), 4MB flash
- **PSRAM**: none — the board wires GPIO16/17 to its RS485 front end, which is
  where a WROVER's PSRAM would live, so it cannot have any
- **CPU**: 240MHz (classic ESP32 defaults to 160MHz under ESP-IDF; raised here)
- **RS485**: built-in isolated transceiver on GPIO21 (RX) / GPIO22 (TX), with
  three enable lines (GPIO16 5V boost, GPIO17 `/RE`, GPIO19 `/SHDN`) that the
  board file drives high at boot
- **Recommended for**: Home-Assistant-only installations. Tested at 18 devices;
  40 should fit — each optimizer costs roughly 600 bytes across the device and
  node tables, so 40 is ~24KB of the ~130KB left free once WiFi is up. Those are
  many small heap allocations rather than one block, so on a board with no PSRAM
  the practical ceiling is fragmentation, not total bytes
- **First flash must be over USB**: the board file enables `sram1_as_iram`, which
  needs an ESP-IDF v5.1+ bootloader. A USB flash updates the bootloader; an OTA
  does not, so OTA-ing an existing device onto this config leaves it unable to
  boot
- **Excludes**: `tigo_server` (needs PSRAM), on-flash history (the smallest tsdb
  layout wants ~7MB — dual OTA slots plus a 3MB LittleFS partition — which 4MB
  cannot hold; Home Assistant keeps long-term history anyway), CCA/cloud import, BLE
- **Ready-to-flash example**: `example-t-can485.yaml`
- **⚠ Flash over USB the first time.** The config sets `sram1_as_iram` (+40KB of
  IRAM, which is what pays for `CONFIG_UART_ISR_IN_IRAM`), and that needs an
  ESP-IDF v5.1+ bootloader. A USB flash updates the bootloader automatically; an
  OTA does not, so an OTA-first device will fail to boot.
- **⚠ Assumes chip revision 3.1.** `minimum_chip_revision: '3.1'` drops the IDF's
  workarounds for older silicon. The bootloader hard-checks it — an older chip
  halts at boot. Your boot log prints `chip revision: v3.1`; lower the value in
  the board file if yours reports less.

Because there is no web UI on this board, panel discovery happens through the
**"Generate YAML Config"** button, which prints a paste-ready `sensor:` block to
the logs. `example-t-can485.yaml` documents that workflow.

## Usage

To use a board configuration, include it in your main YAML file:

```yaml
# In your tigo-monitor.yaml or similar
packages:
  board: !include boards/esp32s3-atoms3r.yaml

# Then add your WiFi, API, and component configurations
wifi:
  ssid: "YOUR_SSID"
  password: "YOUR_PASSWORD"

# ... rest of your config
```

Or copy the relevant sections directly into your main configuration file.

## GPIO Pin Compatibility

Most configurations assume the standard Tigo UART connection:
- **TX Pin**: GPIO1
- **RX Pin**: GPIO3
- **Baud Rate**: 38400

Adjust these in your main configuration if your hardware differs. Boards with a
built-in transceiver already set their own pins — the AtomS3R uses GPIO1/GPIO2,
and the T-CAN485 uses GPIO22/GPIO21 plus three transceiver enable lines.

## Performance Notes

- **ESP32-P4** offers the best performance with massive PSRAM and higher CPU frequency
- **ESP32-S3 with PSRAM** is the sweet spot for most installations
- **Boards without PSRAM** work for sensors-only builds that talk to Home Assistant over the native API. Do not add `tigo_server:` to one — it compiles, then fragments the internal heap to OOM under dashboard polling

## Customization

Feel free to copy and modify these configurations for your specific needs. Key parameters to adjust:
- `number_of_devices`: Maximum devices to track
- `update_interval`: How often to poll sensors (default: 30s)
- Buffer sizes: Increase if seeing missed packets, decrease to save memory
