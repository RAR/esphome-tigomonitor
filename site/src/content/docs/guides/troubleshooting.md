---
title: Troubleshooting Guide
---

Common issues and solutions for ESPHome Tigo Monitor.

## Quick Fixes

| Symptom | Solution |
|---------|----------|
| No devices discovered | Check UART wiring, verify 38400 baud ([Wiring](./wiring.md)) |
| High packet loss | Add `CONFIG_UART_ISR_IN_IRAM: "y"` ([UART Optimization](./uart-optimization.md)) |
| Memory exhaustion | Use ESP32-S3 with PSRAM (required for 15+ devices) |
| CCA sync fails (local HTTP) | Verify CCA IP, check network connectivity |
| CCA sync returns 401 / "HTTP locked" | Tigo firmware 4.0.4+ closed local HTTP — use `cca_source: ble` or `cloud_import: true` |
| BLE CCA won't connect | Close the Tigo phone app (one BLE central at a time) |
| Tigo cloud import fails | Recheck credentials; needs `cloud_import: true`; token may have expired |
| History reads back empty after reboot | Erase the tsdb partition once ([TSDB](./tsdb-integration.md)) |
| Web UI not loading | Confirm `tigo_server` configured, check ESP32 IP |

---

## Device Discovery Issues

### No Devices Found

**Symptoms:** No "Frame received" or "New device discovered" log messages.

**Solutions:**
1. Verify UART wiring (TX→RX, RX→TX) — see the [Wiring Guide](./wiring.md)
2. Confirm baud rate is 38400
3. Check Tigo system is powered and communicating
4. Look for any "Frame" messages in ESPHome logs

### Devices Found But No Barcodes

**Symptoms:** Devices show as "Module XXXX" instead of barcode.

**Explanation:** Frame 27 (16-char barcode) may not transmit immediately. This is normal.

**Solutions:**
- Wait longer (barcodes come from periodic Frame 27)
- Devices work without barcodes
- Use CCA sync to get panel names

---

## UART Communication

### "Packet missed!" Errors

**Cause:** UART interrupt not in IRAM, causing missed frames.

**Solution:** Add to ESP-IDF config:
```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_UART_ISR_IN_IRAM: "y"
```

**Expected miss rate:** 0.02-0.04% is excellent for multi-drop RS485. For the deep dive, see [UART Optimization](./uart-optimization.md).

### High Frame Loss Rate

**Solutions:**
1. Ensure `CONFIG_UART_ISR_IN_IRAM: "y"` is set
2. Size the RX buffer correctly (see below)
3. Check for electrical interference on RS485 line
4. Verify proper termination on RS485 bus

See [UART Optimization](./uart-optimization.md) for the full tuning guide.

### RX Buffer Sizing

`rx_buffer_size:` and `CONFIG_UART_RX_BUFFER_SIZE` are **not** alternatives — set **both to the same value**. The sdkconfig option sizes the ESP-IDF driver's ring buffer; the `uart:` option must match it so ESPHome doesn't fight the driver. Every shipping board YAML sets them together:

```yaml
esp32:
  framework:
    type: esp-idf
    sdkconfig_options:
      CONFIG_UART_RX_BUFFER_SIZE: "4096"

uart:
  id: tigo_uart
  rx_pin: GPIO5
  tx_pin: GPIO6
  baud_rate: 38400
  rx_buffer_size: 4096   # match CONFIG_UART_RX_BUFFER_SIZE
```

2-8 KB is plenty on an ESP32-S3 at 38400 baud. This buffer comes from internal (DMA-capable) RAM, never PSRAM, so an oversized value wastes scarce internal heap — a 32 KB buffer silently spends a sixth of the usable internal RAM. See [UART Optimization](./uart-optimization.md).

---

## Memory Issues

### Socket Creation Failures

**Symptoms:** 
- `Failed to create socket` in logs
- Web interface crashes
- System instability

**Cause:** Internal RAM exhaustion without PSRAM.

**Solutions:**
1. **Immediate:** Reboot ESP32
2. **Permanent:** Upgrade to ESP32-S3 with PSRAM (e.g., M5Stack AtomS3R)

### Memory Limits

| Configuration | Device Limit | Notes |
|---------------|--------------|-------|
| Without PSRAM | ~10 devices | Not recommended, unstable with web UI |
| With PSRAM | 36+ devices | Tested stable |

### PSRAM Not Detected After Enabling

**Symptoms:**
- ESP32 crashes and rolls back to previous firmware
- Logs show `OTA rollback detected! Rolled back from partition 'app1'`
- PSRAM not recognized despite `esptool.py` confirming it exists

**Cause:** ESPHome does not rebuild the bootloader when changing PSRAM configuration. The old bootloader doesn't initialize PSRAM, causing boot failure.

**Solution:**
1. Clean ESPHome build files: `esphome clean <yaml>`
2. Erase flash completely: `esptool.py erase_flash`
3. Flash again: `esphome run <yaml>`

This forces a fresh bootloader build with PSRAM support enabled.

---

## CCA Integration

> CCA sync only fills in friendly panel/string/inverter names. Your local RS485 monitoring — power, voltage, temperature, energy — works fine without it.

### Sync Not Working (local HTTP)

**Checklist:**
1. CCA IP address correct?
2. ESP32 can reach CCA on network?
3. CCA responding? (Test: `http://cca-ip/` in browser)
4. Check logs for "CCA Sync complete" or error messages

### CCA Sync Returns 401 / "HTTP Locked" (Tigo firmware 4.0.4+)

**Symptoms:** Sync fails with a `401` or an "HTTP locked" message. The CCA web page also refuses local logins.

**Cause:** Newer Tigo CCA firmware (4.0.4 and up, including `4.0.5-ct`) closes the local HTTP API. This is a Tigo change, not a bug in this component. Your local RS485 monitoring is unaffected — only friendly-name import from the CCA is blocked.

**Fix — pick one:**
1. **Read the CCA over Bluetooth.** Set `cca_source: ble` and add a `ble_client_id`; `tigo_server` talks the CCA's `mobile_api` over BLE instead of HTTP. See [CCA over Bluetooth](./configuration.md#cca-over-bluetooth-cca_source-ble).
2. **Recover the layout from Tigo's cloud.** Set `cloud_import: true` and sign in on the Tigo Cloud page to pull panel names + string/MPPT/inverter layout. See [Tigo cloud import](./configuration.md#tigo-cloud-import-cloud_import-true).
3. **Enter friendly names manually** from the Tools view — no CCA connection required.

### BLE CCA Won't Connect / Search Finds Nothing

**Symptoms:** The CCA Info **CCA Connection** search returns nothing, or the BLE link never connects.

**Solutions:**
1. **Close the Tigo phone app.** The CCA allows only one BLE central at a time — if the app is connected, the ESP32 can't be.
2. The CCA advertises on the `04:C0:5B` Tigo MAC prefix; the search card filters for it. If nothing shows, move the ESP32 closer or confirm the CCA has BLE enabled.
3. Connection is **on demand** — the link opens for each read (~10 s round trip) and drops afterward, so a brief delay is normal; it isn't held open at boot.
4. Confirm `esp32_ble` and `esp32_ble_tracker` are configured and `ble_client_id` points at a `ble_client:` block. See [CCA over Bluetooth](./configuration.md#cca-over-bluetooth-cca_source-ble).

### Tigo Cloud Import Fails / Login Rejected

**Symptoms:** Cloud sign-in on the Tigo Cloud page is rejected, or layout import returns nothing.

**Solutions:**
1. Recheck your Tigo account **credentials** (the same login the Tigo mobile app uses). Only the resulting bearer token is stored on-device — never your password.
2. The stored **token expires**; if import stops working after it worked before, sign in again to refresh it.
3. Confirm `cloud_import: true` is set — without it the Tigo Cloud page and cloud client aren't compiled in. See [Tigo cloud import](./configuration.md#tigo-cloud-import-cloud_import-true).

### Barcode Mismatch

**Cause:** CCA barcodes may differ slightly from UART Frame 27 barcodes.

**Solution:** Component uses fuzzy matching (last 6 chars, 1-char tolerance). If still not matching, check logs for discovered barcodes and CCA barcodes.

---

## Web Interface

### Not Accessible

**Checklist:**
1. `tigo_server` component configured?
2. Correct ESP32 IP? (Check ESPHome logs)
3. Port 80 not blocked?
4. Try direct IP: `http://x.x.x.x/`

### Slow or Unresponsive

**Solutions:**
1. Use ESP32-S3 with PSRAM
2. Reduce `number_of_devices` if set too high
3. Check heap memory on `/status` page

---

## Energy Data

### Energy Total Resets on Reboot

This is about the **running energy counter** (total kWh), which is separate from the on-flash History database — see [History Reads Back Empty](#history-reads-back-empty--0-records-after-reboot) below for that.

**Normal behavior:** The component saves the energy total to NVS hourly to reduce flash wear.

**Maximum data loss:** 1 hour of energy accumulation on an unexpected reboot.

**Logs to verify:**
- `Restored total energy: X.XX kWh`
- `Energy data saved at hour boundary`

### Energy Not Accumulating

**Checklist:**
1. Sensors publishing? (Check HA entities)
2. Night mode active? (Check binary sensor)
3. Power readings valid? (Non-zero during daylight)

---

## On-Flash History (TSDB)

The History view is backed by an on-flash time-series database (`esp_tsdb`) stored on a LittleFS partition. This is separate from the hourly energy total above — it keeps per-snapshot rollups and per-panel power that survive reboots and OTA updates.

### History Reads Back Empty / "0 records" After Reboot

**Symptoms:** The History view or `/api/tsdb/stats` shows 0 records after every reboot, even though data accumulates while the device is up.

**Cause:** A too-small LittleFS partition left no copy-on-write headroom, so the database couldn't commit and read back empty after a restart. This was a real bug, fixed by right-sizing the partition.

**Solutions:**
1. Update to the current firmware (the partition layout is corrected).
2. **Existing installs may need to erase the tsdb partition once** so LittleFS reformats it at the new size. After that, history persists across reboots.
3. Confirm your config pins `zakery292/esp_tsdb^2.1.0` (or newer) and uses the custom partition table.

See [TSDB Integration](./tsdb-integration.md) for the schema, sizing, and partition setup.

### History View Empty / No `/api/history` Endpoints

**Cause:** esp_tsdb is opt-in — without the extra components and custom partition table it isn't compiled in.

**Solution:** Add the esp_tsdb + littlefs components and the custom partition table as shown in [TSDB Integration](./tsdb-integration.md). The rest of the component works fine without it; you just lose the History view and the `/api/history/*` and `/api/tsdb/stats` endpoints.

---

## Night Mode

**Normal behavior:** After 60 minutes without data:
- Publishes zeros every 10 minutes
- Prevents stale data in Home Assistant
- Temperatures show as unavailable

**Customize timeout:**
```yaml
tigo_monitor:
  night_mode_timeout: 90  # Minutes (1-1440)
```

---

## Compilation Errors

### `fatal error: esphome/components/sensor/sensor.h: No such file or directory`

**Cause:** ESPHome only generates `#include` headers for components that are declared in your YAML. The tigo_monitor C++ code includes `sensor/sensor.h`, `text_sensor/text_sensor.h`, and `binary_sensor/binary_sensor.h`, so these must exist in your config.

**Solution:** Add stub sections to your YAML (even if empty):

```yaml
sensor:

text_sensor:

binary_sensor:
```

You can add actual sensors later — the empty declarations are enough to trigger header generation.

### "has no member" Error

**Cause:** Method renamed during refactoring.

**Solution:** Update to latest component version, or check for stale method names.

### external_components Not Loading

**Cause:** The shorthand `github://` format may not resolve correctly depending on ESPHome version.

**Solution:** Use the expanded format:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/RAR/esphome-tigomonitor
    components: [ tigo_monitor, tigo_server ]
    refresh: 0s
```

For a specific release:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/RAR/esphome-tigomonitor
      ref: v1.3.1
    components: [ tigo_monitor, tigo_server ]
    refresh: 0s
```

### ESP-IDF Errors

**Solutions:**
1. Use ESPHome 2026.5.0+
2. Set `framework: type: esp-idf, version: recommended`
3. Try clean compile: `esphome clean <yaml> && esphome compile <yaml>`

### `fatal error: esp_bt_defs.h: No such file or directory` (BLE build on ESP-IDF 6.0.x)

**Symptom:** `esp32_ble_tracker.h:22:10: fatal error: esp_bt_defs.h: No such file or directory`
while compiling `ble_client` / `esp32_ble_tracker` — only on BLE builds
(`cca_source: ble`/`auto`, or any config with `esp32_ble_tracker`).

**Cause:** an explicit `version: "6.0.x"` framework pin. ESPHome 2026.x maps that
to the pioarduino `prep_IDF6` platform, a **moving branch** that now delivers
**ESP-IDF 6.0.2**. IDF 6.0.2 relocated the Bluedroid public headers to
`components/bt/host/bluedroid/api/include/api/` and no longer puts them on the
global include path, while ESPHome's `esp32_ble_tracker.h` still does
`#include <esp_bt_defs.h>` without declaring `bt` as a component requirement — so
every BLE source fails to compile. This is an upstream ESPHome × IDF-6
incompatibility, not a TigoMonitor issue, and it is independent of the ESPHome
release (`recommended` is IDF **5.5.4** on both 2026.6.5 and 2026.7.0).

**Solution:** don't pin IDF 6 for BLE builds — use the recommended toolchain:

```yaml
esp32:
  framework:
    type: esp-idf
    version: recommended   # IDF 5.5.4 — builds cca_source: ble cleanly
```

IDF 6 is not required for BLE. If you specifically need IDF 6 *and* BLE, it is
blocked until ESPHome adds `bt` to the BLE component's `REQUIRES` upstream; stay
on `recommended` in the meantime. (The non-BLE IDF-6 build path is unaffected —
see `boards/test-idf6-tigomonitor.yaml`.)

---

## Reset Commands

### Reset Node Table

Clears all device mappings:

```yaml
button:
  - platform: tigo_monitor
    name: "Reset Node Table"
    tigo_monitor_id: tigo_hub
    button_type: reset_node_table
```

### Force CCA Re-sync

```yaml
button:
  - platform: tigo_monitor
    name: "Sync from CCA"
    tigo_monitor_id: tigo_hub
    button_type: sync_from_cca
```

---

## Log Messages Reference

| Message | Meaning |
|---------|---------|
| `Frame 27 received` | Long address discovery (16-char barcode) |
| `New device discovered` | First power data from device |
| `Assigned sensor index X` | Device mapped to sensor slot |
| `Energy data saved at hour boundary` | Hourly persistence complete |
| `No Frame 27 long address available` | Device found, waiting for barcode |
| `node table is full` | Increase `number_of_devices` |
| `Maximum number of devices reached` | At configured limit |
| `CCA Sync complete` | Successfully synced with CCA |
| `Packet missed!` | UART frame loss (add ISR to IRAM) |

---

## Getting Help

1. Check ESPHome logs for error messages
2. Review the Diagnostics view for memory info
3. Search existing [GitHub issues](https://github.com/RAR/esphome-tigomonitor/issues)
4. Open a [new issue](https://github.com/RAR/esphome-tigomonitor/issues) with:
   - ESPHome version
   - Hardware (board, PSRAM)
   - Relevant log output
   - Configuration (sanitized)

---

**See also:** [Wiring](./wiring.md) · [UART Optimization](./uart-optimization.md) · [Configuration](./configuration.md) · [← Back to README](https://github.com/RAR/esphome-tigomonitor)
