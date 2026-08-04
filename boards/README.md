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
- **Recommended for**: Home-Assistant-only installations (~20 devices, 40 is
  still comfortable)
- **Excludes**: `tigo_server` (needs PSRAM), on-flash history (the smallest tsdb
  layout wants ~7MB — dual OTA slots plus a 3MB LittleFS partition — which 4MB
  cannot hold; Home Assistant keeps long-term history anyway), CCA/cloud import, BLE
- **Ready-to-flash example**: `example-t-can485.yaml`

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
