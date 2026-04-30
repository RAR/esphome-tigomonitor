# ESPHome Tigo Monitor

An ESPHome component for monitoring Tigo solar optimizers via RS485/UART. Real-time monitoring of individual devices with Home Assistant integration.

![Dashboard](docs/images/Dashboard.png)

## Features

- **Device Monitoring** – Voltage, current, power, temperature, RSSI per optimizer
- **System Aggregation** – Total power, energy (kWh), active device count, peak tracking
- **Built-in Single-Page Web App** – Dashboard heatmap, history, topology, nodes, tools, diagnostics, CCA info
- **On-Flash History** – Per-snapshot rollups + per-panel power persisted via [esp_tsdb](https://github.com/zakery292/esp_tsdb); survives reboots and OTA updates
- **CCA Integration** – Auto-sync panel names from Tigo Cloud Connect Advanced
- **In-UI Naming** – Friendly inverter and string names settable from Topology view, persisted to NVS (YAML stays the source of truth for identity)
- **Per-String Nameplate** – Set the rated watts per panel; health classification and "% of rated" readouts use it
- **Home Assistant** – Energy Dashboard compatible, full API integration, Ingress-proxy friendly

## Requirements

| Requirement | Details |
|-------------|---------|
| **Hardware** | ESP32-S3 with PSRAM recommended (e.g., M5Stack AtomS3R) |
| **Connection** | RS485 to Tigo system at 38400 baud |
| **Framework** | ESP-IDF (not Arduino) |
| **ESPHome** | 2025.10.3+ |

> **Note:** PSRAM is strongly recommended for 15+ devices. Without PSRAM, expect instability with web interface usage.

## Quick Start

### 1. Hardware Setup

Connect ESP32 to Tigo system via RS485:
- **TX:** GPIO6 → Tigo RX
- **RX:** GPIO5 → Tigo TX  
- **Baud:** 38400, 8N1

**Recommended:** [M5Stack Atomic RS485 Base](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base) for easy connection.

### 2. Basic Configuration

```yaml
esphome:
  name: tigo-monitor

esp32:
  board: esp32-s3-devkitc-1
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"

external_components:
  - source:
      type: git
      url: https://github.com/RAR/esphome-tigomonitor
    components: [ tigo_monitor, tigo_server ]
    refresh: 0s

logger:

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome

uart:
  id: tigo_uart
  tx_pin: GPIO6
  rx_pin: GPIO5
  baud_rate: 38400

tigo_monitor:
  id: tigo_hub
  uart_id: tigo_uart
  update_interval: 30s
  number_of_devices: 20
  cca_ip: "192.168.1.100"  # Optional: Your CCA IP

tigo_server:
  tigo_monitor_id: tigo_hub
  port: 80
```

### 3. Add Sensors

> **Important:** You must declare `sensor:`, `text_sensor:`, and `binary_sensor:` sections
> (even if empty) so ESPHome generates the required C++ headers. Without them, compilation will fail
> with `fatal error: esphome/components/sensor/sensor.h: No such file or directory`.

```yaml
sensor:
  # System totals
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Power"
    
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Energy"

  # Individual device (address from web UI discovery)
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "Panel 1"
    power: {}
    voltage_in: {}
    voltage_out: {}
    current_in: {}
    temperature: {}
    efficiency: {}

# Required even if empty — ensures ESPHome generates component headers
text_sensor:

binary_sensor:
```

### 4. Access Web Interface

Navigate to `http://<esp32-ip>/` — you land on the Dashboard view of the single-page app at `/app#dashboard`. Sidebar nav switches between views; the URL hash updates so views are deep-linkable.

| View | URL | Description |
|------|-----|-------------|
| Dashboard | `/app#dashboard` | Hero strip + per-string heatmap + alerts |
| History | `/app#history` | TSDB-backed power & energy charts (day / week / month / year) |
| Topology | `/app#topology` | Inverter → string → panel hierarchy with live V/I/W/°C, rename + nameplate editing |
| Node Table | `/app#nodes` | Device registry with CCA labels, export/import |
| Tools | `/app#tools` | YAML generator + Reset Peak / Clear Nodes / Restart actions |
| Diagnostics | `/app#diagnostics` | Memory, network, UART telemetry, TSDB stats |
| CCA Info | `/app#cca` | Tigo CCA device status with manual refresh |

Legacy paths (`/`, `/nodes`, `/status`, `/yaml`, `/cca`, `/history`) all 302 to the corresponding `#view`.

## PSRAM Configuration

**Required for 15+ devices.** Example for M5Stack AtomS3R:

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
      CONFIG_SPIRAM_MODE_OCT: "y"
      CONFIG_SPIRAM_SPEED_80M: "y"
```

For generic ESP32-S3 boards (e.g., DevKitC-1):

```yaml
esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  framework:
    type: esp-idf

psram:
  mode: octal
  speed: 80MHz
```

> **Tip:** If you previously flashed without PSRAM and are now enabling it, you must
> clean the ESPHome build files **and** erase the ESP32 flash completely (e.g., `esptool.py erase_flash`).
> ESPHome does not rebuild the bootloader automatically when PSRAM settings change.

## Management Buttons

```yaml
button:
  - platform: tigo_monitor
    name: "Generate YAML Config"
    tigo_monitor_id: tigo_hub
    button_type: yaml_generator
  
  - platform: tigo_monitor
    name: "Sync from CCA"
    tigo_monitor_id: tigo_hub
    button_type: sync_from_cca
```

## API Endpoints

All endpoints return JSON. Optional Bearer token authentication. See [`docs/WEB_SERVER_README.md`](docs/WEB_SERVER_README.md#api-endpoints) for the full set.

| Endpoint | Description |
|----------|-------------|
| `/api/overview` | System aggregates (power, energy, device counts) |
| `/api/devices` | Device metrics with string labels |
| `/api/inverters` | Per-inverter rollups + embedded strings (incl. `display_name`, `display_label`, `panel_rating_w`) |
| `/api/nodes` | Node table with CCA metadata; POST `/api/nodes/import` to restore |
| `/api/strings` | Flat per-string aggregates |
| `/api/inverters/rename` | POST `{name, display_name}` — set inverter friendly name |
| `/api/strings/rename` | POST `{label, display_label}` — set string friendly name |
| `/api/strings/rating` | POST `{label, rating_w}` — set per-panel nameplate watts |
| `/api/status` | ESP32 status |
| `/api/tsdb/stats` | LittleFS partition usage + per-DB record counts (only when esp_tsdb enabled) |
| `/api/history/power?range=…` | TSDB-backed system power/energy series |
| `/api/history/panel?slot=N&range=…` | TSDB-backed single-panel power series |
| `/api/panels` | Slot map (panel barcode → DB slot) |
| `/api/health` | Health check (no auth) |

## Documentation

| Document | Description |
|----------|-------------|
| [Wiring Guide](docs/WIRING.md) | RS485 connection to Tigo CCA/TAP |
| [Configuration Guide](docs/CONFIGURATION.md) | Full configuration options |
| [Web Server](docs/WEB_SERVER_README.md) | SPA + API reference |
| [TSDB Integration](docs/tsdb-integration.md) | On-flash time-series history (esp_tsdb) |
| [Troubleshooting](docs/TROUBLESHOOTING.md) | Common issues and solutions |
| [Home Assistant](docs/HOME_ASSISTANT.md) | HA integration and dashboards |
| [UART Optimization](docs/UART_OPTIMIZATION.md) | Reducing packet loss |

## Project Structure

```
components/
├── tigo_monitor/     # Main component (UART parsing, sensors)
└── tigo_server/      # Web server (dashboard, API)
boards/               # Example board configurations
docs/                 # Documentation
examples/             # HA dashboards and automations
```

## Contributing

1. Fork the repository
2. Create a feature branch
3. Test thoroughly
4. Submit a pull request

## License

MIT License – see [LICENSE](LICENSE) for details.

## Acknowledgments

Built on work by:
- [Bobsilvio/tigo_server](https://github.com/Bobsilvio/tigo_server)
- [Bobsilvio/tigosolar-local](https://github.com/Bobsilvio/tigosolar-local)
- [willglynn/taptap](https://github.com/willglynn/taptap)
- [tictactom/tigo_server](https://github.com/tictactom/tigo_server)

---

*All trademarks are property of their respective owners.*
