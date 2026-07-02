# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [2.0.0-beta.2] - 2026-07-02

### Fixed
- **Tigo cloud (`cloud_import`) TLS failed on ESP-IDF 6.0** — `mbedtls_ssl_handshake returned -0x3000` / `No matching trusted root certificate found` when logging in. Root cause: ESPHome disables mbedtls SHA-384/512 on IDF ≥ 6.0 to save flash, and every signature above the leaf in Tigo's chain (mapi.tigoenergy.com → `WE1` → `GTS Root R4`, on Google Trust Services) is SHA-384-based. mbedtls 4.0 then drops `ecdsa-with-SHA384`/`sha384WithRSAEncryption` from its OID table, silently discards the `WE1` intermediate from the server's presented chain during the handshake, and verification fails looking up the wrong issuer. The same missing algorithms also make pinned GTS root PEMs unparseable (`-0x2100` `X509_UNKNOWN_OID`). `tigo_server` now calls ESPHome's `esp32.require_mbedtls_sha512()` (from final-validate, so it lands before the esp32 platform decides to disable them) whenever `cloud_import` or CCA BLE is enabled, and the cloud client verifies against pinned Google Trust Services roots R1–R4 (`tigo_cloud_ca.h`) rather than `esp_crt_bundle` — still needed because IDF 6.0's default bundle dropped the `GlobalSign Root CA` that Tigo's cross-signed chain anchors on. Verified on-device (ESP32-S3, IDF 6.0.1): cloud health/login round-trips green; IDF 5.x builds unchanged.
- **CCA BLE session key could be silently wrong on ESP-IDF 6.0** — the mobile_api session key is a SHA-512 hash computed via PSA on IDF ≥ 6.0, and with SHA-512 compiled out (same ESPHome flash optimization as above) `psa_hash_compute()` fails `NOT_SUPPORTED` while the code ignored the status — yielding a garbage key and a failed CCA BLE login. Covered by the same `require_mbedtls_sha512()` call, and the hash call now checks the PSA status and logs an error instead of continuing with an invalid key. Verified on-device: BLE DEVICE_INFO sync works on IDF 6.0.
- **ESP-IDF 6.0 build with `cca_source: ble`** — `mbedtls/sha512.h` no longer exists on IDF 6.0 (it ships mbedtls 4.0, which removed the legacy `mbedtls_sha512_*` API and its public header in favor of the PSA Crypto API), so BLE builds failed to compile at `tigo_cca_ble.cpp`. The CCA session-key hash now uses PSA (`psa_hash_compute(PSA_ALG_SHA_512, …)`) on IDF ≥ 6.0 and the legacy mbedtls one-shot on 5.x — mirroring esphome's own `sha256` component — so it builds on both. Only affected `cca_source: ble`/`auto` (the include is under `USE_TIGO_CCA_BLE`); the non-BLE IDF-6 path was unaffected, which is why it went unnoticed.

## [2.0.0-beta.1] - 2026-07-02

### Added
- **Find CCA over Bluetooth (CCA Info page)**. A "Find CCA over Bluetooth" card scans for the Tigo CCA and lets you target it **without hardcoding the MAC in YAML**. `TigoWebServer` registers as an `esp32_ble_tracker` listener and collects advertisements whose MAC matches Tigo Energy's `04:C0:5B` OUI (only the CCA advertises BLE on it — optimizers/gateways don't), surfacing each as MAC + RSSI + name. Picking one (or typing a MAC) saves it to NVS and applies it live via `BLEClient::set_address()`, so it overrides the compile-time `ble_client:` MAC and survives reboots; **Revert** drops the override back to the YAML MAC. The scanner runs continuously while the link is idle, so the card accumulates results over a few seconds; the parse callback is deliberately silent (logging every advertisement starves the main loop and drops the web/API). The previously-RE'd Tigo identifiers — service UUID `D3DADCBA-E4FA-4016-BA33-8BCC671999A7` and `iBTigoCC` manufacturer data — are alternative filters, but the OUI is the cheapest reliable one. New endpoints `GET /api/cca/ble-scan` (`?rescan=1` to clear) and `POST /api/cca/ble-mac` (`{mac}` to set, `{reset:true}` to revert); both BLE-only, and the card self-hides on non-BLE builds. `max_uri_handlers` cap raised to 60.
- **On-device configuration (Tools › Device Configuration)**. A handful of runtime knobs that previously only lived in the YAML can now be edited in the web UI and persisted to the ESP32 (NVS), no reflash needed: `power_calibration` (applied immediately — every power calc reads it live), `night_mode_timeout`, `reset_at_midnight`, `sync_cca_on_startup`, and `cca_ip` (kept for older CCA firmware that still serves local HTTP). The YAML values remain the **defaults**: at boot the codegen setters populate the members, then any stored overrides are overlaid on top. Because a stored value wins over the YAML, each field has a **Revert** that clears the override and restores the YAML default (so editing the YAML applies again), and overridden fields are tagged in the UI. Structural/compile-time settings (inverter layout, device count, `cca_source`/`cloud_import`, ports, IDs) and auth (`api_token`/`web_*`) intentionally stay in YAML — the latter because NVS is plaintext-at-rest. New endpoints `GET /api/config` (values + defaults + overridden flags) and `POST /api/config` (`{key,value}` to set, `{reset:key}` to revert); `max_uri_handlers` cap raised to 60.
- **Tigo cloud layout import** (`cloud_import: true` on `tigo_server`). Recovers the panel names + string/MPPT/inverter layout from Tigo's cloud — the device/layout data we lose once CCA firmware 4.0.4+ locks the local HTTP API (BLE gives CCA info/network but not the layout). The web server logs into Tigo's cloud API (`mapi.tigoenergy.com`, the same one the mobile app uses), fetches `GET /api/v3/systems/layout`, and applies each panel's label/string/MPPT to the node table — so it then flows through the node-table export/import and Topology like a local CCA sync. The cloud panel `serial` is the printed optimizer barcode (`4-<6 hex><check>`) whose 6 hex are the radio MAC's last 6, so panels are matched to UART nodes by that last-6 (the two ID spaces are otherwise disjoint). The cloud numbers MPPTs per-inverter, so they're renumbered globally in layout order (MPPT 1,2,3,4,…) to line up with the `inverters: mppts:` YAML convention instead of collapsing every string onto the first inverter. All cloud features live on a dedicated **Tigo Cloud** page (in the sidebar nav whenever the feature is compiled in). Credentials are entered there via a **Configure modal**; **only the resulting bearer token is persisted to NVS, never the password** (NVS is plaintext-at-rest, so a scoped, revocable, months-lived token is the safer thing to store). Layout import moves to a button on the **Topology** page. New endpoints `GET /api/cloud/status`, `POST /api/cloud/login`, `POST /api/cloud/import`; HTTPS is verified against the mbedTLS certificate bundle (enabled automatically when `cloud_import` is set). The page also surfaces Tigo's own analytics — the warning/error counts and per-equipment status from the Energy-Intelligence endpoints (the data behind the portal's status section, `statusCode` 0=ok/1=warning/2=error). A **sidebar box** (shown when connected, labeled as cloud-sourced, refreshed ~10 min while the UI is open) gives at-a-glance severity dots per CCA/Panels, and the page itself shows stat cards, flagged-equipment detail (which check + decoded data, via `equipment-status/latest` + the event-types legend), all equipment listed by string with each device's full per-check status (severity, decoded data with the not-actionable `production` threshold and `totalDuration: -1` sentinel filtered out, and timestamps), and recent status history — pulled on open and on Refresh. Endpoints: `GET /api/cloud/health` (summary), `GET /api/cloud/equipment?view=latest|history`. Opt-in — the cloud client, UI, and cert bundle are only compiled when `cloud_import: true`. Cloud API approach adapted from [Bobsilvio/tigosolar-online](https://github.com/Bobsilvio/tigosolar-online).
- **"Sync to cloud" button on the CCA Info page** (`DEVICE_DATA_EXPORT`). Asks the CCA to push its data to Tigo's cloud now — a paramless protected action over BLE (`POST /api/cca/data-export`), with a best-effort acknowledgement read back from the cached reply. BLE sources only.
- **Manual Topology overrides travel with node-table export/import**. The node-table backup now also carries the customizations set in the Topology view — inverter display names, string display labels, and per-string panel ratings (panel size) — so a restore (or moving config to another unit) keeps them instead of losing them. Export adds `inverters` (`{name, display_name}`) and `strings` (`{label, display_label, panel_rating_w}`) arrays alongside `nodes`, emitting only entries that were actually overridden; import re-applies them after the string/inverter groups rebuild, re-keyed by the same canonical identifiers the overrides persist under (inverter name / string label). Panel friendly names (`cca_label`) already round-tripped in the node records. Backward compatible: older exports without the new arrays import unchanged, and the import response/toast reports how many overrides were applied.
- **CCA network status + WiFi config over BLE**. The CCA Info page gains a **Network status** section rendering Ethernet and WiFi as two cards from the cached `NETWORK_INFO` (IP/subnet/gateway/MAC/SSID/addressing, plus WiFi quality/signal when associated); a link reads "Not connected" until it has an IP, and WiFi with a saved SSID but no IP shows "not associated yet". A **WiFi configuration** card scans for nearby networks (`NETWORK_WIFI_SCAN`; results list SSIDs as links that fill the form), joins (`NETWORK_WIFI_CONNECT`), or wipes (`NETWORK_WIFI_CLEAR`) the CCA's WiFi — join/clear `confirm()`-gated since either can drop the CCA's uplink. `nid`/`pwd` are the exact keys the Tigo app sends; param **values are URI-encoded server-side** (matching the app's `encodeURIComponent`) because the wire format is space-separated, so an SSID/password with a space would otherwise corrupt the frame. New endpoints: `GET /api/cca/network?cmd=` (cached `{age_s,result}`, no BLE side effect), `POST /api/cca/network/poll?cmd=` (allowlisted read — WiFi scan only), `POST /api/cca/network/wifi-connect` (`{nid,pwd}`), `POST /api/cca/network/wifi-clear`. All ride the same one-shot connect → fresh-`sid` → command → cache-by-name → auto-disconnect path. Tightened the auth-failure check to match only `Unauthorized request` rather than any `"msg"` field, so action commands that report success/failure in `msg` aren't dropped. Only built on `cca_source: ble`/`auto`; non-BLE sources answer 400. `NETWORK_ETH_CONFIG` (static-IP write) is intentionally **not** exposed — its exact param keys aren't confirmed from the app and a wrong write could orphan the CCA's wired uplink, so Ethernet stays read-only. The Device information panel was also reworked: genuine health signals (`Cloud/Gateway/Modules Communication`, cellular) render as colored-dot chips, while info strings the CCA buries in `status[]` (`Kernel`, `Discovery`, `Last Data Sync`) parse into a tidy key/value list. Four new routes (cap headroom already raised to 48).
- **CCA topology discovery over BLE**. The CCA Info page gains a **Topology discovery** section that kicks the CCA's optimizer/gateway rescan (`START_DISCOVERY`) and polls progress (`DISCOVERY_STATUS`) — both protected `mobile_api` commands riding the existing fresh-session-`sid` path, each a self-contained one-shot connect → command → auto-disconnect (the CCA is freed for the Tigo phone app between polls). New endpoints `POST /api/cca/discovery/start`, `POST /api/cca/discovery/poll`, and `GET /api/cca/discovery` (cached `{age_s,status}`, no BLE side effect); the Start button is `confirm()`-gated since a rescan briefly interrupts normal optimizer operation. Only built on `cca_source: ble`/`auto` (`USE_TIGO_CCA_BLE`); non-BLE sources answer 400. The status renderer reads progress fields defensively (`discovery_percent`/`percent`, `found_modules`/`modules_found`) pending first-run confirmation of the exact `DISCOVERY_STATUS` schema. Adding three routes also tripped the `httpd` `max_uri_handlers` cap (was 35, only 2 free), which silently drops any handler past the limit and 404s it — `/api/tsdb/stats` registers last so it was the canary; the cap is now 48 with the count math documented inline.
- **Node-table dedup by long address** (#25). Two table entries sharing a 16-char long address are the same physical optimizer — ephemeral 802.15.4 short addresses leak into the data stream during commissioning and can mint a phantom entry (e.g. `8002` alongside `0002`). When Frame 27 vouches for an (addr ↔ long address) pair, any other entry with that long address is merged into it (CCA metadata and sensor index carried over) and dropped, along with its runtime device row; string groups rebuild if metadata moved. Imports apply the same rule.
- **Per-MPPT rollup in Topology** (#21). When an inverter has multiple MPPTs, or multiple strings share one MPPT (e.g. an 8s2p split), the Topology view now groups strings under an MPPT header with subtotal power, string count, and active-panel counts. Single-MPPT/single-string layouts stay flat.
- **Stale panels are visually distinct** (#24, #26). Dashboard heatmap tiles render stale (off-the-bus) panels as hatched gray instead of sharing the dead/zero look; Topology tiles get a dashed border and `stale` badge. The dashboard Alerts panel now also reports stale panels (collapsed to one row when more than 4), and flags a **string producing nothing while the rest of the system is producing** — the tripped-string case. Topology's panel join also drops its leftover `cca_validated` gate (same rule as the firmware fix in #18: a string label alone is authoritative).
- **Per-device staleness zeroing** (#26). New `stale_timeout` option (default 10 min, `0` disables): a device that stops reporting has its production values (power, current, efficiency, duty cycle) zeroed once the timeout passes, instead of polluting HA sensors, string aggregates, the web UI, and TSDB history with its last reading forever — night mode only covered the whole-bus-silent case. Voltage, temperature, and the data-age display keep their last-seen values for diagnostics; a fresh frame clears the flag. `/api/devices` gains a `stale` boolean per device.
- **Alert-count sensors for HA** (#24). Two new aggregate sensors — name them with "stale" (e.g. `Stale Panel Count`) or "zero production"/"no production"/"failed" (e.g. `Zero Production Count`) — report how many panels are stale vs. reporting-but-producing-nothing, for HA notifications. Both publish 0 in night mode so the dark hours don't generate alert noise.
- **Per-string power sensors** (#21, #23). `string_label: "A"` on a `tigo_monitor` sensor entry creates one HA entity carrying that string's aggregated power (same value the web UI shows), published every update cycle and zeroed in night mode. One entity per string instead of seven per panel — on a 64-panel install that's ~50-70 KB of internal RAM back at boot. Feed it into an HA Riemann-sum + Utility Meter for per-string daily energy.
- `allow_partition_access: true` in the example OTA configs — lets ESPHome 2026.5.0+ apply partition-table changes over OTA, so repartitions *after* 2.0 no longer need a serial flash.
- **`get_system_peak_power()` hub method for lambdas** (#29). Returns system-wide peak power (sum of per-inverter high-water marks) — the same value the dashboard uses as its "% of peak" denominator. Pair with the existing `get_total_power()` to compute percent-of-peak from a lambda (e.g. driving status LEDs on a Lilygo board) without scraping the web API: `id(tigo_hub).get_total_power() / id(tigo_hub).get_system_peak_power()`.

### Changed
- Minimum ESPHome bumped to **2026.5.0** (enforced via `esphome: min_version:`) — required by the `allow_partition_access` OTA option.
- `esp_tsdb` dependency now points at the `RAR/esp_tsdb` fork's `tigomonitor` branch (handle-based API + `esp32p4` target) instead of a local path / stale registry pin.
- **Routine heap/frame telemetry moved to debug level** (#23). The per-minute `Heap: Internal … free` and `Frame stats: … processed` lines logged at INFO every 60 s; the same numbers are already on the HA memory sensors, so they're now `ESP_LOGD`. The actionable signals — large memory-drop and low-RAM warnings — stay at WARN; bump the logger to DEBUG to watch a leak in detail.
- **Frame parsing no longer touches internal RAM** (#23). The escape-stripping and hex-conversion helpers staged their work in PSRAM but then copied the result back into an internal-heap `std::string` — every frame allocated its processed copy, a 2×-size hex copy, and per-packet slices internally, all day, scaling with device count. The whole pipeline now uses a PSRAM-backed `frame_string` end-to-end, and fixed-width hex fields are parsed in place (`strtol` on a stack buffer) instead of via `std::stoi(substr(...))` — which also removes a latent panic: exceptions are disabled on ESP-IDF, so `std::stoi` on a malformed bus byte would have aborted.

### Added
- **CCA Info page over BLE for HTTP-locked CCAs** (firmware 4.0.4+/4.0.5-ct). New `cca_source:` on `tigo_server:` — `http` (default), `ble`, or `auto` — chooses where the CCA Info page gets its data. With `ble`/`auto` plus a `ble_client_id`, `tigo_server` itself becomes the BLE client and talks the CCA's `mobile_api` over Bluetooth (single-byte ASCII frames, SHA-512 session-key auth — no HTTP password needed), caching each `DEVICE_INFO` for the page. Same `{sysid,serial,software,status[]}` shape, so the page renders unchanged with a `BLE <addr>` source label; `auto` prefers BLE once it has data and falls back to HTTP, `ble` is authoritative (shows "awaiting refresh" before the first pull). The dashboard's **Refresh from CCA** button drives the BLE pull when `cca_source` is `ble`/`auto` (connect → DEVICE_INFO → auto-disconnect, so the CCA is freed for the Tigo phone app); template-button lambdas (`id(tigo_web).cca_ble_connect()` / `cca_device_info()` / `cca_ble_disconnect()`) stay available for a held connection. The CCA Info page's address field reads **CCA address** (it's a BLE MAC, not an IP, under BLE). The whole BLE client is `#ifdef USE_TIGO_CCA_BLE`-guarded, so `http` builds pull in none of the `ble_client` stack. Set `esp32_ble: use_psram: true` to allocate the Bluetooth stack's buffers from PSRAM (~40 KB of internal RAM back) on tight boards. The dashboard refresh **polls** until the pull actually lands (the BLE round-trip is ~10 s, vs. the instant HTTP path) with a live "connecting…" counter, and folds the CCA's `NETWORK_INFO` (Ethernet/WiFi IP and SSID) into the readout. `esp32_ble: max_connections: 1` trims the BT controller to the single link in use. Folding BLE in overruns the stock 1.75 MB app slot, so a BLE-aware 8 MB layout with 2.0 MB app slots ships as `boards/partitions/tigo-8mb-ble.csv` (one-time reflash to repartition). Protocol adapted from the standalone [esphome-tigo-ble](https://github.com/RAR/esphome-tigo-ble) work.

### Changed
- **CCA Info page explains the firmware HTTP lockdown instead of a bare 401**. Tigo firmware 4.0.4+ (including 4.0.5-ct) closes the local HTTP API this integration reads for panel names/layout, so the CCA answers `DEVICE_INFO` with `401 Unauthorized`. The page used to show a blunt red "Failed to retrieve CCA information: HTTP 401"; it now renders an explanatory panel — the CCA is rejecting the request (not the ESP32 or its credentials), live RS485 monitoring is unaffected, and friendly names can still be imported manually from the Tools page — and the status badge reads **HTTP locked** (amber) rather than **Error** (red). 403 is treated the same way.

### Fixed
- **TSDB history is wiped on every reboot — LittleFS starved of copy-on-write headroom.** The tsdb files were sized to ~98% fill the 3 MB partition (2 MB system + 3×256 KB panels, "~60 KB headroom"). LittleFS is copy-on-write: committing a header write or its block-allocation journal needs a *free* block to write the new copy into. Near-full, no commit ever lands — so `fsync`'d data sits on flash but the metadata still points at empty blocks, headers read back garbage (`Failed to read header` / `Invalid magic`), and every DB reconstructs to `0 records` on boot. Even the clean-shutdown `lfs_unmount` couldn't help (nowhere to write the commit). Fixed by cutting the caps to **1 MB system + 3×192 KB panels (~52% of the partition)**, leaving LittleFS ~1.4 MB for metadata/COW/GC. Also added a `commit_journal_()` that rewrites a tiny marker file after each 5-min snapshot — a dir-tree op that forces the journal commit continuously, so data survives unclean reboots (crash, power loss, watchdog), not just clean ones. **Existing installs must wipe the tsdb partition once** (`esptool erase_region 0x480000 0x300000`, or a full `erase_flash`) since the old oversized files keep the FS full. Capacity is now ~113 days system / ~18 days per panel.
- **`Reset peaks` now clears the string/inverter rollup, not just per-device peaks** (#29). The Tools "Reset peaks" button (and `reset_peak_power()`) only zeroed per-device high-water marks, leaving the string/inverter/system rollup — the dashboard "% of peak" denominator and `get_system_peak_power()` — stuck at its since-boot maximum until a reboot. It now re-baselines the rollup too, so the next update cycle reflects live power. (The rollup peak is in-RAM and not persisted, so it remains "peak since boot" and intentionally does not reset at midnight, unlike per-device peaks and daily energy.)
- **IP and MAC now populate on Ethernet, not just WiFi** (#27). The diagnostics IP/MAC lookup was gated on `#ifdef USE_WIFI` and read from the WiFi component, so a wired build (e.g. Lilygo T-Connect Pro on Ethernet) showed `N/A` despite being reachable. IP now comes from the network layer (`network::get_ip_addresses()`, WiFi and Ethernet alike) and MAC from the efuse helper. RSSI/SSID stay WiFi-only by nature.
- **Internal temperature read 0 °C / N/A when another component owns the sensor** (#28). The ESP32 has a single internal-temperature peripheral that `temperature_sensor_install()` can claim exactly once, so on a board also running the `internal_temperature` platform our install lost the race and read nothing. New optional `internal_temperature_id` on `tigo_server:` points the Diagnostics die-temp readout at an existing `internal_temperature` sensor instead of installing our own — the conflict-free path. When no sensor is wired we still install our own (now over a widened −10…110 °C range so a hot S3 die stays in range). Either way, an unavailable read now emits JSON `null` and the UI shows **N/A** instead of a misleading `0 °C` (`temperature_sensor_get_celsius()`'s return was previously unchecked).
- **`Stale Panel Count` read 0 when dead panels were shown stale in the UI** (#24). The count walked only the runtime device list, but a panel assigned to a sensor slot with no runtime row this session (a removed/dead optimizer) is shown stale in the dashboard via the node-only render path and never appears there. The count now also includes those node-only assigned entries, so the HA sensor matches the stale tiles on the dashboard. Daytime-only — night mode still publishes 0.
- **Crash during OTA when a TSDB write was in flight.** The OTA shutdown hook closed and unmounted the TSDB even when the writer task failed to confirm exit within its timeout, racing a live `lfs_file_write` against `lfs_unmount` (concurrent littlefs access → panic mid-OTA). The close is now aborted instead when the writer doesn't ack — safe because every tsdb write is `fflush`+`fsync`'d, so flash already holds everything but the in-flight row — and the ack timeout is raised 1.5 s → 5 s to cover littlefs GC, the realistic worst case. A retried OTA picks up the writer's late ack and closes cleanly.
- **Steady internal-RAM leak (~3 KB/h per silent node) during daylight** (#23). ESPHome's `make_preference()` heap-allocates a backend that is never freed — it's meant to be called once per key at setup. The publish loop called it every cycle to re-load saved peak power for any node with a sensor index but no runtime data (a dead/spare optimizer or stale table entry), leaking ~28 B per node per cycle, day-only (night mode skips the loop) — which matched the observed flat-overnight / ~4 KB/h-daylight decline exactly. Node-table saves leaked ~2×N backends per save and energy saves 4 per hour, same root cause. All NVS access now goes through a hash-keyed preference cache (`cached_pref_`), so each key allocates its backend exactly once for the lifetime of the device.
- **Web UI exhausted internal RAM and crashed under sustained dashboard polling** (#23). Every `/api/devices`, `/api/strings`, `/api/inverters`, `/api/nodes`, and `/api/yaml` request deep-copied the whole device/node/string table by value. Only the containers are PSRAM-backed — each struct's `std::string` members allocate on the small *internal* heap — so an auto-refreshing dashboard fragmented internal RAM until a large allocation failed and `operator new` aborted (the crash landed in `build_yaml_json`, which copied the node table a second time). The hot JSON endpoints now serialize the live tables directly into the PSRAM response buffer under the state lock instead of snapshotting by value, and `build_yaml_json` sorts a vector of pointers instead of copying each `NodeTableData`. Pair with `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP: "y"` (see README) on large installs to keep socket buffers off the internal heap.
- **ESP-IDF 6.0 build: `fatal error: cJSON.h: No such file or directory`** (#15). IDF 6.0 removed the built-in `json` component that bundled cJSON; the C++ still includes `"cJSON.h"`. `tigo_monitor` now declares the `espressif/cjson` managed component (`^1.7.19`) on IDF ≥ 6.0 — guarded by version, since on IDF 5.x cJSON is still built-in and adding it would collide. One declaration covers `tigo_server` too (shared `src` target). Builds on IDF 5.x are unaffected.

### Documentation
- **Readability pass across the doc set.** Added a Configuration Guide table of contents and orientation, cross-link "See also" footers on every page (no more dead-end docs), and made `CONFIGURATION.md` the single source of truth — trimming the duplicated PSRAM/API/config content out of the README and web-server docs. Added current-generation CCA troubleshooting (firmware 4.0.4+ HTTP lockdown → `cca_source: ble`, BLE connect issues, cloud-import failures), stronger high-voltage wiring-safety warnings, and modernized the Home Assistant automation examples to current HA schema. Documented the previously-undocumented `stale_timeout` and `backlight` options.
- **Removed the internal CCA protocol reverse-engineering doc** and scrubbed personal CCA identifiers (MAC/IP) from the tracked example configs.

## [2.0.0-alpha.1] - 2026-05-20

### Added
- **Panel detail modal on the dashboard** — click any heatmap tile (desktop only) to open a modal with the panel's live readings (V/I/W, temp, RSSI, efficiency, duty cycle) and a power history chart sourced from `/api/history/panel?slot=N&range=day|week|month`.
  - Slot lookup uses `/api/panels` (barcode last-6 → tsdb slot), cached after first fetch.
  - **Peer overlay**: a dashed "string median" line overlays the panel's own line whenever there are ≥2 peers in the same string, so a single-panel dip is visually separable from string-wide shading.
  - **Live refresh**: live readings re-paint on the dashboard's 5-second tick while the modal is open; chart re-fetches on range change / reopen.
  - Hidden on viewports ≤768 px (modal would be too cramped on a phone); the heat tile cursor reverts to default in that case.
- **Sortable Node Table** — every column header (`Name`, `Addr`, `Barcode`, `String / MPPT`, `V`, `A`, `Power`, `Temp`, `Age`, `State`) is clickable. Click cycles ascending/descending; an arrow indicator marks the active column. Numeric columns default to descending on first click; State sorts by health order (`ok → warn → bad → stale`) so reversing surfaces problems first.
- **YAML generator emits `esphome.devices:` block + propagates `device_id`** to child sensors. Group panels into HA devices per-MPPT, per-inverter, per-panel, or stay flat (no devices) via a selector in the Tools view. The choice persists in `localStorage['tools-grouping']`.
  - Schema-level propagation: setting `device_id:` once on a panel base config now flows automatically to all child sensors (power_in, peak_power, voltage_in, etc.) — no need to repeat it.
- **Brand logo mark** — SVG mark in the sidebar header, matching favicon (`docs/images/logo.svg`).
- **Single-page web app at `/app`** (full UI redesign, R1–R7)
  - All views collapsed into one shell with sidebar nav and hash routing (`/app#dashboard`, `/app#history`, `/app#topology`, `/app#nodes`, `/app#tools`, `/app#diagnostics`, `/app#cca`).
  - Legacy paths (`/`, `/nodes`, `/status`, `/yaml`, `/cca`, `/history`) redirect to the corresponding `#view` so existing bookmarks keep working.
  - HA Ingress compliant: redirects use relative `Location: app#...` so they resolve under any URL prefix, and the SPA derives `BASE_PATH` from `window.location.pathname`.
  - Sidebar footer now carries a GitHub link, a `°C` / `°F` toggle (persists via `localStorage['tempUnit']`), and the existing theme toggle.
- **On-flash time-series history** (Phase 3 — esp_tsdb backed)
  - Persistent per-snapshot rollups (system + per-inverter) and per-panel power survive reboots and OTA updates.
  - Three lazy-opened panel DBs covering up to 48 panels (16 base params each).
  - New endpoints: `/api/history/power?range=day|week|month|year`, `/api/history/panel?slot=N&range=...`, `/api/panels`, `/api/tsdb/stats`.
  - Diagnostics view shows per-DB record counts, range, evictions, plus LittleFS partition usage.
  - Requires the `esp_tsdb` external component and a `tsdb` LittleFS partition (see [`docs/tsdb-integration.md`](docs/tsdb-integration.md)).
- **Topology view** — read-only inverter → string → panel hierarchy with live V/I/W/°C, search filter, expand/collapse, color-coded panel tiles.
- **Heatmap dashboard** — fixed-size colored tiles per panel grouped by string, replacing the old variable-height bar chart. Same good/warn/bad/dead bands so the legend still applies.
- **Color legend strip** on the dashboard showing the good/warn/bad/dead thresholds.
- **"This month" hero card** (replaces "Avg temperature") — sums current month's daily history plus today's running total; subline shows kWh/day average.
- **"% vs 5 min ago · % of peak" trend** on the Current Power hero card once a 5-min window of comparison data is available.
- **Inverter and string rename** — click the ✎ next to any inverter or string heading in Topology to set a friendly display name. Persists to NVS via `global_preferences` (keyed by canonical YAML/CCA name). YAML config remains the source of truth for identity; the override is purely cosmetic and used everywhere those names appear (Dashboard, Topology, alerts).
  - New endpoints: `POST /api/inverters/rename`, `POST /api/strings/rename`.
- **Per-string panel nameplate rating** — click the rating pill in Topology to set per-panel watts. Persists to NVS. When set, panel tiles show "% of rated" alongside watts and `panelClass` switches to rating-based health classification (with a "<5% of total nameplate → string sleeping" check so dawn doesn't paint everything red). String roll-up shows total output as % of total nameplate.
  - New endpoint: `POST /api/strings/rating` with `{label, rating_w}` (rating_w=0 clears).
- **Tools view** absorbs the YAML generator and adds inline Reset Peak Power / Reset Node Table / Restart device actions with toast feedback.
- **Diagnostics view** adds a TSDB section (open DBs, record counts, oldest/newest timestamps, evictions, file sizes) alongside the existing memory / network / UART / version cards.
- **Node table Export / Import** restored on the Nodes view (same JSON shape as before).

### Changed
- **Visual polish across the SPA** — a single design language across cards:
  - 2 px accent rainbow rail (`--accent → --accent-2 → --accent-3`) across the top of all major container cards (inverter cards, chart cards, topology inverter cards, the panel detail modal).
  - Subtle diagonal accent gradient overlay (green→blue→transparent) on container cards.
  - Smaller, lighter gradient on grouped stat tiles (`.hist-stat-card`, used on History and Diagnostics) — no rail, since rails on every tile turn into wallpaper.
  - Daily-energy bars use a vertical gradient (top → faded at baseline) instead of flat 0.85-opacity accent-2.
  - System power chart's area uses a vertical gradient (accent at top, transparent at bottom) instead of a flat 12 %-opacity slab.
  - Sidebar gets a subtle vertical gradient (accent overlay top → `bg-2 → bg-3` base).
  - "Current Power" hero card picks up the same accent diagonal gradient as the panel modal, with an accent-green border tint.
  - Brand name in sidebar bumped from inherited 14 px → 16 px.
- **Panel heat tile** "% rated" badge moved to its own line for readability on narrow tiles.
- **Dashboard mobile layout** — responsive sidebar drawer + tile reflow for ≤768 px viewports.
- **TSDB buffer pools moved to PSRAM** (`TSDB_ALLOC_PSRAM`) — reclaims ~28 KB of internal heap on units with PSRAM. Internal heap low-water improved from ~110 KB to ~150 KB on the 8 MB AtomS3R reference rig.
- Diagnostics view active-sockets card no longer crashes when the inner span is replaced (`innerHTML`-based render).
- Sparkline buffers and string-grouping maps continue to key on canonical names so renames don't lose history or break grouping.

### Fixed
- **Aggregate sensors stuck at "Unknown value"** when their names collided under keyword inference. No-address sensors infer their type from `name` keywords, but the matcher used raw substring tests: `"Invalid Checksum Count"` matched `sum` inside "check**sum**" (→ power sum), `"Free Internal RAM"` matched `e in` inside "fre**e in**ternal" (→ energy), and `count` was tested before `frame`. Each hub setter holds a single pointer, so colliding names overwrote each other and the losers never published. The Tools YAML generator emits exactly these names, so generated configs were affected too (surfaced in discussion #11). Keyword matching is now whole-word (`\b` boundaries) via one shared classifier, with specific categories ordered before broad ones.
- **OTA reboot crash inside `tsdb_close_h`** (`IllegalInstruction` → newlib `_lock_close` assertion → `panic_abort`). The drain loop only checked `uxQueueMessagesWaiting() == 0`, but the writer task could already have pulled the last row and be mid-`fwrite` — holding the FILE's per-stream lock. `fclose` then tripped newlib. Fix: writer recognises a sentinel `EncodedRow` (timestamp = `UINT32_MAX`), gives a `writer_done_` binary semaphore, and self-deletes. `flush_and_close` enqueues the sentinel after its drain and waits on the semaphore (1500 ms cap) before any `tsdb_close_h` — no possible race with `fclose`. Filed upstream as a note on `zakery292/esp_tsdb#1` suggesting `tsdb_close_h` mirror the mutex-held fclose pattern already used in `tsdb_sync_h`.
- **`process_power_frame` / `process_09_frame` substr OOB** crashes on short or malformed RS485 frames. `process_09_frame` did `frame.substr(40, 6)` without checking length; with `-fno-exceptions` (ESP-IDF default), `std::out_of_range` becomes `abort` and the unwind surfaced as `InstFetchPrivilege`. Added upfront length guards. `process_27_frame` already guarded its reads.
- **Panel modal showed "—" for Current in** — the `/api/devices` JSON field is named `"current"` (filled from `device.current_in`), not `"current_in"`; modal now reads the correct key. Duty cycle is already a percent in the JSON, so the suffix is now `%`.
- **Panel modal card was transparent** because the CSS used `var(--bg-1)`, which isn't defined (`:root` defines `--bg`, `--bg-2`, `--bg-3`, `--bg-4`). Switched to `--bg-2`.
- **Panel modal showed prior panel's history** during the in-flight fetch when reopening on a different panel — chart polylines now reset to empty + "…" placeholder on every open/range-change.
- **YAML generator emitted redundant `tigo_mppt_mppt_4`-style IDs** when the user's label already started with the type tag. Generator now skips the type prefix when the slug already begins with it (so "MPPT 4" → `tigo_mppt_4`, "FlexBoss A" → `tigo_inverter_flexboss_a`).
- **History wiped on every reboot** when using esp_tsdb (only with `feat/handle-based-api` fork). Root cause: `tsdb_open` used `stat()` to detect file existence, but joltwallet's `esp_littlefs` returns `ENOENT` from `stat()` for files that `fopen("rb")` immediately reads bytes back from. Every boot took the create-new path and `fopen("w+b")` truncated the existing file. Fix in the upstream PR (`zakery292/esp_tsdb#1`): try `fopen(..., "r+b")` first; fall through to create-new only on failure. Slot map (panel_map.json) was unaffected because it uses `open(wb)+write+close` per save, which never hits the bad code path.
- **Nodes view crashed at active-sockets** during R5 diag-view rendering (fixed in `2f14ad8`).
- **Persistence cleanup on shutdown**: `TigoMonitorComponent::on_shutdown` now drains the TSDB writer queue (best-effort, 800 ms cap), `tsdb_close_h`s every open handle, and unmounts LittleFS so the journal commits cleanly before `esp_restart`.

### Dependencies
- New optional dependency: `esp_tsdb` (currently from local fork at `feat/handle-based-api`; will return to a registry pin once `zakery292/esp_tsdb#1` is merged and tagged).
- New required dependency for history features: `joltwallet/littlefs` (any 1.16+).

## [1.4.3] - 2026-04-17

### Added
- **Home Assistant Ingress support** ([#7](https://github.com/RAR/esphome-tigomonitor/pull/7)) — contributed by [@aeozyalcin](https://github.com/aeozyalcin)
  - Web UI now works when proxied through Home Assistant Ingress (e.g. the [hass_ingress](https://github.com/lovelylain/hass_ingress) integration)
  - Nav links are set dynamically from `window.location.pathname` so they resolve under any URL prefix
  - `apiFetch()` prepends the detected `BASE_PATH` to all `/api/*` requests
  - Post-reset redirect on the Status page returns to the ingress-prefixed root
  - Purely client-side JavaScript change — no C++ or ESPHome config changes required for standalone access
  - See [`docs/WEB_SERVER_README.md`](docs/WEB_SERVER_README.md#home-assistant-ingress) for setup details

## [1.4.2] - 2026-03-19

### Fixed
- **ESPHome 2026.3 compatibility: `register_component` removed from public API**
  - Removed redundant `App.register_component(this)` call in `setup()` — component is already registered via codegen
- **Deprecation warning: `wifi_ssid()` deprecated in ESPHome 2026.3.0**
  - Switched to `wifi_ssid_to()` buffer-based API (old API removed in 2026.9.0)

## [1.4.1] - 2026-02-25

### Fixed
- **Remote package compile error: `esp_http_client.h` not found**
  - `__init__.py` used `cg.add_library()` which doesn't work for ESP-IDF built-in components
  - Now uses `esp32.include_builtin_idf_component("esp_http_client")` — the correct ESPHome pattern
  - Only affected remote/git installations; local path installs worked because the IDF component was already resolved
- **Deprecation warning: `IPAddress::str()` removed in ESPHome 2026.8.0**
  - Switched to `str_to()` API in web server status page

## [1.4.0] - 2026-02-25

### Added
- **Output-Channel Telemetry** ([#6](https://github.com/RAR/esphome-tigomonitor/pull/6)) — contributed by [@aeozyalcin](https://github.com/aeozyalcin)
  - New per-device `current_out` and `power_out` sensors derived from duty cycle
  - New `power_in` sensor (explicit input power with calibration applied)
  - New hub-level `Total Output Power` aggregate sensor
  - Input vs output power split enables accurate per-device efficiency tracking
  - `power_out` and `current_out` displayed on web UI device cards
- **TS4-A-S (Monitor-Only) Module Support** ([#5](https://github.com/RAR/esphome-tigomonitor/issues/5))
  - Auto-detect monitor-only modules (voltage_out ≈ 0, voltage_in > 1V)
  - Monitor-only devices mirror input values to output and report 100% efficiency
- **Aggregate Sensor Naming: Input/Output Distinction**
  - Hub sensors now support `input`/`output` keywords for power and energy
  - e.g. "Total Input Power", "Total Output Power", "Total Input Energy", "Total Output Energy"
  - Backward compatible — existing names like "Total System Power" still route to input variants

### Fixed
- **Duty Cycle Sensor: HA Now Reports 0–100%**
  - Raw UART byte (0–255) was published directly to Home Assistant as "%"
  - Now normalized to 0–100% matching the web UI display
- **YAML Generator: Hub-level sensor output format** ([#4](https://github.com/RAR/esphome-tigomonitor/issues/4))
  - YAML config page (`/yaml`) was generating hub-level sensors in a nested sub-key format (`power_sum:`, `energy_sum:`, etc.) under a single platform entry, which is invalid
  - Hub sensors now correctly generate as separate `- platform: tigo_monitor` entries with keyword-rich names that match the sensor type auto-detection in `sensor.py`
  - Names updated to use proper keywords: "Total System Power", "Total System Energy", "Active Device Count", etc.
- **DeviceData struct field initialization** — `current_out`, `power_in`, `power_out` now default to `0.0f`

### Changed
- **Power calculation consolidated into `DeviceData`** — power was previously calculated in 8 separate places; now computed once during frame processing
- **Daily energy history and overview totals switched to output-channel tracking**
- **Documentation: Quick Start overhaul** ([#3](https://github.com/RAR/esphome-tigomonitor/issues/3))
  - `external_components` now uses expanded `type: git` / `url:` format instead of shorthand `github://` which may not resolve correctly
  - Board changed from `esp32dev` to `esp32-s3-devkitc-1` (generic ESP32-S3)
  - Added `logger:` to base config
  - Added required empty `sensor:`, `text_sensor:`, `binary_sensor:` stub sections with explanation — without these, compilation fails due to missing generated headers
  - Added generic ESP32-S3 PSRAM config example alongside existing M5Stack example
  - Added PSRAM bootloader gotcha: must clean build + erase flash when switching from non-PSRAM to PSRAM config
- **Documentation: Expanded system-level sensor reference** in `CONFIGURATION.md`
  - Added all 9 hub-level sensor types with complete YAML examples (was only showing 4)
  - Documented full keyword list for each sensor type including all accepted synonyms
  - Added note clarifying that hub sensors must each be their own platform entry
- **Troubleshooting: New sections**
  - Added fix for `fatal error: sensor/sensor.h: No such file or directory` compile error
  - Added `external_components` loading troubleshooting with correct format examples
  - Added PSRAM not detected after enabling (bootloader rebuild required)

## [1.3.1] - 2025-12-03

### Added
- **CCA Firmware 4.x Support**
  - Support both 13-byte (pre-4.x) and 15-byte (4.x+) power frame formats
  - Auto-detect format based on data length field (0x0D vs 0x0F)
  - Parse slot counter and RSSI from correct positions for each format
  - Maintains full backward compatibility with older firmware
  - Addresses issue reported in willglynn/taptap#20
- **Frame 27 Diagnostic Counters**
  - Added `command_frame_count` to track all 0B10/0B0F command frames received
  - Added `frame_27_count` to track specifically Frame 27 node table responses
  - Both counters exposed via `/api/status` JSON endpoint for debugging
- **Improved Frame 27 Logging**
  - Log starting_index and entry count when processing Frame 27
  - Warning when node table is full and cannot create new entries
  - Info log when saving node table after Frame 27 updates

### Fixed
- **Frame 27 Node Table Parsing**
  - Fixed incorrect payload offset in Frame 27 (Node Table Response) parsing
  - Previous code read `num_entries` from offset 18, but per taptap protocol that position contains `starting_index`
  - Correct structure: `[starting_index:4 hex][num_entries:4 hex][entries...]` at offset 18
  - Now correctly parses all device entries (was only parsing 2 per frame instead of 12)
  - Node table now properly populates with all device long addresses from gateway
  - CCA sync now has complete device data to match against
- **Midnight Reset Memory Leak**
  - Fixed preference handle accumulation causing heap fragmentation
  - Eliminated heap allocations in Frame 27 processing loop
  - Use stack-allocated buffers instead of std::string temporaries
- **Heap Monitoring Bug**
  - Fixed unsigned underflow in heap change calculation
- **Daily Energy Date Display**
  - Fixed date calculation for energy history chart labels

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
