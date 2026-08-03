# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Docs screenshots generate themselves.** The guides now show the real device UI — dashboard, history, topology, node table, diagnostics — rendered during the docs build by driving the actual `app.html` against synthetic API responses. No firmware, no hardware, and nothing committed: the images are rebuilt from the current UI every time, so they cannot drift from it. All data is invented, with IPs and MACs from the IETF documentation ranges and a reserved fake-serial prefix, enforced by a test so a real serial, SSID or address can't reach a published page.

### Changed
- **History resolution is now yours to set, and twice as fine by default.** The snapshot interval had been coarsened twice — 5 to 30 minutes, then 30 to 60 — not because anyone wanted less detail, but because each snapshot's flash writes were what triggered the crash below, so the only lever available was doing it less often. With the underlying fault addressed it becomes a normal trade-off, so it's now a setting: `history_interval`, 5 to 1440 minutes, defaulting to 30. Lower is not free — each database holds a fixed number of readings however often you fill it, so twice the detail is half the span, and every snapshot rewrites its files end to end regardless of how little changed. At the 30-minute default you get about 4 months of per-panel history and 2 years system-wide; at 10 minutes, about 5 weeks and 7 months, with roughly three times the flash wear. Anything under 15 minutes says so at build time. Retuning later is safe — each row stores energy since the previous snapshot rather than a running total, so lifetime figures stay correct and existing history isn't invalidated.
- **Docs rewritten for first-time installers.** A new **Start Here** guide covers what to buy, what the jargon means, and the five steps from parts on the desk to a live dashboard — the "Get Started" button used to drop you into a high-voltage warning and a terminal-block diagram. The sidebar is now ordered by when you need something rather than alphabetically, the Config Builder explains what each field is for, and the reference-heavy pages say up front whether you need to read them. No behaviour changed.
- **Docs site recoloured to the brand mark.** Link and accent colours moved from green to the mark's silicon indigo and busbar silver; amber keeps its one meaning — this part is live. The blueprint grid behind the headline is now a PV array receding toward the horizon — cells, busbars and module frames, drawn in CSS gradients so it stays sharp at any size and re-tints with the theme.
- **New brand mark.** The green-to-blue tile is replaced by an optimizer emitting a telemetry frame, in silicon indigo and amber. It's drawn at three sizes rather than scaled — the device favicon, the device UI sidebar, and the docs — so it stays legible in a browser tab. The docs site had no logo or favicon configured before and now carries the same mark the device serves.

### Removed
- **Boards without PSRAM are no longer offered.** The Config Builder listed the M5Stack AtomS3/AtomS3 Lite and a generic ESP32 DevKit, both of which lack PSRAM — so it could hand you a ready-to-flash YAML for a board the project doesn't support. Both are gone from the builder, along with their `boards/*.yaml` reference configs, and a test now blocks a PSRAM-less board from being added back.

### Fixed
- **The flash-write crash appears to be fixed, not just made rarer.** The device now executes from PSRAM (`execute_from_psram`), and firmware configures this itself — you don't have to know it exists. Every history commit takes an ESP-IDF lock that disables the instruction cache across both cores; with code running from flash, that stall raced the WiFi/BLE radio ISRs and faulted. ESP-IDF skips the cache-disable entirely when instructions and read-only data live in PSRAM instead, which removes the race rather than shrinking its window. Previous builds survived 13.5, 42.0 and 14.0 hours before faulting; the current one has run **142 hours and counting** with no movement in any memory watermark, and with the per-panel databases already at full size — so it has been running at the worst-case flash cost the whole time, not easing into it. Costs about 1.7 MB of the 8 MB PSRAM, which moves out of the heap to hold the relocated code. Applies to ESP32-S3 boards with PSRAM configured; the reference config and the Config Builder set the flag explicitly as well. Note that the 142-hour run was at the old hourly cadence, and the new default writes twice as often — so the fix now carries more load than the run that demonstrated it.

- **Opening the Diagnostics page could reboot the device.** The page asked for `/api/tsdb/stats` every 10 seconds, and building that response was the only thing in the whole web UI that made the device read flash. On the ESP32-S3 every flash access stalls the other core, and doing it from the web server while the history writer is mid-write faults the writer. Reproduced on demand: four concurrent callers killed the device in about eleven seconds, while four concurrent callers of `/api/status` — same load, no flash — ran indefinitely. The figures now come from a snapshot the history writer takes during its own commit, so serving them touches no flash at any request rate. They can be up to one snapshot interval old, and the page now says how old ("sampled 4m ago") rather than implying they're live. This did not itself fix the underlying flash-versus-cache fault — that is addressed separately above — but it removed the way the UI was provoking it.
- **Opening the web UI could reboot the device.** History pages read the TSDB from the web server's task while the periodic snapshot writes it from another — and on the ESP32-S3 every flash access, read included, cuts the instruction cache on both cores, so the two collide and fault (`Fault - Unknown`). All LittleFS access now goes through one lock. History requests can queue briefly (a month-range query holds the flash ~2.3 s) and return an error rather than hang if they wait more than 15 s.

## [2.0.0-beta.5] - 2026-07-25

### Added
- **BLE on the ESP32-P4.** The P4 has no radio of its own, but ESPHome runs the Bluedroid host on it against the ESP32-C6 companion's controller — so `cca_source: ble` works over the same SDIO link that carries Wi-Fi. The Config Wizard now offers BLE for this board. No repartition needed; the BLE build fits the existing 3 MB app slots at 1.80 MB.

### Fixed
- **P4 managed only one CCA-over-BLE session per boot.** After the first session Bluedroid switches from `LE_Create_Connection` (`0x200D`) to `LE_Extended_Create_Connection` (`0x2043`), and the C6 refuses the latter with HCI Cmd Disallowed — so every later connect failed with GATT 133. Choosing BLE on a P4 now sets `CONFIG_BT_BLE_50_FEATURES_SUPPORTED=n`, keeping Bluedroid on the initiator the C6 accepts. Verified on hardware: 3 of 3 consecutive refreshes. The trade is extended advertising/scanning, which the CCA doesn't use — only relevant if you also want a `bluetooth_proxy` on the same board.

## [2.0.0-beta.4] - 2026-07-24

### Added
- **CCA Info/Network snapshot survives reboot.** The Info and Network pages were RAM-only, so a restart blanked them until the next BLE round-trip. Both are now cached to NVS and restored at boot, so `/cca` repopulates instantly with no BLE traffic. "Last synced" is epoch-based, so it reports the true age across a restart.
- **Config Wizard** (GitHub Pages) — generates a complete, board-tailored starter YAML for AtomS3R, AtomS3, ESP32-P4 EVBoard and generic ESP32, covering PSRAM, partitions, `esp_hosted`, CCA source, cloud import and secrets. Board data is drift-checked against `boards/*.yaml` in CI.

### Fixed
- **ESP32 die temperature always read `null`.** The sensor never installed: we requested a -10…110 °C range, which spans two hardware ranges and matches none, so the install failed. Now requests -10…80 °C — a real range, and the most accurate one.
- **A TSDB database vanished from the History diagnostics table** when its stats call hit the 5 s lock timeout (which happens when a poll lands on a flush). The row now shows as unavailable with the error instead of silently disappearing. Also fixes invalid JSON when the *first* database was the one skipped.
- **ESP32-P4 configs boot-looped and lacked the 2.0 storage scaffolding** (discussion #31). The fix is `execute_from_psram: true` — without it a PSRAM-resident task stack becomes unreachable during flash cache-disable windows and the board asserts before ESPHome starts. If a board still crash-loops, drop PSRAM to `100MHz` or `20MHz` (valid P4 speeds are 20/100/200 MHz). The examples also gained `flash_size: 16MB`, the tsdb partition table, `esp_tsdb` + `littlefs`, and the `esp32_hosted:` C6 block.

### Changed
- **Internal RAM freed by moving allocations to PSRAM.** mbedtls TLS buffers (idle floor +11.8 KB on the AtomS3R reference build, and the 18.6 KB handshake dip is gone); cloud API responses; all cJSON parsing, process-wide; CCA HTTP bodies and cloud credentials; the string members inside the node/device/string structs (~100 permanently-resident internal allocations at 40 nodes); and the BLE CCA caches.
- **Config Builder and docs synced with the new sdkconfig defaults** — `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` and `CONFIG_VFS_SUPPORT_DIR`. New troubleshooting entries for the 0 KB size column, the `stats unavailable` row, and a dead die-temperature reading.
- **Corrected a stale root cause in the TSDB guide.** The reboot-wipes-history bug was `CONFIG_VFS_SUPPORT_DIR` being off — which compiles out the VFS directory syscalls, so `stat()` fails for every path — not `esp_littlefs` mishandling `stat()`.

## [2.0.0-beta.3] - 2026-07-21

### Fixed
- **Dashboard "Today's energy" showed a runaway lifetime total** (~1067 kWh for an 86 kWh day). It read the raw lifetime accumulator, which only zeroes at midnight when `reset_at_midnight` is on (off by default). `/api/overview` now exposes a proper `today_energy` day-delta, leaving the accumulators monotonic for Home Assistant. Also fixes the "This month" card double-counting today.

### Changed
- **BLE builds must stay on `version: recommended`** (#33). An explicit `version: "6.0.x"` pin selects a moving branch now delivering IDF 6.0.2, which relocated the Bluedroid headers off the global include path — any BLE source then fails with `fatal error: esp_bt_defs.h: No such file or directory`. Upstream ESPHome issue; IDF 6 + BLE stays blocked until it's fixed there. The default boards use `recommended` already and are unaffected.
- **`esp_tsdb` now comes from the upstream registry release** (`zakery292/esp_tsdb^2.1.0`) instead of the fork — 2.1.0 merged everything we needed. The P4 keeps the fork pin until 2.1.0 lists the `esp32p4` target.
- **"Sync to cloud" is now "Sync with cloud"** (CCA Info page) — the check-in is bidirectional: the CCA pushes telemetry *and* pulls pending config changes.

## [2.0.0-beta.2] - 2026-07-02

### Fixed
- **Tigo cloud TLS failed on ESP-IDF 6.0** — `No matching trusted root certificate found` on login. ESPHome disables mbedtls SHA-384/512 on IDF ≥ 6.0 to save flash, and every signature above the leaf in Tigo's chain is SHA-384, so the intermediate was silently discarded mid-handshake. `tigo_server` now requires SHA-512 whenever `cloud_import` or CCA BLE is enabled, and verifies against pinned Google Trust Services roots R1–R4.
- **CCA BLE session key could be silently wrong on IDF 6.0** — the key is a SHA-512 hash, and with SHA-512 compiled out the hash failed while the return status went unchecked, yielding a garbage key and a failed login. Same fix, plus the status is now checked and logged.
- **IDF 6.0 build failed with `cca_source: ble`** — `mbedtls/sha512.h` no longer exists on mbedtls 4.0. The session-key hash now uses PSA on IDF ≥ 6.0 and the legacy API on 5.x.

## [2.0.0-beta.1] - 2026-07-02

### Added
- **CCA Info page over BLE, for HTTP-locked CCAs** (firmware 4.0.4+). Tigo closed the local HTTP API this integration reads. New `cca_source:` — `http` (default), `ble`, or `auto` — lets `tigo_server` talk the CCA's `mobile_api` over Bluetooth instead, so the page keeps working. **Folding BLE in overruns the stock 1.75 MB app slot**, so 8 MB boards need a one-time repartition to `boards/partitions/tigo-8mb-ble.csv`.
- **Find CCA over Bluetooth** (CCA Info page) — scan for the CCA and pick it from a list instead of hardcoding its MAC in YAML. The choice persists to NVS and overrides the compile-time MAC; **Revert** restores the YAML value.
- **CCA network status + WiFi config over BLE** — Ethernet/WiFi status cards, plus scan, join and clear for the CCA's WiFi. Ethernet stays read-only by design: a wrong static-IP write could orphan the CCA's uplink.
- **CCA topology discovery over BLE** — kick the CCA's optimizer/gateway rescan and poll its progress.
- **Tigo cloud layout import** (`cloud_import: true`) — recovers panel names and the string/MPPT/inverter layout from Tigo's cloud, which is exactly what's lost once CCA firmware locks local HTTP. A new Tigo Cloud page also surfaces Tigo's own warning/error analytics per device. Only the bearer token is persisted, never the password.
- **On-device configuration** (Tools › Device Configuration) — edit `power_calibration`, `night_mode_timeout`, `reset_at_midnight`, `sync_cca_on_startup` and `cca_ip` in the web UI and persist to NVS, no reflash. YAML remains the default and each field has a **Revert**. Structural settings and auth stay in YAML, since NVS is plaintext at rest.
- **Manual Topology overrides travel with node-table export/import** — inverter names, string labels and per-string panel ratings survive a restore or a move to another unit.
- **Per-string power sensors** (#21, #23) via `string_label:` — one HA entity per string instead of seven per panel, worth ~50–70 KB of internal RAM at boot on a 64-panel install.
- **Per-device staleness zeroing** (#26). New `stale_timeout` (default 10 min, `0` disables): a device that stops reporting has its production values zeroed instead of reporting its last reading forever. Voltage and temperature keep their last-seen values.
- **Stale panels are visually distinct** (#24, #26) — hatched dashboard tiles, a `stale` badge in Topology, and alerts for stale panels and for a string producing nothing while the rest of the system is.
- **Alert-count sensors for HA** (#24) — stale-panel and zero-production counts, both publishing 0 in night mode.
- **Node-table dedup by long address** (#25) — merges the phantom entries that ephemeral commissioning addresses can mint.
- **Per-MPPT rollup in Topology** (#21) for multi-MPPT inverters and shared-MPPT strings. Single-string layouts stay flat.
- **`get_system_peak_power()` hub method** (#29) — system-wide peak for lambdas, to compute percent-of-peak without scraping the web API.
- `allow_partition_access: true` in the example OTA configs, so repartitions after 2.0 no longer need a serial flash.

### Changed
- Minimum ESPHome is now **2026.5.0**, required by `allow_partition_access`.
- **CCA Info page explains the firmware HTTP lockdown** instead of showing a bare red `HTTP 401`. The badge reads **HTTP locked** (amber), and the page notes that RS485 monitoring is unaffected.
- **Frame parsing no longer touches internal RAM** (#23). The pipeline was staging in PSRAM then copying back to an internal-heap string on every frame. It's PSRAM-backed end to end now, which also removes a latent abort — exceptions are off, so `std::stoi` on a malformed bus byte would have panicked.
- **Routine heap/frame telemetry moved to debug level** (#23); the actionable low-RAM warnings stay at WARN.
- `esp_tsdb` now points at the `RAR/esp_tsdb` fork's `tigomonitor` branch.

### Fixed
- **TSDB history wiped on every reboot** — LittleFS was starved of copy-on-write headroom. The tsdb files filled ~98% of the partition, and LittleFS needs a free block to commit metadata into, so no commit ever landed: data reached flash but the metadata still pointed at empty blocks, and every database read back as `0 records`. Caps cut to 1 MB system + 3×192 KB panels (~52% of the partition), plus a journal-commit marker so data now survives unclean reboots too. **Existing installs must wipe the tsdb partition once** — `esptool erase_region 0x480000 0x300000`, or a full `erase_flash`. Capacity is ~113 days system / ~18 days per panel.
- **Web UI exhausted internal RAM and crashed under sustained dashboard polling** (#23). Every hot JSON endpoint deep-copied the whole device/node/string table, and the struct strings live on the small internal heap — so an auto-refreshing dashboard fragmented it until an allocation aborted. Those endpoints now serialize the live tables straight into the PSRAM response buffer.
- **Steady internal-RAM leak of ~3 KB/h per silent node during daylight** (#23). `make_preference()` allocates a backend that is never freed and is meant to be called once per key; the publish loop called it every cycle. All NVS access now goes through a preference cache.
- **Crash during OTA when a TSDB write was in flight.** The shutdown hook unmounted the filesystem even when the writer task hadn't confirmed exit, racing a live write against `lfs_unmount`. The close is now aborted instead — safe, since every write is already `fsync`'d.
- **`Reset peaks` now clears the string/inverter rollup**, not just per-device peaks (#29) — the dashboard's "% of peak" denominator was stuck until a reboot.
- **IP and MAC now populate on Ethernet, not just WiFi** (#27) — a wired build showed `N/A` despite being reachable.
- **Internal temperature read 0 °C / N/A when another component owns the sensor** (#28). The ESP32 has one such peripheral and it can be claimed once. New `internal_temperature_id` points our readout at an existing sensor instead of fighting for it.
- **`Stale Panel Count` read 0** while the dashboard showed those panels stale (#24) — the count skipped entries with no runtime row this session.
- **ESP-IDF 6.0 build failed with `fatal error: cJSON.h`** (#15) — IDF 6.0 dropped the bundled `json` component; `espressif/cjson` is now declared on IDF ≥ 6.0 only.

### Documentation
- **Readability pass across the doc set** — Configuration Guide table of contents, "See also" cross-links on every page, and `CONFIGURATION.md` as the single source of truth. Added current-generation CCA troubleshooting, stronger high-voltage wiring warnings, and modernized Home Assistant examples.
- Removed the internal CCA protocol reverse-engineering doc and scrubbed personal CCA identifiers from the tracked example configs.

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
  - Requires the `esp_tsdb` external component and a `tsdb` LittleFS partition (see [TSDB Integration](https://rar.github.io/esphome-tigomonitor/guides/tsdb-integration/)).
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
- **YAML generator emitted redundant `tigo_mppt_mppt_4`-style IDs** when the user's label already started with the type tag. Generator now skips the type prefix when the slug already begins with it (so "MPPT 4" → `tigo_mppt_4`, "South Roof" → `tigo_inverter_south_roof`).
- **History wiped on every reboot** when using esp_tsdb (only with `feat/handle-based-api` fork). Root cause: `tsdb_open` used `stat()` to detect file existence, but joltwallet's `esp_littlefs` returns `ENOENT` from `stat()` for files that `fopen("rb")` immediately reads bytes back from. Every boot took the create-new path and `fopen("w+b")` truncated the existing file. Fix in the upstream PR (`zakery292/esp_tsdb#1`): try `fopen(..., "r+b")` first; fall through to create-new only on failure. Slot map (panel_map.json) was unaffected because it uses `open(wb)+write+close` per save, which never hits the bad code path.
- **Nodes view crashed at active-sockets** during R5 diag-view rendering (fixed in `2f14ad8`).
- **Persistence cleanup on shutdown**: `TigoMonitorComponent::on_shutdown` now drains the TSDB writer queue (best-effort, 800 ms cap), `tsdb_close_h`s every open handle, and unmounts LittleFS so the journal commits cleanly before `esp_restart`.

### Dependencies
- New optional dependency: `esp_tsdb` (currently from local fork at `feat/handle-based-api`; will return to a registry pin once `zakery292/esp_tsdb#1` is merged and tagged).
- New required dependency for history features: `joltwallet/littlefs` (any 1.16+).

## [1.4.5] - 2026-06-01

### Fixed
- **Wrong HA units on diagnostic aggregate sensors** (e.g. "Invalid Checksum Count" reported in **W**, "Min Free Internal RAM" in **kWh**). The aggregate-sensor type was inferred by naive substring matching, so `"sum"` matched inside *check`sum`* (→ power/W) and `"e in"` matched inside *fre`e in`ternal* (→ energy/kWh). Classification is now whole-word (regex `\b`) with an explicit rule order, so checksum/frame/RAM names route correctly. Backport of the classifier already on the 2.0 (`next`) line.
- **Crash (`std::bad_alloc` abort) loading web pages with many devices.** Each HTML page was assembled as a single `html.append(R"html(...)" + api_token_ + R"html(...)")` expression, which built the entire page as one large `std::string` temporary in **internal** RAM before copying it to the PSRAM-backed buffer. On a busy device (e.g. 66 optimizers + TSDB) that internal allocation could throw `bad_alloc` and abort (seen in `get_dashboard_html`). The API-token splice is now three separate `append()`s so each literal streams straight to PSRAM with no large internal temporary. Affected all five HTML pages.
- **Crash (`IllegalInstruction` abort) on power frames** ([#17](https://github.com/RAR/esphome-tigomonitor/issues/17)). `process_power_frame()` read fixed-offset fields without checking the frame was long enough; a checksum-passing but short frame handed an empty substring to `std::stoi()`, which threw `std::invalid_argument` and aborted the device. Added a length guard that drops short frames instead of crashing.
- **CCA 4.x power frames not parsed — wrong new-format slot/RSSI offsets** ([#14](https://github.com/RAR/esphome-tigomonitor/discussions/14)/[#17](https://github.com/RAR/esphome-tigomonitor/issues/17)). Decoding real 4.x frames showed the 15-byte "new format" is the legacy 13-byte layout plus 2 trailing pad bytes (`0x0000`) appended **after** RSSI — it does not insert bytes or shift the tail. The code read slot counter at char 40 and RSSI at char 44, both past the end of a 44-char frame (so every new-format frame was dropped, and previously crashed). Slot counter and RSSI now read at the legacy offsets (34 / 38) for both formats; voltage/current/temperature were already correct.

## [1.4.4] - 2026-05-30

> **Bridge release — the recommended hop before 2.0.** Functionally identical
> to 1.4.3; its only job is to upgrade the OTA receiver. 2.0 reshapes the flash
> partition layout for the on-flash time-series database, and that repartition
> can only be applied over OTA by a firmware that already speaks the new OTA
> protocol. 1.4.3 does not — so without this bridge the 1.x → 2.0 jump needs a
> serial flash.
>
> **Upgrade path:** `1.4.3` → OTA → `1.4.4` → OTA → `2.0.0`. The `1.4.3 → 1.4.4`
> hop is an ordinary OTA. The `1.4.4 → 2.0` hop then performs the verified
> partition reshape over OTA — no USB cable. The reshape still wipes NVS, so
> export your node table (Tools → Export) before the second hop.

### Added
- `allow_partition_access: true` in the example OTA configs. Built on
  ESPHome 2026.5.0+, this opts the OTA receiver into the partition-table
  update protocol so a later firmware can repartition the device over OTA.

### Changed
- Built against ESPHome 2026.5.0+ (required for the OTA partition-update
  protocol).

### Fixed
- **ESP-IDF 6.0 build: `fatal error: cJSON.h: No such file or directory`** ([#15](https://github.com/RAR/esphome-tigomonitor/issues/15)). IDF 6.0 removed the built-in `json` component that bundled cJSON; the C++ still includes `"cJSON.h"`. `tigo_monitor` now declares the `espressif/cjson` managed component (`^1.7.19`) on IDF ≥ 6.0 — guarded by version, since on IDF 5.x cJSON is still built-in and adding it would collide. One declaration covers `tigo_server` too (shared `src` target). Builds on the default/recommended IDF 5.x toolchain are unaffected.

## [1.4.3] - 2026-04-17

### Added
- **Home Assistant Ingress support** ([#7](https://github.com/RAR/esphome-tigomonitor/pull/7)) — contributed by [@aeozyalcin](https://github.com/aeozyalcin)
  - Web UI now works when proxied through Home Assistant Ingress (e.g. the [hass_ingress](https://github.com/lovelylain/hass_ingress) integration)
  - Nav links are set dynamically from `window.location.pathname` so they resolve under any URL prefix
  - `apiFetch()` prepends the detected `BASE_PATH` to all `/api/*` requests
  - Post-reset redirect on the Status page returns to the ingress-prefixed root
  - Purely client-side JavaScript change — no C++ or ESPHome config changes required for standalone access
  - See [Web Server & API](https://rar.github.io/esphome-tigomonitor/guides/web-server/#home-assistant-ingress) for setup details

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
