# ESPHome Tigo Monitor Component

A comprehensive ESPHome component for monitoring Tigo solar power optimizers via UART communication. Built for the ESP-IDF framework and enables real-time monitoring of individual Tigo devices with system-wide power and energy tracking through Home Assistant.

## 🌟 Features

- **ESP-IDF Framework**: Built specifically for ESP-IDF for optimal performance and reliability
- **Individual Device Monitoring**: Track voltage, current, power, temperature, and RSSI for each Tigo optimizer
- **System Aggregation**: Total system power, energy production (kWh), and active device count
- **Device Discovery**: Automatic detection and persistent mapping of Tigo devices
- **Barcode Identification**: Device identification using Frame 09 barcode data
- **Power Efficiency Analytics**: Conversion efficiency, power factor, duty cycle, and load factor metrics
- **Device Information**: Firmware version and device info extraction from string responses
- **Energy Dashboard Integration**: Compatible with Home Assistant's Energy Dashboard
- **Persistent Storage**: Device mappings and energy data survive reboots
- **Flexible Configuration**: Support for individual sensors or combined device sensors
- **Management Tools**: Built-in buttons for YAML generation and device management
- **OTA Updates**: Over-the-air firmware updates

## 📋 Requirements

- ESP32 development board
- UART connection to Tigo communication system (38400 baud)
- ESPHome 2025.10.3 or newer
- Home Assistant (optional, for full integration)

## 🔧 Framework Requirements

This component requires the ESP-IDF framework:

### ESP-IDF Framework
- Uses native ESP-IDF libraries for optimal performance
- Better memory efficiency and reliability
- Memory usage: ~10% RAM, ~49% Flash
- Robust implementation with advanced features

Configure ESP-IDF framework in your configuration:
```yaml
esp32:
  board: esp32dev
  framework:
    type: esp-idf
    version: recommended
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
    components: [ tigo_monitor ]
    # Optional: specify a specific version/branch
    # ref: v1.0.0  # Use a specific release tag
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
```

Press the "Print Device Mappings" button to see discovered devices in the logs.

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

- `📦 Frame 09 - Device Identity:` - Device barcode information
- `New device discovered:` - Power data from new devices
- `Assigned sensor index X to device YYYY` - Device mapping assignments

### Troubleshooting Logs

- `No Frame 09 barcode available for device XXXX` - Device found but no barcode yet
- `Cannot create node entry - node table is full` - Increase `number_of_devices`
- `Maximum number of devices reached` - Adjust configuration limit

## 🏠 Home Assistant Integration

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
│   ├── __init__.py          # Component initialization
│   ├── button.py            # Management button platform
│   ├── sensor.py            # Sensor platform configuration
│   ├── tigo_monitor.cpp      # Main component implementation
│   └── tigo_monitor.h        # Component header file
├── examples/                # Configuration examples
├── tigo-server.yaml         # Main configuration file
└── README.md               # This file
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

### Common Issues

1. **No devices discovered**
   - Check UART wiring and baud rate
   - Verify Tigo system is communicating
   - Check ESPHome logs for Frame detection

2. **Devices discovered but no barcodes**
   - Frame 09 data may not be transmitted by all systems
   - Devices will work without barcodes (shows as "mod#XXXX")

3. **Energy data resets on reboot**
   - Component automatically saves energy data to flash
   - Check logs for "Restored total energy" messages

4. **Too many devices**
   - Increase `number_of_devices` in configuration
   - Monitor memory usage in compilation output

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

- **Memory Usage**: ~11% RAM, ~49% Flash (typical ESP32)
- **Update Rate**: 30-60 seconds recommended for normal operation
- **Device Limit**: Tested with up to 20 devices
- **Persistence**: Energy data saved every 10 updates to reduce flash wear

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
- **[willglynn/taptap](https://github.com/willglynn/taptap)** - Innovative approach to Tigo system monitoring and reverse engineering insights
- **[tictactom/tigo_server](https://github.com/tictactom/tigo_server)** - Additional Tigo monitoring implementation and protocol documentation
- **ESPHome Framework** - Providing the robust platform for ESP32-based home automation components
- **Home Assistant Community** - Inspiration and integration patterns for energy monitoring solutions

Special thanks to all the developers who contributed to understanding and documenting the Tigo communication protocols through their open-source work.

## 📞 Support

For issues and questions:

1. Check the troubleshooting section above
2. Review ESPHome logs for error messages
3. Open an issue on the project repository
4. Include your configuration and relevant log excerpts

---

**Happy Solar Monitoring! ☀️**
