# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

ESPHome external component for monitoring Tigo solar optimizers via RS485/UART. Three C++ components (`tigo_monitor`, `tigo_server`, `tigo_master`) run on ESP32 using the **ESP-IDF framework** (not Arduino). FreeRTOS tasks/semaphores handle all async operations.

There are two builds of the same monitor:
- **ESP32**: the components above.
- **Linux daemon** (`linux/`, `tigomonitord`): runs on a Raspberry Pi, shipped as a .deb and a Docker image. It can also be the bus master that replaces the CCA.

The two builds share the protocol and data headers in `components/tigo_monitor/` (`tigo_protocol.h`, `bus_book.h`, `bus_watch.h`, `array_grid.h`, `link_stats.h`, `mesh_history.h`, …). Both also serve the same web page, `components/tigo_server/web/app.html`. A change to a shared header changes both builds.

## Build Commands

```bash
# Compile (check for errors without flashing)
esphome compile boards/esp32s3-atoms3r.yaml

# Compile and flash
esphome run boards/esp32s3-atoms3r.yaml

# View logs
esphome logs boards/esp32s3-atoms3r.yaml
```

## Tests

```bash
# Host tests of the shared headers, plus the Linux build (run inside the dev container, as CI does)
docker build -t tigod-dev -f linux/Dockerfile.dev linux
docker run --rm -v "$PWD":/src -w /src tigod-dev linux/scripts/build.sh

# End-to-end scripts for the Linux daemon (CI runs every linux/scripts/*-e2e.sh)
linux/scripts/<name>-e2e.sh

# Web UI tests (app.html): run before any app.html PR
cd site && npm test
```

- Host tests live in `tests/host/`. A new test must also be added to the list in `linux/scripts/build.sh`, or nothing runs it.
- CI runs three workflows: `.github/workflows/pi.yml` (the host tests, the e2e scripts, the .deb and the image), `esp32.yml` (`esphome compile` of `test-local-tigomonitor` and the relay standby) and `pages.yml` (the docs site).
- Hardware behaviour (frame loss, heap, flash timing) is checked on the rig by hand, and measured, not assumed.

## Architecture

### Component Structure

- **`components/tigo_monitor/`** — Core component: UART frame parsing, device tracking, sensor publishing, CCA sync, plus the headers shared with the Linux build
  - `__init__.py`, `sensor.py`, `button.py`, `binary_sensor.py` — Python config schemas + ESPHome codegen (no runtime logic)
  - `tigo_monitor.h` / `tigo_monitor.cpp` — C++ implementation (~5,200 lines)
- **`components/tigo_server/`** — HTTP web server: the single-page app plus the JSON API
  - `__init__.py` — Config schema
  - `tigo_web_server.h` / `tigo_web_server.cpp` — C++ implementation (~5,000 lines)
  - `web/app.html` — the web UI, shared with the Linux build; `web_assets_gen.py` gzips it into the firmware
- **`components/tigo_master/`** — an ESP32 bus master or standby (`master_core.cpp` is also built into the host tests)
- **`linux/`** — `tigomonitord`: `src/` is the daemon, `scripts/` the build and e2e tests, `packaging/` the .deb, `Dockerfile` the image
- **`ha-app/`** (with `repository.yaml` at the root) — the Home Assistant app: a thin layer on the released image (`build.yaml`) whose `run.sh` starts the daemon with `--state-dir /data --trust-ingress 172.30.32.2` and offers HA's MQTT broker once

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

**Boards without PSRAM** (e.g. `boards/esp32-lilygo-t-can485.yaml`) are supported for *sensors-only* builds — `tigo_monitor` without `tigo_server`. The `psram_*` aliases still compile there; `psram_malloc()` keys off the IDF's `CONFIG_SPIRAM` and resolves straight to the internal heap when it is unset, so no call site changes. Keep it that way: gate on `CONFIG_SPIRAM` inside the allocator rather than sprinkling new `#ifdef`s at usage sites, and never assume `USE_ESP_IDF` implies PSRAM exists. The web server genuinely does require PSRAM and must stay out of those configs.

### Web Server

One page, `/app` (`web/app.html`, served gzipped). The old paths (`/`, `/nodes`, `/status`, `/history`, `/cca`) redirect to views in it. About 50 JSON routes under `/api/`: see the `httpd_register_uri_handler` calls in `tigo_web_server.cpp`. Auth: `api_token` for API, HTTP Basic for HTML pages. All responses built in PSRAM via `PSRAMString`. Handlers read monitor state under `with_state_lock()`. A long answer is written in chunks so it doesn't hold that lock, and the UART loop, for long.

### CCA Integration

Fuzzy barcode matching (`match_barcode()`): compares last 6 chars of UART-discovered addresses against CCA barcodes with 1-char tolerance. On match, populates `inverter_name`, `mppt_label`, `panel_name` in `NodeInfo`.

## Key Development Rules

- **Always apply `power_calibration_` multiplier** to ALL power calculations (individual sensors, string aggregation, web API, power sums)
- **Avoid string allocations in loops** — reuse static buffers to prevent heap fragmentation (see CHANGELOG 1.2.0 for the pattern)
- **JSON field naming**: `snake_case` in JSON, `kebab-case` in HTML IDs, `camelCase` in JavaScript
- **When renaming methods** in `tigo_monitor.h`, update all call sites: header → member variable → web server → Python config → JavaScript
- **When changing any `/api/*` JSON shape** (rename a field, restructure a response), update `site/screenshots/fixtures.mjs` in the same commit. The docs screenshots are rendered from the real `app.html` against those fixtures at build time — a stale fixture doesn't fail the build, it publishes screenshots of a UI rendering `undefined`. Check with `cd site && npm run screenshots` — which also refreshes the committed README images in `docs/images/`, so commit those too
- **Sensor/text_sensor/binary_sensor** are pulled in by `AUTO_LOAD` in `tigo_monitor/__init__.py`, because `tigo_monitor.h` includes all three unconditionally. YAML no longer needs to declare empty stanzas for them. If you add an unconditional `#include` of another ESPHome component to that header, add it to `AUTO_LOAD` too — otherwise configs that don't happen to use it fail with "No such file or directory". Prefer a `USE_*` guard (as `time`/`button` already do) when the dependency is genuinely optional
- `CONFIG_UART_ISR_IN_IRAM: "y"` in sdkconfig is required for reduced frame loss

## Git Workflow

- **`main`**: Trunk. Stable releases are tagged `vX.Y.Z` here, and current development also lands here via short-lived branches. Base for new work and the target for PRs.
- **`feature/*`, `fix/*`**: Short-lived branches created off `main`, merged back to `main`.

### Release Process

1. Update `CHANGELOG.md` under `## [Unreleased]` (group by Added/Fixed/Changed/Removed).
2. Move the `## [Unreleased]` entries under a new `## [X.Y.Z]` heading, create an annotated tag `vX.Y.Z`, push, and create the GitHub release.
3. Set `ha-app/config.yaml`'s `version` and `ha-app/build.yaml`'s image tags to `X.Y.Z` in the release commit (`tests/host/ha_app_test.py` fails if they disagree). Home Assistant offers the update as soon as `main` has it and then pulls the tag's images, so push the tag first and let CI publish them before `main` moves.

(There is no version constant to bump: the tag is the version. The Pi's CI builds it from `git describe`. The ESP32 works it out at compile time in `components/tigo_server/build_version.py`, from the `ref:` its config pinned. Both report it as `tigomonitor_version` in `/api/status`, and the sidebar shows it. ESPHome's own version stays in `esphome_version`.)
