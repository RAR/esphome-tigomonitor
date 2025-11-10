# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.1.0] - 2025-11-10

### Added
- **Memory Monitoring Sensors** for Home Assistant
  - Internal RAM Free (KB) - Current free internal RAM
  - Internal RAM Min (KB) - Minimum free since boot (watermark)
  - PSRAM Free (KB) - Current free PSRAM
  - Stack Free (bytes) - Current task stack free space
  - All sensors update every 60 seconds
  - Keyword-based auto-detection in YAML config
  - Enables memory health tracking and alerting in Home Assistant

### Fixed
- **Critical Memory Leaks** in frame processing
  - `frame_to_hex_string()`: Changed from `+=` to `push_back()` to eliminate repeated allocations
  - `remove_escape_sequences()`: Changed from `+=` to `push_back()` to eliminate repeated allocations
  - Both functions process hundreds of frames per hour
  - Fixes gradual heap exhaustion even without web UI usage
  - Stable memory usage over long-term operation
- **CCA Data Persistence Bug**
  - Fixed `load_node_table()` not loading CCA data after frame09_barcode removal
  - Save format changed from 10 fields to 9 fields, but load logic was checking for 10+ fields
  - Now properly handles both 9-field (current) and 10-field (old) formats
  - CCA labels now correctly display after restart with `sync_cca_on_startup: false`
  - Added backward compatibility for old saved data
- **Negative Temperature Support**
  - Temperature values now correctly handle sub-zero readings
  - Implemented proper 12-bit two's complement conversion
  - Supports range: -204.8°C to +204.7°C
  - Critical for winter operation and cold climate installations

### Changed
- **PSRAM Optimization for Frame Processing**
  - `frame_to_hex_string()` now uses `psram_string` internally (saves 1-3KB per frame)
  - `remove_escape_sequences()` now uses `psram_string` internally (saves 500-1500 bytes per frame)
  - Combined savings: 3-5KB internal RAM per frame processed
  - Conditional compilation: PSRAM path on ESP-IDF, original code on other platforms
  - Preserves internal RAM for critical operations
- **Enhanced Memory Logging**
  - Added more detailed logging for CCA data loading
  - Improved diagnostic messages for node table restoration
  - Better tracking of memory usage patterns

### Technical Details
- Memory leak fixes eliminate continuous heap fragmentation
- PSRAM optimizations reduce internal RAM pressure by 3-5KB per frame
- Frame processing functions handle thousands of frames per day
- `push_back()` with `reserve()` provides single allocation vs repeated allocations with `+=`
- PSRAM access ~4x slower but negligible impact (microseconds) vs memory benefits
- All optimizations maintain backward compatibility

### Performance Impact
- **Stable heap usage**: No more gradual memory exhaustion
- **Reduced fragmentation**: Internal RAM allocations eliminated for large buffers
- **Long-term reliability**: Systems can run indefinitely without memory issues
- **PSRAM utilization**: Large temporary buffers now in PSRAM instead of internal RAM

### Upgrade Notes
1. Recommended for all users - critical stability fixes
2. Memory monitoring sensors are optional but recommended for diagnostics
3. No configuration changes required for bug fixes
4. No breaking changes - fully backward compatible

### Breaking Changes
None - All changes are backward compatible.

## [1.0.0] - 2025-11-06

### Added - Authentication & Security
- **HTTP Basic Authentication** for web pages (username/password)
  - Native browser authentication prompts
  - Session-based credential caching
  - Protects all HTML pages: Dashboard, Node Table, ESP Status, YAML Config, CCA Info
  - Optional configuration - backward compatible
- **API Bearer Token Authentication** for all `/api/*` endpoints
  - Standard `Authorization: Bearer <token>` header format
  - Separate from web authentication for flexible access control
  - Returns proper 401 responses with JSON error messages
  - Optional configuration - backward compatible

### Added - Web Interface
- **Complete Web Server** with 5 comprehensive pages
  - Dashboard with real-time system stats and live device monitoring
  - Node Table with CCA labels, hierarchy display, and device management
  - ESP32 Status page with memory metrics and system information
  - YAML Config Generator with one-click copy
  - CCA Info page with device status and manual refresh
- **String-Grouped Dashboard Layout**
  - Devices organized by CCA strings with visual sections
  - String summary cards showing aggregate metrics per string
  - Gradient headers for visual hierarchy
  - Historical peak power tracking per device
- **Dark Mode Support** across all web pages
  - Toggle switch with persistent preference (localStorage)
  - Smooth transitions and optimized contrast
- **Temperature Unit Toggle** (Fahrenheit/Celsius)
  - Persistent preference stored in browser
  - Applies to all temperature displays
- **Auto-refresh** capabilities (5-30 second intervals)
- **Mobile-responsive** design for all pages

### Added - CCA Integration
- **Automatic CCA Sync** on startup (configurable)
- **Panel Name Mapping** from Tigo CCA
  - Frame 27 (16-char) barcode matching
  - Inverter → String → Panel hierarchy display
  - Persistent label storage across reboots
- **Manual CCA Sync Button** for on-demand updates
- **CCA Device Info Display** with comprehensive status
  - Connection status, software version, system ID
  - Discovery progress, uptime, last config sync
  - Manual refresh capability

### Added - Device Management
- **Historical Peak Power Tracking**
  - Per-device maximum power recording
  - Flash-persistent storage
  - Reset capability via web interface and API
- **Remote ESP32 Restart** button (web + API)
- **Individual Node Deletion** from web interface
  - Confirmation dialogs for safety
  - Frees up sensor indices for reuse
- **Node Table Enhancements**
  - CCA validation badges
  - Barcode information display
  - Sensor assignment tracking

### Added - Performance & Memory
- **PSRAM Optimization**
  - Automatic PSRAM detection and usage
  - Large HTTP buffers allocated from PSRAM
  - JSON parsing uses PSRAM when available
  - Supports 36+ devices with M5Stack AtomS3R (8MB PSRAM)
- **Flash Wear Optimization**
  - Energy data saved hourly (24 writes/day vs 288)
  - Flash lifespan: ~11 years @ 100k cycles, ~114 years @ 1M cycles
  - Maximum 1 hour energy data loss on unexpected reboot
- **OTA/Shutdown Data Persistence**
  - Automatic energy data save before OTA updates
  - Shutdown hook for clean data persistence
- **Night Mode**
  - Automatic zero publishing after 1 hour of no data
  - Prevents stale data in Home Assistant
  - 10-minute update interval during night mode

### Added - Documentation
- **Comprehensive README Updates**
  - Authentication configuration examples (web, API, combined)
  - PSRAM requirements and hardware recommendations
  - Security best practices
  - Troubleshooting guide expansions
  - Use case scenarios
- **UI Screenshots** with anonymized data
  - All 5 web pages documented visually
  - Home Assistant integration examples
- **Web Server Documentation** (WEB_SERVER_README.md)
- **Example Configuration Files**
  - `example-web-server.yaml` for quick starts

### Changed
- **Frame 27 (16-char) as Primary Barcode Source**
  - More reliable device identification
  - Frame 09 (6-char) no longer used
- **String Aggregation Improvements**
  - Proper CCA string ID handling
  - Persistent string metadata
  - Real-time aggregate calculations
- **Memory Efficiency**
  - HTTP server stack size optimized
  - JSON builders use efficient string handling
  - PSRAM-aware memory allocation

### Fixed
- **CCA Refresh Socket Exhaustion**
  - Proper socket cleanup after HTTP requests
  - Memory leak prevention
  - Device sorting improvements
- **Web Page Refresh Issues**
  - Proper timestamp updates on CCA refresh
  - Consistent data age indicators
  - Fixed stale data display
- **YAML Configuration Generator**
  - Corrected YAML formatting issues
  - Proper indentation and structure
- **Compiler Warnings**
  - Removed redundant USE_ESP_IDF defines
  - Clean compilation with no warnings
- **UART Packet Loss**
  - Added CONFIG_UART_ISR_IN_IRAM recommendation
  - Moves ISR to IRAM for faster processing
  - Significantly reduces missed packets

### Technical Details
- **ESP-IDF 5.4.2** support
- **ESPHome 2025.10.4+** compatibility
- **mbedtls** for base64 encoding/decoding (Basic Auth)
- **Custom HTTP server** using ESP-IDF httpd component
- **Memory footprint**: ~12% RAM, ~60% Flash with web server
- **Tested hardware**: M5Stack AtomS3R with 36 devices

### Breaking Changes
None - All changes are backward compatible. Web authentication and API tokens are optional features.

### Upgrade Notes
1. Update ESPHome to 2025.10.4 or newer
2. For 15+ devices, ensure ESP32-S3 with PSRAM (M5Stack AtomS3R recommended)
3. Add `CONFIG_UART_ISR_IN_IRAM: "y"` to sdkconfig_options for better UART reliability
4. Optionally add web authentication and/or API token for security
5. Review new configuration options in README

### Security Recommendations
- Use strong passwords for web authentication (10+ characters)
- Use randomly generated tokens for API authentication (32+ characters)
- Consider separate credentials for web and API access
- Use HTTPS if exposing outside local network
- Limit access via firewall rules

---

## Initial Development
Previous commits were part of the initial development phase. This is the first official release with comprehensive feature set and documentation.

[1.1.0]: https://github.com/RAR/esphome-tigomonitor/releases/tag/v1.1.0
[1.0.0]: https://github.com/RAR/esphome-tigomonitor/releases/tag/v1.0.0
