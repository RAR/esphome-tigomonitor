# Tigo Monitor Custom Web Server

This custom web UI provides a comprehensive dashboard for monitoring your Tigo solar optimizer system. Built using ESP-IDF's HTTP server component, it runs independently from ESPHome's built-in web server.

## Features

### 📊 Dashboard (`/`)
- Real-time monitoring of all connected solar panels
- Live metrics for each device:
  - Power output (W)
  - Input/Output voltage (V)
  - Current (A)
  - Temperature (°C)
  - Efficiency (%)
  - Duty cycle (%)
  - RSSI signal strength (dBm)
- Auto-refreshes every 5 seconds
- System-wide statistics (total power, active devices, averages)

### 📈 Overview (`/overview`)
- System-wide aggregated metrics
- Total power output
- Total current
- Average efficiency across all devices
- Average temperature
- Active device count

### 🗂️ Node Table (`/nodes`)
- Complete device mapping table
- Shows all discovered devices
- Frame 27 long addresses (16-char, primary barcode)
- Frame 09 barcodes (6-char, fallback)
- Sensor index assignments
- Device checksums

### 💾 ESP32 Status (`/status`)
- System information
  - Uptime (days, hours, minutes)
  - ESPHome version
  - Compilation timestamp
- Memory usage
  - Heap memory (free/total with visual progress bar)
  - PSRAM (if available)
- Real-time system health monitoring

### ⚙️ YAML Configuration (`/yaml`)
- Auto-generated ESPHome YAML configuration
- Based on discovered devices
- Copy-to-clipboard functionality
- Ready to paste into your ESPHome config
- Updates as new devices are discovered

## Configuration

Add the web server to your ESPHome YAML configuration:

```yaml
tigo_monitor:
  id: tigo_hub
  number_of_devices: 20
  update_interval: 30s
  web_server:
    port: 80  # Optional, defaults to 80
```

## API Endpoints

The web server provides RESTful JSON APIs for all data:

- `GET /api/devices` - All device data with metrics
- `GET /api/overview` - System overview statistics
- `GET /api/nodes` - Node table mappings
- `GET /api/status` - ESP32 system status
- `GET /api/yaml` - Generated YAML configuration

All API endpoints support CORS for external access.

## Technical Details

### Framework
- **HTTP Server**: ESP-IDF native HTTP server (`esp_http_server`)
- **Framework**: ESP-IDF only (not Arduino)
- **Port**: Configurable (default 80)
- **Auto-refresh**: JavaScript-based polling (5-10 seconds)

### Resources
- **Stack size**: 8KB per request
- **Max URI handlers**: 16 routes
- **Memory**: Optimized for ESP32 with minimal overhead

### Design
- **Responsive**: Mobile-friendly responsive design
- **Modern UI**: Clean, professional interface
- **Real-time**: Auto-updating data without page refresh
- **Fast**: Minimal JavaScript, no external dependencies

## Usage

1. **Enable in YAML**: Add `web_server:` section to your `tigo_monitor` configuration
2. **Flash firmware**: Upload to your ESP32
3. **Connect**: Navigate to `http://<esp32-ip-address>` in your browser
4. **Monitor**: View real-time data from all your Tigo optimizers

## Pages Overview

### Dashboard
Perfect for daily monitoring with at-a-glance metrics for all panels.

### Overview  
Bird's-eye view of entire system performance and statistics.

### Node Table
Technical view showing device discovery and mapping details.

### ESP32 Status
Hardware health monitoring and system diagnostics.

### YAML Config
Quick setup assistant - generates configuration automatically.

## Notes

- Web server runs on ESP32's WiFi interface
- No external web server required
- All processing happens on-device
- Lightweight and fast
- Perfect for local network monitoring
- Can be accessed from any device on your network

## Example URLs

If your ESP32 IP is `192.168.1.100`:

- Dashboard: `http://192.168.1.100/`
- Overview: `http://192.168.1.100/overview`
- Nodes: `http://192.168.1.100/nodes`
- Status: `http://192.168.1.100/status`
- YAML: `http://192.168.1.100/yaml`
- API: `http://192.168.1.100/api/devices`

## Browser Compatibility

Works with all modern browsers:
- Chrome/Edge
- Firefox
- Safari
- Mobile browsers

No plugins or extensions required!
