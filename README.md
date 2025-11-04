# ESPHome Tigo Monitor Component

A comprehensive ESPHome component for monitoring Tigo solar power optimizers via UART communication. This component enables real-time monitoring of individual Tigo devices and provides system-wide power and energy tracking through Home Assistant.

## 🌟 Features

- **Individual Device Monitoring**: Track voltage, current, power, temperature, and RSSI for each Tigo optimizer
- **System Aggregation**: Total system power, energy production (kWh), and active device count
- **Device Discovery**: Automatic detection and persistent mapping of Tigo devices
- **Barcode Identification**: Device identification using Frame 09 barcode data
- **Energy Dashboard Integration**: Compatible with Home Assistant's Energy Dashboard
- **Persistent Storage**: Device mappings and energy data survive reboots
- **Flexible Configuration**: Support for individual sensors or combined device sensors
- **Management Tools**: Built-in buttons for YAML generation and device management

## 📋 Requirements

- ESP32 development board
- UART connection to Tigo communication system (38400 baud)
- ESPHome 2025.10.3 or newer
- Home Assistant (optional, for full integration)

## 🛠️ Installation

### 1. Download the Component

Clone or download this repository to your ESPHome configuration directory:

```bash
git clone https://github.com/your-repo/esphome-tigomonitor.git
cd esphome-tigomonitor
```

### 2. Hardware Setup

Connect your ESP32 to the Tigo communication system:

- **TX Pin**: GPIO1 (connects to Tigo RX)
- **RX Pin**: GPIO3 (connects to Tigo TX)
- **Baud Rate**: 38400
- **Data**: 8 bits, No parity, 1 stop bit

### 3. Basic Configuration

Create or update your ESPHome YAML configuration:

```yaml
esphome:
  name: tigo-monitor
  friendly_name: "Tigo Monitor"

esp32:
  board: esp32dev
  framework:
    type: arduino

# External components
external_components:
  - source: 
      type: local
      path: components

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

text_sensor:
  # Device barcode/serial number
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "Tigo Device 1"
    barcode: {}
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

## 🐛 Troubleshooting

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

- **Memory Usage**: ~12% RAM, ~54% Flash (typical ESP32)
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