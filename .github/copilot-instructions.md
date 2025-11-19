# ESPHome Tigo Monitor AI Agent Instructions

## Architecture Overview

This is an **ESPHome custom component** for monitoring Tigo solar optimizers via RS485/UART. Two main C++ components work together:

- **`tigo_monitor/`**: UART frame parser, device tracking, sensor publishing, CCA sync
- **`tigo_server/`**: ESP-IDF HTTP web server with 5 pages + RESTful JSON API

**Critical: ESP-IDF framework required** (not Arduino). All async operations use FreeRTOS tasks/semaphores.

## Memory Architecture: PSRAM is King

**PSRAM usage is fundamental** - not optional for 15+ devices:

```cpp
// PSRAM-backed containers (see tigo_monitor.h lines 1-90)
psram_vector<DeviceData> devices_;           // Device list in PSRAM
psram_map<std::string, NodeInfo> node_table_; // Barcode mapping in PSRAM
PSRAMString json_buffer;                      // API responses in PSRAM
```

**Always use PSRAM containers** for large data structures. Internal RAM is <200KB; PSRAM is 8MB. Web server allocates all JSON/HTML in PSRAM via `PSRAMString` class.

## Configuration Validation Pattern

Sensors use **keyword-based type inference** (see `sensor.py` lines 290-330):

```python
# Sensor type detected from name keywords
has_energy_keywords = any(keyword in sensor_name for keyword in ["energy", "kwh"])
has_frame_keywords = any(keyword in sensor_name for keyword in ["frame", "missed"])
# Returns appropriate schema: ENERGY_SUM_CONFIG_SCHEMA, MISSED_FRAME_CONFIG_SCHEMA, etc.
```

**When adding new sensor types**, update both keyword detection and schema mapping.

## Code Organization Rules

### 1. Python Config (\_\_init\_\_.py, sensor.py, button.py)
- **Schema validation only** - no runtime logic
- Use `cv.float_range()` for bounded floats (e.g., `power_calibration: 0.5-2.0`)
- Keyword-based sensor type inference (see pattern above)

### 2. C++ Headers (tigo_monitor.h, tigo_web_server.h)
- **Setter methods** for config values: `set_power_calibration(float)`
- **Getter methods** for cached data: `get_total_power()`, `get_device_count()`
- Use forward declarations to avoid circular includes

### 3. C++ Implementation (tigo_monitor.cpp, tigo_web_server.cpp)
- **Frame processing**: `process_frame()` → `process_power_frame()` / `process_09_frame()` / `process_27_frame()`
- **Sensor publishing**: Apply `power_calibration_` multiplier to ALL power calculations
- **Web handlers**: Static methods with `httpd_req_t*` parameter

## Development Workflows

### Building & Flashing
```bash
# From workspace root
esphome run tigo-monitor.yaml

# Compile only (check for errors)
esphome compile tigo-monitor.yaml
```

**Common compile errors:**
- Missing `get_*()` method after renaming → Update web server API calls
- Undefined PSRAM types → Add `#ifdef USE_ESP_IDF` guards
- `'class X' has no member 'Y'` → Check for stale method names after refactoring

### Testing Changes
1. **Sensor changes**: Check Home Assistant entities update correctly
2. **Web UI changes**: Test both light/dark modes, mobile responsive
3. **CCA sync**: Verify barcode fuzzy matching works (see `match_barcode()`)
4. **Memory leaks**: Monitor heap before/after midnight reset (see CHANGELOG 1.2.0)

### UART Optimization Context
- **RX buffer size**: 8192 bytes for 30+ devices (see `tigo-monitor.yaml` line 15)
- **ISR in IRAM**: `CONFIG_UART_ISR_IN_IRAM: "y"` reduces frame loss (see `UART_OPTIMIZATION.md`)
- **Processing rate**: `MAX_BYTES_PER_LOOP = 4096` (doubled from 2048 to handle display overhead)
- **Typical miss rate**: 0.02-0.04% is excellent for multi-drop RS485

## Common Patterns

### Adding a Power Calculation
**Always apply `power_calibration_` multiplier**:

```cpp
// WRONG
float power = voltage_out * current_in;

// CORRECT (see tigo_monitor.cpp line 1151, 1223, 1273, etc.)
float power = voltage_out * current_in * power_calibration_;
```

Applied at: individual sensors, string aggregation, web API, power sum calculation.

### Adding Web UI Features
1. **HTML page** in `tigo_web_server.cpp` (see dashboard: lines 1800-2100)
2. **JSON API endpoint** for data (see `api_devices_handler()`)
3. **Register handler** in `setup()` with `httpd_register_uri_handler()`
4. **Use PSRAM**: `PSRAMString html; html.reserve(50000);`
5. **Add auth check**: `if (!check_web_auth(req)) return ...;`

### String Grouping Pattern
CCA provides MPPT/string hierarchy:

```cpp
// Node structure (tigo_monitor.h line 145)
struct NodeInfo {
  std::string inverter_name;  // e.g., "South Inverter"
  std::string mppt_label;     // e.g., "MPPT1"
  std::string panel_name;     // e.g., "A1" (from CCA)
  // ...
};
```

Web dashboard groups devices by `mppt_label`, shows aggregated metrics per string.

### Persistent Data Pattern
Use `esphome::ESPPreferences` for flash storage (see `save_peak_power_data()`):

```cpp
// Reuse string buffer to prevent memory leaks (CHANGELOG 1.2.0)
static std::string pref_key;
pref_key.reserve(32);
for (auto &device : devices_) {
  pref_key = "peak_"; pref_key += device.addr;  // Reuse, not allocate
  prefs.put_float(pref_key.c_str(), device.peak_power);
}
```

**Avoid**: `std::string pref_key = "peak_" + device.addr;` in loops (causes heap fragmentation).

## Web Server Architecture

Five HTML pages + RESTful API:
- `/` - Dashboard (string-grouped device cards)
- `/nodes` - Node table (barcode mapping)
- `/status` - ESP32 diagnostics
- `/yaml` - Config generator
- `/cca` - CCA device info

**API endpoints** (all return JSON):
- `/api/devices` - Device metrics with string labels
- `/api/overview` - System aggregates
- `/api/strings` - Per-string aggregated data
- `/api/status` - ESP32 status
- `/api/health` - Health check (no auth)

**Auth pattern**: `api_token` for API, `web_username`/`web_password` for HTML pages (HTTP Basic).

## CCA Integration

**Fuzzy barcode matching** (see `match_barcode()` in tigo_monitor.cpp):
1. UART discovers devices via Frame 27 (16-char long address)
2. CCA provides panel config with barcodes (may differ slightly)
3. Fuzzy match: check last 6 chars, allow 1-char difference
4. On match: populate `inverter_name`, `mppt_label`, `panel_name`

**Sync flow**: `sync_cca_on_startup: true` → HTTP GET to CCA → parse JSON → `match_and_populate_devices()`

## File Change Impact Map

- **`__init__.py`**: Requires ESPHome recompile, affects all YAML configs
- **`sensor.py`**: Schema changes require YAML updates, keyword changes affect sensor detection
- **`tigo_monitor.cpp`**: Frame parsing changes affect all data accuracy
- **`tigo_web_server.cpp`**: Web UI only, safe to iterate quickly
- **`CHANGELOG.md`**: Update for all user-facing changes

## When Things Break

**"has no member" compile error** → Check for renamed methods (e.g., `missed_packet` → `missed_frame`)

**High frame miss rate (>0.1%)** → Check `CONFIG_UART_RX_BUFFER_SIZE`, increase `MAX_BYTES_PER_LOOP`

**Memory leak** → Look for string allocations in loops, use static buffers (see CHANGELOG 1.2.0)

**Web UI not updating** → Check JavaScript uses correct JSON field names (e.g., `data.missed_frames`)

**CCA sync fails** → Check CCA IP reachable, verify JSON format matches `parse_cca_data()`

## Common Refactoring Patterns

### 1. Renaming Methods Across Components

When renaming methods in `tigo_monitor.h`, update all call sites systematically:

```cpp
// Step 1: Update header (tigo_monitor.h)
- uint32_t get_missed_packet_count() const { return missed_packet_count_; }
+ uint32_t get_missed_frame_count() const { return missed_frame_count_; }

// Step 2: Update member variable
- uint32_t missed_packet_count_ = 0;
+ uint32_t missed_frame_count_ = 0;

// Step 3: Find all usages - grep is your friend
grep -r "get_missed_packet_count" components/

// Step 4: Update web server calls (tigo_web_server.cpp)
- uint32_t missed = parent_->get_missed_packet_count();
+ uint32_t missed = parent_->get_missed_frame_count();

// Step 5: Update Python config (sensor.py)
- CONF_MISSED_PACKET = "missed_packet"
+ CONF_MISSED_FRAME = "missed_frame"
```

**Use bulk rename tools**: `sed -i 's/missed_packet/missed_frame/g' file.cpp` for mechanical changes.

### 2. Adding New Configuration Parameters

Follow the three-file pattern:

```python
# 1. Python config (__init__.py)
CONF_POWER_CALIBRATION = 'power_calibration'

CONFIG_SCHEMA = cv.Schema({
    cv.Optional(CONF_POWER_CALIBRATION, default=1.0): cv.float_range(min=0.5, max=2.0),
})

@coroutine
def to_code(config):
    cg.add(var.set_power_calibration(config[CONF_POWER_CALIBRATION]))
```

```cpp
// 2. Header (tigo_monitor.h)
void set_power_calibration(float multiplier) { power_calibration_ = multiplier; }
float get_power_calibration() const { return power_calibration_; }

protected:
  float power_calibration_ = 1.0f;
```

```cpp
// 3. Apply in implementation (tigo_monitor.cpp)
float power = voltage_out * current_in * power_calibration_;  // Use it!
```

**Critical**: Apply new multipliers/modifiers at ALL calculation points - search for existing calculations.

### 3. Adding New Sensor Types with Keyword Detection

Pattern used throughout `sensor.py`:

```python
# Step 1: Define constant and keywords
CONF_NEW_SENSOR = "new_sensor"
has_new_keywords = any(keyword in sensor_name for keyword in ["new", "custom", "special"])

# Step 2: Create schema
NEW_SENSOR_CONFIG_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=2,
    state_class=STATE_CLASS_MEASUREMENT,
    icon="mdi:new-icon",
).extend({
    cv.GenerateID(CONF_TIGO_MONITOR_ID): cv.use_id(TigoMonitorComponent),
})

# Step 3: Add to validation chain
if has_new_keywords:
    return NEW_SENSOR_CONFIG_SCHEMA(config)

# Step 4: Add to sensor registration
elif has_new_keywords:
    cg.add(hub.add_new_sensor(sens))
```

### 4. Memory Leak Prevention in Loops

**Bad pattern** (causes heap fragmentation):
```cpp
for (auto &device : devices_) {
  std::string key = "prefix_" + device.addr;  // NEW allocation every iteration!
  prefs.put_float(key.c_str(), device.value);
}
```

**Good pattern** (CHANGELOG 1.2.0 fix):
```cpp
static std::string key;  // Allocated once
key.reserve(32);         // Pre-allocate capacity
for (auto &device : devices_) {
  key = "prefix_";       // Reuse buffer
  key += device.addr;    // Append, no realloc
  prefs.put_float(key.c_str(), device.value);
}
```

**Pattern applies to**: Flash I/O, JSON building, repeated string operations in loops.

### 5. Web UI JSON Field Updates

When changing C++ data structures, update web UI in lockstep:

```cpp
// C++ change (tigo_web_server.cpp)
- snprintf(buffer, "\"missed_packets\":%u", missed_packets);
+ snprintf(buffer, "\"missed_frames\":%u", missed_frames);
```

```javascript
// JavaScript change (same file, HTML section)
- document.getElementById('missed-packets').textContent = data.missed_packets;
+ document.getElementById('missed-frames').textContent = data.missed_frames;
```

```html
<!-- HTML element change -->
- <div id="missed-packets">--</div>
+ <div id="missed-frames">--</div>
```

**Use consistent naming**: `snake_case` in JSON, `kebab-case` in HTML IDs, `camelCase` in JavaScript.

### 6. Adding Cached Getter Methods

For display optimizations (see CHANGELOG 1.2.0):

```cpp
// Header (tigo_monitor.h)
float get_total_power() const { return cached_total_power_; }
protected:
  float cached_total_power_ = 0.0f;

// Update cache during sensor publish (tigo_monitor.cpp)
void publish_sensors() {
  cached_total_power_ = 0.0f;
  for (auto &device : devices_) {
    cached_total_power_ += device.power;
  }
  // ... publish sensors
}
```

**Benefit**: Display lambda becomes O(1) instead of O(n), eliminates iteration overhead.

### 7. PSRAM Container Migration

When migrating to PSRAM for large data:

```cpp
// Before (internal RAM)
std::vector<DeviceData> devices_;
std::map<std::string, NodeInfo> node_table_;

// After (PSRAM)
#ifdef USE_ESP_IDF
psram_vector<DeviceData> devices_;
psram_map<std::string, NodeInfo> node_table_;
#else
std::vector<DeviceData> devices_;  // Fallback for non-ESP32
std::map<std::string, NodeInfo> node_table_;
#endif
```

**No code changes needed** - PSRAM allocators are STL-compatible.

## Release Process

1. Update `CHANGELOG.md` with new version and date
2. Update version references in `README.md`
3. Test on real hardware (AtomS3R recommended)
4. Git tag: `git tag v1.x.x && git push --tags`
5. Create GitHub release with changelog excerpt
