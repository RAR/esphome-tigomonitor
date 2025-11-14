# Board Configuration Files

This directory contains optimized ESPHome configurations for different ESP32 boards used with the Tigo Monitor system.

## Available Configurations

### ESP32-S3 Boards

#### `esp32s3-atoms3.yaml` - M5Stack AtomS3 (No PSRAM)
- **Board**: M5Stack AtomS3
- **PSRAM**: 8MB (Octal)
- **CPU**: 240MHz
- **Recommended for**: Standard installations (up to ~30 devices)
- **UART Buffers**: 2048 RX / 512 TX

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

### Standard ESP32 Boards

#### `esp32-dev.yaml` - Generic ESP32 DevKit (No PSRAM)
- **Board**: Generic ESP32 DevKit
- **PSRAM**: None
- **CPU**: 240MHz
- **Recommended for**: Small installations (up to ~12 devices)
- **UART Buffers**: 1024 RX / 256 TX
- **Note**: Limited by available heap memory

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

All configurations assume the standard Tigo UART connection:
- **TX Pin**: GPIO1
- **RX Pin**: GPIO3
- **Baud Rate**: 38400

Adjust these in your main configuration if your hardware differs.

## Performance Notes

- **ESP32-P4** offers the best performance with massive PSRAM and higher CPU frequency
- **ESP32-S3** variants are the sweet spot for most installations
- **Standard ESP32** is adequate for small systems but may struggle with web server + many devices

## Customization

Feel free to copy and modify these configurations for your specific needs. Key parameters to adjust:
- `number_of_devices`: Maximum devices to track
- `update_interval`: How often to poll sensors (default: 30s)
- Buffer sizes: Increase if seeing missed packets, decrease to save memory
