# ESPHome Tigo Monitor Component

A comprehensive ESPHome component for monitoring Tigo solar power optimizers via UART communication. Built for the ESP-IDF framework and enables real-time monitoring of individual Tigo devices with system-wide power and energy tracking through Home Assistant.

## 🌟 Features

### Core Monitoring
- **ESP-IDF Framework**: Built specifically for ESP-IDF for optimal performance and reliability
- **Individual Device Monitoring**: Track voltage, current, power, temperature, and RSSI for each Tigo optimizer
- **System Aggregation**: Total system power, energy production (kWh), and active device count
- **Device Discovery**: Automatic detection and persistent mapping of Tigo devices
- **Barcode Identification**: Device identification using Frame 09 and Frame 27 barcode data
- **Power Efficiency Analytics**: Conversion efficiency, power factor, duty cycle, and load factor metrics
- **Device Information**: Firmware version and device info extraction from string responses
- **Energy Dashboard Integration**: Compatible with Home Assistant's Energy Dashboard
- **Persistent Storage**: Device mappings and energy data survive reboots

### Web Interface
- **Built-in Web Dashboard**: Custom web server with 5 comprehensive pages
  - **Dashboard**: Real-time overview with system stats and live device metrics
    - Total Power, Current, Energy (kWh), Efficiency, Temperature, Active Devices
    - Temperature unit toggle (°F/°C) with persistent preference
    - Device cards with CCA-friendly names and real-time metrics
  - **Node Table**: Complete device registry with CCA labels and hierarchy
    - Individual node deletion capability
    - CCA validation badges
  - **ESP32 Status**: System health, memory usage, task count, and uptime
  - **YAML Config Generator**: Automatic sensor configuration generation
  - **CCA Info**: Tigo CCA device information and status monitoring
    - Manual refresh button for on-demand updates
- **Mobile Responsive**: Optimized layouts for desktop and mobile devices
- **Auto-refresh**: Live updates without page reloads
- **No External Dependencies**: Runs entirely on the ESP32

### CCA Integration
- **Automatic CCA Sync**: Query Tigo CCA for panel configuration data
- **Panel Name Mapping**: Automatically label panels with CCA-assigned names
- **Hierarchy Display**: Shows Inverter → String → Panel relationships
- **Barcode Matching**: Fuzzy matching between UART devices and CCA configuration
- **Sync on Boot**: Optional automatic synchronization on startup
- **Manual Sync Button**: On-demand CCA configuration refresh
- **Device Validation**: Visual indicators for CCA-validated devices

### Advanced Features
- **Flash Wear Optimization**: Energy data saved hourly (24 writes/day) for extended flash lifespan
- **Night Mode**: Automatic zero publishing when no data received (prevents stale data at night)
- **UART Optimization**: ISR in IRAM for reduced packet loss
- **Flexible Configuration**: Support for individual sensors or combined device sensors
- **Management Tools**: Built-in buttons for YAML generation and device management
- **OTA Updates**: Over-the-air firmware updates

## 📋 Requirements

- ESP32 development board (ESP32-S3 recommended, tested on M5Stack AtomS3)
- UART connection to Tigo communication system (38400 baud)
- ESPHome 2025.10.3 or newer
- Home Assistant (optional, for full integration)
- Tigo CCA (Cloud Connect Advanced) - optional, for panel name auto-labeling

## 🔧 Framework Requirements

This component requires the ESP-IDF framework:

### ESP-IDF Framework
- Uses native ESP-IDF libraries for optimal performance
- Better memory efficiency and reliability
- Memory usage: ~10% RAM, ~49% Flash (without web server), ~15% RAM with web server
- Robust implementation with advanced features
- Required for web server functionality

Configure ESP-IDF framework in your configuration:
```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf
    version: recommended
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"  # Reduces UART packet loss
```

## 🛠️ Installation

### 1. Hardware Setup

Connect your ESP32 to the Tigo communication system:

- **TX Pin**: GPIO1 (connects to Tigo RX)
- **RX Pin**: GPIO3 (connects to Tigo TX)
- **Baud Rate**: 38400
- **Data**: 8 bits, No parity, 1 stop bit

### 2. Basic Configuration

Create or update your ESPHome YAML configuration:

```yaml
esphome:
  name: tigo-monitor
  friendly_name: "Tigo Monitor"

esp32:
  board: esp32dev
  framework:
    type: esp-idf

# External components
external_components:
  - source: github://RAR/esphome-tigomonitor
    components: [ tigo_monitor, tigo_server ]
    # Optional: specify a specific version/branch
    # ref: v1.0.0  # Use a specific release tag
    # ref: dev     # Use development branch
    # ref: main    # Use main branch (default)

# WiFi configuration
wifi:
  ssid: "YOUR_WIFI_SSID"
  password: "YOUR_WIFI_PASSWORD"

# Enable Home Assistant API
api:

# Enable OTA updates
ota:
  - platform: esphome

# UART configuration for Tigo communication
uart:
  id: tigo_uart
  tx_pin: GPIO1
  rx_pin: GPIO3
  baud_rate: 38400

# Tigo Monitor component
tigo_monitor:
  id: tigo_hub
  uart_id: tigo_uart
  update_interval: 30s
  number_of_devices: 20  # Set to your actual number of Tigo devices
  cca_ip: "192.168.1.100"  # Optional: IP address of your Tigo CCA
  sync_cca_on_startup: true  # Optional: Auto-sync CCA config on boot (default: true)

# Optional: Web Server for monitoring dashboard
tigo_server:
  tigo_monitor_id: tigo_hub
  port: 80  # Default web server port
```

### Alternative Installation (Development)

For development or customization, you can use a local copy:

```yaml
external_components:
  - source: 
      type: local
      path: components  # Path to local components folder
```

First clone the repository:
```bash
git clone https://github.com/RAR/esphome-tigomonitor.git
cd esphome-tigomonitor
```

## 🔧 Configuration Options

### Tigo Monitor Component

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `uart_id` | ID | Required | UART component ID for communication |
| `update_interval` | Time | 60s | How often to publish sensor data |
| `number_of_devices` | Integer | 5 | Maximum number of Tigo devices to track |
| `cca_ip` | String | None | IP address of Tigo CCA for automatic panel labeling |
| `sync_cca_on_startup` | Boolean | true | Automatically sync CCA configuration on boot |

### Tigo Web Server Component

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `tigo_monitor_id` | ID | Required | Reference to tigo_monitor component |
| `port` | Integer | 80 | HTTP port for web interface |

The web server provides:
- **Dashboard** (`/`) - Live device monitoring with real-time metrics
  - System stats: Total Power, Current, Energy, Efficiency, Temperature, Active Devices
  - Temperature toggle (°F/°C) with localStorage persistence
  - Device cards with CCA labels and live metrics
- **Node Table** (`/nodes`) - Complete device list with CCA labels
  - Delete individual nodes
  - CCA validation indicators
- **ESP32 Status** (`/status`) - System health and resource usage
- **YAML Config** (`/yaml`) - Auto-generated sensor configuration
- **CCA Info** (`/cca`) - Tigo CCA device status and information
  - Manual refresh capability

### Sensor Types

The component supports several sensor configurations:

#### System-Level Sensors (No Address Required)

```yaml
sensor:
  # Total power from all devices
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Power"
    
  # Cumulative energy production
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Total System Energy"
    
  # Number of active devices
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Active Device Count"
```

#### Individual Device Sensors

```yaml
sensor:
  # Individual device with all sensor types
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"  # Device address from discovery
    name: "Tigo Device 1"
    power: {}
    voltage_in: {}
    voltage_out: {}
    current_in: {}
    temperature: {}
    rssi: {}
    duty_cycle: {}        # PWM duty cycle (0-100%)
    efficiency: {}        # Conversion efficiency (%)
    power_factor: {}      # Voltage regulation ratio
    load_factor: {}       # Composite load metric

text_sensor:
  # Device barcode/serial number and info
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "Tigo Device 1"
    barcode: {}
    firmware_version: {}  # Device firmware version
    device_info: {}       # Additional device information
```

## 🚀 Quick Start Guide

### Step 1: Initial Setup

1. Flash the basic configuration to your ESP32
2. Check the logs for device discovery messages
3. Use the management buttons to generate sensor configurations

### Step 2: Device Discovery

The component includes helpful management buttons:

```yaml
button:
  - platform: tigo_monitor
    name: "Generate YAML Config"
    tigo_monitor_id: tigo_hub
    button_type: yaml_generator
  
  - platform: tigo_monitor
    name: "Print Device Mappings"
    tigo_monitor_id: tigo_hub
    button_type: device_mappings
  
  - platform: tigo_monitor
    name: "Sync from CCA"
    tigo_monitor_id: tigo_hub
    button_type: sync_from_cca  # Fetch panel names from Tigo CCA
  
  - platform: tigo_monitor
    name: "Reset Node Table"
    tigo_monitor_id: tigo_hub
    button_type: reset_node_table
```

Press the "Print Device Mappings" button to see discovered devices in the logs.

## 🌐 Web Interface

The component includes a built-in web server accessible at `http://<esp32-ip>/`

![Dashboard](docs/images/Dashboard.png)
*Dashboard page showing system statistics and live device monitoring with temperature toggle*

### Web Dashboard Features

#### Dashboard Page (`/`)
- **System Statistics**: 6 stat cards showing real-time system metrics
  - Total Power (W), Total Current (A), Total Energy (kWh)
  - Active Devices, Average Efficiency (%), Average Temperature
- **Temperature Unit Toggle**: Switch between Fahrenheit and Celsius
  - Preference saved to browser localStorage
  - Applies to all temperature displays
- **Live Device Cards**: Real-time monitoring for each device
  - CCA-friendly names (e.g., "East Roof Panel 3")
  - Two-line headers: CCA label + barcode/address
  - Power, voltage, current, temperature, efficiency, RSSI
  - Data age indicators
- **Auto-refresh**: Updates every 5 seconds

#### Node Table Page (`/nodes`)

![Node Table](docs/images/NodeTable.png)
*Node table showing all discovered devices with CCA labels and delete functionality*

- **Complete Device List**: All discovered devices with sensor assignments
- **CCA Integration**: Shows panel names from Tigo CCA (if configured)
- **Hierarchy Display**: Inverter → String → Panel structure
- **Barcode Information**: Frame 27 barcode data (16-char long address)
- **Validation Badges**: Visual indicators for CCA-validated devices
- **Node Management**: Delete individual nodes with confirmation
  - Removes node from persistent storage
  - Frees up sensor index for reuse

#### ESP32 Status Page (`/status`)

![ESP32 Status](docs/images/ESP32Status.png)
*ESP32 status page with system information and memory metrics*

- **System Information**: Uptime, ESPHome version, compilation time
- **Memory Metrics**: Heap and PSRAM usage with visual progress bars
- **Minimum Free Memory**: Lowest memory levels (helps detect memory leaks)
- **Task Count**: Number of active FreeRTOS tasks
- **Auto-refresh**: Updates every 5 seconds

#### YAML Config Page (`/yaml`)

![YAML Config](docs/images/YAML.png)
*Auto-generated YAML configuration with one-click copy*

- **Auto-generated Configuration**: Complete sensor definitions for all devices
- **CCA Labels**: Uses panel names from CCA if synced
- **Copy to Clipboard**: One-click copy functionality
- **Device Count**: Shows number of configured devices

#### CCA Info Page (`/cca`)

![CCA Info](docs/images/CCA.png)
*CCA device information with manual refresh capability*

- **Connection Status**: Real-time CCA connectivity
- **Device Information**: Serial number, software version, system ID
- **System Status**: Cloud connection, gateway communication, module status
- **Discovery Progress**: Current discovery status (e.g., "36/36")
- **Uptime**: CCA uptime in human-readable format
- **Last Config Sync**: When configuration was last pulled from cloud
- **Manual Refresh Button**: On-demand CCA device info update
- **Auto-refresh**: Updates every 30 seconds

### Accessing the Web Interface

```
http://<esp32-ip-address>/        # Dashboard (with system stats and live devices)
http://<esp32-ip-address>/nodes    # Node Table
http://<esp32-ip-address>/status   # ESP32 Status
http://<esp32-ip-address>/yaml     # YAML Config
http://<esp32-ip-address>/cca      # CCA Info
```

## 🔗 CCA Integration

### Automatic Panel Labeling

The component can query your Tigo CCA (Cloud Connect Advanced) to automatically label panels with their configured names.

#### Configuration

```yaml
tigo_monitor:
  id: tigo_hub
  uart_id: tigo_uart
  cca_ip: "192.168.1.100"  # Your CCA IP address
  sync_cca_on_startup: true  # Auto-sync on boot
```

#### Features

- **Barcode Matching**: Matches UART devices with CCA configuration using Frame 27 (16-char) barcodes
- **Hierarchy Mapping**: Shows Inverter → String → Panel relationships
- **Label Storage**: CCA labels persist across reboots
- **Manual Sync**: Button to refresh CCA configuration on demand
- **Auto-sync**: Optional automatic synchronization on startup (5-second delay for WiFi)
- **Web Refresh**: On-demand CCA device info refresh via web interface

#### CCA Sync Button

```yaml
button:
  - platform: tigo_monitor
    name: "Sync from CCA"
    tigo_monitor_id: tigo_hub
    button_type: sync_from_cca
```

Press this button to query the CCA and match panels to their configured names. The component will:
1. Query CCA's `/cgi-bin/summary_config` endpoint
2. Parse panel configuration with serial numbers
3. Match CCA serials to UART device barcodes
4. Store matched labels persistently

#### Benefits

- **User-Friendly Names**: "East Roof Panel 3" instead of "Tigo Device 1"
- **System Visualization**: See which string and inverter each panel belongs to
- **YAML Generation**: Auto-generated configs use CCA names
- **Web Interface**: Node Table displays CCA labels and hierarchy

### Step 3: Generate Configuration

Press the "Generate YAML Config" button to get sensor configurations for all discovered devices. Copy the generated YAML from the logs into your configuration.

### Step 4: Add Individual Sensors

Update your configuration with the generated sensor definitions and reflash.

## ⚡ Power Efficiency Metrics

The component provides advanced power efficiency analytics for each Tigo device:

### Available Efficiency Metrics

| Metric | Description | Formula | Typical Range | Use Case |
|--------|-------------|---------|---------------|----------|
| **Conversion Efficiency** | DC-DC conversion efficiency | `(Power Out / Power In) × 100%` | 90-98% | Monitor optimizer performance |
| **Power Factor** | Voltage regulation ratio | `Voltage Out / Voltage In` | 0.8-1.2 | Assess voltage regulation |
| **Load Factor** | Composite load metric | `(Duty Cycle / 100) × (Power Out / 1000)` | Variable | Overall load assessment |
| **Duty Cycle** | PWM duty cycle percentage | `(Raw Value / 255) × 100%` | 0-100% | Control system monitoring |

### Configuration Example

```yaml
sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "Panel 1"
    # Standard power metrics
    power: {}
    voltage_in: {}
    voltage_out: {}
    current_in: {}
    # Efficiency analytics
    efficiency:
      name: "Panel 1 Efficiency"
      unit_of_measurement: "%"
      accuracy_decimals: 1
    power_factor:
      name: "Panel 1 Power Factor"
      accuracy_decimals: 3
    duty_cycle:
      name: "Panel 1 Duty Cycle"
      unit_of_measurement: "%"
      accuracy_decimals: 1
    load_factor:
      name: "Panel 1 Load Factor"
      accuracy_decimals: 2
```

### Efficiency Monitoring Benefits

- **Performance Trending**: Track optimizer efficiency over time
- **Fault Detection**: Identify underperforming devices
- **Maintenance Scheduling**: Predict maintenance needs
- **System Optimization**: Optimize system configuration
- **Comparative Analysis**: Compare device performance

## 📊 Monitoring and Logs

### Device Discovery Logs

Look for these log messages:

- `📦 Frame 27 received` - Long address (16-char barcode) discovery
- `New device discovered:` - Power data from new devices
- `Assigned sensor index X to device YYYY` - Device mapping assignments
- `Energy data saved at hour boundary` - Hourly energy persistence confirmation

### Troubleshooting Logs

- `No Frame 27 long address available for device XXXX` - Device found but no barcode yet
- `Cannot create node entry - node table is full` - Increase `number_of_devices`
- `Maximum number of devices reached` - Adjust configuration limit

## 🏠 Home Assistant Integration

![Home Assistant](docs/images/HA.png)
*Home Assistant Energy Dashboard integration showing solar production tracking*

### Energy Dashboard

The component provides sensors compatible with Home Assistant's Energy Dashboard:

1. **Energy Production**: Use "Total System Energy" sensor
2. **Power Monitoring**: Use "Total System Power" sensor
3. **Device Tracking**: Use "Active Device Count" for system health

### Automation Examples

```yaml
# Home Assistant automation
automation:
  - alias: "Tigo System Alert"
    trigger:
      platform: numeric_state
      entity_id: sensor.active_device_count
      below: 15  # Alert if fewer than expected devices
    action:
      service: notify.mobile_app
      data:
        message: "Tigo system has fewer active devices than expected"
```

## 📁 Project Structure

```
components/
├── tigo_monitor/
│   ├── __init__.py           # Component initialization and config validation
│   ├── button.py             # Management button platform (YAML gen, CCA sync, etc.)
│   ├── sensor.py             # Sensor platform configuration
│   ├── tigo_monitor.cpp      # Main component implementation
│   └── tigo_monitor.h        # Component header file
├── tigo_server/
│   ├── __init__.py           # Web server component initialization
│   ├── tigo_web_server.cpp   # Web server implementation (5 pages + APIs)
│   └── tigo_web_server.h     # Web server header file
├── examples/                 # Configuration examples
├── tigo-server.yaml          # Main configuration file
└── README.md                 # This file
```

## 🔧 Advanced Configuration

### Custom Sensor Names

```yaml
sensor:
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "Roof Panel 1"  # Custom name
    power:
      name: "Roof Panel 1 Power Output"
      filters:
        - sliding_window_moving_average:
            window_size: 5
```

### Filtering and Smoothing

Add filters to smooth noisy sensor data:

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

## 🏠 Home Assistant Dashboard Integration

### Ready-to-Use Dashboard Configurations

This repository includes complete Home Assistant dashboard configurations:

#### 📊 Main Dashboard (`home-assistant-dashboard.yaml`)
- **System Overview**: Total power, energy, and device count gauges
- **Individual Panel Performance**: Power output grids and detailed metrics
- **Efficiency Analytics**: Conversion efficiency trends and analysis
- **Temperature Monitoring**: Panel temperature graphs and alerts
- **Device Management**: Control buttons and device information
- **Load Factor Analysis**: Composite performance metrics

#### 📱 Mobile Dashboard (`home-assistant-mobile-dashboard.yaml`)
- **Optimized for Mobile**: Compact layout with fold-entity-row cards
- **Quick Status**: Glance cards for instant system overview
- **Interactive Charts**: Touch-friendly mini-graph cards
- **Smart Summaries**: Automatic best/worst performer identification
- **Quick Actions**: Easy access to device management functions

#### 🤖 Automation Examples (`home-assistant-automations.yaml`)
- **Efficiency Alerts**: Notifications when panels drop below 90% efficiency
- **Temperature Monitoring**: High temperature warnings (>70°C)
- **Communication Alerts**: Signal strength monitoring and alerts
- **Daily Reports**: Automated energy production summaries
- **Maintenance Reminders**: Weekly performance summaries

### Installation Instructions

1. **Copy Dashboard Configuration**:
   ```yaml
   # Copy content from home-assistant-dashboard.yaml
   # Paste into Home Assistant Dashboard editor
   # Customize entity names to match your setup
   ```

2. **Add Automations**:
   ```yaml
   # Add automations from home-assistant-automations.yaml
   # Customize notification targets and thresholds
   ```

3. **Mobile Setup**:
   ```yaml
   # Create new dashboard tab using home-assistant-mobile-dashboard.yaml
   # Perfect for quick monitoring on phones/tablets
   ```

### Entity Name Mapping

Update the dashboard entity names to match your ESPHome configuration:

```yaml
# Example mapping:
# Dashboard uses: sensor.tigo_device_1_power
# Your setup might be: sensor.solar_panel_east_power
# 
# Find and replace in dashboard files:
# - tigo_device_1 → your_device_name
# - tigo_monitor → your_esphome_name
```

### Dashboard Features

- **Real-time Monitoring**: Live power, voltage, and efficiency data
- **Historical Trends**: 24-hour temperature and efficiency graphs  
- **Performance Analytics**: Efficiency comparison and load factor analysis
- **Alert Integration**: Visual indicators for system health
- **Energy Dashboard Ready**: Compatible with HA Energy Dashboard
- **Maintenance Tools**: Built-in device discovery and config generation

## 🐛 Troubleshooting

### ESP-IDF Framework

**Compilation Errors with ESP-IDF**
- Ensure ESPHome 2025.10.4+ for ESP-IDF 5.4.2 support
- Check framework version: `type: esp-idf, version: recommended`
- Memory usage may require adjusting `number_of_devices`
- Add `CONFIG_UART_ISR_IN_IRAM: "y"` to sdkconfig_options to reduce packet loss

### Common Issues

1. **No devices discovered**
   - Check UART wiring and baud rate (38400)
   - Verify Tigo system is communicating
   - Check ESPHome logs for Frame detection
   - Look for "📦 Frame" messages in logs

2. **Devices discovered but no barcodes**
   - Frame 27 data may not be transmitted by all systems immediately
   - Devices will work without barcodes (shows as "Module XXXX")
   - Wait longer for Frame 27 (16-char barcode) to be received
   - Frame 09 (6-char) barcodes are no longer used

3. **"Packet missed!" errors**
   - Add `CONFIG_UART_ISR_IN_IRAM: "y"` to ESP-IDF sdkconfig_options
   - This moves UART interrupt handler to IRAM for faster processing
   - Significantly reduces packet loss

4. **CCA sync not working**
   - Verify CCA IP address is correct
   - Check network connectivity between ESP32 and CCA
   - CCA must be on same network or routable from ESP32
   - Check CCA is responding (try accessing http://cca-ip in browser)
   - Review logs for "CCA Sync complete" or error messages

5. **Web interface not accessible**
   - Verify `tigo_server` component is configured
   - Check ESP32 IP address (look in ESPHome logs)
   - Ensure port 80 is not blocked by firewall
   - Try accessing http://esp32-ip directly

6. **Energy data resets on reboot**
   - Component automatically saves energy data to flash
   - Check logs for "Restored total energy" and "Energy data saved at hour boundary" messages
   - Data is saved at the top of each hour to reduce flash wear (24 writes/day)
   - Maximum data loss on unexpected reboot: 1 hour of energy accumulation

7. **Too many devices**
   - Increase `number_of_devices` in configuration
   - Monitor memory usage in compilation output
   - Consider reducing web server features if memory is tight

8. **Night mode / Zero publishing**
   - Component automatically enters night mode after 1 hour of no data
   - Publishes zeros every 10 minutes to prevent stale data in Home Assistant
   - This is normal behavior - devices resume when sun comes up

### Reset Commands

If you need to reset device mappings:

```yaml
button:
  - platform: tigo_monitor
    name: "Reset Node Table"
    tigo_monitor_id: tigo_hub
    button_type: reset_node_table
```

## 📈 Performance Notes

- **Memory Usage**: ~11-15% RAM (15% with web server), ~49% Flash (typical ESP32)
- **Update Rate**: 30-60 seconds recommended for normal operation
- **Device Limit**: Tested with up to 36 devices
- **Flash Wear Optimization**: Energy data saved hourly at the top of each hour
  - 24 writes/day (vs 288 with every-10-updates approach)
  - Flash lifespan: ~11 years @ 100k cycles, ~114 years @ 1M cycles
  - Maximum data loss: 1 hour of energy on unexpected reboot
- **Web Server**: Adds ~4-5% RAM overhead, provides comprehensive monitoring interface
- **UART Optimization**: ISR in IRAM significantly reduces packet loss
- **CCA Queries**: HTTP requests add ~2-3 second delay during sync
- **Night Mode**: Automatic zero publishing after 1 hour of no data (every 10 minutes)

## 🤝 Contributing

Contributions are welcome! Please:

1. Fork the repository
2. Create a feature branch
3. Test your changes thoroughly
4. Submit a pull request with clear description

## 📄 License

This project is licensed under the MIT License - see the LICENSE file for details.

## 🙏 Acknowledgments

This project builds upon the excellent work of several open-source contributors:

- **[Bobsilvio/tigo_server](https://github.com/Bobsilvio/tigo_server)** - Original Arduino-based Tigo monitoring implementation that provided the foundation for understanding Tigo communication protocols
- **[Bobsilvio/tigosolar-local](https://github.com/Bobsilvio/tigosolar-local)** - Local monitoring solution for Tigo solar systems with valuable protocol insights
- **[willglynn/taptap](https://github.com/willglynn/taptap)** - Innovative approach to Tigo system monitoring and reverse engineering insights
- **[tictactom/tigo_server](https://github.com/tictactom/tigo_server)** - Additional Tigo monitoring implementation and protocol documentation
- **ESPHome Framework** - Providing the robust platform for ESP32-based home automation components

Special thanks to all the developers who contributed to understanding and documenting the Tigo communication protocols through their open-source work.

## 📞 Support

For issues and questions:

1. Check the troubleshooting section above
2. Review ESPHome logs for error messages
3. Open an issue on the project repository
4. Include your configuration and relevant log excerpts

---

**Happy Solar Monitoring! ☀️**
