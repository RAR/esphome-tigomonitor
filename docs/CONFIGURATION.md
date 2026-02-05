# Configuration Guide

Complete configuration reference for ESPHome Tigo Monitor.

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

---

## Tigo Web Server Component

```yaml
tigo_server:
  tigo_monitor_id: tigo_hub
  port: 80
  api_token: "your-secret-token"
  web_username: "admin"
  web_password: "secure-password"
```

### Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `tigo_monitor_id` | ID | Required | Reference to tigo_monitor |
| `port` | Integer | 80 | HTTP port |
| `api_token` | String | None | Bearer token for API auth |
| `web_username` | String | None | HTTP Basic Auth username |
| `web_password` | String | None | HTTP Basic Auth password |

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

No `address` required:

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
```

Sensor type is inferred from name keywords:
- `power` → Total power sensor
- `energy`, `kwh` → Energy sensor
- `count`, `device` → Device count sensor
- `frame`, `missed` → Missed frame counter

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

See [boards/](../boards/) for complete board-specific configurations.
