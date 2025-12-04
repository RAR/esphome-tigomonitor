# ESPHome Tigo Monitor Component

An ESPHome component for monitoring Tigo solar power optimizers via UART communication. Enables real-time monitoring of individual Tigo devices with system-wide power and energy tracking through Home Assistant.

## 🌟 Features

### Core Monitoring
- **ESP-IDF Framework**: Built specifically for ESP-IDF for optimal performance and reliability
- **Individual Device Monitoring**: Track voltage, current, power, temperature, and RSSI for each Tigo optimizer
- **System Aggregation**: Total system power, energy production (kWh), and active device count
- **Device Discovery**: Automatic detection and persistent mapping of Tigo devices
- **Barcode Identification**: Device identification using Frame 27 barcode data
- **Power Efficiency Analytics**: Conversion efficiency, power factor, duty cycle, and load factor metrics
- **Device Information**: Firmware version and device info extraction from string responses
- **Energy Dashboard Integration**: Compatible with Home Assistant's Energy Dashboard
- **Persistent Storage**: Device mappings and energy data survive reboots

### Web Interface
- **Built-in Web Dashboard**: Custom web server with 5 comprehensive pages
  - **Dashboard**: Real-time overview with system stats and live device metrics
    - Total Power, Current, Energy (kWh), Efficiency, Temperature, Active Devices
    - Temperature unit toggle (°F/°C) with persistent preference
    - **String-Grouped Layout**: Devices organized by CCA strings with summary cards
    - String summary cards show: Total Power, Peak Power, Avg Efficiency, Device Count
    - Visual hierarchy with gradient headers for each string/inverter
    - Device cards with CCA-friendly names and real-time metrics
    - Historical peak power tracking per device
    - Dark mode support with persistent preference
  - **Node Table**: Complete device registry with CCA labels and hierarchy
    - Individual node deletion capability
    - CCA validation badges
  - **ESP32 Status**: System health, memory usage, task count, and uptime
    - Remote restart button with confirmation
  - **YAML Config Generator**: Automatic sensor configuration generation
  - **CCA Info**: Tigo CCA device information and status monitoring
    - Manual refresh button for on-demand updates
- **Mobile Responsive**: Optimized layouts for desktop and mobile devices
- **Auto-refresh**: Live updates without page reloads (5 second intervals)
- **No External Dependencies**: Runs entirely on the ESP32

### CCA Integration
- **Automatic CCA Sync**: Query Tigo CCA for panel configuration data
- **Panel Name Mapping**: Automatically label panels with CCA-assigned names
- **Hierarchy Display**: Shows MPPT → String → Panel relationships
- **Barcode Matching**: Fuzzy matching between UART devices and CCA configuration
- **Sync on Boot**: Optional automatic synchronization on startup
- **Manual Sync Button**: On-demand CCA configuration refresh
- **Device Validation**: Visual indicators for CCA-validated devices
- **String Aggregation**: Real-time aggregated metrics per string
  - Per-string power, peak power, current, voltage, temperature
  - Per-string efficiency (average, min, max)
  - Active device count per string
- **Persistent Metadata**: CCA configuration survives reboots

### Advanced Features
- **Flash Wear Optimization**: Energy data saved hourly (24 writes/day) for extended flash lifespan
- **Peak Power Tracking**: Historical maximum power per device with flash persistence
- **OTA/Shutdown Data Persistence**: Automatic data save before OTA updates or restarts
- **String-Level Analytics**: Aggregated metrics for each string with API endpoint
- **Night Mode**: Automatic zero publishing when no data received (prevents stale data at night)
- **UART Optimization**: ISR in IRAM for reduced packet loss
- **Flexible Configuration**: Support for individual sensors or combined device sensors
- **Management Tools**: Built-in buttons for YAML generation and device management
- **OTA Updates**: Over-the-air firmware updates
- **RESTful API**: JSON endpoints for all system and device data
  - `/api/devices` - Individual device metrics with string labels
  - `/api/overview` - System-wide aggregates
  - `/api/strings` - Per-string aggregated data
  - `/api/nodes` - Node table with CCA metadata
  - `/api/status` - ESP32 system status
  - `/api/yaml` - Generated YAML configuration
  - `/api/cca` - CCA connection info and device data
  - `/api/restart` - Remote system restart
  - `/api/health` - Health check endpoint (no auth required)

## 📋 Requirements

- **ESP32 development board with PSRAM strongly recommended**
  - **Recommended:** ESP32-S3 with PSRAM (e.g., ESP32-S3-DevKitC-1-N8R8)
  - The web interface and JSON APIs allocate data in PSRAM when available, significantly reducing internal RAM pressure
  - Without PSRAM, the system will use internal RAM which may cause stability issues with many devices or web interface usage
  - **Small systems (< 10 devices):** ESP32 or ESP32-S3 without PSRAM may work but is not recommended
  - **Medium to large systems (10+ devices):** ESP32-S3 with PSRAM **required** for stability
  - **Tested:** M5Stack AtomS3 (no PSRAM, works up to ~15-20 devices with web server)
  - **Recommended:** [M5Stack AtomS3R](https://docs.m5stack.com/en/core/AtomS3R) (8MB PSRAM, supports 36+ devices)
- UART connection to Tigo communication system (38400 baud)
- ESPHome 2025.10.3 or newer
- Home Assistant (optional, for full integration)
- Tigo CCA (Cloud Connect Advanced) - optional, for panel name auto-labeling

**Memory Requirements:**
- **Without PSRAM (<15 devices):** Minimum 30-40KB free heap needed for web server + CCA queries
- **With PSRAM (15+ devices):** 8MB additional RAM, eliminates memory constraints

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

Connect your ESP32 to the Tigo communication system via RS485:

**Recommended Hardware:**
- **M5Stack AtomS3R** (8MB PSRAM) for 15+ devices
- **M5Stack Atomic RS485 Base** ([link](https://docs.m5stack.com/en/atom/Atomic%20RS485%20Base)) - provides easy RS485 connection with built-in level shifters

**UART Connection:**
- **TX Pin**: GPIO6 (connects to Tigo RX via RS485)
- **RX Pin**: GPIO5 (connects to Tigo TX via RS485)
- **Baud Rate**: 38400
- **Data**: 8 bits, No parity, 1 stop bit

**Note:** The Tigo communication system uses RS485. The Atomic RS485 Base provides the necessary level shifting and makes connection straightforward.

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
  tx_pin: GPIO6
  rx_pin: GPIO5
  baud_rate: 38400
  data_bits: 8
  parity: NONE
  stop_bits: 1

# Tigo Monitor component
tigo_monitor:
  id: tigo_hub
  uart_id: tigo_uart
  update_interval: 30s
  number_of_devices: 20  # Set to your actual number of Tigo devices
  cca_ip: "192.168.1.100"  # Optional: IP address of your Tigo CCA
  sync_cca_on_startup: true  # Optional: Auto-sync CCA config on boot (default: true)
  
  # Optional: Group MPPTs by inverter for organized dashboard view
  inverters:
    - name: "Inverter 1"
      mppts:
        - "MPPT 1"
        - "MPPT 2"
    - name: "Inverter 2"
      mppts:
        - "MPPT 3"
        - "MPPT 4"

# Optional: Web Server for monitoring dashboard
tigo_server:
  tigo_monitor_id: tigo_hub
  port: 80  # Default web server port
```

**Inverter Grouping**: When inverters are configured, the web dashboard automatically displays a hierarchical view:
- Inverters at the top level with aggregate stats
- MPPTs grouped under their assigned inverter
- Strings and panels nested under each MPPT
- Visual indentation and colored headers for easy navigation

Without inverter configuration, the dashboard shows MPPTs at the top level.

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

### 3. PSRAM Configuration (Required for 15+ Devices)

**PSRAM is required when monitoring 15 or more Tigo devices.** The M5Stack AtomS3R has 8MB of PSRAM (AtomS3 does not have PSRAM).

**For ESP32-S3 with PSRAM (M5Stack AtomS3R):**

```yaml
esphome:
  name: tigo-monitor
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

**Benefits of PSRAM:**
- **Required for 15+ devices** - eliminates memory constraints
- **8MB additional RAM** for HTTP buffers, JSON parsing, and web server
- **Prevents socket creation failures** from memory exhaustion
- **Better performance** with concurrent web requests and CCA queries

The component automatically uses PSRAM when available for HTTP buffers, JSON parsing, and web server operations, falling back to regular heap if PSRAM is not present.

## 🔧 Configuration Options

### Tigo Monitor Component

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `uart_id` | ID | Required | UART component ID for communication |
| `update_interval` | Time | 60s | How often to publish sensor data |
| `number_of_devices` | Integer | 5 | Maximum number of Tigo devices to track |
| `cca_ip` | String | None | IP address of Tigo CCA for automatic panel labeling |
| `sync_cca_on_startup` | Boolean | true | Automatically sync CCA configuration on boot |
| `time_id` | ID | None | Optional time component for midnight reset feature |
| `reset_at_midnight` | Boolean | false | Reset peak power and total energy at midnight (requires time_id) |
| `power_calibration` | Float | 1.0 | Power calibration multiplier (range: 0.5-2.0) |
| `night_mode_timeout` | Integer | 60 | Minutes without data before entering night mode (range: 1-1440) |
| `inverters` | List | None | Optional inverter grouping configuration (see below) |

#### Inverter Grouping

You can optionally define inverters and manually assign MPPTs to each inverter for organized display in the web UI and `/api/inverters` endpoint. This is useful when you have multiple inverters with MPPTs that you want to group and track separately.

**Configuration:**

```yaml
tigo_monitor:
  id: tigo_hub
  uart_id: uart_bus
  number_of_devices: 36
  inverters:
    - name: "Inverter 1"
      mppts:
        - "MPPT 1"
        - "MPPT 2"
    - name: "Inverter 2"
      mppts:
        - "MPPT 3"
        - "MPPT 4"
```

**Notes:**
- MPPT labels must match the labels from CCA (e.g., "MPPT 1", "MPPT 2")
- The web UI will group strings by inverter when configured
- `/api/inverters` endpoint provides aggregated power, peak, and device counts per inverter
- Inverter grouping is optional - if not configured, the UI shows MPPTs individually
- Each inverter shows total power, peak power, and device counts across all its MPPTs

#### Midnight Reset Feature

To enable automatic daily reset of peak power and total energy sensors at midnight:

1. Configure a time component (e.g., HomeAssistant time or SNTP)
2. Reference it in the tigo_monitor component using `time_id`
3. Set `reset_at_midnight: true` on the tigo_monitor component

**Example:**

```yaml
time:
  - platform: homeassistant
    id: ha_time

tigo_monitor:
  id: tigo_hub
  uart_id: uart_bus
  number_of_devices: 20
  time_id: ha_time
  reset_at_midnight: true  # Resets all peak power and total energy at midnight

sensor:
  # This will reset to 0 every day at midnight
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Daily Energy Production"
  
  # Individual device peak power - also resets at midnight
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    address: "1234"
    name: "Panel 1"
    peak_power:
```

**Notes:**
- Without a time component, the reset feature cannot be enabled
- Reset happens when the component detects a day change (based on day_of_year)
- All peak power sensors and total energy sensor reset simultaneously
- Values are published and saved to persistent storage after reset
- Useful for tracking daily production totals

#### Power Calibration

If total power readings differ from your inverter or other monitoring sources, you can apply a calibration multiplier:

```yaml
tigo_monitor:
  id: tigo_hub
  uart_id: uart_bus
  number_of_devices: 20
  power_calibration: 1.05  # Increase all power readings by 5%
```

**Notes:**
- Accepts values from 0.5 to 2.0 (50% to 200%)
- Applied to all power calculations: individual device power, string power, total power
- Multiplier affects energy calculations as well
- Default: 1.0 (no adjustment)

#### Night Mode Timeout

Customize how long to wait without receiving data before entering night mode (when zeros are published to prevent stale data):

```yaml
tigo_monitor:
  id: tigo_hub
  uart_id: uart_bus
  number_of_devices: 20
  night_mode_timeout: 90  # Wait 90 minutes before entering night mode
```

**Notes:**
- Accepts values from 1 to 1440 minutes (1 minute to 24 hours)
- Default: 60 minutes (1 hour)
- When triggered, publishes zero values every 10 minutes until data resumes
- Useful for systems with intermittent communication or extended shadow periods
- Night mode binary sensor indicates current state

### Tigo Web Server Component

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `tigo_monitor_id` | ID | Required | Reference to tigo_monitor component |
| `port` | Integer | 80 | HTTP port for web interface |
| `api_token` | String | None | Bearer token for API authentication (optional) |
| `web_username` | String | None | Username for HTTP Basic Auth on web pages (optional) |
| `web_password` | String | None | Password for HTTP Basic Auth on web pages (optional) |

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

### API Authentication

The web interface supports optional API token authentication to secure access to all `/api/*` endpoints:

```yaml
tigo_server:
  tigo_monitor_id: tigo_hub
  port: 80
  api_token: "your-secret-token-here"  # Optional - if omitted, API is open
```

**Features:**
- **Optional**: If no token is configured, API endpoints remain open (backward compatible)
- **Bearer Token**: Uses standard `Authorization: Bearer <token>` header format
- **All API Endpoints**: Protects all `/api/*` endpoints including:
  - `/api/devices`, `/api/overview`, `/api/strings`
  - `/api/nodes`, `/api/status`, `/api/yaml`, `/api/cca`
  - `/api/restart`, `/api/reset_peak_power`, `/api/cca_refresh`, `/api/node_delete`
- **Exception**: `/api/health` does not require authentication (for monitoring systems)
- **Web Pages Unaffected**: Dashboard and other HTML pages remain accessible
- **401 Unauthorized**: Returns proper HTTP 401 with JSON error when token is missing/invalid

**Health Check Endpoint:**

The `/api/health` endpoint is always accessible without authentication for use with monitoring and health check systems:

```bash
# Health check - no authentication required
curl http://10.75.0.45/api/health
# Returns: {"status":"ok","uptime":12345,"heap_free":245760,"heap_min_free":198432}
```

Response includes:
- `status`: Always "ok" if responding
- `uptime`: Uptime in seconds
- `heap_free`: Current free heap memory in bytes
- `heap_min_free`: Minimum free heap since boot in bytes

**Usage Examples:**

```bash
# Without authentication (if api_token not configured)
curl http://10.75.0.45/api/devices

# With authentication
curl -H "Authorization: Bearer your-secret-token-here" http://10.75.0.45/api/devices

# Test authentication
curl http://10.75.0.45/api/status
# Returns: {"error":"Authorization required"}

curl -H "Authorization: Bearer wrong-token" http://10.75.0.45/api/status
# Returns: {"error":"Invalid token"}
```

**Security Recommendations:**
- Use a strong, randomly generated token (e.g., 32+ characters)
- Avoid using the token in URLs (use headers only)
- Use HTTPS if exposing the API outside your local network
- Rotate tokens periodically

### Web Page Authentication

The web interface supports optional HTTP Basic Authentication to protect access to all HTML pages:

```yaml
tigo_server:
  tigo_monitor_id: tigo_hub
  port: 80
  web_username: "admin"              # Optional - username for web pages
  web_password: "your-password"      # Optional - password for web pages
  api_token: "your-api-token"        # Optional - separate token for API
```

**Features:**
- **HTTP Basic Auth**: Standard browser authentication (username/password prompt)
- **Optional**: If not configured, web pages remain open (backward compatible)
- **Protected Pages**: All HTML pages require authentication:
  - `/` (Dashboard), `/nodes` (Node Table), `/status` (ESP Status)
  - `/yaml` (YAML Config), `/cca` (CCA Info)
- **API Separate**: API endpoints use Bearer token authentication (see API Authentication section)
- **Browser Caching**: Credentials cached per browser session (no repeated prompts)
- **Logout**: Clear browser cache/cookies or restart browser to logout

**Use Cases:**
- **Web + API Auth**: Protect both web interface and API with different credentials
  ```yaml
  tigo_server:
    web_username: "admin"
    web_password: "web-password"
    api_token: "api-secret-token"
  ```
  - Browser users: Prompted for username/password once per session
  - API clients: Must send `Authorization: Bearer api-secret-token` header

- **Web Only**: Protect web pages, leave API open for programmatic access
  ```yaml
  tigo_server:
    web_username: "admin"
    web_password: "secure-password"
    # No api_token = API remains open
  ```

- **API Only**: Protect API, leave web pages open (previous behavior)
  ```yaml
  tigo_server:
    api_token: "api-secret-token"
    # No web_username/web_password = pages remain open
  ```

**Security Recommendations:**
- Use strong passwords (10+ characters, mixed case, numbers, symbols)
- Different credentials for web and API access
- Change default passwords immediately
- Consider using HTTPS for production deployments
- Limit access via firewall rules to trusted networks

**Browser Experience:**
- First visit prompts for username/password
- Credentials stored in browser session
- No login page needed - standard HTTP Basic Auth dialog
- Works on all browsers and mobile devices

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
  
  # UART diagnostic counters (optional)
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Invalid Checksum Count"
  
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    name: "Missed Packet Count"
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
    peak_power: {}
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

binary_sensor:
  # Night mode indicator
  - platform: tigo_monitor
    tigo_monitor_id: tigo_hub
    night_mode:
      name: "Solar Night Mode"
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
- **Hierarchy Display**: MPPT → String → Panel structure
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
- **Hierarchy Mapping**: Shows MPPT → String → Panel relationships
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

- `Frame 27 received` - Long address (16-char barcode) discovery
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
   - Look for "Frame" messages in logs

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
   - **Solution:** Use ESP32-S3 with PSRAM (e.g., M5Stack AtomS3R) for 10+ devices

8. **Socket creation failures / Memory exhaustion**
   - Symptom: `Failed to create socket` errors in logs, web interface crashes, or system instability
   - Cause: Internal RAM fragmentation from repeated allocations without PSRAM
   - **Immediate fix:** Reboot ESP32 to temporarily clear memory fragmentation
   - **Permanent solution:** Upgrade to ESP32-S3 with PSRAM (e.g., M5Stack AtomS3R)
   - **Note:** Without PSRAM, JSON builders and web interface allocate in internal RAM causing fragmentation over time
   - With PSRAM, all buffers use PSRAM eliminating internal RAM pressure

9. **Night mode / Zero publishing**
   - Component automatically enters night mode after 1 hour of no data
   - Publishes zeros every 10 minutes to prevent stale data in Home Assistant (temperatures show as unavailable)
   - Binary sensor available to track night mode state
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

### Memory Usage
- **With PSRAM (Strongly Recommended):**
  - Base: ~10% internal RAM + 8MB PSRAM
  - With Web Server: ~12% internal RAM + PSRAM for all buffers and JSON data
  - **Capacity:** 36+ devices tested successfully
  - **Stability:** Web interface and JSON APIs allocate data in PSRAM, preventing internal RAM fragmentation
  - **No socket creation issues** - stable under concurrent web requests

- **Without PSRAM (Not Recommended):**
  - Base: ~10-11% RAM, ~49% Flash
  - With Web Server: ~15-20% RAM (highly variable)
  - **Limitation:** May work for small systems (< 10 devices) but not recommended
  - **Stability Issues:** Internal RAM allocations cause fragmentation over time
  - **Risk:** Socket creation failures, web interface crashes, memory exhaustion
  - **Note:** The system will function but may become unstable with web interface usage

### Performance Metrics
- **Update Rate**: 30-60 seconds recommended for normal operation
- **Device Limits**: 
  - **With PSRAM (Recommended):** 36+ devices tested successfully
  - Without PSRAM: Up to ~10 devices 
  - **Strong Recommendation:** Use ESP32-S3 with PSRAM (e.g., M5Stack AtomS3R) for all deployments
- **PSRAM Optimization**: All HTTP buffers, JSON data, HTML pages, and web server operations use PSRAM when available, automatically falling back to internal RAM if not present
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

---

## ⚖️ Trademark Disclaimer

All trademarks, logos, and brand names are the property of their respective owners. All company, product, and service names used in this project are for identification purposes only. Use of these names, trademarks, and brands does not imply endorsement.
