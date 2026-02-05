# ESPHome Tigo Monitor

An ESPHome component for monitoring Tigo solar optimizers via RS485/UART. Real-time monitoring of individual devices with Home Assistant integration.

![Dashboard](docs/images/Dashboard.png)

## Features

- **Device Monitoring** – Voltage, current, power, temperature, RSSI per optimizer
- **System Aggregation** – Total power, energy (kWh), active device count
- **Built-in Web UI** – Dashboard, node table, status, YAML generator, CCA info
- **CCA Integration** – Auto-sync panel names from Tigo Cloud Connect Advanced
- **Home Assistant** – Energy Dashboard compatible, full API integration
- **Persistent Storage** – Device mappings and energy data survive reboots

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
  board: esp32dev
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"

external_components:
  - source: github://RAR/esphome-tigomonitor
    components: [ tigo_monitor, tigo_server ]

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

```yaml
sensor:
  # System totals
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Power"
    
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Energy"

  # Individual device
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
```

### 4. Access Web Interface

Navigate to `http://<esp32-ip>/` for the dashboard.

| Page | URL | Description |
|------|-----|-------------|
| Dashboard | `/` | Real-time system overview |
| Node Table | `/nodes` | Device registry with CCA labels |
| Status | `/status` | ESP32 health and memory |
| YAML Config | `/yaml` | Auto-generated sensor config |
| CCA Info | `/cca` | Tigo CCA device status |

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

All endpoints return JSON. Optional Bearer token authentication.

| Endpoint | Description |
|----------|-------------|
| `/api/devices` | Device metrics with string labels |
| `/api/overview` | System aggregates |
| `/api/strings` | Per-string data |
| `/api/status` | ESP32 status |
| `/api/health` | Health check (no auth) |

## Documentation

| Document | Description |
|----------|-------------|
| [Wiring Guide](docs/WIRING.md) | RS485 connection to Tigo CCA/TAP |
| [Configuration Guide](docs/CONFIGURATION.md) | Full configuration options |
| [Web Server](docs/WEB_SERVER_README.md) | Web UI and API details |
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
