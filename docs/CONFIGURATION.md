# Configuration Guide

Complete YAML reference for the ESPHome Tigo Monitor component — every option on the `tigo_monitor` and `tigo_server` platforms, the sensors they expose, and the ESP-IDF/PSRAM settings the firmware needs. New here? Start with the [Quick Start in the README](../README.md#quick-start), then come back for the details.

## Contents

- [Tigo Monitor component](#tigo-monitor-component) — core options, inverter grouping, calibration
- [Tigo Web Server component](#tigo-web-server-component) — web UI/API, CCA over Bluetooth, cloud import, auth
- [Sensors](#sensors) — system totals, per-device, HA sub-device grouping
- [Management buttons](#management-buttons)
- [Efficiency metrics](#efficiency-metrics)
- [ESP-IDF framework &amp; PSRAM](#esp-idf-framework) — including [large-install internal-RAM tuning](#large-installs--internal-ram)
- [On-flash history (esp_tsdb)](#on-flash-history-esp_tsdb)
- [Filtering and smoothing](#filtering-and-smoothing)

## Tigo Monitor Component

```yaml
tigo_monitor:
  id: tigo_hub
  uart_id: tigo_uart
  update_interval: 30s
  number_of_devices: 20
  cca_ip: "192.168.1.100"
  sync_cca_on_startup: true
  time_id: ha_time
  reset_at_midnight: true
  power_calibration: 1.0
  night_mode_timeout: 60
  inverters:
    - name: "Inverter 1"
      mppts: ["MPPT 1", "MPPT 2"]
```

### Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `uart_id` | ID | Required | UART component ID |
| `update_interval` | Time | 60s | Sensor publish interval |
| `number_of_devices` | Integer | 5 | Max devices to track |
| `cca_ip` | String | None | Tigo CCA IP address |
| `sync_cca_on_startup` | Boolean | true | Auto-sync CCA on boot |
| `time_id` | ID | None | Time component for midnight reset |
| `reset_at_midnight` | Boolean | false | Reset daily totals at midnight |
| `power_calibration` | Float | 1.0 | Power multiplier (0.5-2.0) |
| `night_mode_timeout` | Integer | 60 | Minutes before night mode (1-1440) |
| `stale_timeout` | Integer | 10 | Minutes without data before a device's production values (power, current, efficiency, duty cycle) zero out. `0` disables. Voltage/temperature keep their last reading for diagnostics |
| `inverters` | List | None | Inverter grouping config |

### Inverter Grouping

Group MPPTs by inverter for organized dashboard display:

```yaml
inverters:
  - name: "South Inverter"
    mppts:
      - "MPPT 1"
      - "MPPT 2"
  - name: "North Inverter"
    mppts:
      - "MPPT 3"
      - "MPPT 4"
```

MPPT labels must match CCA labels exactly. The web dashboard shows hierarchy: Inverter → MPPT → String → Panel.

#### Renaming from the UI

Inverter and string display names are editable from the Topology view (✎ next to each label). Overrides are persisted to NVS, keyed by canonical YAML/CCA name. The YAML-defined `name:` is still the immutable identity used everywhere internally; the override only affects display. Empty override = falls back to canonical name. Useful when you want friendlier names ("South Roof") without redeploying YAML.

#### Per-string panel nameplate (rating)

Click the rating pill in the Topology view to set the per-panel nameplate watts for a string (uint16, 0 = unset). Persisted to NVS. When set:

- Panel tiles show "% of rated" alongside watts.
- Health classification uses rating-vs-power instead of median-vs-peer (with a "string sleeping" check at <5% of total nameplate so dawn doesn't paint everything red).
- String aggregate roll-up shows output as % of total nameplate.

Falls back to median-based behavior when unset.

### Midnight Reset

Reset peak power and energy daily:

```yaml
time:
  - platform: homeassistant
    id: ha_time

tigo_monitor:
  id: tigo_hub
  uart_id: tigo_uart
  time_id: ha_time
  reset_at_midnight: true
```

### Power Calibration

Adjust if readings differ from inverter:

```yaml
tigo_monitor:
  power_calibration: 1.05  # +5% to all power readings
```

Applied to: individual device power, string aggregates, total system power, energy calculations.

> **Tip:** `power_calibration` (and `night_mode_timeout`, `reset_at_midnight`, `sync_cca_on_startup`, `cca_ip`) can also be edited live in the web UI under **Tools → Device Configuration** and stored on-device, so you can tune them without reflashing. See [On-device configuration](#on-device-configuration-tools--device-configuration).

---

## Tigo Web Server Component

```yaml
tigo_server:
  tigo_monitor_id: tigo_hub
  port: 80
  api_token: "your-secret-token"
  web_username: "admin"
  web_password: "secure-password"
  # Optional: read the CCA over Bluetooth (firmware 4.0.4+ locks local HTTP)
  cca_source: ble            # http (default) | ble | auto
  ble_client_id: tigo_cca_ble
  # Optional: recover the panel/string layout from Tigo's cloud
  cloud_import: true
```

### Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `tigo_monitor_id` | ID | Required | Reference to tigo_monitor |
| `port` | Integer | 80 | HTTP port |
| `api_token` | String | None | Bearer token for API auth |
| `web_username` | String | None | HTTP Basic Auth username |
| `web_password` | String | None | HTTP Basic Auth password |
| `backlight` | ID | None | Light component to expose via the optional `POST /api/backlight` endpoint (units with a backlight wired) |
| `cca_source` | Enum | `http` | CCA Info data source: `http`, `ble`, or `auto`. `ble`/`auto` require `ble_client_id` |
| `ble_client_id` | ID | None | A `ble_client:` for the CCA's BLE MAC. The MAC is a default/seed — reselect it via the **CCA Connection** search and store it on-device |
| `cloud_import` | Boolean | `false` | Enable the Tigo Cloud page + cloud layout import (compiles the cloud client + TLS cert bundle) |

### CCA over Bluetooth (`cca_source: ble`)

Tigo CCA firmware 4.0.4+ (incl. 4.0.5-ct) locks the local HTTP API, so the CCA Info page can instead source data over Bluetooth. With `cca_source: ble` and a `ble_client_id`, `tigo_server` becomes the BLE client and talks the CCA's `mobile_api` over GATT. The link is opened on demand and dropped after each read so the Tigo phone app can still connect (the CCA allows one BLE central at a time).

```yaml
ble_client:
  - mac_address: "04:C0:5B:AA:BB:CC"   # default/seed — required by ble_client, overridable in the UI
    id: tigo_cca_ble
    auto_connect: false                 # we connect on demand; don't hold the link at boot

esp32_ble:
  use_psram: true       # BLE host buffers from PSRAM
  max_connections: 1

esp32_ble_tracker:
  scan_parameters:
    interval: 320ms
    window: 60ms
    active: true
```

You don't have to know the MAC up front: the CCA Info page's **CCA Connection** card scans for the CCA by its Tigo `04:C0:5B` address prefix and lets you pick it. The choice is saved on-device (NVS) and overrides the YAML MAC across reboots; **Revert** restores the YAML value. The `ble_client:` block (with *some* MAC) is still required — `mac_address` is a required field, so leave your real MAC there as the default.

### On-device configuration (Tools → Device Configuration)

These runtime knobs can be set in the web UI and persisted to the ESP32 (NVS) without reflashing — handy for tuning `power_calibration` against a reference meter, or changing behavior on a deployed unit:

| Knob | Notes |
|------|-------|
| `power_calibration` | Applied immediately (every power calc reads it live) |
| `night_mode_timeout` | Minutes of silence before readings are zeroed |
| `reset_at_midnight` | Reset peaks + daily energy at local midnight (needs a time source) |
| `sync_cca_on_startup` | Query the CCA over local HTTP at boot (only with a `cca_ip`) |
| `cca_ip` | CCA IP for local HTTP queries (older firmware) |

The YAML value is the **default**. A stored value overrides it until you press **Revert**, which clears the override so future YAML edits apply again (Revert is enabled only when the live value differs from the default). Structural settings (inverter layout, device count, ports, IDs) and auth (`api_token`/`web_*`) stay YAML-only — the latter because NVS is plaintext-at-rest.

### Tigo cloud import (`cloud_import: true`)

When the CCA's local HTTP is locked, the panel names + string/MPPT/inverter layout can be recovered from Tigo's cloud (the same API the mobile app uses). Enter your Tigo account in the **Configure** modal on the Tigo Cloud page — **only the resulting bearer token is persisted to NVS, never the password**. The page also shows Tigo's own per-equipment health/status/history; layout import is a button on the Topology page. HTTPS is verified against the mbedTLS certificate bundle, which `cloud_import` enables automatically.

### Authentication

**API Authentication** (Bearer token):
```bash
curl -H "Authorization: Bearer your-token" http://esp32/api/devices
```

**Web Authentication** (HTTP Basic):
- Browser prompts for username/password
- Credentials cached per session

**Health Check** (`/api/health`) requires no authentication.

---

## Sensors

### System-Level Sensors

No `address` required. Each hub sensor is a separate platform entry — sensor type is **auto-detected from the `name` keywords**:

```yaml
sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Power"
    
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Energy"
    
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Active Device Count"
  
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Missed Frame Count"

  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Invalid Checksum Count"

  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Free Internal RAM"

  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Min Free Internal RAM"

  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Free PSRAM"

  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Free Stack"
```

Sensor type is inferred from name keywords:
- `power`, `total`, `sum`, `watt`, `system`, `combined` → Total power sensor
- `energy`, `kwh`, `kilowatt`, `wh` → Energy sensor
- `count`, `devices`, `discovered`, `active`, `number` → Device count sensor
- `frame`, `missed`, `lost`, `dropped` → Missed frame counter
- `checksum`, `invalid`, `crc`, `error` → Invalid checksum counter
- `internal`, `ram`, `heap` (with `min`/`minimum`/`watermark`) → Min free internal RAM
- `internal`, `ram`, `heap` (without min keywords) → Free internal RAM
- `psram` → Free PSRAM sensor
- `stack` → Free stack sensor

> **Keyword precedence:** matching is order-sensitive. `checksum`/`frame` are
> matched **before** `count`, so a name like `"Invalid Checksum Count"` or
> `"Missed Frame Count"` resolves to the checksum/frame counter rather than the
> device-count sensor even though it contains the word "count". Likewise
> `psram` is matched before the generic `ram` keyword.

> **Important:** Each hub-level sensor must be its own `- platform: tigo_monitor` entry.
> Do **not** nest them as sub-keys (e.g., `power_sum:`) under a single platform entry — that format is only for per-device sensors.

### Individual Device Sensors

Requires `address` from device discovery:

```yaml
sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "Panel 1"
    power: {}
    peak_power: {}
    voltage_in: {}
    voltage_out: {}
    current_in: {}
    temperature: {}
    rssi: {}
    duty_cycle: {}
    efficiency: {}
    power_factor: {}
    load_factor: {}
```

### Grouping panels into HA sub-devices

ESPHome's [sub-devices feature](https://esphome.io/components/esphome/#esphome-devices)
lets one ESPHome node expose multiple logical "devices" to Home Assistant. The
generator in the Tools view emits this for you — pick **Per MPPT**, **Per inverter**,
or **Per panel** in the grouping selector and the YAML it produces will include
an `esphome.devices:` block plus a `device_id:` on each child sensor.

If you're hand-writing the YAML, the same pattern works: declare the device once
on the panel's base config and the schema propagates it to every child sensor
(power_in, peak_power, voltage_in, etc.) — no need to repeat `device_id:` on each:

```yaml
esphome:
  name: tigo-monitor
  devices:
    - id: tigo_mppt_1
      name: "MPPT 1"
    - id: tigo_mppt_2
      name: "MPPT 2"

sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "Panel 1"
    device_id: tigo_mppt_1   # propagates to all child sensors below
    power: {}
    voltage_in: {}
    current_in: {}
    temperature: {}

  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "5678"
    name: "Panel 2"
    device_id: tigo_mppt_1
    power: {}
    voltage_in: {}
    current_in: {}
    temperature: {}
```

After flashing, Home Assistant shows "MPPT 1" and "MPPT 2" as separate device
cards, each grouping the entities for its panels.

### Text Sensors

```yaml
text_sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "Panel 1"
    barcode: {}
    firmware_version: {}
    device_info: {}
```

### Binary Sensors

```yaml
binary_sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    night_mode:
      name: "Solar Night Mode"
```

---

## Management Buttons

```yaml
button:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Generate YAML Config"
    button_type: yaml_generator
  
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Print Device Mappings"
    button_type: device_mappings
  
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Sync from CCA"
    button_type: sync_from_cca
  
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Reset Node Table"
    button_type: reset_node_table
```

---

## Efficiency Metrics

| Metric | Formula | Range | Description |
|--------|---------|-------|-------------|
| Efficiency | `(Power Out / Power In) × 100%` | 90-98% | DC-DC conversion |
| Power Factor | `Voltage Out / Voltage In` | 0.8-1.2 | Voltage regulation |
| Duty Cycle | `(Raw / 255) × 100%` | 0-100% | PWM duty cycle |
| Load Factor | `(Duty / 100) × (Power / 1000)` | Variable | Composite metric |

---

## ESP-IDF Framework

Required configuration:

```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf
    version: recommended
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"  # Reduces packet loss
```

### PSRAM (ESP32-S3)

Required for 15+ devices:

```yaml
esphome:
  platformio_options:
    board_build.flash_mode: dio

psram:
  mode: octal
  speed: 80MHz

esp32:
  board: m5stack-atoms3
  variant: esp32s3
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240: "y"
      CONFIG_ESP32S3_DATA_CACHE_64KB: "y"
      CONFIG_ESP32S3_DATA_CACHE_LINE_64B: "y"
      CONFIG_SPIRAM_MODE_OCT: "y"
      CONFIG_SPIRAM_SPEED_80M: "y"
```

> **Re-flashing to enable PSRAM:** if you previously flashed *without* PSRAM, clean the ESPHome build files **and** erase the ESP32 flash completely (`esptool.py erase_flash`) before re-flashing. ESPHome doesn't rebuild the bootloader automatically when PSRAM settings change.

### Large installs — internal RAM

PSRAM holds the bulk device data, but a few things still come out of the small **internal** (DMA-capable) heap and grow with install size. On a big array (30+ devices) they can exhaust it — the tell-tale symptom is new TCP connections resetting after a few minutes of uptime (OTA fails with `Connection reset by peer`, `esphome logs` won't attach) while the existing Home Assistant connection keeps working and a reboot temporarily clears it.

Check the low-water mark at any time — `heap_min_free` in `curl http://<device-ip>/api/health`. If it drops into the low single-digit KB, tune these:

- **WiFi/lwIP buffers.** Move them into PSRAM to free internal RAM:

  ```yaml
  esp32:
    framework:
      type: esp-idf
      sdkconfig_options:
        CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP: "y"
  ```

- **Entity count.** Every per-panel sensor declared in YAML costs ~110–150 bytes of internal RAM at boot (the sensor object plus API registration). It scales: 64 panels × 7 sensor types ≈ 450 entities ≈ **50–70 KB** — the largest static consumer on a big install. Declare only the per-panel sensors you actually use in Home Assistant; the web dashboard shows every metric for every panel regardless, at no per-entity cost. For HA aggregates, prefer the `string_label:` per-string power sensor (one entity per string) over declaring — or lambda-summing — per-panel sensors.
- **UART RX buffer.** `CONFIG_UART_RX_BUFFER_SIZE` (and the matching `rx_buffer_size:` on the `uart:` component) allocate from internal RAM, never PSRAM. 2–8 KB is plenty on an ESP32-S3 at 38400 baud; a 32 KB buffer silently spends a sixth of the usable internal heap. See [UART Optimization](UART_OPTIMIZATION.md).

---

## On-Flash History (esp_tsdb)

Persistent time-series history is opt-in via two extra dependencies and a custom partition table. See [`docs/tsdb-integration.md`](tsdb-integration.md) for the full schema, sizing, and query reference.

Quick form (8 MB AtomS3R):

```yaml
esp32:
  framework:
    type: esp-idf
    components:
      - zakery292/esp_tsdb^2.1.0    # see tsdb-integration.md re: pinning
      - joltwallet/littlefs^1.16
    sdkconfig_options:
      CONFIG_PARTITION_TABLE_CUSTOM: "y"
      CONFIG_PARTITION_TABLE_FILENAME: "boards/partitions/tigo-8mb.csv"
```

Without these, the rest of the component still works — you just lose the History view and the `/api/history/*` and `/api/tsdb/stats` endpoints.

---

## Filtering and Smoothing

Add ESPHome filters to any sensor:

```yaml
sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Power"
    filters:
      - sliding_window_moving_average:
          window_size: 5
          send_every: 1
```

---

## Complete Example

See [`boards/`](../boards/) for complete, ready-to-flash board configs — e.g. [`boards/esp32s3-atoms3r.yaml`](../boards/esp32s3-atoms3r.yaml) for the recommended M5Stack AtomS3R (PSRAM + esp_tsdb + tuned UART buffers).

---

**See also:** [Web Server &amp; API](WEB_SERVER_README.md) · [Wiring](WIRING.md) · [Troubleshooting](TROUBLESHOOTING.md) · [← Back to README](../README.md)
