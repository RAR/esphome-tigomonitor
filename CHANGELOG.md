# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.3.0] - 2025-11-27

### Added
- **New Release Banner**
  - Dismissable banner on dashboard showing when new GitHub releases are available
  - Checks GitHub API on page load for latest release
  - Displays version info and direct link to release page
  - Per-version dismissal stored in localStorage
  - Beautiful gradient purple design with emoji icon
- **GitHub Project Link** in Web UI Header
  - GitHub logo icon button added to all 5 web pages
  - Links to https://github.com/RAR/esphome-tigomonitor
  - Styled consistently with temperature and theme toggle buttons
- **Daily Energy History Chart**
  - 7-day energy production bar chart on dashboard
  - Automatically archives daily totals at midnight
  - Persists energy baseline across reboots for accurate daily tracking
  - Saves energy data when entering night mode
  - Responsive chart resizing on window resize
- **Power Calibration Feature**
  - New `power_calibration` configuration option (default: 1.0)
  - Multiplier applied to all power calculations (sensors, web UI, displays)
  - Allows calibration against inverter or other reference measurements
  - Range: 0.5 to 2.0 (50% to 200%)
  - Example: `power_calibration: 1.184` for 18.4% adjustment

### Fixed
- **Night Mode Display Issues**
  - Dashboard API now returns zero watts during night mode instead of residual cached power
  - Cached total power properly zeroed when all devices go offline
- **Daily Energy Tracking**
  - Fixed race condition between `update_daily_energy()` and `check_midnight_reset()` archival
  - Only midnight reset archives when `reset_at_midnight` enabled
  - Energy baseline (`energy_at_day_start_`) now persisted to flash
  - Today's energy correctly calculated as current minus baseline in chart
  - Removed duplicate "today" bar from energy history chart
- **Web UI Performance**
  - Fixed duplicate energy history API call on initial page load
  - Added `energyHistoryLoaded` flag to prevent redundant fetches
- **CSS Consistency**
  - Removed duplicate `.temp-toggle` CSS rule from Status page
  - All header control buttons styled consistently across pages

### Changed
- **Terminology Consistency**
  - Renamed all "packet" references to "frame" for serial communication accuracy
  - Updated `missed_packet` sensor to `missed_frame` (config name: `missed_frame`)
  - Updated API methods: `add_missed_frame_sensor()`, `get_missed_frame_count()`
  - Log messages now use "Frame missed!" instead of "Packet missed!"
  - Tigo serial protocol uses frames, not packets

### Improved
- **Midnight Reset Optimization**
  - Batched flash writes to reduce heap fragmentation
  - Eliminated 38 individual preference saves
  - Consolidated into single `save_persistent_data()` call

## [1.2.0] - 2025-11-16

### Added
- **AtomS3R Display Package** (`boards/atoms3r-display.yaml`)
  - 128x128 ST7789V LCD display showing real-time solar monitor status
  - LP5562 LED driver integration for RGB status LED and LCD backlight
  - Displays total power, device count, online status, and WiFi indicator
  - Optimized display lambda with cached values (O(1) instead of O(n))
  - 5-second update interval to minimize CPU load
  - Web UI backlight toggle button in Actions section
- **Packet Statistics Display** in Web UI
  - Total frames processed counter
  - Missed packet percentage (miss rate)
  - Provides context for packet loss (typical: 0.02-0.04% for RS485)
  - Shows 99.96%+ success rate is excellent performance
- **Comprehensive Memory Leak Diagnostics**
  - Heap monitoring before/after midnight reset operations
  - Detailed logging of memory deltas during peak power reset
  - Minimum heap watermark tracking
  - Identifies exact source of RAM consumption
- **Fast Display Helper Methods**
  - `get_device_count()`: Returns device count without iteration
  - `get_online_device_count()`: Returns cached online count
  - `get_total_power()`: Returns cached total power
  - Cached values updated during sensor publish cycle
  - Eliminates 5-10ms CPU blocking during display updates

### Fixed
- **Memory Leak in Midnight Reset** (2KB RAM loss per day)
  - Optimized `reset_peak_power()`: Pre-allocate static string buffer, reuse across loop
  - Optimized `save_peak_power_data()`: Eliminate temporary string allocations
  - Optimized `load_peak_power_data()`: Eliminate temporary string allocations
  - Optimized `save_node_table()`: Reuse string buffer for preference keys
  - Optimized `load_node_table()`: Reuse string buffer for preference keys
  - Root cause: `std::string pref_key = "peak_" + device.addr` created 30+ allocations per reset
  - Solution: Static string buffer with reserve(32), reuse with `pref_key = "peak_"; pref_key += device.addr`
  - Eliminates heap fragmentation from alloc/free cycles
  - Zero heap allocations per iteration after first call

### Changed
- **UART Processing Optimization**
  - Doubled `MAX_BYTES_PER_LOOP` from 2KB to 4KB per iteration
  - Handles display overhead and I2C operations more efficiently
  - Reduces risk of RX buffer overflow during SPI/I2C operations
- **Display Update Frequency**
  - Increased from 2s to 5s in atoms3r-display.yaml
  - 60% reduction in SPI overhead
  - More CPU time available for UART processing
- **Enhanced Packet Miss Logging**
  - Now logs buffer size and UART availability when packet missed
  - Example: "Packet missed! Found END before START (buffer: 14 bytes, available: 68)"
  - Helps diagnose buffer overflow vs. bus collision
- **Packet Statistics Tracking**
  - Added `total_frames_processed_` counter
  - Incremented in `process_frame()` for every successful frame
  - Enables miss rate calculation: `(missed / (total + missed)) × 100`
  - Periodic logging every 60 seconds with heap stats

### Performance
- **Display Lambda Optimization**
  - Before: 5-10ms CPU time per update (device iteration + calculations)
  - After: <0.5ms CPU time per update (3 getter calls + rendering)
  - 90%+ reduction in display CPU overhead
  - Eliminated allocation of Color() objects in lambda
  - No more string formatting or power calculations during display
- **Memory Leak Resolution**
  - Before: ~2KB RAM loss every midnight reset
  - After: Zero measurable RAM loss at midnight
  - Improved long-term stability for 24/7 operation
- **Packet Reception Performance**
  - Measured: 0.02-0.04% miss rate (99.96-99.98% success)
  - ~60,000 frames processed per hour with 30 devices
  - Missed packets occur in synchronized bursts (bus collisions)
  - Performance near theoretical maximum for multi-drop RS485

### Documentation
- **UART_OPTIMIZATION.md**: Comprehensive guide for packet loss troubleshooting
  - Root cause analysis (display SPI, I2C, processing bottlenecks)
  - Buffer size recommendations (8KB RX for display users, 1KB TX listen-only)
  - Step-by-step optimization procedures
  - Testing and validation procedures
  - When to use ESP32-P4 for large installations
- **Updated boards/README.md**
  - Notes on buffer size increases needed with display
  - Clarifies TX buffer can be small for listen-only mode
  - References UART optimization guide

### Board Configurations
- **atoms3r-display.yaml**: Complete AtomS3R display package with LP5562
- **Updated esp32s3-atoms3r.yaml**: Added notes about display buffer requirements
- **Updated esp32p4-evboard.yaml**: Corrected TX buffer to 1024 (listen-only)

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
