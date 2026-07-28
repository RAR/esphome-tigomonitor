# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESPHome external component for monitoring Tigo solar optimizers via RS485/UART. Two C++ components (`tigo_monitor`, `tigo_server`) run on ESP32 using the **ESP-IDF framework** (not Arduino). FreeRTOS tasks/semaphores handle all async operations.

## Build Commands

```bash
# Compile (check for errors without flashing)
esphome compile boards/esp32s3-atoms3r.yaml

# Compile and flash
esphome run boards/esp32s3-atoms3r.yaml

# View logs
esphome logs boards/esp32s3-atoms3r.yaml
```

There is no test suite. Testing is manual (sensor updates in HA, web UI, CCA sync, memory monitoring).

## Architecture

### Component Structure

- **`components/tigo_monitor/`** — Core component: UART frame parsing, device tracking, sensor publishing, CCA sync
  - `__init__.py`, `sensor.py`, `button.py`, `binary_sensor.py` — Python config schemas + ESPHome codegen (no runtime logic)
  - `tigo_monitor.h` / `tigo_monitor.cpp` — C++ implementation (~3,200 lines)
- **`components/tigo_server/`** — HTTP web server with 5 HTML pages + RESTful JSON API
  - `__init__.py` — Config schema
  - `tigo_web_server.h` / `tigo_web_server.cpp` — C++ implementation (~4,200 lines)

### Three-File Config Pattern

All features follow this pattern — Python schema, C++ header setter, C++ implementation:

1. **Python** (`__init__.py` or `.py`): Schema validation via `cv.Schema`, codegen calls `cg.add(var.set_xxx(...))`
2. **C++ header**: Setter method `set_xxx()` stores value in member variable
3. **C++ impl**: Uses the stored value in processing logic

### Frame Processing Pipeline

UART reads 12-byte telemetry frames from RS485 bus. Three frame types:
- **Power frames** (0x0D/0x0F): Voltage, current, power, temperature
- **Status frames** (0x09): Device status flags
- **Node table frames** (0x27): 16-char device addresses/barcodes

`process_frame()` dispatches to `process_power_frame()`, `process_09_frame()`, `process_27_frame()`.

### Sensor Type Inference

`sensor.py` uses keyword-based detection to route sensor configs to the correct schema. Keywords like "energy"/"kwh" → energy sensor, "frame"/"missed" → missed frame counter, "power"/"watt" → power sensor. When adding new sensor types, update both keyword detection and schema mapping.

### PSRAM-First Memory Design

PSRAM is required for 15+ devices. Custom STL-compatible allocators (`PSRAMAllocator`) back large data structures:
- `psram_vector<DeviceData>` for device list
- `psram_map<std::string, NodeInfo>` for node table
- `PSRAMString` for JSON/HTML response building

Always use PSRAM containers for large data. Internal RAM is <200KB; PSRAM is 8MB. Use `#ifdef USE_ESP_IDF` guards for PSRAM types.

### Web Server

5 HTML pages (`/`, `/nodes`, `/status`, `/yaml`, `/cca`) + JSON API endpoints (`/api/devices`, `/api/overview`, `/api/strings`, `/api/status`, `/api/health`, `/api/inverters`, `/api/energy-history`). Auth: `api_token` for API, HTTP Basic for HTML pages. All responses built in PSRAM via `PSRAMString`.

### CCA Integration

Fuzzy barcode matching (`match_barcode()`): compares last 6 chars of UART-discovered addresses against CCA barcodes with 1-char tolerance. On match, populates `inverter_name`, `mppt_label`, `panel_name` in `NodeInfo`.

## Key Development Rules

- **Always apply `power_calibration_` multiplier** to ALL power calculations (individual sensors, string aggregation, web API, power sums)
- **Avoid string allocations in loops** — reuse static buffers to prevent heap fragmentation (see CHANGELOG 1.2.0 for the pattern)
- **JSON field naming**: `snake_case` in JSON, `kebab-case` in HTML IDs, `camelCase` in JavaScript
- **When renaming methods** in `tigo_monitor.h`, update all call sites: header → member variable → web server → Python config → JavaScript
- **When changing any `/api/*` JSON shape** (rename a field, restructure a response), update `site/screenshots/fixtures.mjs` in the same commit. The docs screenshots are rendered from the real `app.html` against those fixtures at build time — a stale fixture doesn't fail the build, it publishes screenshots of a UI rendering `undefined`. Check with `cd site && npm run screenshots`
- **Sensor/text_sensor/binary_sensor sections** must be declared in YAML (even if empty) or compilation fails with missing header errors
- `CONFIG_UART_ISR_IN_IRAM: "y"` in sdkconfig is required for reduced frame loss

## Git Workflow

- **`main`**: Trunk. Stable releases are tagged `vX.Y.Z` here, and current development also lands here via short-lived branches. Base for new work and the target for PRs.
- **`feature/*`, `fix/*`**: Short-lived branches created off `main`, merged back to `main`.
- Legacy branches `dev` (abandoned ~2026-04) and `next` (lagging, no unique work) are **not** the current line — start new work from `main`.

### Release Process

1. Update `CHANGELOG.md` under `## [Unreleased]` (group by Added/Fixed/Changed/Removed).
2. Move the `## [Unreleased]` entries under a new `## [X.Y.Z]` heading, create an annotated tag `vX.Y.Z`, push, and create the GitHub release.

(The web UI shows ESPHome's own version via `esphome/core/version.h` — there is no project-specific version constant to bump.)
